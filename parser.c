// SQL 파서 (docs/parser.md). 전역 상태 없이 parse_next()의 지역 Parser만 써요.
#include "dbms.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEAR_MAX   32  // 오류 메시지에 넣는 토큰 원문 최대 바이트 수 (7장)
#define INT64_DIGITS 19  // INT64_MAX·INT64_MIN의 자릿수

typedef enum { TOK_KEYWORD, TOK_IDENT, TOK_INT, TOK_STRING, TOK_SYMBOL, TOK_END } TokenKind;

typedef struct {
    TokenKind   kind;
    const char *start;                   // sql 안의 시작 위치 (오류 메시지용)
    size_t      len;
    int64_t     i;                       // TOK_INT
    char        s[DB_STRING_MAX + 1];    // TOK_STRING, TOK_IDENT, TOK_KEYWORD
} Token;

typedef struct {
    const char *sql;
    size_t      length;
    size_t      pos;      // 다음에 읽을 위치
    Token       cur;      // 지금 보고 있는 토큰 (미리 보기 1개)
    char       *error;
} Parser;

static const char *const KEYWORDS[] = {"INSERT", "INTO", "VALUES", "SELECT", "FROM", "WHERE"};

// locale에 따라 결과가 바뀌는 <ctype.h> 대신 ASCII만 봐요.
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_ident_start(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_ident_char(char c) { return is_ident_start(c) || is_digit(c); }

// ASCII 대소문자를 가리지 않고 같은 단어인지 봐요.
static int same_word(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char y = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

static void set_error(Parser *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error, PARSE_ERROR_SIZE, fmt, ap);
    va_end(ap);
}

// 토큰 원문을 앞 NEAR_MAX바이트까지 옮기고 제어 문자는 '?'로 바꿔요. 메시지를 한 줄로 두려고요.
static void token_text(char dst[NEAR_MAX + 1], const char *src, size_t len) {
    if (len > NEAR_MAX) len = NEAR_MAX;
    for (size_t k = 0; k < len; k++) {
        unsigned char c = (unsigned char)src[k];
        dst[k] = (c < 0x20 || c == 0x7f) ? '?' : (char)c;
    }
    dst[len] = '\0';
}

static void error_near(Parser *p, const char *expected) {
    if (p->cur.kind == TOK_END) {
        set_error(p, "unexpected end of statement: expected %s", expected);
        return;
    }
    char text[NEAR_MAX + 1];
    token_text(text, p->cur.start, p->cur.len);
    set_error(p, "near '%s': expected %s", text, expected);
}

static void error_byte(Parser *p, char c) {
    unsigned char u = (unsigned char)c;
    if (u > 0x20 && u < 0x7f)
        set_error(p, "unexpected character '%c'", c);
    else
        set_error(p, "unexpected byte 0x%02x", u);
}

static int lex_word(Parser *p, Token *t) {
    while (p->pos < p->length && is_ident_char(p->sql[p->pos])) p->pos++;
    t->len = (size_t)(p->sql + p->pos - t->start);
    if (t->len > DB_NAME_MAX) {
        set_error(p, "identifier too long (max %d bytes)", DB_NAME_MAX);
        return 0;
    }
    memcpy(t->s, t->start, t->len);
    t->s[t->len] = '\0';
    t->kind = TOK_IDENT;
    for (size_t k = 0; k < sizeof KEYWORDS / sizeof KEYWORDS[0]; k++)
        if (same_word(t->s, KEYWORDS[k])) t->kind = TOK_KEYWORD;
    return 1;
}

static int lex_int(Parser *p, Token *t) {
    int negative = p->sql[p->pos] == '-';
    if (negative) p->pos++;
    size_t digits_start = p->pos;
    while (p->pos < p->length && is_digit(p->sql[p->pos])) p->pos++;
    t->len = (size_t)(p->sql + p->pos - t->start);
    if (p->pos < p->length && is_ident_start(p->sql[p->pos])) {  // 12abc
        error_byte(p, p->sql[p->pos]);
        return 0;
    }

    // 앞자리 0을 빼고 자릿수를 먼저 봐요. sql은 NUL로 끝나지 않을 수 있어서 strtoll에는 복사본을 넘겨요.
    size_t d = digits_start;
    while (d + 1 < p->pos && p->sql[d] == '0') d++;
    size_t digits = p->pos - d;
    char buf[1 + INT64_DIGITS + 1];
    if (digits <= INT64_DIGITS) {
        size_t n = 0;
        if (negative) buf[n++] = '-';
        memcpy(buf + n, p->sql + d, digits);
        buf[n + digits] = '\0';
        errno = 0;
        t->i = strtoll(buf, NULL, 10);
        if (errno != ERANGE) {
            t->kind = TOK_INT;
            return 1;
        }
    }
    char text[NEAR_MAX + 1];
    token_text(text, t->start, t->len);
    set_error(p, "integer out of range: %s", text);
    return 0;
}

static int lex_string(Parser *p, Token *t) {
    size_t out = 0;
    p->pos++;  // 여는 따옴표
    for (;;) {
        if (p->pos >= p->length) {
            set_error(p, "unterminated string");
            return 0;
        }
        char c = p->sql[p->pos++];
        if (c == '\'') {
            if (p->pos < p->length && p->sql[p->pos] == '\'') p->pos++;  // '' → '
            else break;
        } else if (c == '\0') {
            // C 문자열로 넘기면 값이 여기서 잘려요. 조용히 다른 값을 저장하지 않게 거부해요.
            error_byte(p, c);
            return 0;
        }
        if (out == DB_STRING_MAX) {
            set_error(p, "string too long (max %d bytes)", DB_STRING_MAX);
            return 0;
        }
        t->s[out++] = c;
    }
    t->s[out] = '\0';
    t->len = (size_t)(p->sql + p->pos - t->start);
    t->kind = TOK_STRING;
    return 1;
}

// p->cur에 다음 토큰을 넣어요. ';'와 입력 끝은 TOK_END이고 ';'는 소비하지 않아요. 실패하면 error를 쓰고 0.
static int next_token(Parser *p) {
    while (p->pos < p->length && is_space(p->sql[p->pos])) p->pos++;
    Token *t = &p->cur;
    t->start = p->sql + p->pos;
    t->len = 0;
    if (p->pos >= p->length || p->sql[p->pos] == ';') {
        t->kind = TOK_END;
        return 1;
    }

    char c = p->sql[p->pos];
    if (is_ident_start(c)) return lex_word(p, t);
    if (is_digit(c) || (c == '-' && p->pos + 1 < p->length && is_digit(p->sql[p->pos + 1])))
        return lex_int(p, t);
    if (c == '\'') return lex_string(p, t);
    switch (c) {
    case '(': case ')': case ',': case '*': case '=':
        p->pos++;
        t->len = 1;
        t->kind = TOK_SYMBOL;
        return 1;
    default:
        error_byte(p, c);
        return 0;
    }
}

static int is_keyword(const Parser *p, const char *kw) {
    return p->cur.kind == TOK_KEYWORD && same_word(p->cur.s, kw);
}

static int is_symbol(const Parser *p, char c) {
    return p->cur.kind == TOK_SYMBOL && p->cur.start[0] == c;
}

static int expect_keyword(Parser *p, const char *kw) {
    if (!is_keyword(p, kw)) {
        error_near(p, kw);
        return 0;
    }
    return next_token(p);
}

static int expect_symbol(Parser *p, char c) {
    if (!is_symbol(p, c)) {
        char quoted[4] = {'\'', c, '\'', '\0'};
        error_near(p, quoted);
        return 0;
    }
    return next_token(p);
}

static int expect_ident(Parser *p, char dst[DB_NAME_MAX + 1], const char *what) {
    if (p->cur.kind != TOK_IDENT) {
        error_near(p, what);
        return 0;
    }
    memcpy(dst, p->cur.s, p->cur.len + 1);
    return next_token(p);
}

static int parse_literal(Parser *p, Value *out) {
    if (p->cur.kind == TOK_INT) {
        out->type = VALUE_INT;
        out->i = p->cur.i;
    } else if (p->cur.kind == TOK_STRING) {
        out->type = VALUE_STRING;
        memcpy(out->s, p->cur.s, sizeof out->s);
    } else {
        error_near(p, "value");
        return 0;
    }
    return next_token(p);
}

// insert := INSERT INTO ident VALUES '(' literal { ',' literal } ')'
static int parse_insert(Parser *p, Command *out) {
    out->type = CMD_INSERT;
    if (!expect_keyword(p, "INSERT") || !expect_keyword(p, "INTO") ||
        !expect_ident(p, out->table, "table name") || !expect_keyword(p, "VALUES") ||
        !expect_symbol(p, '('))
        return 0;
    for (;;) {
        if (out->value_count == DB_VALUES_MAX) {
            set_error(p, "too many values (max %d)", DB_VALUES_MAX);
            return 0;
        }
        if (!parse_literal(p, &out->values[out->value_count])) return 0;
        out->value_count++;
        if (!is_symbol(p, ',')) return expect_symbol(p, ')');
        if (!next_token(p)) return 0;
    }
}

// select := SELECT '*' FROM ident [ WHERE ident '=' literal ]
static int parse_select(Parser *p, Command *out) {
    out->type = CMD_SELECT;
    if (!expect_keyword(p, "SELECT") || !expect_symbol(p, '*') || !expect_keyword(p, "FROM") ||
        !expect_ident(p, out->table, "table name"))
        return 0;
    if (!is_keyword(p, "WHERE")) return 1;
    out->has_where = 1;
    return next_token(p) && expect_ident(p, out->where_column, "column name") &&
           expect_symbol(p, '=') && parse_literal(p, &out->where_value);
}

// 오류 뒤 커서를 작은따옴표 밖의 다음 ';' 바로 뒤로, 없으면 입력 끝으로 옮겨요.
static void skip_to_statement_end(Parser *p) {
    int in_string = 0;
    while (p->pos < p->length) {
        char c = p->sql[p->pos++];
        if (c == '\'') in_string = !in_string;
        else if (c == ';' && !in_string) return;
    }
}

DbStatus parse_next(const char *sql, size_t length, size_t *cursor,
                    Command *out, char error[PARSE_ERROR_SIZE]) {
    assert(sql && cursor && out && error && *cursor <= length);
    memset(out, 0, sizeof *out);
    error[0] = '\0';
    Parser p = {.sql = sql, .length = length, .pos = *cursor, .error = error};

    // 공백과 빈 문장(;)은 건너뛰고 번호도 세지 않아요.
    while (p.pos < length && (is_space(sql[p.pos]) || sql[p.pos] == ';')) p.pos++;
    if (p.pos >= length) {
        *cursor = length;
        return DB_END;
    }

    size_t statement_start = p.pos;
    int ok = next_token(&p);
    if (ok) {
        if (is_keyword(&p, "INSERT")) ok = parse_insert(&p, out);
        else if (is_keyword(&p, "SELECT")) ok = parse_select(&p, out);
        else { error_near(&p, "INSERT or SELECT"); ok = 0; }
    }
    if (ok && p.cur.kind != TOK_END) {
        error_near(&p, "end of statement");
        ok = 0;
    }
    if (ok) {
        if (p.pos < length) p.pos++;  // TOK_END가 입력 끝이 아니면 ';' 위에 있어요
        *cursor = p.pos;
        return DB_OK;
    }

    memset(out, 0, sizeof *out);
    p.pos = statement_start;  // 문장 처음부터 다시 세야 따옴표 안팎이 맞아요
    skip_to_statement_end(&p);
    *cursor = p.pos;
    return DB_ERR_SYNTAX;
}

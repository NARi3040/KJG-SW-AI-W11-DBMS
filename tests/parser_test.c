// 사용법: make test
// docs/parser.md 9장(T-1~T-20)과 docs/test.md 3장(P01·P02)을 확인해요. parser.c만 링크해요.
#include "dbms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(id, cond)                                                        \
    do {                                                                       \
        if (cond) {                                                            \
            printf("PASS %s\n", id);                                           \
        } else {                                                               \
            printf("FAIL %s: %s (%s:%d)\n", id, #cond, __FILE__, __LINE__);    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

typedef struct {
    Command cmd;
    char    error[PARSE_ERROR_SIZE];
    size_t  cursor;
} Run;

// sql 앞 length 바이트를 NUL 없는 딱 맞는 버퍼에 옮겨 불러요. 경계 밖을 읽으면 ASan이 잡아요.
static DbStatus parse_bytes(Run *r, const char *sql, size_t length) {
    char *buf = malloc(length ? length : 1);
    if (!buf) {
        perror("malloc");
        exit(2);
    }
    memcpy(buf, sql, length);
    DbStatus st = parse_next(buf, length, &r->cursor, &r->cmd, r->error);
    free(buf);
    return st;
}

static DbStatus parse(Run *r, const char *sql) { return parse_bytes(r, sql, strlen(sql)); }

// 처음부터 한 번 불러요.
static DbStatus parse_once(Run *r, const char *sql) {
    r->cursor = 0;
    return parse(r, sql);
}

static int is_zero_command(const Command *c) {
    static const Command zero;  // 읽기 전용 비교 대상
    return memcmp(c, &zero, sizeof zero) == 0;
}

// 오류 계약: 버퍼 안에서 NUL로 끝나고 줄바꿈이 없으며, 명령은 0으로 채워져 있어요.
static int syntax_error(const Run *r, DbStatus st, const char *expected) {
    return st == DB_ERR_SYNTAX && memchr(r->error, '\0', PARSE_ERROR_SIZE) != NULL &&
           strchr(r->error, '\n') == NULL && strcmp(r->error, expected) == 0 &&
           is_zero_command(&r->cmd);
}

static void test_doc_cases(void) {
    Run r = {0};
    char sql[1024];
    DbStatus st;

    st = parse_once(&r, "INSERT INTO users VALUES ('Kim Min', 23);");
    CHECK("T-1", st == DB_OK && r.cmd.type == CMD_INSERT && strcmp(r.cmd.table, "users") == 0 &&
                     r.cmd.value_count == 2 && r.cmd.values[0].type == VALUE_STRING &&
                     strcmp(r.cmd.values[0].s, "Kim Min") == 0 && r.cmd.values[1].type == VALUE_INT &&
                     r.cmd.values[1].i == 23 && r.cmd.has_where == 0 && r.cursor == 41 &&
                     r.error[0] == '\0');

    st = parse_once(&r, "SELECT * FROM users");
    CHECK("T-2", st == DB_OK && r.cmd.type == CMD_SELECT && r.cmd.has_where == 0 && r.cursor == 19 &&
                     parse(&r, "SELECT * FROM users") == DB_END);

    st = parse_once(&r, "select * from users where id = 7");
    CHECK("T-3", st == DB_OK && r.cmd.type == CMD_SELECT && strcmp(r.cmd.table, "users") == 0 &&
                     r.cmd.value_count == 0 && r.cmd.has_where == 1 &&
                     strcmp(r.cmd.where_column, "id") == 0 && r.cmd.where_value.type == VALUE_INT &&
                     r.cmd.where_value.i == 7);

    st = parse_once(&r, "SELECT * FROM users WHERE name = 'O''Brien';");
    CHECK("T-4", st == DB_OK && r.cmd.has_where == 1 && strcmp(r.cmd.where_column, "name") == 0 &&
                     r.cmd.where_value.type == VALUE_STRING &&
                     strcmp(r.cmd.where_value.s, "O'Brien") == 0);

    st = parse_once(&r, "INSERT INTO users VALUES ('a;b', -5);");
    CHECK("T-5", st == DB_OK && strcmp(r.cmd.values[0].s, "a;b") == 0 && r.cmd.values[1].i == -5 &&
                     parse(&r, "INSERT INTO users VALUES ('a;b', -5);") == DB_END);

    st = parse_once(&r, ";; ;");
    CHECK("T-6", st == DB_END && r.cursor == 4);

    st = parse_once(&r, "");
    CHECK("T-7", st == DB_END && r.cursor == 0);

    st = parse_once(&r, "SELECT * FORM users; SELECT * FROM users;");
    CHECK("T-8", syntax_error(&r, st, "near 'FORM': expected FROM") && r.cursor == 20 &&
                     parse(&r, "SELECT * FORM users; SELECT * FROM users;") == DB_OK);

    st = parse_once(&r, "INSERT INTO users VALUES ('abc");
    CHECK("T-9", syntax_error(&r, st, "unterminated string") && r.cursor == 30);

    st = parse_once(&r, "INSERT INTO users VALUES (99999999999999999999);");
    CHECK("T-10", syntax_error(&r, st, "integer out of range: 99999999999999999999"));

    const char *exact = "SELECT * FROM users WHERE id = 7";
    r.cursor = 0;
    st = parse_bytes(&r, exact, 32);
    CHECK("T-10a", strlen(exact) == 32 && st == DB_OK && r.cmd.where_value.i == 7 && r.cursor == 32);

    // 33바이트 버퍼에 length = 32. 마지막 '8'은 범위 밖이에요.
    char *wide = malloc(33);
    if (!wide) {
        perror("malloc");
        exit(2);
    }
    memcpy(wide, "SELECT * FROM users WHERE id = 78", 33);
    r.cursor = 0;
    st = parse_next(wide, 32, &r.cursor, &r.cmd, r.error);
    CHECK("T-10b", st == DB_OK && r.cmd.where_value.i == 7 && r.cursor == 32);
    free(wide);

    snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('%0256d');", 0);
    memset(sql + 27, 'a', 256);
    CHECK("T-11", syntax_error(&r, parse_once(&r, sql), "string too long (max 255 bytes)"));

    snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('%0255d');", 0);
    memset(sql + 27, 'a', 255);
    st = parse_once(&r, sql);
    CHECK("T-12", st == DB_OK && strlen(r.cmd.values[0].s) == 255);

    snprintf(sql, sizeof sql, "SELECT * FROM %032d;", 0);
    memset(sql + 14, 'a', 32);
    CHECK("T-13", syntax_error(&r, parse_once(&r, sql), "identifier too long (max 31 bytes)"));

    st = parse_once(&r, "INSERT INTO users VALUES (1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1);");
    CHECK("T-14", syntax_error(&r, st, "too many values (max 16)"));

    st = parse_once(&r, "INSERT INTO users VALUES (1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1);");
    CHECK("T-15", st == DB_OK && r.cmd.value_count == 16);

    st = parse_once(&r, "SELECT * FROM users users;");
    CHECK("T-16", syntax_error(&r, st, "near 'users': expected end of statement"));

    st = parse_once(&r, "SELECT * FROM users #");
    CHECK("T-17", syntax_error(&r, st, "unexpected character '#'"));

    st = parse_once(&r, "INSERT INTO users VALUES (");
    CHECK("T-18", syntax_error(&r, st, "unexpected end of statement: expected value"));

    st = parse_once(&r, "INSERT INTO users VALUES ()");
    CHECK("T-19", syntax_error(&r, st, "near ')': expected value"));

    st = parse_once(&r, "INSERT INTO users VALUES (- 5)");
    CHECK("T-20", syntax_error(&r, st, "unexpected character '-'"));
}

// docs/test.md 3장
static void test_unit_cases(void) {
    Run r = {0};
    const char *sql = "INSERT INTO users VALUES ('Mina', 20);\nSELECT * FROM users WHERE age = 31;\n";
    DbStatus a = parse(&r, sql);
    int insert_ok = a == DB_OK && r.cmd.type == CMD_INSERT && strcmp(r.cmd.values[0].s, "Mina") == 0 &&
                    r.cmd.values[1].i == 20 && r.cursor == 38;
    DbStatus b = parse(&r, sql);
    int select_ok = b == DB_OK && r.cmd.type == CMD_SELECT && strcmp(r.cmd.where_column, "age") == 0 &&
                    r.cmd.where_value.i == 31 && r.cursor == 74;
    CHECK("P01", insert_ok && select_ok && parse(&r, sql) == DB_END && r.cursor == strlen(sql));

    DbStatus bad_select = parse_once(&r, "SELECT FROM users;");
    int bad_select_ok = syntax_error(&r, bad_select, "near 'FROM': expected '*'");
    DbStatus open_insert = parse_once(&r, "INSERT INTO users VALUES ('Mina', 20");
    CHECK("P02", bad_select_ok && syntax_error(&r, open_insert, "unexpected end of statement: expected ')'"));
}

// 9장 표에는 없지만 5~7장에 적힌 규칙
static void test_doc_rules(void) {
    Run r = {0};
    char sql[1024];
    char expected[PARSE_ERROR_SIZE];
    DbStatus st;

    st = parse_once(&r, "SELECT * FROM select");
    CHECK("R-keyword-table", syntax_error(&r, st, "near 'select': expected table name"));

    st = parse_once(&r, "SELECT * FROM users WHERE = 1");
    CHECK("R-column-name", syntax_error(&r, st, "near '=': expected column name"));

    st = parse_once(&r, "UPDATE users");
    CHECK("R-first-token", syntax_error(&r, st, "near 'UPDATE': expected INSERT or SELECT"));

    st = parse_once(&r, "iNsErT iNtO USERS vAlUeS ('CaseCheck', 20)");
    CHECK("R-keyword-case", st == DB_OK && strcmp(r.cmd.table, "USERS") == 0 &&
                                strcmp(r.cmd.values[0].s, "CaseCheck") == 0);

    st = parse_once(&r, "INSERT INTO users VALUES ('', -9223372036854775808)");
    CHECK("R-int64-min-empty-string", st == DB_OK && r.cmd.values[0].type == VALUE_STRING &&
                                          r.cmd.values[0].s[0] == '\0' &&
                                          r.cmd.values[1].i == INT64_MIN);

    st = parse_once(&r, "INSERT INTO users VALUES (9223372036854775808)");
    CHECK("R-int64-max-plus-1", syntax_error(&r, st, "integer out of range: 9223372036854775808"));

    st = parse_once(&r, "INSERT INTO users VALUES (00000000000000000000000000000042)");
    CHECK("R-leading-zeros", st == DB_OK && r.cmd.values[0].i == 42);

    // 900자리 정수는 앞 32자리만 메시지에 나와요.
    memset(sql, '9', 900);
    sql[900] = '\0';
    char statement[1024];
    snprintf(statement, sizeof statement, "INSERT INTO users VALUES (%.900s)", sql);
    snprintf(expected, sizeof expected, "integer out of range: %.32s", sql);
    CHECK("R-near-cut-32", syntax_error(&r, parse_once(&r, statement), expected));

    st = parse_once(&r, "SELECT * FROM users 'a\nb\tc'");
    CHECK("R-control-char", syntax_error(&r, st, "near ''a?b?c'': expected end of statement"));

    st = parse_once(&r, "INSERT INTO users VALUES (12abc)");
    CHECK("R-digit-letter", syntax_error(&r, st, "unexpected character 'a'"));

    r.cursor = 0;
    st = parse_bytes(&r, "SELECT * FROM users \0", 21);
    CHECK("R-nul-byte", syntax_error(&r, st, "unexpected byte 0x00"));

    r.cursor = 0;
    st = parse_bytes(&r, "INSERT INTO users VALUES ('a\0b', 1)", 35);
    CHECK("R-nul-in-string", syntax_error(&r, st, "unexpected byte 0x00") && r.cursor == 35);

    st = parse_once(&r, "SELECT * FROM \xEA\xB0\x80");  // 따옴표 밖의 UTF-8 '가'
    CHECK("R-non-ascii", syntax_error(&r, st, "unexpected byte 0xea"));

    st = parse_once(&r, "SELECT # 'a;b'; SELECT * FROM users");
    CHECK("R-skip-quoted-semicolon", syntax_error(&r, st, "unexpected character '#'") && r.cursor == 15);

    // 오류 뒤 DB_END: 명령은 0, 오류 문자열은 비어 있어요.
    const char *tail = "SELECT * FORM users;  ;\n";
    r.cursor = 0;
    parse(&r, tail);
    st = parse(&r, tail);
    CHECK("R-end-clears", st == DB_END && r.error[0] == '\0' && is_zero_command(&r.cmd) &&
                              r.cursor == strlen(tail));
}

int main(void) {
    test_doc_cases();
    test_unit_cases();
    test_doc_rules();
    printf("%s: %d failed\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}

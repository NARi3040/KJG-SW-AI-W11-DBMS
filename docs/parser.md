# 파서 세부 설계 (`parser.c` · 박준호)

> **기준 문서:** [모듈 구성과 호출 계약](architecture.md)(큰 설계). 이 문서는 그중 파서 담당 부분만 자세히 적어요.
> P-1~P-3은 수용됐어요. [2장](#2-큰-설계에-반영한-결정)의 고정 크기 명령, 오류 버퍼, WHERE 지원을 공통 계약으로 사용해요.

## 목차

1. [맡는 것과 맡지 않는 것](#1-맡는-것과-맡지-않는-것)
2. [큰 설계에 반영한 결정](#2-큰-설계에-반영한-결정)
3. [공개 타입과 함수](#3-공개-타입과-함수)
4. [문장 경계](#4-문장-경계)
5. [토큰 규칙](#5-토큰-규칙)
6. [문법](#6-문법)
7. [반환값과 오류](#7-반환값과-오류)
8. [내부 구조](#8-내부-구조)
9. [테스트](#9-테스트)
10. [정하지 않은 것](#10-정하지-않은-것)

## 1. 맡는 것과 맡지 않는 것

| 맡는 것 | 맡지 않는 것 (담당) |
| --- | --- |
| SQL 문자열에서 문장 하나의 경계 찾기 | SQL 파일 읽기, 문장 번호 세기, 오류 출력 (`main.c` · 최무헌) |
| 글자를 토큰으로 자르기 (렉서) | 테이블이 `users`인지, 값 개수·자료형이 맞는지 검사 (`execute.c` · 최무헌) |
| 토큰 순서가 문법에 맞는지 검사 | `id`·`age`의 범위 검사 (`execute.c` · 최무헌) |
| 결과를 `Command`에 채우기 | 파일 읽기·쓰기 (`storage.c` · 노도현) |

- 파서는 다른 모듈의 함수를 호출하지 않아요. 표준 라이브러리만 써요.
- 렉서는 따로 파일을 두지 않고 `parser.c` 안의 `static` 함수로 둬요. 렉서를 쓰는 곳이 파서 하나뿐이라서예요.

## 2. 큰 설계에 반영한 결정

P-1~P-3을 수용했어요. 아래 이전 계약은 변경 이력이며, 현재 계약은 결정 열과 3장을 따라요.

| 번호 | 이전 계약 | 수용한 결정 | 이유 | 영향받는 곳 |
| --- | --- | --- | --- | --- |
| P-1 | `parse_next()`가 `Command`를 malloc하고 `command_free()`로 해제 | `Command`를 **고정 크기 구조체**로 두고, 호출자가 준비한 공간에 채워요. `command_free()`는 없애요 | 이름·문자열·값 개수에 한도(31바이트, 255바이트, 16개)를 두면 크기를 고정할 수 있어요. 메모리 부족 오류와 해제 누락이 사라져요. Phase 3에서 스레드마다 스택에 두면 돼요 | `main.c` 호출부 (3.3절), 공통 헤더 한도 적용 |
| P-2 | 문법 오류의 원인을 전달하는 방식 미정 | `parse_next()`가 호출자의 문자 배열에 영어 한 줄 오류 메시지를 써요 | `main`이 "몇 번째 문장, 무엇이 틀렸는지"를 출력할 수 있어요 | `main.c` 오류 출력 |
| P-3 | SQL 문법은 INSERT, SELECT | SELECT에 `WHERE 열 = 값` 조건 하나를 더해요 | Phase 2가 `WHERE id = ?`에서 B+ 트리를 쓰라고 해요. 지금 넣으면 Phase 2에서 파서를 다시 열지 않아도 돼요 | `execute.c`가 조건을 검사하고 행을 걸러야 해요 |

- P-1의 파서 한도는 3.1절 상수로 수용했어요. 저장할 name의 스키마 제약과 age 범위는 실행기, 저장 담당자가 이 표현 범위 안에서 맞춰요.
- P-3에서 조건 열을 `id`로 묶지 않는 이유: Phase 2 명세가 "ID 기준 SELECT(인덱스)와 다른 필드 기준 SELECT(선형 탐색)의 속도 비교"를 요구해서 `WHERE name = ...`도 필요해요.
- P-3에서 파서는 문법만 봐요. 조건 열이 `id`, `name`, `age` 중 하나인지, 값의 자료형이 맞는지는 실행기가 검사해요.
- Phase 1은 실행기가 파일 순회 결과에서 조건을 걸러요. 요청별 지역 상태와 내부 콜백으로 일치 행만 전달하고, 외부 콜백이 성공한 행 수를 따로 세요. 구체적인 흐름은 [실행기 설계 5장](execute.md#5-select와-where-처리)을 따라요. B+ 트리 조회 경로는 Phase 2에서 연결하며, 파서 조건 표현은 유지해요.

## 3. 공개 타입과 함수

### 3.1 `dbms.h`에 들어갈 파서 쪽 정의

```c
#include <stddef.h>
#include <stdint.h>

#define DB_NAME_MAX      31   // 식별자 최대 바이트 수 (끝의 '\0' 제외)
#define DB_STRING_MAX    255  // 문자열 값 최대 바이트 수 (끝의 '\0' 제외)
#define DB_VALUES_MAX    16   // INSERT 값 최대 개수
#define PARSE_ERROR_SIZE 128  // 오류 메시지 버퍼 크기 ('\0' 포함)

typedef enum { CMD_INSERT = 1, CMD_SELECT = 2 } CommandType;
typedef enum { VALUE_INT = 1, VALUE_STRING = 2 } ValueType;

typedef struct {
    ValueType type;
    int64_t   i;                      // type == VALUE_INT일 때만 의미 있어요
    char      s[DB_STRING_MAX + 1];   // type == VALUE_STRING일 때만. 따옴표를 벗기고 '' → ' 로 바꾼 값
} Value;

typedef struct {
    CommandType type;
    char        table[DB_NAME_MAX + 1];
    size_t      value_count;          // INSERT: 1 ~ DB_VALUES_MAX, SELECT: 0
    Value       values[DB_VALUES_MAX];
    int         has_where;            // SELECT에 WHERE가 있으면 1, 없으면 0. INSERT는 항상 0
    char        where_column[DB_NAME_MAX + 1];
    Value       where_value;
} Command;
```

- `Command` 하나는 4,712바이트예요(x86-64, gcc 15.2에서 `sizeof`로 잰 값). 기본 스택은 8MB(`ulimit -s`, pthread 기본값도 같음)라 지역 변수로 둬도 돼요.
- `DbStatus`의 실제 이름은 공동으로 정해요. 이 문서는 `DB_OK`, `DB_END`, `DB_ERR_SYNTAX`로 써요.
- `DB_VALUES_MAX`가 16인 이유: `users`는 값 2개면 되지만, 값이 더 많은 INSERT도 문법 오류가 아니라 실행기의 "값 개수 불일치" 오류로 보여 주려고 여유를 둬요. 16개를 넘으면 파서가 오류를 내요.

### 3.2 함수

```c
DbStatus parse_next(const char *sql, size_t length, size_t *cursor,
                    Command *out, char error[PARSE_ERROR_SIZE]);
```

| 인자 | 방향 | 내용 |
| --- | --- | --- |
| `sql` | 입력 | SQL 파일 전체. `'\0'`로 끝나지 않아도 돼요. 파서는 읽기만 하고 보관하지 않아요 |
| `length` | 입력 | `sql`의 바이트 수 |
| `cursor` | 입출력 | 넣을 때: 읽기 시작할 위치. 나올 때: 다음 문장을 읽을 위치 (7장) |
| `out` | 출력 | `DB_OK`일 때만 내용이 의미 있어요. 다른 반환값에서는 0으로 채워져 있어요 |
| `error` | 출력 | `DB_ERR_SYNTAX`일 때 영어 한 줄 오류 메시지 (줄바꿈 없이 `'\0'`로 끝남). 다른 반환값에서는 빈 문자열이에요 |

- `sql`, `cursor`, `out`, `error` 중 NULL이 있거나 `*cursor > length`이면 `assert`로 멈춰요. 호출자의 버그라서 정상 경로로 다루지 않아요.
- 전역 변수와 `static` 버퍼를 쓰지 않아요. 여러 스레드가 각자의 `out`, `error`로 동시에 불러도 돼요.

### 3.3 `main.c` 호출 예

```c
size_t cursor = 0;
int statement_no = 0;
Command cmd;
char error[PARSE_ERROR_SIZE];

for (;;) {
    DbStatus st = parse_next(sql, length, &cursor, &cmd, error);
    if (st == DB_END) break;
    statement_no++;
    if (st != DB_OK) {
        fprintf(stderr, "[문장 %d] %s\n", statement_no, error);
        break;  // 큰 설계: 오류면 처리를 멈춰요
    }
    /* execute(storage, &cmd, print_row, out, &result); */
}
```

- 명령 해제 함수는 없어요. 호출자의 `Command`를 재사용하고, 그 밖의 흐름은 큰 설계 3장과 같아요.

## 4. 문장 경계

| 입력 (`sql` 전체) | `parse_next()` 연속 호출 결과 |
| --- | --- |
| `SELECT * FROM users; SELECT * FROM users;` | OK, OK, END |
| `SELECT * FROM users` (끝에 `;` 없음) | OK, END |
| `;; SELECT * FROM users;` | OK, END (빈 문장은 건너뛰고 번호도 세지 않아요) |
| `INSERT INTO users VALUES ('a;b', 1);` | OK, END (문자열 안의 `;`에서는 끊지 않아요) |
| 빈 문자열, 공백·줄바꿈만 있음 | END |
| `SELECT * FROM users; INSERT INTO users VALUES ('a` | OK, SYNTAX (`unterminated string`) |

- 문장 끝은 작은따옴표 밖의 `;` 또는 입력 끝이에요. 작은따옴표를 만날 때마다 "문자열 안/밖"을 뒤집으면 `''`도 저절로 맞아요.
- 공백은 스페이스, 탭, `\r`, `\n`이에요.
- 주석(`--`, `/* */`)은 받지 않아요. 쓰면 `unexpected character '-'` 같은 오류가 나요.
- 실제 구현은 경계를 먼저 찾지 않고, 토큰을 하나씩 읽다가 `;`나 입력 끝을 만나면 문장을 끝내요. 위 표와 결과는 같아요.

## 5. 토큰 규칙

| 종류 | 규칙 | 예 |
| --- | --- | --- |
| 키워드 | `INSERT`, `INTO`, `VALUES`, `SELECT`, `FROM`, `WHERE`. 대소문자를 가리지 않아요 | `select`, `Select` |
| 식별자 | 영문자나 `_`로 시작하고 영문자·숫자·`_`만 써요. 최대 31바이트. 대소문자는 그대로 둬요 | `users`, `age` |
| 정수 | 숫자 1개 이상. 바로 앞에 `-`가 붙으면 음수예요. `int64_t` 범위 안이어야 해요 | `23`, `-5` |
| 문자열 | 작은따옴표로 감싸요. 안의 작은따옴표는 `''`로 써요. 따옴표를 벗긴 값이 최대 255바이트 | `'Kim Min'`, `'O''Brien'` |
| 기호 | `(` `)` `,` `*` `=` `;` | |

- 키워드와 같은 이름은 식별자로 쓸 수 없어요. 예: `SELECT * FROM select`는 문법 오류예요.
- 정수는 `strtoll`로 읽고 `errno == ERANGE`이면 오류예요. `atoi`는 범위를 넘어도 오류 없이 틀린 값을 돌려줘요.
- 정수 토큰은 `length` 안에서만 읽고, `strtoll`에는 해당 토큰을 NUL 종료한 문자열로 전달해요. NUL 종료가 보장되지 않은 SQL 버퍼의 포인터를 그대로 넘기지 않아요.
- `int64_t`보다 좁은 범위(예: `age`는 int32)는 실행기가 검사해요. 파서는 열의 자료형을 모르기 때문이에요.
- `-` 뒤에 숫자가 바로 오지 않으면 오류예요. `- 5`, `--`는 정수가 아니에요.
- `12abc`처럼 숫자 바로 뒤에 영문자가 붙으면 오류예요.
- 문자열 안의 바이트는 그대로 복사해요. UTF-8 한글도 바이트 수로 세요(한 글자 3바이트).
- 그 밖의 글자(`#`, `"`, `.`, `!` 등)와 `'\0'` 바이트는 오류예요.

## 6. 문법

```text
statement  := insert | select
insert     := INSERT INTO ident VALUES '(' literal { ',' literal } ')'
select     := SELECT '*' FROM ident [ WHERE ident '=' literal ]
literal    := integer | string
```

- 문장 끝(`;` 또는 입력 끝) 앞에 토큰이 더 있으면 오류예요. 예: `SELECT * FROM users users;`
- `SELECT` 뒤에는 `*`만 받아요. `print_row()`가 `id`, `name`, `age`를 모두 출력하기 때문이에요.
- `INSERT`의 열 목록 `(name, age)`, 여러 행 `VALUES (...), (...)`는 받지 않아요.
- `VALUES ()`처럼 값이 0개면 오류예요.
- 방법은 재귀 하강이에요. 규칙 하나마다 함수 하나를 두고, 토큰을 앞에서부터 기대한 것과 비교하며 하나씩 소비해요. 문장이 두 종류뿐이라 파서 생성기(flex/bison)는 쓰지 않아요.

### 결과 예

| 입력 | `Command` |
| --- | --- |
| `INSERT INTO users VALUES ('Kim Min', 23);` | `type = CMD_INSERT`, `table = "users"`, `value_count = 2`, `values[0] = {VALUE_STRING, s = "Kim Min"}`, `values[1] = {VALUE_INT, i = 23}`, `has_where = 0` |
| `select * from users where id = 7` | `type = CMD_SELECT`, `table = "users"`, `value_count = 0`, `has_where = 1`, `where_column = "id"`, `where_value = {VALUE_INT, i = 7}` |
| `SELECT * FROM users WHERE name = 'O''Brien';` | `has_where = 1`, `where_column = "name"`, `where_value = {VALUE_STRING, s = "O'Brien"}` |

## 7. 반환값과 오류

| 반환값 | 뜻 | `*cursor` |
| --- | --- | --- |
| `DB_OK` | 문장 하나를 `out`에 채움 | 그 문장의 `;` 바로 뒤. `;`가 없으면 `length` |
| `DB_END` | `*cursor`부터 끝까지 공백과 `;`만 있음 | `length` |
| `DB_ERR_SYNTAX` | 문장이 틀림. `error`에 이유 | 틀린 문장의 `;` 바로 뒤. `;`가 없거나 문자열이 닫히지 않았으면 `length` |

- 오류가 나도 커서를 다음 문장으로 옮겨 둬요. Phase 1의 `main`은 첫 오류에서 중단하고 이후 SQL은 실행하지 않아요. 오류 전에 성공한 INSERT는 되돌리지 않아요.
- 메모리를 할당하지 않으므로 메모리 오류는 없어요.

### 오류 메시지

메시지에는 문장 번호를 넣지 않아요. 번호는 `main`이 앞에 붙여요.

| 상황 | `error` |
| --- | --- |
| 기대한 토큰이 아님 | `near 'FORM': expected FROM` |
| 첫 토큰이 INSERT·SELECT가 아님 | `near 'UPDATE': expected INSERT or SELECT` |
| 이름 자리에 이름이 아닌 것 | `near 'select': expected table name`, `near '=': expected column name` |
| 문장이 중간에 끝남 | `unexpected end of statement: expected ')'` |
| 문장 뒤에 토큰이 더 있음 | `near 'users': expected end of statement` |
| 모르는 글자 | `unexpected character '#'` |
| 출력할 수 없는 바이트 (`'\0'` 등) | `unexpected byte 0x00` |
| 닫히지 않은 문자열 | `unterminated string` |
| 정수 범위 초과 | `integer out of range: 99999999999999999999` |
| 문자열 길이 초과 | `string too long (max 255 bytes)` |
| 식별자 길이 초과 | `identifier too long (max 31 bytes)` |
| 값 개수 초과 | `too many values (max 16)` |

- `near '...'`와 `integer out of range: ...`에 넣는 토큰 글자는 최대 32바이트까지만 쓰고 뒤를 잘라요. 메시지가 `PARSE_ERROR_SIZE`를 넘지 않게 하려고요. 예: 숫자 900개짜리 정수는 앞 32자리만 나와요.
- 오류 메시지는 영어 한 줄로 쓰며 줄바꿈을 넣지 않아요. P-2에서 수용한 공통 계약이에요.

## 8. 내부 구조

`parser.c` 밖으로 내보내지 않는 `static` 함수와 구조체예요. 이름은 바뀔 수 있어요.

```c
typedef enum { TOK_KEYWORD, TOK_IDENT, TOK_INT, TOK_STRING, TOK_SYMBOL, TOK_END } TokenKind;

typedef struct {
    TokenKind   kind;
    const char *start;   // sql 안의 시작 위치 (오류 메시지용)
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

static int  next_token(Parser *p);          // p->cur에 다음 토큰. 실패 시 error를 쓰고 0
static int  expect_keyword(Parser *p, const char *kw);
static int  expect_symbol(Parser *p, char c);
static int  parse_literal(Parser *p, Value *out);
static int  parse_insert(Parser *p, Command *out);
static int  parse_select(Parser *p, Command *out);
static void skip_to_statement_end(Parser *p); // 오류 뒤 커서를 다음 문장으로
```

- 토큰 배열을 만들지 않고, 파서가 필요할 때 `next_token()`으로 하나씩 받아요. 토큰 개수 한도가 필요 없어져요.
- 모든 상태는 `parse_next()` 지역 변수인 `Parser` 안에 있어요.
- `strtok`은 쓰지 않아요. 공백이 든 문자열을 두 토큰으로 가르고, 함수 안에 상태를 숨겨 두어 Phase 3 스레드에서 꼬여요.

## 9. 테스트

`tests/parser_test.c`에서 `parse_next()`를 직접 불러 아래 값을 확인해요. 다른 모듈 없이 `parser.c`만 링크해요.

| 번호 | 입력 | 기대 결과 |
| --- | --- | --- |
| T-1 | `INSERT INTO users VALUES ('Kim Min', 23);` | OK, 6장 결과 예 1행과 같음, `cursor = 41` |
| T-2 | `SELECT * FROM users` | OK, `has_where = 0`, `cursor = 19`. 다시 부르면 END |
| T-3 | `select * from users where id = 7` | OK, 6장 결과 예 2행과 같음 |
| T-4 | `SELECT * FROM users WHERE name = 'O''Brien';` | OK, `where_value.s = "O'Brien"` |
| T-5 | `INSERT INTO users VALUES ('a;b', -5);` | OK, `values[0].s = "a;b"`, `values[1].i = -5`. 다시 부르면 END |
| T-6 | `;; ;` | END, `cursor = 4` |
| T-7 | 빈 문자열 (`length = 0`) | END, `cursor = 0` |
| T-8 | `SELECT * FORM users; SELECT * FROM users;` | SYNTAX, `error = "near 'FORM': expected FROM"`, `cursor = 20`. 다시 부르면 OK |
| T-9 | `INSERT INTO users VALUES ('abc` | SYNTAX, `error = "unterminated string"`, `cursor = 30` |
| T-10 | `INSERT INTO users VALUES (99999999999999999999);` | SYNTAX, `error = "integer out of range: 99999999999999999999"` |
| T-10a | NUL 없이 정확히 32바이트인 `SELECT * FROM users WHERE id = 7` 버퍼, `length = 32` | OK, `where_value.i = 7`, `cursor = length`. 경계 밖을 읽지 않음 |
| T-10b | NUL 없이 33바이트인 `SELECT * FROM users WHERE id = 78` 버퍼, `length = 32` (마지막 `8`은 범위 밖) | OK, `where_value.i = 7`, `cursor = length`. 범위 밖 숫자를 변환에 포함하지 않음 |
| T-11 | 따옴표 안에 `a` 256개 | SYNTAX, `error = "string too long (max 255 bytes)"` |
| T-12 | 따옴표 안에 `a` 255개 | OK, `strlen(values[0].s) = 255` |
| T-13 | 식별자 `a` 32개를 테이블 이름으로 | SYNTAX, `error = "identifier too long (max 31 bytes)"` |
| T-14 | 값 17개 (`1,1,...,1`) | SYNTAX, `error = "too many values (max 16)"` |
| T-15 | 값 16개 | OK, `value_count = 16` |
| T-16 | `SELECT * FROM users users;` | SYNTAX, `error = "near 'users': expected end of statement"` |
| T-17 | `SELECT * FROM users #` | SYNTAX, `error = "unexpected character '#'"` |
| T-18 | `INSERT INTO users VALUES (` | SYNTAX, `error = "unexpected end of statement: expected value"` |
| T-19 | `INSERT INTO users VALUES ()` | SYNTAX, `error = "near ')': expected value"` |
| T-20 | `INSERT INTO users VALUES (- 5)` | SYNTAX, `error = "unexpected character '-'"` |

- `cursor` 기대값은 0부터 센 바이트 위치예요. 예: T-1은 `;`가 40번 위치라서 그 바로 뒤인 41이에요.
- 파일 단위 테스트(`.sql`과 기대 출력 `.out` 비교)는 최무헌 담당이에요. 파서 쪽 경계 사례는 위 표를 그대로 옮겨 쓰면 돼요.

## 10. 정하지 않은 것

| 항목 | 내용 | 같이 정할 사람 |
| --- | --- | --- |
| `DbStatus` 정의 | `DB_OK`, `DB_END`, `DB_ERR_SYNTAX` 이름은 사용해요. 나머지 오류 상수와 enum의 수치 값은 공통 헤더에서 맞춰요 | 전원 |

#ifndef DBMS_H
#define DBMS_H

// 모듈이 함께 쓰는 정의예요. 지금은 파서 쪽(docs/parser.md 3.1절)만 있어요.
// 나머지 타입과 상태 값, 수치 값 합의는 #4에서 더해요.

#include <stddef.h>
#include <stdint.h>

#define DB_NAME_MAX      31   // 식별자 최대 바이트 수 (끝의 '\0' 제외)
#define DB_STRING_MAX    255  // 문자열 값 최대 바이트 수 (끝의 '\0' 제외)
#define DB_VALUES_MAX    16   // INSERT 값 최대 개수
#define PARSE_ERROR_SIZE 128  // 오류 메시지 버퍼 크기 ('\0' 포함)

typedef enum { DB_OK = 0, DB_END, DB_ERR_SYNTAX } DbStatus;

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

// sql[*cursor]부터 문장 하나를 읽어 out에 채워요. 계약은 docs/parser.md 3.2절과 7장이에요.
DbStatus parse_next(const char *sql, size_t length, size_t *cursor,
                    Command *out, char error[PARSE_ERROR_SIZE]);

#endif

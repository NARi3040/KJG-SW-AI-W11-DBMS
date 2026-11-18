/*
 * dbms.h - DBMS 공통 공개 헤더 (C11)
 *
 * 이 파일 하나만 포함하면 되고, <stddef.h>와 <stdint.h> 외에는 쓰지 않아요.
 * 선언만 있으며 구현은 각 .c 파일에서 작성해요. 공개 구조체는 호출자가 준 공간을
 * 채우는 고정 크기 값이라 해제 함수가 없어요.
 */
#ifndef DBMS_H
#define DBMS_H

#include <stddef.h>
#include <stdint.h>

/* 한도: 끝의 '\0'은 세지 않은 바이트 수예요. */
#define DB_NAME_MAX 31       /* 식별자(테이블·열 이름) 최대 길이 */
#define DB_STRING_MAX 255    /* 문자열 값 최대 길이 */
#define DB_VALUES_MAX 16     /* INSERT 값 최대 개수 */
#define PARSE_ERROR_SIZE 128 /* 오류 메시지 버퍼 크기 ('\0' 포함) */

/* users.name 최대 바이트 수 ('\0' 제외, 빈 문자열 허용) */
#define USERS_NAME_MAX 64

/* 상태 값과 ExecuteResult 필드는 이슈 #4의 팀 검토 초안이에요.
 * 값은 순서대로 0부터 이어지며 상수와 비교해요.
 * DB_END는 파서만 쓰는 정상 입력 끝, DB_ERR_ARG는 인자나 지원하지 않는 명령,
 * DB_ERR_WRITE_UNCERTAIN은 행이 저장됐는지 알 수 없는 상태예요. */
typedef enum {
    DB_OK = 0,
    DB_END = 1,
    DB_ERR_SYNTAX,
    DB_ERR_ARG,
    DB_ERR_SCHEMA,
    DB_ERR_TYPE,
    DB_ERR_RANGE,
    DB_ERR_NOMEM,
    DB_ERR_IO,
    DB_ERR_CORRUPT,
    DB_ERR_OUTPUT,
    DB_ERR_WRITE_UNCERTAIN
} DbStatus;

/* ---------- 파서 ---------- */

typedef enum { CMD_INSERT = 1, CMD_SELECT = 2 } CommandType;
typedef enum { VALUE_INT = 1, VALUE_STRING = 2 } ValueType;

typedef struct {
    ValueType type;
    int64_t i;                    /* type == VALUE_INT일 때만 의미 있어요 */
    char s[DB_STRING_MAX + 1];    /* type == VALUE_STRING일 때만. '' → ' 로 바꾼 값 */
} Value;

typedef struct {
    CommandType type;
    char table[DB_NAME_MAX + 1];
    size_t value_count;           /* INSERT: 1 ~ DB_VALUES_MAX, SELECT: 0 */
    Value values[DB_VALUES_MAX];
    int has_where;                /* SELECT에 WHERE가 있으면 1, INSERT는 항상 0 */
    char where_column[DB_NAME_MAX + 1];
    Value where_value;
} Command;

/* ---------- 저장 ---------- */

/* 필드는 storage.c 안에만 있고, 다른 모듈은 포인터로만 써요. */
typedef struct Storage Storage;

typedef struct {
    int64_t id;                     /* 1부터 시작하는 자동 ID */
    char name[USERS_NAME_MAX + 1];  /* '\0'으로 끝나요 */
    int32_t age;
    uint64_t row_index;             /* 0부터 센 레코드 위치 */
} Row;

typedef struct {
    int64_t id;          /* 발급한 ID */
    uint64_t row_index;  /* 저장한 레코드 위치 */
} InsertResult;

/* ---------- 실행 ---------- */

/* 한 행과 호출자 상태를 받는 함수 포인터 타입이에요. 행은 이 호출 동안에만
 * 유효하므로 보관하려면 콜백이 복사해요. */
typedef DbStatus (*RowHandler)(const Row *row, void *user_data);

typedef struct {
    InsertResult insert;   /* INSERT가 DB_OK일 때만 의미 있어요 */
    size_t row_count;      /* SELECT에서 외부 콜백이 성공한 행 수 */
} ExecuteResult;

/* ---------- 공개 함수 ---------- */

/* sql은 읽기만 하고 보관하지 않아요. DB_OK일 때만 out이 유효하고, 그 밖에는
 * out을 0으로 채우고 error는 비워요(DB_ERR_SYNTAX만 영어 한 줄 메시지).
 * DB_OK에서는 cursor가 반드시 전진하고 error는 비어요.
 * 문법 오류에서도 cursor는 다음 문장 위치로 옮겨요. */
DbStatus parse_next(const char *sql, size_t length, size_t *cursor,
                    Command *out, char error[PARSE_ERROR_SIZE]);

/* INSERT는 콜백을 부르지 않고, DB_OK일 때 out_result->insert만 유효해요.
 * SELECT의 row_count는 외부 콜백이 성공한 행 수이고 중간 오류가 나도 그때까지
 * 센 값이 남아요. 지원하지 않는 명령은 DB_ERR_ARG예요. */
DbStatus execute(Storage *storage, const Command *command,
                 RowHandler on_row, void *user_data,
                 ExecuteResult *out_result);

/* path는 '\0'으로 끝나는 파일 경로예요. 실패하면 *out_storage는 NULL이에요. */
DbStatus storage_open(const char *path, Storage **out_storage);

/* name은 복사하고 보관하지 않아요. out_result는 DB_OK일 때만 유효해요.
 * DB_ERR_WRITE_UNCERTAIN은 자동으로 다시 시도하지 않아요. */
DbStatus storage_insert(Storage *storage, const char *name,
                        int64_t age, InsertResult *out_result);

/* 행마다 on_row(&row, user_data)를 불러요. *out_count는 0부터 시작해 콜백이
 * DB_OK를 돌려준 횟수이고 오류 때도 그때까지의 값이 남아요. 콜백 안에서 같은
 * Storage로 저장 함수를 다시 부르지 않아요. */
DbStatus storage_scan(Storage *storage, RowHandler on_row,
                      void *user_data, size_t *out_count);

/* id가 같은 행을 0개 또는 1개 on_row로 넘겨요. 콜백 계약은 storage_scan()과
 * 같아요. 못 찾으면 오류가 아니라 DB_OK이고 *out_count는 0이에요. 0·음수
 * id도 0행이에요(SQL의 id < 1 거절은 실행기가 해요). */
DbStatus storage_find_by_id(Storage *storage, int64_t id, RowHandler on_row,
                            void *user_data, size_t *out_count);

/* NULL이면 아무것도 하지 않고 DB_OK예요. 성공·실패 모두 정리하고, 호출 뒤
 * 포인터는 무효예요. 닫기를 다시 시도하지 않아요. */
DbStatus storage_close(Storage *storage);

#endif /* DBMS_H */

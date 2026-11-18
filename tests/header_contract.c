/*
 * header_contract.c - dbms.h·bptree.h 계약 검사 (문법 검사 전용, 링크 불필요)
 *
 * 포함 가드, 한도·배열 용량, 공개 타입 필드, 함수 시그니처를 확인해요.
 * 구현이 없으므로 실행하지 않고 -fsyntax-only로만 검사해요.
 */
#include "dbms.h"
#include "dbms.h"
#include "bptree.h"
#include "bptree.h"

/* ---------- 한도 ---------- */

_Static_assert(DB_NAME_MAX == 31, "DB_NAME_MAX");
_Static_assert(DB_STRING_MAX == 255, "DB_STRING_MAX");
_Static_assert(DB_VALUES_MAX == 16, "DB_VALUES_MAX");
_Static_assert(PARSE_ERROR_SIZE == 128, "PARSE_ERROR_SIZE");
_Static_assert(USERS_NAME_MAX == 64, "USERS_NAME_MAX");
_Static_assert(BPTREE_ORDER >= 3, "BPTREE_ORDER");

/* ---------- 배열 용량 ---------- */

_Static_assert(sizeof(((Value *)0)->s) == DB_STRING_MAX + 1, "Value.s");
_Static_assert(sizeof(((Command *)0)->table) == DB_NAME_MAX + 1, "Command.table");
_Static_assert(sizeof(((Command *)0)->values) == DB_VALUES_MAX * sizeof(Value),
               "Command.values");
_Static_assert(sizeof(((Command *)0)->where_column) == DB_NAME_MAX + 1,
               "Command.where_column");
_Static_assert(sizeof(((Row *)0)->name) == USERS_NAME_MAX + 1, "Row.name");

/* ---------- enum 값 ---------- */

_Static_assert(DB_OK == 0, "DB_OK");
_Static_assert(DB_END == 1, "DB_END");
_Static_assert(DB_ERR_SYNTAX == 2, "DB_ERR_SYNTAX");
_Static_assert(DB_ERR_ARG == 3, "DB_ERR_ARG");
_Static_assert(DB_ERR_SCHEMA == 4, "DB_ERR_SCHEMA");
_Static_assert(DB_ERR_TYPE == 5, "DB_ERR_TYPE");
_Static_assert(DB_ERR_RANGE == 6, "DB_ERR_RANGE");
_Static_assert(DB_ERR_NOMEM == 7, "DB_ERR_NOMEM");
_Static_assert(DB_ERR_IO == 8, "DB_ERR_IO");
_Static_assert(DB_ERR_CORRUPT == 9, "DB_ERR_CORRUPT");
_Static_assert(DB_ERR_OUTPUT == 10, "DB_ERR_OUTPUT");
_Static_assert(DB_ERR_WRITE_UNCERTAIN == 11, "DB_ERR_WRITE_UNCERTAIN");

_Static_assert(CMD_INSERT == 1 && CMD_SELECT == 2, "CommandType");
_Static_assert(VALUE_INT == 1 && VALUE_STRING == 2, "ValueType");

/* ---------- 필드 타입 ---------- */

_Static_assert(_Generic(((Value *)0)->type, ValueType: 1, default: 0), "Value.type");
_Static_assert(_Generic(((Value *)0)->i, int64_t: 1, default: 0), "Value.i");

_Static_assert(_Generic(((Command *)0)->type, CommandType: 1, default: 0), "Command.type");
_Static_assert(_Generic(((Command *)0)->value_count, size_t: 1, default: 0),
               "Command.value_count");
_Static_assert(_Generic(((Command *)0)->has_where, int: 1, default: 0), "Command.has_where");
_Static_assert(_Generic(((Command *)0)->where_value, Value: 1, default: 0),
               "Command.where_value");

_Static_assert(_Generic(((Row *)0)->id, int64_t: 1, default: 0), "Row.id");
_Static_assert(_Generic(((Row *)0)->age, int32_t: 1, default: 0), "Row.age");
_Static_assert(_Generic(((Row *)0)->row_index, uint64_t: 1, default: 0), "Row.row_index");

_Static_assert(_Generic(((InsertResult *)0)->id, int64_t: 1, default: 0), "InsertResult.id");
_Static_assert(_Generic(((InsertResult *)0)->row_index, uint64_t: 1, default: 0),
               "InsertResult.row_index");

_Static_assert(_Generic(((ExecuteResult *)0)->insert, InsertResult: 1, default: 0),
               "ExecuteResult.insert");
_Static_assert(_Generic(((ExecuteResult *)0)->row_count, size_t: 1, default: 0),
               "ExecuteResult.row_count");

/* ---------- 시그니처 ---------- */

static DbStatus sample_row_handler(const Row *row, void *user_data) {
    (void)row;
    (void)user_data;
    return DB_OK;
}

int main(void) {
    DbStatus (*parse_next_fn)(const char *, size_t, size_t *, Command *, char *) =
        parse_next;
    DbStatus (*execute_fn)(Storage *, const Command *, RowHandler, void *,
                           ExecuteResult *) = execute;
    DbStatus (*storage_open_fn)(const char *, Storage **) = storage_open;
    DbStatus (*storage_insert_fn)(Storage *, const char *, int64_t, InsertResult *) =
        storage_insert;
    DbStatus (*storage_scan_fn)(Storage *, RowHandler, void *, size_t *) = storage_scan;
    DbStatus (*storage_find_by_id_fn)(Storage *, int64_t, RowHandler, void *, size_t *) =
        storage_find_by_id;
    DbStatus (*storage_close_fn)(Storage *) = storage_close;
    DbStatus (*bptree_create_fn)(BpTree **) = bptree_create;
    void (*bptree_destroy_fn)(BpTree *) = bptree_destroy;
    DbStatus (*bptree_insert_fn)(BpTree *, int64_t, uint64_t) = bptree_insert;
    int (*bptree_find_fn)(const BpTree *, int64_t, uint64_t *) = bptree_find;
    int (*bptree_check_fn)(const BpTree *) = bptree_check;
    RowHandler handler = sample_row_handler;

    (void)parse_next_fn;
    (void)execute_fn;
    (void)storage_open_fn;
    (void)storage_insert_fn;
    (void)storage_scan_fn;
    (void)storage_find_by_id_fn;
    (void)storage_close_fn;
    (void)bptree_create_fn;
    (void)bptree_destroy_fn;
    (void)bptree_insert_fn;
    (void)bptree_find_fn;
    (void)bptree_check_fn;
    (void)handler;
    return 0;
}

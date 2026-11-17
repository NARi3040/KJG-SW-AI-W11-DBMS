/*
 * execute.c - 명령 검증과 INSERT/SELECT 실행
 *
 * 저장 함수를 호출하기 전에 명령을 모두 검증해요. SELECT WHERE는 저장소를
 * 한 번 스캔하면서 실행기 내부 필터로 일치 행만 외부 콜백에 전달해요.
 */
#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1 /* POSIX strcasecmp under strict -std=c11 */
#endif

#include "dbms.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define DB_STRCASECMP _stricmp
#else
#include <strings.h>
#define DB_STRCASECMP strcasecmp
#endif

typedef enum {
    WHERE_ID,
    WHERE_NAME,
    WHERE_AGE
} WhereColumn;

typedef struct {
    WhereColumn column;
    const Command *command;
    RowHandler on_row;
    void *user_data;
    size_t matched;
} WhereFilter;

/* 고정 배열 안의 NUL까지만 세요. NUL이 없으면 SIZE_MAX예요. */
static size_t bounded_length(const char *text, size_t capacity) {
    const char *nul = memchr(text, '\0', capacity);
    return nul == NULL ? SIZE_MAX : (size_t)(nul - text);
}

static int table_is_users(const Command *command) {
    size_t length = bounded_length(command->table, sizeof command->table);

    return length != SIZE_MAX && DB_STRCASECMP(command->table, "users") == 0;
}

/* name 값: NUL 누락은 스키마 오류, 64바이트 초과는 범위 오류예요. */
static DbStatus validate_name(const char *text, size_t capacity) {
    size_t length = bounded_length(text, capacity);

    if (length == SIZE_MAX) {
        return DB_ERR_SCHEMA;
    }
    return length > USERS_NAME_MAX ? DB_ERR_RANGE : DB_OK;
}

static DbStatus execute_insert(Storage *storage, const Command *command,
                               ExecuteResult *out_result) {
    const Value *values = command->values;
    InsertResult insert_result = {0, 0};
    DbStatus status;

    if (command->value_count != 2) {
        return DB_ERR_SCHEMA;
    }
    if (values[0].type != VALUE_STRING || values[1].type != VALUE_INT) {
        return DB_ERR_TYPE;
    }
    status = validate_name(values[0].s, sizeof values[0].s);
    if (status != DB_OK) {
        return status;
    }
    if (values[1].i < INT32_MIN || values[1].i > INT32_MAX) {
        return DB_ERR_RANGE;
    }

    status = storage_insert(storage, values[0].s, values[1].i, &insert_result);
    if (status != DB_OK) {
        return status;
    }
    out_result->insert = insert_result;
    return DB_OK;
}

static DbStatus validate_where(const Command *command, WhereColumn *out_column) {
    size_t length = bounded_length(command->where_column,
                                   sizeof command->where_column);

    if (length == SIZE_MAX) {
        return DB_ERR_SCHEMA;
    }

    if (DB_STRCASECMP(command->where_column, "id") == 0) {
        *out_column = WHERE_ID;
        if (command->where_value.type != VALUE_INT) {
            return DB_ERR_TYPE;
        }
        if (command->where_value.i < 1) {
            return DB_ERR_RANGE;
        }
        return DB_OK;
    }

    if (DB_STRCASECMP(command->where_column, "name") == 0) {
        *out_column = WHERE_NAME;
        if (command->where_value.type != VALUE_STRING) {
            return DB_ERR_TYPE;
        }
        return validate_name(command->where_value.s,
                             sizeof command->where_value.s);
    }

    if (DB_STRCASECMP(command->where_column, "age") == 0) {
        *out_column = WHERE_AGE;
        if (command->where_value.type != VALUE_INT) {
            return DB_ERR_TYPE;
        }
        if (command->where_value.i < INT32_MIN ||
            command->where_value.i > INT32_MAX) {
            return DB_ERR_RANGE;
        }
        return DB_OK;
    }

    return DB_ERR_SCHEMA;
}

static int row_matches(const Row *row, const WhereFilter *filter) {
    switch (filter->column) {
    case WHERE_ID:
        return row->id == filter->command->where_value.i;
    case WHERE_NAME:
        return strcmp(row->name, filter->command->where_value.s) == 0;
    case WHERE_AGE:
        return (int64_t)row->age == filter->command->where_value.i;
    }
    return 0;
}

/* 불일치는 DB_OK로 건너뛰고, 일치 행의 외부 콜백 결과를 그대로 전달해요. */
static DbStatus filter_row(const Row *row, void *user_data) {
    WhereFilter *filter = user_data;
    DbStatus status;

    if (!row_matches(row, filter)) {
        return DB_OK;
    }
    status = filter->on_row(row, filter->user_data);
    if (status == DB_OK) {
        filter->matched++;
    }
    return status;
}

static DbStatus scan_all(Storage *storage, RowHandler on_row, void *user_data,
                         ExecuteResult *out_result) {
    size_t count = 0;
    DbStatus status = storage_scan(storage, on_row, user_data, &count);

    out_result->row_count = count;
    return status;
}

static DbStatus scan_where(Storage *storage, const Command *command,
                           RowHandler on_row, void *user_data,
                           ExecuteResult *out_result) {
    WhereFilter filter;
    size_t scan_count = 0;
    DbStatus status;

    status = validate_where(command, &filter.column);
    if (status != DB_OK) {
        return status;
    }
    filter.command = command;
    filter.on_row = on_row;
    filter.user_data = user_data;
    filter.matched = 0;

    status = storage_scan(storage, filter_row, &filter, &scan_count);
    out_result->row_count = filter.matched;
    return status;
}

DbStatus execute(Storage *storage, const Command *command, RowHandler on_row,
                 void *user_data, ExecuteResult *out_result) {
    if (out_result == NULL) {
        return DB_ERR_ARG;
    }
    memset(out_result, 0, sizeof *out_result);

    if (storage == NULL || command == NULL) {
        return DB_ERR_ARG;
    }
    if (command->type != CMD_INSERT && command->type != CMD_SELECT) {
        return DB_ERR_ARG;
    }
    if (!table_is_users(command)) {
        return DB_ERR_SCHEMA;
    }

    if (command->type == CMD_INSERT) {
        return execute_insert(storage, command, out_result);
    }

    if (on_row == NULL) {
        return DB_ERR_ARG;
    }
    if (!command->has_where) {
        return scan_all(storage, on_row, user_data, out_result);
    }
    return scan_where(storage, command, on_row, user_data, out_result);
}

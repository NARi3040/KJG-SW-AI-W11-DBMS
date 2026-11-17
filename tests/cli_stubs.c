/*
 * cli_stubs.c - main.c CLI 계약 테스트용 대역
 *
 * 실제 파서/저장 구현이 아니에요. SQL을 해석하지 않고, CLI_STUB_SCENARIO
 * 환경 변수로 고른 미리 정한 응답만 재생해요. 테스트 픽스처의 SQL 내용은
 * CLI가 길이와 바이트를 그대로 넘기는지 확인하는 불투명 입력일 뿐이에요.
 */
#include "dbms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_calls;
static int execute_calls;
static int close_calls;

static const char *scenario(void) {
    const char *name = getenv("CLI_STUB_SCENARIO");
    return name != NULL ? name : "empty";
}

static void write_marker(const char *path, const char *text) {
    FILE *file;

    if (path == NULL) {
        return;
    }
    file = fopen(path, "wb");
    if (file != NULL) {
        (void)fputs(text, file);
        (void)fclose(file);
    }
}

static void set_command(Command *out, CommandType type) {
    memset(out, 0, sizeof *out);
    out->type = type;
    (void)snprintf(out->table, sizeof out->table, "%s", "users");
}

DbStatus parse_next(const char *sql, size_t length, size_t *cursor,
                    Command *out, char error[PARSE_ERROR_SIZE]) {
    const char *name = scenario();
    const char *expected_length = getenv("CLI_STUB_EXPECT_LENGTH");

    (void)sql;
    parse_calls++;
    memset(out, 0, sizeof *out);
    error[0] = '\0';

    if (expected_length != NULL &&
        (size_t)strtoull(expected_length, NULL, 10) != length) {
        fprintf(stderr, "STUB LENGTH MISMATCH %zu\n", length);
        *cursor = length;
        return DB_ERR_SYNTAX;
    }

    if (strcmp(name, "happy") == 0) {
        if (parse_calls == 1) {
            set_command(out, CMD_INSERT);
            out->value_count = 2;
            out->values[0].type = VALUE_STRING;
            (void)snprintf(out->values[0].s, sizeof out->values[0].s, "%s", "Mina");
            out->values[1].type = VALUE_INT;
            out->values[1].i = 20;
            *cursor = 1;
            return DB_OK;
        }
        if (parse_calls == 2) {
            set_command(out, CMD_SELECT);
            *cursor = length;
            return DB_OK;
        }
        return DB_END;
    }

    if (strcmp(name, "syntax_after_insert") == 0) {
        if (parse_calls == 1) {
            set_command(out, CMD_INSERT);
            *cursor = 1;
            return DB_OK;
        }
        if (parse_calls == 2) {
            (void)snprintf(error, PARSE_ERROR_SIZE, "%s",
                           "near 'FROM': expected FROM");
            *cursor = length;
            return DB_ERR_SYNTAX;
        }
        return DB_END;
    }

    if (strcmp(name, "syntax") == 0) {
        if (parse_calls == 1) {
            (void)snprintf(error, PARSE_ERROR_SIZE, "%s",
                           "near 'FROM': expected FROM");
            *cursor = length;
            return DB_ERR_SYNTAX;
        }
        return DB_END;
    }

    if (strcmp(name, "badcursor") == 0) {
        if (parse_calls == 1) {
            set_command(out, CMD_SELECT);
            *cursor = 0; /* 이전 커서 그대로: 계약 위반 */
            return DB_OK;
        }
        return DB_END;
    }

    if (strcmp(name, "overflowcursor") == 0) {
        if (parse_calls == 1) {
            set_command(out, CMD_SELECT);
            *cursor = length + 1; /* 길이 초과: 계약 위반 */
            return DB_OK;
        }
        return DB_END;
    }

    if (strcmp(name, "exec_error") == 0 ||
        strcmp(name, "select_partial") == 0 ||
        strcmp(name, "escaping") == 0) {
        if (parse_calls == 1) {
            set_command(out, CMD_SELECT);
            *cursor = length;
            return DB_OK;
        }
        return DB_END;
    }

    if (strcmp(name, "write_uncertain") == 0) {
        if (parse_calls == 1) {
            set_command(out, CMD_INSERT);
            *cursor = length;
            return DB_OK;
        }
        return DB_END;
    }

    /* empty, nul, open_fail, close_fail: 명령 없음 */
    return DB_END;
}

DbStatus execute(Storage *storage, const Command *command, RowHandler on_row,
                 void *user_data, ExecuteResult *out_result) {
    const char *name = scenario();

    (void)storage;
    execute_calls++;
    memset(out_result, 0, sizeof *out_result);

    if (strcmp(name, "happy") == 0) {
        Row first = {1, "Mina", 20, 0};
        Row second = {2, "Joon", 31, 1};

        if (command->type == CMD_INSERT) {
            out_result->insert.id = 7;
            return DB_OK;
        }
        if (on_row(&first, user_data) != DB_OK ||
            on_row(&second, user_data) != DB_OK) {
            return DB_ERR_OUTPUT;
        }
        out_result->row_count = 2;
        return DB_OK;
    }

    if (strcmp(name, "syntax_after_insert") == 0) {
        out_result->insert.id = 7;
        return DB_OK;
    }

    if (strcmp(name, "exec_error") == 0) {
        out_result->row_count = 3; /* 부분 행 수 */
        return DB_ERR_SCHEMA;
    }

    if (strcmp(name, "select_partial") == 0) {
        Row row = {1, "ok", 1, 0};

        if (on_row(&row, user_data) != DB_OK) {
            return DB_ERR_OUTPUT;
        }
        out_result->row_count = 1;
        return DB_ERR_OUTPUT;
    }

    if (strcmp(name, "escaping") == 0) {
        Row first;
        Row second;

        memset(&first, 0, sizeof first);
        first.id = 1;
        (void)snprintf(first.name, sizeof first.name, "%s", "a\\b\tc\nd\re");
        first.age = -5;
        memset(&second, 0, sizeof second);
        second.id = 2;
        second.age = 2147483647;
        if (on_row(&first, user_data) != DB_OK ||
            on_row(&second, user_data) != DB_OK) {
            return DB_ERR_OUTPUT;
        }
        out_result->row_count = 2;
        return DB_OK;
    }

    if (strcmp(name, "write_uncertain") == 0) {
        if (execute_calls == 1) {
            return DB_ERR_WRITE_UNCERTAIN;
        }
        out_result->insert.id = 999; /* 재시도하면 출력에 드러나요 */
        return DB_OK;
    }

    return DB_ERR_ARG;
}

DbStatus storage_open(const char *path, Storage **out_storage) {
    static char token;
    const char *marker = getenv("CLI_STUB_OPEN_MARKER");

    (void)path;
    write_marker(marker, "1\n");

    if (strcmp(scenario(), "open_fail") == 0) {
        *out_storage = NULL;
        return DB_ERR_IO;
    }
    *out_storage = (Storage *)&token;
    return DB_OK;
}

DbStatus storage_close(Storage *storage) {
    const char *marker = getenv("CLI_STUB_CLOSE_MARKER");
    char count[16];

    close_calls++;
    (void)snprintf(count, sizeof count, "%d\n", close_calls);
    write_marker(marker, count);

    if (storage == NULL) {
        return DB_OK;
    }
    if (strcmp(scenario(), "close_fail") == 0) {
        return DB_ERR_IO;
    }
    return DB_OK;
}

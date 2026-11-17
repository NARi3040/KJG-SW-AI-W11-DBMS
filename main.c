/*
 * main.c - CLI: SQL 파일 읽기, 명령 실행, 출력과 종료 정리
 *
 * 파서·실행기·저장 모듈을 호출만 하고 직접 SQL을 해석하거나 저장하지 않아요.
 * 첫 오류에서 멈추고, 공통 종료 경로에서 한 번만 close/free/fflush 해요.
 */
#include "dbms.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define STORAGE_PATH "users.sans"
#define SQL_MAX_BYTES ((size_t)134217728) /* 128 MiB */

static const char *status_category(DbStatus status) {
    switch (status) {
    case DB_OK: return "ok";
    case DB_END: return "end";
    case DB_ERR_SYNTAX: return "syntax";
    case DB_ERR_ARG: return "argument";
    case DB_ERR_SCHEMA: return "schema";
    case DB_ERR_TYPE: return "type";
    case DB_ERR_RANGE: return "range";
    case DB_ERR_NOMEM: return "memory";
    case DB_ERR_IO: return "io";
    case DB_ERR_CORRUPT: return "corrupt";
    case DB_ERR_OUTPUT: return "output";
    case DB_ERR_WRITE_UNCERTAIN: return "write-uncertain";
    }
    return "internal";
}

static const char *status_cause(DbStatus status) {
    switch (status) {
    case DB_ERR_SYNTAX: return "invalid SQL";
    case DB_ERR_ARG: return "unsupported command";
    case DB_ERR_SCHEMA: return "invalid table, column, or value count";
    case DB_ERR_TYPE: return "value type mismatch";
    case DB_ERR_RANGE: return "value out of range";
    case DB_ERR_NOMEM: return "out of memory";
    case DB_ERR_IO: return "storage I/O failure";
    case DB_ERR_CORRUPT: return "storage file is corrupt";
    case DB_ERR_OUTPUT: return "cannot write output";
    case DB_ERR_WRITE_UNCERTAIN: return "write result uncertain; not retried";
    default: return "unexpected status";
    }
}

/* 파일 전체를 바이너리로 읽어요. 중간 NUL은 유지하고 마지막에만 NUL을 붙여요. */
static char *read_sql_file(const char *path, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    char *sql = NULL;
    long size;

    if (file == NULL) {
        fprintf(stderr, "input: %s: cannot open\n", path);
        return NULL;
    }
    /* shortcut: seek-based sizing needs a regular file; add chunked reads when pipes matter. */
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "input: %s: cannot determine size\n", path);
        goto fail;
    }
    if ((unsigned long long)size > (unsigned long long)SQL_MAX_BYTES) {
        fprintf(stderr, "input: %s: file exceeds 128 MiB limit\n", path);
        goto fail;
    }
    sql = malloc((size_t)size + 1);
    if (sql == NULL) {
        fprintf(stderr, "input: %s: out of memory\n", path);
        goto fail;
    }
    if (fread(sql, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "input: %s: cannot read\n", path);
        goto fail;
    }
    sql[size] = '\0';
    if (fclose(file) != 0) {
        /* fclose 실패도 스트림은 닫힌 상태라 다시 닫지 않아요. */
        fprintf(stderr, "input: %s: cannot close\n", path);
        free(sql);
        return NULL;
    }
    *out_length = (size_t)size;
    return sql;

fail:
    fclose(file);
    free(sql);
    return NULL;
}

/* id<TAB>name<TAB>age. name의 \\, 탭, LF, CR만 이스케이프해요. */
static DbStatus print_row(const Row *row, void *user_data) {
    FILE *out = user_data;
    const unsigned char *p;

    if (fprintf(out, "%lld\t", (long long)row->id) < 0) {
        return DB_ERR_OUTPUT;
    }
    for (p = (const unsigned char *)row->name; *p != '\0'; p++) {
        const char *escape = NULL;
        int failed;

        switch (*p) {
        case '\\': escape = "\\\\"; break;
        case '\t': escape = "\\t"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        default: break;
        }
        failed = escape != NULL ? fputs(escape, out) == EOF : fputc(*p, out) == EOF;
        if (failed) {
            return DB_ERR_OUTPUT;
        }
    }
    if (fprintf(out, "\t%lld\n", (long long)row->age) < 0) {
        return DB_ERR_OUTPUT;
    }
    return DB_OK;
}

int main(int argc, char *argv[]) {
    char *sql = NULL;
    size_t length = 0;
    Storage *storage = NULL;
    size_t cursor = 0;
    size_t statement = 1;
    int failed = 0;
    Command command;
    char error[PARSE_ERROR_SIZE];
    ExecuteResult result;

    if (argc != 2) {
        fprintf(stderr, "usage: %s SQL_FILE\n", argv[0]);
        return 1;
    }

#ifdef SIGPIPE
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "start: cannot ignore SIGPIPE\n");
        return 1;
    }
#endif

    sql = read_sql_file(argv[1], &length);
    if (sql == NULL) {
        return 1;
    }

    {
        DbStatus status = storage_open(STORAGE_PATH, &storage);
        if (status != DB_OK) {
            fprintf(stderr, "open: %s\n", status_cause(status));
            free(sql);
            return 1;
        }
    }

    for (;;) {
        size_t previous = cursor;
        DbStatus status;
        const char *cause;

        if (cursor > length) {
            fprintf(stderr, "statement %zu: parse: cursor out of range\n", statement);
            failed = 1;
            break;
        }

        error[0] = '\0';
        status = parse_next(sql, length, &cursor, &command, error);
        if (status == DB_END) {
            break;
        }
        if (status != DB_OK) {
            cause = status == DB_ERR_SYNTAX ? error : status_cause(status);
            if (*cause == '\0') {
                cause = status_cause(status);
            }
            fprintf(stderr, "statement %zu: %s: %s\n", statement,
                    status_category(status), cause);
            failed = 1;
            break;
        }

        if (cursor <= previous || cursor > length) {
            fprintf(stderr, "statement %zu: parse: parser returned an invalid cursor\n",
                    statement);
            failed = 1;
            break;
        }

        result.insert.id = 0;
        result.insert.row_index = 0;
        result.row_count = 0;
        status = execute(storage, &command, print_row, stdout, &result);
        if (status != DB_OK) {
            if (command.type == CMD_SELECT) {
                fprintf(stderr, "statement %zu: %s: %s (rows=%zu)\n", statement,
                        status_category(status), status_cause(status),
                        result.row_count);
            } else {
                fprintf(stderr, "statement %zu: %s: %s\n", statement,
                        status_category(status), status_cause(status));
            }
            failed = 1;
            break;
        }

        if (command.type == CMD_INSERT) {
            if (printf("INSERT OK id=%lld\n", (long long)result.insert.id) < 0) {
                fprintf(stderr, "statement %zu: output: cannot write output\n",
                        statement);
                failed = 1;
                break;
            }
        } else if (printf("SELECT OK rows=%zu\n", result.row_count) < 0) {
            fprintf(stderr, "statement %zu: output: cannot write output\n", statement);
            failed = 1;
            break;
        }

        statement++;
    }

    {
        DbStatus close_status = storage_close(storage);
        if (close_status != DB_OK) {
            fprintf(stderr, "close: %s\n", status_cause(close_status));
            failed = 1;
        }
        storage = NULL;
    }

    free(sql);

    if (fflush(stdout) != 0) {
        fprintf(stderr, "output: cannot flush stdout\n");
        failed = 1;
    }

    return failed ? 1 : 0;
}

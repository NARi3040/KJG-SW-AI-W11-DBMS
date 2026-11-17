/*
 * test_parser.c - P01/P02 parser contract checks
 *
 * Only parse_next() is called; no storage/execute modules are needed.
 * P01: INSERT + WHERE SELECT sequence, fixed Command arrays, cursor advance.
 * P02: syntax errors keep a one-line NUL-terminated message in 128 bytes.
 */
#include "dbms.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

_Static_assert(sizeof(((Command *)0)->table) == DB_NAME_MAX + 1,
               "Command.table is fixed");
_Static_assert(sizeof(((Command *)0)->values) == DB_VALUES_MAX * sizeof(Value),
               "Command.values is fixed");
_Static_assert(sizeof(((Command *)0)->where_column) == DB_NAME_MAX + 1,
               "Command.where_column is fixed");
_Static_assert(sizeof(((Value *)0)->s) == DB_STRING_MAX + 1,
               "Value.s is fixed");

static void check_syntax_contract(const char *sql) {
    size_t length = strlen(sql);
    size_t cursor = 0;
    Command command;
    char error[PARSE_ERROR_SIZE];
    DbStatus status;
    int terminated;

    memset(&command, 0xAA, sizeof command);
    memset(error, 0xAA, sizeof error);
    status = parse_next(sql, length, &cursor, &command, error);

    CHECK(status == DB_ERR_SYNTAX);
    CHECK(cursor <= length);
    CHECK(command.type == 0);
    CHECK(command.value_count == 0);
    CHECK(command.has_where == 0);
    CHECK(command.table[0] == '\0');
    CHECK(command.values[0].type == 0);
    CHECK(command.where_column[0] == '\0');
    CHECK(command.where_value.type == 0);

    terminated = memchr(error, '\0', sizeof error) != NULL;
    CHECK(terminated);
    if (terminated) {
        CHECK(error[0] != '\0');
        CHECK(strlen(error) < PARSE_ERROR_SIZE);
        CHECK(strchr(error, '\n') == NULL);
        CHECK(strchr(error, '\r') == NULL);
    }
}

int main(void) {
    const char *sql =
        "INSERT INTO users VALUES ('Mina', 20); "
        "SELECT * FROM users WHERE age = 20;";
    size_t length = strlen(sql);
    size_t cursor = 0;
    size_t first_cursor;
    Command command;
    char error[PARSE_ERROR_SIZE];

    memset(&command, 0, sizeof command);
    memset(error, 0, sizeof error);

    /* P01: INSERT */
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_OK);
    CHECK(cursor > 0 && cursor <= length);
    CHECK(command.type == CMD_INSERT);
    CHECK(strcmp(command.table, "users") == 0);
    CHECK(command.value_count == 2);
    CHECK(command.values[0].type == VALUE_STRING);
    CHECK(strcmp(command.values[0].s, "Mina") == 0);
    CHECK(command.values[1].type == VALUE_INT);
    CHECK(command.values[1].i == 20);
    CHECK(command.has_where == 0);
    CHECK(error[0] == '\0');

    /* P01: WHERE SELECT */
    first_cursor = cursor;
    memset(&command, 0, sizeof command);
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_OK);
    CHECK(cursor > first_cursor && cursor <= length);
    CHECK(command.type == CMD_SELECT);
    CHECK(strcmp(command.table, "users") == 0);
    CHECK(command.value_count == 0);
    CHECK(command.has_where == 1);
    CHECK(strcmp(command.where_column, "age") == 0);
    CHECK(command.where_value.type == VALUE_INT);
    CHECK(command.where_value.i == 20);
    CHECK(error[0] == '\0');

    /* P01: normal end */
    memset(&command, 0, sizeof command);
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_END);
    CHECK(cursor == length);
    CHECK(error[0] == '\0');

    /* P02: syntax errors stay one line and NUL-terminated */
    check_syntax_contract("SELECT FROM users;");
    check_syntax_contract("INSERT INTO users VALUES ('Mina', 20");

    if (failures != 0) {
        fprintf(stderr, "%d parser test failure(s)\n", failures);
        return 1;
    }
    printf("all parser tests passed\n");
    return 0;
}

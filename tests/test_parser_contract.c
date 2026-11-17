/*
 * test_parser_contract.c - P01/P02 parser contract checks
 *
 * Only parse_next() is called; no storage/execute modules are needed.
 * P01: INSERT + WHERE SELECT sequence, fixed Command arrays, cursor advance.
 * P02: syntax errors keep a one-line NUL-terminated message in 128 bytes and
 *      the next statement still parses.
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

/* Every call starts from garbage so a missing memset or stale error is caught. */
static void poison(Command *command, char error[PARSE_ERROR_SIZE]) {
    memset(command, 0xAA, sizeof *command);
    memset(error, 0xAA, PARSE_ERROR_SIZE);
}

static int is_zero_command(const Command *command) {
    static const unsigned char zero[sizeof *command] = {0};
    return memcmp(command, &zero, sizeof zero) == 0;
}

/* P02: syntax error keeps the documented error shape and still advances. */
static void check_syntax_contract(const char *sql) {
    size_t length = strlen(sql);
    size_t cursor = 0;
    Command command;
    char error[PARSE_ERROR_SIZE];
    DbStatus status;
    int terminated;

    poison(&command, error);
    status = parse_next(sql, length, &cursor, &command, error);

    CHECK(status == DB_ERR_SYNTAX);
    CHECK(cursor > 0 && cursor <= length);
    CHECK(is_zero_command(&command));

    terminated = memchr(error, '\0', sizeof error) != NULL;
    CHECK(terminated);
    if (terminated) {
        CHECK(error[0] != '\0');
        CHECK(strlen(error) < PARSE_ERROR_SIZE);
        CHECK(strchr(error, '\n') == NULL);
        CHECK(strchr(error, '\r') == NULL);
    }
}

/* A syntax error skips to ';' and the next parse still returns a valid command. */
static void check_recovery_after_error(const char *sql, const char *table) {
    size_t length = strlen(sql);
    size_t cursor = 0;
    size_t after_error;
    Command command;
    char error[PARSE_ERROR_SIZE];

    poison(&command, error);
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_ERR_SYNTAX);
    CHECK(cursor > 0 && cursor < length);
    after_error = cursor;

    poison(&command, error);
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_OK);
    CHECK(cursor > after_error && cursor <= length);
    CHECK(command.type == CMD_SELECT);
    CHECK(strncmp(command.table, table, sizeof command.table) == 0);
    CHECK(error[0] == '\0');
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

    /* P01: INSERT */
    poison(&command, error);
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_OK);
    CHECK(cursor > 0 && cursor <= length);
    CHECK(command.type == CMD_INSERT);
    CHECK(strncmp(command.table, "users", sizeof command.table) == 0);
    CHECK(command.value_count == 2);
    CHECK(command.values[0].type == VALUE_STRING);
    CHECK(strncmp(command.values[0].s, "Mina", sizeof command.values[0].s) == 0);
    CHECK(command.values[1].type == VALUE_INT);
    CHECK(command.values[1].i == 20);
    CHECK(command.has_where == 0);
    CHECK(error[0] == '\0');

    /* P01: WHERE SELECT */
    first_cursor = cursor;
    poison(&command, error);
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_OK);
    CHECK(cursor > first_cursor && cursor <= length);
    CHECK(command.type == CMD_SELECT);
    CHECK(strncmp(command.table, "users", sizeof command.table) == 0);
    CHECK(command.value_count == 0);
    CHECK(command.has_where == 1);
    CHECK(strncmp(command.where_column, "age", sizeof command.where_column) == 0);
    CHECK(command.where_value.type == VALUE_INT);
    CHECK(command.where_value.i == 20);
    CHECK(error[0] == '\0');

    /* P01: normal end */
    first_cursor = cursor;
    poison(&command, error);
    CHECK(parse_next(sql, length, &cursor, &command, error) == DB_END);
    CHECK(cursor == length && cursor >= first_cursor);
    CHECK(is_zero_command(&command));
    CHECK(error[0] == '\0');

    /* P02: syntax errors stay one line and NUL-terminated */
    check_syntax_contract("SELECT FROM users;");
    check_syntax_contract("INSERT INTO users VALUES ('Mina', 20");

    /* P02: the statement after a bad one still parses */
    check_recovery_after_error("SELECT FROM users; SELECT * FROM users;", "users");

    if (failures != 0) {
        fprintf(stderr, "%d parser test failure(s)\n", failures);
        return 1;
    }
    printf("all parser tests passed\n");
    return 0;
}

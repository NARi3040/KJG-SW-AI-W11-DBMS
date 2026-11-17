/*
 * test_execute.c - execute.c 계약 검사 (저장 대역, assert 기반)
 *
 * 실제 저장소를 쓰지 않고 호출 횟수, 전달 값, 재생할 행만 기록하는 작은
 * 대역으로 E01~E03과 검증·부분 행 수·오류 전달을 확인해요.
 */
#include "dbms.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int insert_calls;
    int scan_calls;
    DbStatus insert_status;
    DbStatus scan_status;
    InsertResult insert_result;
    char name[DB_STRING_MAX + 1];
    int64_t age;
    Row rows[8];
    size_t row_count;
    size_t fail_after; /* 이 인덱스 앞에서 scan_status를 반환해요 */
    RowHandler seen_handler;
    void *seen_user_data;
} FakeStorage;

typedef struct {
    int calls;
    int fail_on_call;
    DbStatus fail_status;
    size_t count;
    Row rows[8];
    void *last_user_data;
} CallbackState;

static FakeStorage fake;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static Storage *fake_storage(void) {
    return (Storage *)&fake;
}

static void reset_fake(void) {
    memset(&fake, 0, sizeof fake);
    fake.insert_status = DB_OK;
    fake.scan_status = DB_OK;
    fake.fail_after = SIZE_MAX;
    fake.insert_result.id = 42;
    fake.insert_result.row_index = 7;
}

DbStatus storage_insert(Storage *storage, const char *name, int64_t age,
                        InsertResult *out_result) {
    (void)storage;
    fake.insert_calls++;
    fake.age = age;
    (void)snprintf(fake.name, sizeof fake.name, "%s", name);
    if (fake.insert_status == DB_OK) {
        *out_result = fake.insert_result;
    }
    return fake.insert_status;
}

DbStatus storage_scan(Storage *storage, RowHandler on_row, void *user_data,
                      size_t *out_count) {
    size_t i;

    (void)storage;
    fake.scan_calls++;
    fake.seen_handler = on_row;
    fake.seen_user_data = user_data;
    *out_count = 0;
    for (i = 0; i < fake.row_count; i++) {
        DbStatus status;
        if (i == fake.fail_after) {
            return fake.scan_status;
        }
        status = on_row(&fake.rows[i], user_data);
        if (status != DB_OK) {
            return status;
        }
        (*out_count)++;
    }
    return fake.scan_status;
}

static DbStatus record_row(const Row *row, void *user_data) {
    CallbackState *state = user_data;

    state->calls++;
    state->last_user_data = user_data;
    if (state->count < 8) {
        state->rows[state->count] = *row;
    }
    state->count++;
    if (state->fail_on_call == state->calls) {
        return state->fail_status;
    }
    return DB_OK;
}

static Command make_command(CommandType type, const char *table) {
    Command command;

    memset(&command, 0, sizeof command);
    command.type = type;
    (void)snprintf(command.table, sizeof command.table, "%s", table);
    return command;
}

static Command make_insert(const char *name, int64_t age) {
    Command command = make_command(CMD_INSERT, "users");

    command.value_count = 2;
    command.values[0].type = VALUE_STRING;
    (void)snprintf(command.values[0].s, sizeof command.values[0].s, "%s", name);
    command.values[1].type = VALUE_INT;
    command.values[1].i = age;
    return command;
}

static Command make_select(const char *table) {
    return make_command(CMD_SELECT, table);
}

static Value int_value(int64_t value) {
    Value result;

    memset(&result, 0, sizeof result);
    result.type = VALUE_INT;
    result.i = value;
    return result;
}

static Value string_value(const char *text) {
    Value result;

    memset(&result, 0, sizeof result);
    result.type = VALUE_STRING;
    (void)snprintf(result.s, sizeof result.s, "%s", text);
    return result;
}

static void set_where(Command *command, const char *column, Value value) {
    command->has_where = 1;
    (void)snprintf(command->where_column, sizeof command->where_column, "%s",
                   column);
    command->where_value = value;
}

static Row make_row(int64_t id, const char *name, int32_t age) {
    Row row;

    memset(&row, 0, sizeof row);
    row.id = id;
    (void)snprintf(row.name, sizeof row.name, "%s", name);
    row.age = age;
    return row;
}

static void poison(ExecuteResult *result) {
    memset(result, 0xAA, sizeof *result);
}

static int result_is_zero(const ExecuteResult *result) {
    return result->insert.id == 0 && result->insert.row_index == 0 &&
           result->row_count == 0;
}

static void test_insert_success_e01(void) {
    Command command = make_insert("Mina", 20);
    CallbackState callback;
    ExecuteResult result;

    reset_fake();
    memset(&callback, 0, sizeof callback);
    poison(&result);

    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(fake.insert_calls == 1);
    CHECK(fake.scan_calls == 0);
    CHECK(strcmp(fake.name, "Mina") == 0);
    CHECK(fake.age == 20);
    CHECK(result.insert.id == 42);
    CHECK(result.insert.row_index == 7);
    CHECK(result.row_count == 0);
    CHECK(callback.calls == 0);
}

static void test_insert_empty_and_boundaries(void) {
    char name64[USERS_NAME_MAX + 1];
    char name65[USERS_NAME_MAX + 2];
    Command command;
    ExecuteResult result;

    reset_fake();
    memset(name64, 'a', USERS_NAME_MAX);
    name64[USERS_NAME_MAX] = '\0';
    memset(name65, 'a', USERS_NAME_MAX + 1);
    name65[USERS_NAME_MAX + 1] = '\0';

    command = make_insert("", INT32_MIN);
    poison(&result);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) == DB_OK);
    CHECK(fake.insert_calls == 1);
    CHECK(strcmp(fake.name, "") == 0);
    CHECK(fake.age == INT32_MIN);

    reset_fake();
    command = make_insert(name64, INT32_MAX);
    poison(&result);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) == DB_OK);
    CHECK(fake.insert_calls == 1);
    CHECK(strcmp(fake.name, name64) == 0);
    CHECK(fake.age == INT32_MAX);

    reset_fake();
    command = make_insert(name65, 20);
    poison(&result);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.insert_calls == 0);
    CHECK(result_is_zero(&result));

    reset_fake();
    command = make_insert("Mina", (int64_t)INT32_MAX + 1);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.insert_calls == 0);

    reset_fake();
    command = make_insert("Mina", (int64_t)INT32_MIN - 1);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.insert_calls == 0);
}

static void test_insert_validation_e02(void) {
    Command command;
    ExecuteResult result;

    reset_fake();
    command = make_insert("Mina", 20);
    command.value_count = 0;
    poison(&result);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.insert_calls == 0);
    CHECK(result_is_zero(&result));

    command.value_count = 1;
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.insert_calls == 0);

    command.value_count = 3;
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.insert_calls == 0);

    command = make_insert("Mina", 20);
    command.values[0].type = VALUE_INT;
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_TYPE);
    CHECK(fake.insert_calls == 0);

    command = make_insert("Mina", 20);
    command.values[1].type = VALUE_STRING;
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_TYPE);
    CHECK(fake.insert_calls == 0);

    command = make_insert("Mina", 20);
    memset(command.table, 'u', sizeof command.table);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.insert_calls == 0);

    command = make_insert("Mina", 20);
    memset(command.values[0].s, 'a', sizeof command.values[0].s);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.insert_calls == 0);

    command = make_insert("Mina", 20);
    command.type = (CommandType)99;
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_ARG);
    CHECK(fake.insert_calls == 0);

    command = make_insert("Mina", 20);
    (void)snprintf(command.table, sizeof command.table, "%s", "other");
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.insert_calls == 0);

    reset_fake();
    command = make_insert("Mina", 20);
    (void)snprintf(command.table, sizeof command.table, "%s", "UsErS");
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) == DB_OK);
    CHECK(strcmp(command.table, "UsErS") == 0);
    CHECK(fake.insert_calls == 1);
}

static void test_null_arguments_and_result_init(void) {
    Command command = make_insert("Mina", 20);
    ExecuteResult result;

    reset_fake();
    poison(&result);
    CHECK(execute(NULL, &command, NULL, NULL, &result) == DB_ERR_ARG);
    CHECK(result_is_zero(&result));
    CHECK(fake.insert_calls == 0);
    CHECK(fake.scan_calls == 0);

    poison(&result);
    CHECK(execute(fake_storage(), NULL, NULL, NULL, &result) == DB_ERR_ARG);
    CHECK(result_is_zero(&result));
    CHECK(fake.insert_calls == 0);
    CHECK(fake.scan_calls == 0);

    CHECK(execute(fake_storage(), &command, NULL, NULL, NULL) == DB_ERR_ARG);
    CHECK(fake.insert_calls == 0);
    CHECK(fake.scan_calls == 0);
}

static void test_select_no_where_e03(void) {
    Command command = make_select("users");
    CallbackState callback;
    ExecuteResult result;

    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.rows[2] = make_row(3, "Hana", 27);
    fake.row_count = 3;
    memset(&callback, 0, sizeof callback);
    poison(&result);

    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(fake.scan_calls == 1);
    CHECK(fake.seen_handler == record_row);
    CHECK(fake.seen_user_data == &callback);
    CHECK(callback.calls == 3);
    CHECK(callback.last_user_data == &callback);
    CHECK(callback.rows[0].id == 1);
    CHECK(callback.rows[2].id == 3);
    CHECK(result.row_count == 3);
    CHECK(result.insert.id == 0);

    reset_fake();
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(fake.scan_calls == 1);
    CHECK(callback.calls == 0);
    CHECK(result.row_count == 0);

    reset_fake();
    (void)snprintf(command.table, sizeof command.table, "%s", "USERS");
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(strcmp(command.table, "USERS") == 0);
    CHECK(fake.scan_calls == 1);

    reset_fake();
    poison(&result);
    CHECK(execute(fake_storage(), &command, NULL, &callback, &result) ==
          DB_ERR_ARG);
    CHECK(fake.scan_calls == 0);
    CHECK(result_is_zero(&result));
}

static void test_select_where_matches_and_zero(void) {
    Command command = make_select("users");
    CallbackState callback;
    ExecuteResult result;

    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.rows[2] = make_row(3, "Mina", 20);
    fake.rows[3] = make_row(4, "Hana", 27);
    fake.rows[4] = make_row(5, "", 0);
    fake.row_count = 5;

    set_where(&command, "name", string_value("Mina"));
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(fake.scan_calls == 1);
    CHECK(callback.calls == 2);
    CHECK(callback.last_user_data == &callback);
    CHECK(callback.rows[0].id == 1);
    CHECK(callback.rows[1].id == 3);
    CHECK(result.row_count == 2);

    command = make_select("users");
    set_where(&command, "NAME", string_value("Mina"));
    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.rows[2] = make_row(3, "Mina", 20);
    fake.row_count = 3;
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(strcmp(command.where_column, "NAME") == 0);
    CHECK(callback.calls == 2);
    CHECK(result.row_count == 2);

    command = make_select("users");
    set_where(&command, "name", string_value("Nobody"));
    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.row_count = 1;
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(fake.scan_calls == 1);
    CHECK(callback.calls == 0);
    CHECK(result.row_count == 0);

    command = make_select("users");
    set_where(&command, "name", string_value(""));
    reset_fake();
    fake.rows[0] = make_row(5, "", 0);
    fake.rows[1] = make_row(6, "Mina", 20);
    fake.row_count = 2;
    memset(&callback, 0, sizeof callback);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(callback.calls == 1);
    CHECK(callback.rows[0].id == 5);
    CHECK(result.row_count == 1);

    command = make_select("users");
    set_where(&command, "name", string_value("mina"));
    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.row_count = 1;
    memset(&callback, 0, sizeof callback);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(callback.calls == 0);
    CHECK(result.row_count == 0);

    command = make_select("users");
    set_where(&command, "age", int_value(31));
    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.row_count = 2;
    memset(&callback, 0, sizeof callback);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(callback.calls == 1);
    CHECK(callback.rows[0].id == 2);
    CHECK(result.row_count == 1);

    command = make_select("users");
    set_where(&command, "id", int_value(3));
    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(3, "Hana", 27);
    fake.row_count = 2;
    memset(&callback, 0, sizeof callback);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(callback.calls == 1);
    CHECK(callback.rows[0].id == 3);
    CHECK(result.row_count == 1);
}

static void test_select_where_boundaries(void) {
    char name64[USERS_NAME_MAX + 1];
    Command command;
    CallbackState callback;
    ExecuteResult result;

    memset(name64, 'a', USERS_NAME_MAX);
    name64[USERS_NAME_MAX] = '\0';

    reset_fake();
    fake.rows[0] = make_row(1, name64, INT32_MIN);
    fake.rows[1] = make_row(2, "other", INT32_MAX);
    fake.row_count = 2;
    command = make_select("users");
    set_where(&command, "name", string_value(name64));
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(callback.calls == 1);
    CHECK(callback.rows[0].id == 1);
    CHECK(result.row_count == 1);

    reset_fake();
    fake.rows[0] = make_row(INT64_MAX, "other", INT32_MAX);
    fake.row_count = 1;
    command = make_select("users");
    set_where(&command, "id", int_value(INT64_MAX));
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(callback.calls == 1);
    CHECK(callback.rows[0].id == INT64_MAX);
    CHECK(result.row_count == 1);

    command = make_select("users");
    set_where(&command, "id", int_value(INT64_MIN));
    reset_fake();
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.scan_calls == 0);
    CHECK(result_is_zero(&result));

    reset_fake();
    fake.rows[0] = make_row(1, "x", INT32_MIN);
    fake.rows[1] = make_row(2, "y", INT32_MAX);
    fake.row_count = 2;
    command = make_select("users");
    set_where(&command, "age", int_value(INT32_MIN));
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_OK);
    CHECK(callback.calls == 1);
    CHECK(callback.rows[0].id == 1);
    CHECK(result.row_count == 1);
}

static void test_select_where_validation_e02(void) {
    Command command;
    CallbackState callback;
    ExecuteResult result;

    memset(&callback, 0, sizeof callback);

    reset_fake();
    command = make_select("users");
    set_where(&command, "id", int_value(0));
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.scan_calls == 0);
    CHECK(result_is_zero(&result));

    command = make_select("users");
    set_where(&command, "id", int_value(-1));
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "id", string_value("7"));
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_TYPE);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "age", int_value((int64_t)INT32_MAX + 1));
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "age", int_value((int64_t)INT32_MIN - 1));
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_RANGE);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "age", string_value("31"));
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_TYPE);
    CHECK(fake.scan_calls == 0);

    {
        char name65[USERS_NAME_MAX + 2];
        memset(name65, 'a', USERS_NAME_MAX + 1);
        name65[USERS_NAME_MAX + 1] = '\0';
        command = make_select("users");
        set_where(&command, "name", string_value(name65));
        CHECK(execute(fake_storage(), &command, record_row, &callback,
                      &result) == DB_ERR_RANGE);
        CHECK(fake.scan_calls == 0);
    }

    command = make_select("users");
    set_where(&command, "name", string_value("Mina"));
    memset(command.where_value.s, 'a', sizeof command.where_value.s);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "name", int_value(1));
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_TYPE);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "nickname", string_value("Mina"));
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "name", string_value("Mina"));
    memset(command.where_column, 'n', sizeof command.where_column);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.scan_calls == 0);

    command = make_select("other");
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_SCHEMA);
    CHECK(fake.scan_calls == 0);

    command = make_select("users");
    set_where(&command, "id", int_value(1));
    CHECK(execute(fake_storage(), &command, NULL, &callback, &result) ==
          DB_ERR_ARG);
    CHECK(fake.scan_calls == 0);
    CHECK(result_is_zero(&result));
}

static void test_select_failures_and_partial_counts(void) {
    Command command;
    CallbackState callback;
    ExecuteResult result;

    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.rows[2] = make_row(3, "Mina", 20);
    fake.rows[3] = make_row(4, "Mina", 20);
    fake.row_count = 4;
    command = make_select("users");
    set_where(&command, "name", string_value("Mina"));
    memset(&callback, 0, sizeof callback);
    callback.fail_on_call = 2;
    callback.fail_status = DB_ERR_OUTPUT;
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_OUTPUT);
    CHECK(callback.calls == 2);
    CHECK(result.row_count == 1);

    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.rows[2] = make_row(3, "Mina", 20);
    fake.row_count = 3;
    command = make_select("users");
    memset(&callback, 0, sizeof callback);
    callback.fail_on_call = 2;
    callback.fail_status = DB_ERR_OUTPUT;
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_OUTPUT);
    CHECK(callback.calls == 2);
    CHECK(result.row_count == 1);

    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.rows[2] = make_row(3, "Mina", 20);
    fake.row_count = 3;
    fake.fail_after = 2;
    fake.scan_status = DB_ERR_IO;
    command = make_select("users");
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_IO);
    CHECK(callback.calls == 2);
    CHECK(result.row_count == 2);

    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Joon", 31);
    fake.rows[2] = make_row(3, "Mina", 20);
    fake.row_count = 3;
    fake.fail_after = 2;
    fake.scan_status = DB_ERR_IO;
    command = make_select("users");
    set_where(&command, "name", string_value("Mina"));
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_IO);
    CHECK(callback.calls == 1);
    CHECK(result.row_count == 1);

    reset_fake();
    fake.row_count = 3;
    fake.fail_after = 0;
    fake.scan_status = DB_ERR_CORRUPT;
    command = make_select("users");
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_CORRUPT);
    CHECK(callback.calls == 0);
    CHECK(result.row_count == 0);

    reset_fake();
    fake.rows[0] = make_row(1, "Mina", 20);
    fake.rows[1] = make_row(2, "Mina", 20);
    fake.row_count = 2;
    fake.scan_status = DB_ERR_IO;
    command = make_select("users");
    set_where(&command, "name", string_value("Mina"));
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_IO);
    CHECK(callback.calls == 2);
    CHECK(result.row_count == 2);
}

static void test_insert_storage_errors(void) {
    Command command = make_insert("Mina", 20);
    CallbackState callback;
    ExecuteResult result;

    reset_fake();
    fake.insert_status = DB_ERR_WRITE_UNCERTAIN;
    memset(&callback, 0, sizeof callback);
    poison(&result);
    CHECK(execute(fake_storage(), &command, record_row, &callback, &result) ==
          DB_ERR_WRITE_UNCERTAIN);
    CHECK(fake.insert_calls == 1);
    CHECK(callback.calls == 0);
    CHECK(result_is_zero(&result));

    reset_fake();
    fake.insert_status = DB_ERR_IO;
    poison(&result);
    CHECK(execute(fake_storage(), &command, NULL, NULL, &result) == DB_ERR_IO);
    CHECK(fake.insert_calls == 1);
    CHECK(result_is_zero(&result));
}

int main(void) {
    test_insert_success_e01();
    test_insert_empty_and_boundaries();
    test_insert_validation_e02();
    test_null_arguments_and_result_init();
    test_select_no_where_e03();
    test_select_where_matches_and_zero();
    test_select_where_boundaries();
    test_select_where_validation_e02();
    test_select_failures_and_partial_counts();
    test_insert_storage_errors();

    if (failures != 0) {
        fprintf(stderr, "%d execute test failure(s)\n", failures);
        return 1;
    }
    printf("all execute tests passed\n");
    return 0;
}

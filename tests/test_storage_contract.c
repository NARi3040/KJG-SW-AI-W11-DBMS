/*
 * test_storage_contract.c - S01/S02 storage contract checks
 *
 * Uses the relative path "users.sans" only. Run this executable in an empty
 * temporary working directory (tests/run_tests.py does that).
 * S01: empty scan, insert two rows, scan again with distinct IDs.
 * S02: close, reopen, add one row; old rows survive and the new ID is distinct.
 */
#include "dbms.h"

#include <stdio.h>
#include <string.h>

#define MAX_ROWS 8

typedef struct {
    Row rows[MAX_ROWS];
    size_t count;
} ScanState;

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }
    (void)fclose(file);
    return 1;
}

static DbStatus copy_row(const Row *row, void *user_data) {
    ScanState *state = user_data;

    if (state->count < MAX_ROWS) {
        state->rows[state->count] = *row;
    }
    state->count++;
    return DB_OK;
}

int main(void) {
    Storage *storage = NULL;
    InsertResult first = {0, 0};
    InsertResult second = {0, 0};
    InsertResult third = {0, 0};
    ScanState scan;
    size_t count = 0;
    DbStatus status;

    if (file_exists("users.sans")) {
        fprintf(stderr,
                "storage test requires an empty temporary cwd (users.sans exists)\n");
        return 1;
    }

    /* S01: empty storage */
    status = storage_open("users.sans", &storage);
    CHECK(status == DB_OK);
    CHECK(storage != NULL);
    if (status != DB_OK || storage == NULL) {
        (void)remove("users.sans");
        return 1;
    }

    memset(&scan, 0, sizeof scan);
    status = storage_scan(storage, copy_row, &scan, &count);
    CHECK(status == DB_OK);
    CHECK(count == 0);
    CHECK(scan.count == 0);

    /* S01: insert two rows and scan again */
    status = storage_insert(storage, "Mina", 20, &first);
    CHECK(status == DB_OK);
    CHECK(first.id >= 1);
    CHECK(first.row_index == 0);

    status = storage_insert(storage, "Joon", 31, &second);
    CHECK(status == DB_OK);
    CHECK(second.id != first.id);
    CHECK(second.row_index == 1);

    memset(&scan, 0, sizeof scan);
    count = 0;
    status = storage_scan(storage, copy_row, &scan, &count);
    CHECK(status == DB_OK);
    CHECK(count == 2);
    CHECK(scan.count == 2);
    CHECK(scan.rows[0].id == first.id);
    CHECK(strcmp(scan.rows[0].name, "Mina") == 0);
    CHECK(scan.rows[0].age == 20);
    CHECK(scan.rows[1].id == second.id);
    CHECK(strcmp(scan.rows[1].name, "Joon") == 0);
    CHECK(scan.rows[1].age == 31);

    status = storage_close(storage);
    CHECK(status == DB_OK);
    storage = NULL;

    /* S02: reopen, add a row, and verify old rows and distinct new ID */
    status = storage_open("users.sans", &storage);
    CHECK(status == DB_OK);
    CHECK(storage != NULL);
    if (status != DB_OK || storage == NULL) {
        (void)remove("users.sans");
        return 1;
    }

    status = storage_insert(storage, "Hana", 27, &third);
    CHECK(status == DB_OK);
    CHECK(third.id != first.id);
    CHECK(third.id != second.id);

    memset(&scan, 0, sizeof scan);
    count = 0;
    status = storage_scan(storage, copy_row, &scan, &count);
    CHECK(status == DB_OK);
    CHECK(count == 3);
    CHECK(scan.count == 3);
    CHECK(scan.rows[0].id == first.id);
    CHECK(strcmp(scan.rows[0].name, "Mina") == 0);
    CHECK(scan.rows[0].age == 20);
    CHECK(scan.rows[1].id == second.id);
    CHECK(strcmp(scan.rows[1].name, "Joon") == 0);
    CHECK(scan.rows[1].age == 31);
    CHECK(scan.rows[2].id == third.id);
    CHECK(strcmp(scan.rows[2].name, "Hana") == 0);
    CHECK(scan.rows[2].age == 27);

    status = storage_close(storage);
    CHECK(status == DB_OK);
    storage = NULL;

    (void)remove("users.sans");

    if (failures != 0) {
        fprintf(stderr, "%d storage test failure(s)\n", failures);
        return 1;
    }
    printf("all storage tests passed\n");
    return 0;
}

/*
 * storage_test.c - 저장 모듈 단위 테스트 (docs/storage.md 9장 T-1~T-29)
 *
 * T-23이 storage.c의 static 바이트 변환 함수를 직접 확인하므로 storage.c를
 * 이 파일에 포함해서 함께 컴파일해요. storage.c를 따로 링크하지 않아요.
 *
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -I. -o storage_test tests/storage_test.c
 *   ./storage_test [임시 파일을 만들 디렉터리]
 *
 * 디렉터리를 주지 않으면 현재 디렉터리에 storage_test_*.sans를 만들고 지워요.
 */
#include "storage.c"

#define MAX_ROWS 200
#define PATH_SIZE 512
#define FILE_BUF_SIZE (64 + 80 * 200)

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

/* 실패하면 그 테스트 함수를 끝내요. 열기에 실패한 Storage로 계속 부르지 않으려고요. */
#define REQUIRE(cond)                                                        \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return;                                                          \
        }                                                                    \
    } while (0)

/* ---------- 테스트 도우미 ---------- */

typedef struct {
    Row rows[MAX_ROWS];
    size_t n;
    size_t fail_at;           /* 이 순번(0부터)의 행에서 오류를 돌려줘요. SIZE_MAX면 없음 */
    Storage *insert_into;     /* NULL이 아니면 행마다 이 Storage에 INSERT해요 (T-27) */
} Collector;

static void collector_init(Collector *c) {
    memset(c, 0, sizeof(*c));
    c->fail_at = SIZE_MAX;
}

static DbStatus collect_row(const Row *row, void *user_data) {
    Collector *c = user_data;
    if (c->n == c->fail_at) {
        return DB_ERR_OUTPUT;
    }
    if (c->insert_into != NULL) {
        InsertResult ignored;
        if (storage_insert(c->insert_into, "Extra", 1, &ignored) != DB_OK) {
            return DB_ERR_IO;
        }
    }
    if (c->n < MAX_ROWS) {
        c->rows[c->n] = *row; /* 행은 콜백 안에서만 유효하므로 복사해요 */
    }
    c->n++;
    return DB_OK;
}

static const char *temp_dir = ".";

static void make_path(char *out, const char *name) {
    snprintf(out, PATH_SIZE, "%s/storage_test_%s.sans", temp_dir, name);
    remove(out);
}

static long file_size(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) {
        size = ftell(fp);
    }
    fclose(fp);
    return size;
}

static size_t read_file(const char *path, unsigned char *buf, size_t cap) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    size_t n = fread(buf, 1, cap, fp);
    fclose(fp);
    return n;
}

static int write_file(const char *path, const void *buf, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return 0;
    }
    int ok = fwrite(buf, 1, len, fp) == len;
    return fclose(fp) == 0 && ok;
}

static int append_file(const char *path, const void *buf, size_t len) {
    FILE *fp = fopen(path, "ab");
    if (fp == NULL) {
        return 0;
    }
    int ok = fwrite(buf, 1, len, fp) == len;
    return fclose(fp) == 0 && ok;
}

/* 파일 at 위치에 bytes를 덮어써요. */
static int patch_file(const char *path, long at, const void *bytes, size_t len) {
    unsigned char buf[FILE_BUF_SIZE];
    size_t n = read_file(path, buf, sizeof(buf));
    if (at < 0 || (size_t)at + len > n) {
        return 0;
    }
    memcpy(buf + at, bytes, len);
    return write_file(path, buf, n);
}

static int truncate_file(const char *path, size_t len) {
    unsigned char buf[FILE_BUF_SIZE];
    size_t n = read_file(path, buf, sizeof(buf));
    return len <= n && write_file(path, buf, len);
}

/* 새 파일을 만들고 count건을 저장한 뒤 닫아요. 이름은 "R<번호>", 나이는 번호예요. */
static int make_rows(const char *path, int count) {
    Storage *s = NULL;
    if (storage_open(path, &s) != DB_OK) {
        return 0;
    }
    int ok = 1;
    for (int i = 0; i < count && ok; i++) {
        char name[16];
        InsertResult r;
        snprintf(name, sizeof(name), "R%d", i + 1);
        ok = storage_insert(s, name, i + 1, &r) == DB_OK;
    }
    return storage_close(s) == DB_OK && ok;
}

static DbStatus scan_path(const char *path, Collector *c, size_t *count) {
    Storage *s = NULL;
    DbStatus status = storage_open(path, &s);
    if (status != DB_OK) {
        return status;
    }
    status = storage_scan(s, collect_row, c, count);
    storage_close(s);
    return status;
}

/* ---------- 테스트 ---------- */

static void t01_t02_new_file(void) {
    char path[PATH_SIZE];
    make_path(path, "t01");
    Storage *s = NULL;
    REQUIRE(storage_open(path, &s) == DB_OK);
    REQUIRE(s != NULL);
    CHECK(file_size(path) == 64);

    unsigned char buf[64];
    CHECK(read_file(path, buf, sizeof(buf)) == 64);
    CHECK(memcmp(buf, "SANS", 4) == 0);
    CHECK(get_u64le(buf + 16) == 0); /* row_count 0 */
    CHECK(get_u64le(buf + 24) == 1); /* next_id 1 */

    /* T-2 */
    Collector c;
    collector_init(&c);
    size_t count = 99;
    CHECK(storage_scan(s, collect_row, &c, &count) == DB_OK);
    CHECK(count == 0);
    CHECK(c.n == 0);
    CHECK(storage_close(s) == DB_OK);
    remove(path);
}

static void t03_exact_bytes(void) {
    static const unsigned char expected[144] = {
        0x53, 0x41, 0x4E, 0x53, 0x01, 0x00, 0x40, 0x00, 0x50, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 32..63 예약 0 */
        [64] = 0x01, [72] = 0x17, [76] = 0x07,
        [80] = 0x4B, 0x69, 0x6D, 0x20, 0x4D, 0x69, 0x6E,
    };
    char path[PATH_SIZE];
    make_path(path, "t03");
    Storage *s = NULL;
    InsertResult r;
    REQUIRE(storage_open(path, &s) == DB_OK);
    CHECK(storage_insert(s, "Kim Min", 23, &r) == DB_OK);
    CHECK(r.id == 1);
    CHECK(r.row_index == 0);
    CHECK(storage_close(s) == DB_OK);

    unsigned char buf[FILE_BUF_SIZE];
    CHECK(read_file(path, buf, sizeof(buf)) == 144);
    CHECK(memcmp(buf, expected, sizeof(expected)) == 0);
    remove(path);
}

static void t04_scan_three(void) {
    char path[PATH_SIZE];
    make_path(path, "t04");
    CHECK(make_rows(path, 3));
    Collector c;
    collector_init(&c);
    size_t count = 0;
    CHECK(scan_path(path, &c, &count) == DB_OK);
    CHECK(count == 3);
    CHECK(c.n == 3);
    for (size_t i = 0; i < 3 && i < c.n; i++) {
        char name[16];
        snprintf(name, sizeof(name), "R%d", (int)i + 1);
        CHECK(c.rows[i].id == (int64_t)i + 1);
        CHECK(c.rows[i].age == (int32_t)i + 1);
        CHECK(c.rows[i].row_index == i);
        CHECK(strcmp(c.rows[i].name, name) == 0);
    }
    remove(path);
}

static void t05_reopen(void) {
    char path[PATH_SIZE];
    make_path(path, "t05");
    CHECK(make_rows(path, 3));
    Storage *s = NULL;
    InsertResult r;
    REQUIRE(storage_open(path, &s) == DB_OK);
    CHECK(storage_insert(s, "Next", 25, &r) == DB_OK);
    CHECK(r.id == 4);
    CHECK(r.row_index == 3);
    CHECK(storage_close(s) == DB_OK);
    CHECK(file_size(path) == 64 + 80 * 4);
    remove(path);
}

/* 한 건을 저장하고 다시 읽은 행을 돌려줘요. */
static DbStatus insert_and_read(const char *tag, const char *name, int64_t age,
                                Row *out) {
    char path[PATH_SIZE];
    make_path(path, tag);
    Storage *s = NULL;
    InsertResult r;
    DbStatus status = storage_open(path, &s);
    if (status == DB_OK) {
        status = storage_insert(s, name, age, &r);
    }
    storage_close(s);
    if (status == DB_OK) {
        Collector c;
        collector_init(&c);
        size_t count = 0;
        status = scan_path(path, &c, &count);
        if (status == DB_OK && c.n == 1) {
            *out = c.rows[0];
        } else if (status == DB_OK) {
            status = DB_ERR_CORRUPT;
        }
    }
    remove(path);
    return status;
}

/* insert_and_read()가 실패하면 row가 채워지지 않으므로, 결과를 읽기 전에
 * REQUIRE로 멈춰요. 한 케이스의 실패가 다른 케이스를 건너뛰지 않게 나눠요. */
static void t06_long_name(void) {
    Row row;
    char name64[65];
    memset(name64, 'a', 64);
    name64[64] = '\0';
    REQUIRE(insert_and_read("t06", name64, 1, &row) == DB_OK);
    CHECK(strlen(row.name) == 64);
    CHECK(strcmp(row.name, name64) == 0);
}

static void t08_utf8_name(void) {
    /* '김민수' UTF-8 9바이트 */
    static const char kim[] = "\xEA\xB9\x80\xEB\xAF\xBC\xEC\x88\x98";
    Row row;
    CHECK(sizeof(kim) - 1 == 9);
    REQUIRE(insert_and_read("t08", kim, 1, &row) == DB_OK);
    CHECK(strcmp(row.name, kim) == 0);
}

static void t09_empty_name(void) {
    Row row;
    REQUIRE(insert_and_read("t09", "", 1, &row) == DB_OK);
    CHECK(row.name[0] == '\0');
}

static void t10_age_bound(const char *tag, int32_t age) {
    Row row;
    REQUIRE(insert_and_read(tag, "Age", age, &row) == DB_OK);
    CHECK(row.age == age);
}

/* 실패한 INSERT가 파일을 바꾸지 않는지 확인해요 (T-7, T-11). */
static void expect_range_no_change(const char *tag, const char *name, int64_t age) {
    char path[PATH_SIZE];
    make_path(path, tag);
    CHECK(make_rows(path, 1));
    unsigned char before[FILE_BUF_SIZE], after[FILE_BUF_SIZE];
    size_t n_before = read_file(path, before, sizeof(before));

    Storage *s = NULL;
    InsertResult r;
    r.id = 77;
    REQUIRE(storage_open(path, &s) == DB_OK);
    CHECK(storage_insert(s, name, age, &r) == DB_ERR_RANGE);
    CHECK(r.id == 0 && r.row_index == 0);
    CHECK(storage_close(s) == DB_OK);

    size_t n_after = read_file(path, after, sizeof(after));
    CHECK(n_before == n_after);
    CHECK(memcmp(before, after, n_before) == 0);
    remove(path);
}

static void t07_t11_ranges(void) {
    char name65[66];
    memset(name65, 'a', 65);
    name65[65] = '\0';
    expect_range_no_change("t07", name65, 1);

    /* T-11, 반대쪽 경계도 함께 */
    expect_range_no_change("t11a", "Over", (int64_t)INT32_MAX + 1);
    expect_range_no_change("t11b", "Under", (int64_t)INT32_MIN - 1);
}

static void t12_callback_error(void) {
    char path[PATH_SIZE];
    make_path(path, "t12");
    CHECK(make_rows(path, 5));
    Collector c;
    collector_init(&c);
    c.fail_at = 2; /* 3번째 행 */
    size_t count = 0;
    CHECK(scan_path(path, &c, &count) == DB_ERR_OUTPUT);
    CHECK(count == 2);
    remove(path);
}

static void expect_open(const char *path, DbStatus want) {
    Storage dummy;
    Storage *s = &dummy; /* 실패하면 NULL로 바뀌는지 보려고 NULL이 아닌 값을 넣어요 */
    DbStatus got = storage_open(path, &s);
    CHECK(got == want);
    if (got == DB_OK) {
        CHECK(s != NULL);
        storage_close(s);
    } else {
        CHECK(s == NULL);
    }
}

static void t13_t17_open_checks(void) {
    char path[PATH_SIZE];

    /* T-13: 0바이트 파일 */
    make_path(path, "t13");
    CHECK(write_file(path, "", 0));
    expect_open(path, DB_OK);
    CHECK(file_size(path) == 64);
    remove(path);

    /* T-14: 10바이트 파일 */
    make_path(path, "t14");
    CHECK(write_file(path, "0123456789", 10));
    expect_open(path, DB_ERR_CORRUPT);
    remove(path);

    /* T-15: magic XANS */
    make_path(path, "t15");
    CHECK(make_rows(path, 1));
    CHECK(patch_file(path, 0, "X", 1));
    expect_open(path, DB_ERR_CORRUPT);
    remove(path);

    /* T-16: next_id = row_count + 2 */
    make_path(path, "t16");
    CHECK(make_rows(path, 1));
    unsigned char id[8];
    put_u64le(id, 3);
    CHECK(patch_file(path, 24, id, sizeof(id)));
    expect_open(path, DB_ERR_CORRUPT);
    remove(path);

    /* T-17: 3건 저장 후 레코드 2개 길이로 자름 */
    make_path(path, "t17");
    CHECK(make_rows(path, 3));
    CHECK(truncate_file(path, 64 + 80 * 2));
    expect_open(path, DB_ERR_CORRUPT);
    remove(path);
}

static void t18_trailing_garbage(void) {
    char path[PATH_SIZE];
    make_path(path, "t18");
    CHECK(make_rows(path, 2));
    unsigned char junk[30];
    memset(junk, 0xAB, sizeof(junk));
    CHECK(append_file(path, junk, sizeof(junk)));

    Storage *s = NULL;
    Collector c;
    collector_init(&c);
    size_t count = 0;
    InsertResult r;
    REQUIRE(storage_open(path, &s) == DB_OK);
    CHECK(storage_scan(s, collect_row, &c, &count) == DB_OK);
    CHECK(count == 2);
    CHECK(storage_insert(s, "Third", 3, &r) == DB_OK);
    CHECK(r.id == 3);
    CHECK(storage_close(s) == DB_OK);

    collector_init(&c);
    CHECK(scan_path(path, &c, &count) == DB_OK);
    CHECK(count == 3);
    CHECK(c.n == 3 && strcmp(c.rows[2].name, "Third") == 0);
    remove(path);
}

static void t19_t20_bad_records(void) {
    char path[PATH_SIZE];
    Collector c;
    size_t count = 0;

    /* T-19: 2번째 레코드의 id를 99로 */
    make_path(path, "t19");
    CHECK(make_rows(path, 3));
    unsigned char id[8];
    put_u64le(id, 99);
    CHECK(patch_file(path, 64 + 80, id, sizeof(id)));
    collector_init(&c);
    CHECK(scan_path(path, &c, &count) == DB_ERR_CORRUPT);
    CHECK(count == 1);
    remove(path);

    /* T-20: 1번째 레코드의 name_len을 65로 */
    make_path(path, "t20");
    CHECK(make_rows(path, 3));
    unsigned char len = 65;
    CHECK(patch_file(path, 64 + 12, &len, 1));
    collector_init(&c);
    CHECK(scan_path(path, &c, &count) == DB_ERR_CORRUPT);
    CHECK(count == 0);
    remove(path);

    /* T-28: 0이어야 하는 바이트가 0이 아님. 2번째 레코드의 이름("R2") 뒤 패딩,
     * 1번째 레코드의 예약 바이트를 각각 바꿔요. */
    unsigned char x = 'x';
    make_path(path, "t28a");
    CHECK(make_rows(path, 3));
    CHECK(patch_file(path, 64 + 80 + 16 + 2 + 5, &x, 1));
    collector_init(&c);
    CHECK(scan_path(path, &c, &count) == DB_ERR_CORRUPT);
    CHECK(count == 1);
    remove(path);

    make_path(path, "t28b");
    CHECK(make_rows(path, 3));
    CHECK(patch_file(path, 64 + 13, &x, 1));
    collector_init(&c);
    CHECK(scan_path(path, &c, &count) == DB_ERR_CORRUPT);
    CHECK(count == 0);
    remove(path);

    /* T-29: 3번째 레코드의 이름("R3") 안에 0 바이트 */
    unsigned char zero = 0;
    make_path(path, "t29");
    CHECK(make_rows(path, 3));
    CHECK(patch_file(path, 64 + 80 * 2 + 16 + 1, &zero, 1));
    collector_init(&c);
    CHECK(scan_path(path, &c, &count) == DB_ERR_CORRUPT);
    CHECK(count == 2);
    remove(path);
}

static void t21_t22_misc(void) {
    char path[PATH_SIZE];
    snprintf(path, sizeof(path), "%s/storage_test_no_such_dir/users.sans", temp_dir);
    expect_open(path, DB_ERR_IO);

    CHECK(storage_close(NULL) == DB_OK);
}

static void t23_byte_helpers(void) {
    unsigned char b[8];

    put_u64le(b, (uint64_t)-1);
    CHECK(get_u64le(b) == UINT64_MAX);
    put_u64le(b, 0x0102030405060708u);
    CHECK(b[0] == 0x08 && b[7] == 0x01);
    CHECK(get_u64le(b) == 0x0102030405060708u);

    put_u32le(b, 0xA1B2C3D4u);
    CHECK(b[0] == 0xD4 && b[3] == 0xA1);
    CHECK(get_u32le(b) == 0xA1B2C3D4u);

    put_u16le(b, 0x1234);
    CHECK(b[0] == 0x34 && b[1] == 0x12);
    CHECK(get_u16le(b) == 0x1234);

    CHECK(u64_to_i64((uint64_t)-1) == -1);
    CHECK(u64_to_i64(0x8000000000000000u) == INT64_MIN);
    CHECK(u32_to_i32(0x80000000u) == INT32_MIN);
    CHECK(u32_to_i32(0xFFFFFFFFu) == -1);
}

/* count건을 저장하고 scan해서 마지막 행을 확인해요 (T-24~T-26). */
static void scan_batch_case(const char *tag, int count) {
    char path[PATH_SIZE];
    make_path(path, tag);
    CHECK(make_rows(path, count));
    Collector c;
    collector_init(&c);
    size_t got = 0;
    CHECK(scan_path(path, &c, &got) == DB_OK);
    CHECK(got == (size_t)count);
    CHECK(c.n == (size_t)count);
    if (c.n == (size_t)count && count > 0) {
        const Row *last = &c.rows[count - 1];
        CHECK(c.rows[0].id == 1 && c.rows[0].row_index == 0);
        CHECK(last->id == count);
        CHECK(last->row_index == (uint64_t)count - 1);
    }
    remove(path);
}

static void t24_t26_batches(void) {
    scan_batch_case("t24", 1);
    scan_batch_case("t25", 128);
    scan_batch_case("t26", 129);
}

/* T-27: 계약 위반이지만, 콜백이 같은 Storage에 INSERT해도 scan은 시작할 때의
 * 행만 제자리에서 읽어요 (5.3절). 129건이면 묶음 경계를 지나요. */
static void t27_insert_during_scan(void) {
    char path[PATH_SIZE];
    make_path(path, "t27");
    CHECK(make_rows(path, 129));

    Storage *s = NULL;
    Collector c;
    collector_init(&c);
    size_t count = 0;
    REQUIRE(storage_open(path, &s) == DB_OK);
    c.insert_into = s;
    CHECK(storage_scan(s, collect_row, &c, &count) == DB_OK);
    CHECK(count == 129);
    CHECK(c.n == 129 && c.rows[128].id == 129 && strcmp(c.rows[128].name, "R129") == 0);
    CHECK(storage_close(s) == DB_OK);

    collector_init(&c);
    CHECK(scan_path(path, &c, &count) == DB_OK);
    CHECK(count == 129 * 2);
    remove(path);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        temp_dir = argv[1];
    }

    t01_t02_new_file();
    t03_exact_bytes();
    t04_scan_three();
    t05_reopen();
    t06_long_name();
    t08_utf8_name();
    t09_empty_name();
    t10_age_bound("t10a", INT32_MIN);
    t10_age_bound("t10b", INT32_MAX);
    t07_t11_ranges();
    t12_callback_error();
    t13_t17_open_checks();
    t18_trailing_garbage();
    t19_t20_bad_records();
    t21_t22_misc();
    t23_byte_helpers();
    t24_t26_batches();
    t27_insert_during_scan();

    printf("storage_test: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

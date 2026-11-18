/*
 * storage.c - users 테이블의 .sans 바이너리 저장 (docs/storage.md)
 *
 * 파일은 64바이트 헤더 뒤에 80바이트 고정 레코드가 이어져요. 정수는 모두
 * 리틀 엔디언으로 필드마다 직접 바꿔 써요. 레코드를 먼저 쓰고 헤더의
 * row_count를 나중에 고쳐서, 헤더가 "어디까지 저장됐는지"의 기준이 돼요.
 *
 * id 인덱스(B+ 트리, bptree.c)는 메모리에만 있어요. 열 때 파일 전체를 읽어
 * 다시 만들고, INSERT가 헤더까지 쓴 뒤 넣어요(docs/phase2-3.md 3.1·3.3절).
 */

/* 어떤 #include보다 먼저 둬요. Linux에서 fseeko·ftello·strnlen을 선언하고
 * off_t를 64비트로 맞춰요. MinGW-w64에서는 printf 계열이 MinGW 자체 구현으로
 * 바뀌지만(__USE_MINGW_ANSI_STDIO), 저장 모듈은 printf를 쓰지 않아요. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "dbms.h"
#include "bptree.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h> /* off_t */
#endif

#define SANS_MAGIC "SANS"
#define SANS_VERSION 1
#define SANS_HEADER_SIZE 64
#define SANS_RECORD_SIZE 80
#define SCAN_BATCH 128

/* 64 + row_count * 80이 int64 범위를 넘지 않는 최대 행 수예요. */
#define SANS_MAX_ROWS ((uint64_t)((INT64_MAX - SANS_HEADER_SIZE) / SANS_RECORD_SIZE))

struct Storage {
    FILE *fp;           /* 위치는 하나뿐이라 읽고 쓸 때마다 위치부터 잡아요 */
    uint64_t row_count; /* 확정된 레코드 수. 파일 헤더와 같아요 */
    int64_t next_id;    /* 다음에 줄 ID */
    int broken;         /* 헤더 쓰기 결과를 모를 때 1 */
    BpTree *index;      /* id → row_index. NULL이면 "인덱스 안 씀"이고 id 조회는
                           전체 순회해요(재구축 중 손상, INSERT 때 메모리 부족) */
};

/* ---------- 바이트 변환 (파일을 건드리지 않는 순수 함수) ---------- */

static void put_u16le(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xFFu);
    p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_u32le(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
    }
}

static void put_u64le(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
    }
}

static uint16_t get_u16le(const unsigned char *p) {
    return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

static uint32_t get_u32le(const unsigned char *p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint64_t get_u64le(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* 2의 보수 비트를 그대로 옮겨요. 범위 밖 변환이 구현 정의가 되지 않도록
 * 음수는 직접 계산해요. */
static int64_t u64_to_i64(uint64_t v) {
    if (v <= (uint64_t)INT64_MAX) {
        return (int64_t)v;
    }
    return -(int64_t)(UINT64_MAX - v) - 1;
}

static int32_t u32_to_i32(uint32_t v) {
    if (v <= (uint32_t)INT32_MAX) {
        return (int32_t)v;
    }
    return -(int32_t)(UINT32_MAX - v) - 1;
}

static int all_zero(const unsigned char *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static uint64_t record_offset(uint64_t row_index) {
    return SANS_HEADER_SIZE + row_index * SANS_RECORD_SIZE;
}

static void encode_header(unsigned char out[SANS_HEADER_SIZE], uint64_t row_count,
                          int64_t next_id) {
    memset(out, 0, SANS_HEADER_SIZE);
    memcpy(out, SANS_MAGIC, 4);
    put_u16le(out + 4, SANS_VERSION);
    put_u16le(out + 6, SANS_HEADER_SIZE);
    put_u32le(out + 8, SANS_RECORD_SIZE);
    put_u64le(out + 16, row_count);
    put_u64le(out + 24, (uint64_t)next_id);
}

/* 헤더 검사(5.1절 4단계) 중 파일 크기와 관계없는 것만 해요. */
static DbStatus decode_header(const unsigned char in[SANS_HEADER_SIZE],
                              uint64_t *row_count, int64_t *next_id) {
    if (memcmp(in, SANS_MAGIC, 4) != 0 || get_u16le(in + 4) != SANS_VERSION ||
        get_u16le(in + 6) != SANS_HEADER_SIZE ||
        get_u32le(in + 8) != SANS_RECORD_SIZE || !all_zero(in + 12, 4) ||
        !all_zero(in + 32, 32)) {
        return DB_ERR_CORRUPT;
    }

    uint64_t count = get_u64le(in + 16);
    int64_t id = u64_to_i64(get_u64le(in + 24));
    if (count > SANS_MAX_ROWS || id < 1 || (uint64_t)id != count + 1) {
        return DB_ERR_CORRUPT;
    }

    *row_count = count;
    *next_id = id;
    return DB_OK;
}

static void encode_record(unsigned char out[SANS_RECORD_SIZE], int64_t id,
                          const char *name, size_t name_len, int32_t age) {
    memset(out, 0, SANS_RECORD_SIZE);
    put_u64le(out, (uint64_t)id);
    put_u32le(out + 8, (uint32_t)age);
    out[12] = (unsigned char)name_len;
    memcpy(out + 16, name, name_len);
}

/* 레코드 검사(5.3절 4단계)와 Row 복원이에요. */
static DbStatus decode_record(const unsigned char in[SANS_RECORD_SIZE],
                              uint64_t row_index, Row *out) {
    int64_t id = u64_to_i64(get_u64le(in));
    size_t name_len = in[12];

    if (id < 1 || (uint64_t)id != row_index + 1 || name_len > USERS_NAME_MAX ||
        !all_zero(in + 13, 3) ||
        !all_zero(in + 16 + name_len, USERS_NAME_MAX - name_len) ||
        memchr(in + 16, 0, name_len) != NULL) {
        return DB_ERR_CORRUPT;
    }

    memset(out, 0, sizeof(*out));
    out->id = id;
    out->age = u32_to_i32(get_u32le(in + 8));
    memcpy(out->name, in + 16, name_len);
    out->name[name_len] = '\0';
    out->row_index = row_index;
    return DB_OK;
}

/* ---------- 파일 위치 (환경별 차이는 이 두 함수에만 둬요) ---------- */

/* offset은 호출 전에 int64 범위 안으로 확인돼 있어요. 성공 1, 실패 0. */
static int seek64(FILE *fp, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(fp, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(fp, (off_t)offset, SEEK_SET) == 0;
#endif
}

/* 파일 끝으로 이동해 크기를 재요. 성공 1, 실패 0. */
static int file_size64(FILE *fp, uint64_t *out_size) {
#ifdef _WIN32
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        return 0;
    }
    __int64 pos = _ftelli64(fp);
#else
    if (fseeko(fp, 0, SEEK_END) != 0) {
        return 0;
    }
    off_t pos = ftello(fp);
#endif
    if (pos < 0) {
        return 0;
    }
    *out_size = (uint64_t)pos;
    return 1;
}

/* ---------- 읽기·쓰기 (매번 위치부터 잡아요) ---------- */

/* 위치 이동 → fwrite → fflush. 셋 다 성공하면 1, 하나라도 실패하면 0. */
static int write_at(FILE *fp, uint64_t offset, const void *buf, size_t len) {
    if (!seek64(fp, offset)) {
        return 0;
    }
    if (fwrite(buf, 1, len, fp) != len) {
        return 0;
    }
    return fflush(fp) == 0;
}

/* 위치 이동 → fread. 위치 이동 실패나 읽기 오류면 SIZE_MAX, 아니면 읽은
 * 바이트 수예요(파일 끝이면 len보다 작아요). */
static size_t read_at(FILE *fp, uint64_t offset, void *buf, size_t len) {
    if (!seek64(fp, offset)) {
        return SIZE_MAX;
    }
    size_t got = fread(buf, 1, len, fp);
    if (got < len && ferror(fp)) {
        clearerr(fp);
        return SIZE_MAX;
    }
    return got;
}

/* 새 파일·0바이트 파일 초기화용: 64바이트 헤더 전체를 써요. */
static int write_header(FILE *fp, uint64_t row_count, int64_t next_id) {
    unsigned char header[SANS_HEADER_SIZE];
    encode_header(header, row_count, next_id);
    return write_at(fp, 0, header, sizeof(header));
}

/* INSERT 5단계용: 16번 위치에 row_count·next_id 16바이트만 써요(5.2절).
 * 바뀌지 않는 헤더 필드는 건드리지 않아요. */
static int write_counts(FILE *fp, uint64_t row_count, int64_t next_id) {
    unsigned char counts[16];
    put_u64le(counts, row_count);
    put_u64le(counts + 8, (uint64_t)next_id);
    return write_at(fp, 16, counts, sizeof(counts));
}

/* ---------- 레코드 순회와 인덱스 ---------- */

/* 0번부터 total개 레코드를 묶음으로 읽어 검사한 행마다 on_row를 불러요.
 * only_id가 1 이상이면 그 id인 행만 넘기고 첫 일치에서 멈춰요(인덱스를 못 쓸
 * 때의 id 조회). *out_count는 on_row가 DB_OK를 돌려준 횟수예요. 묶음마다 위치를
 * 다시 잡으므로 콜백이 파일 위치를 옮겨도 다음 묶음은 제자리에서 읽어요. */
static DbStatus scan_records(Storage *storage, uint64_t total, int64_t only_id,
                             RowHandler on_row, void *user_data,
                             size_t *out_count) {
    unsigned char buf[SCAN_BATCH * SANS_RECORD_SIZE];
    uint64_t next = 0;

    while (next < total) {
        uint64_t left = total - next;
        size_t batch = left < SCAN_BATCH ? (size_t)left : SCAN_BATCH;
        size_t want = batch * SANS_RECORD_SIZE;

        size_t got = read_at(storage->fp, record_offset(next), buf, want);
        if (got == SIZE_MAX) {
            return DB_ERR_IO;
        }
        if (got < want) {
            return DB_ERR_CORRUPT; /* 파일이 중간에 줄었어요 */
        }

        for (size_t i = 0; i < batch; i++) {
            Row row;
            DbStatus status =
                decode_record(buf + i * SANS_RECORD_SIZE, next + i, &row);
            if (status != DB_OK) {
                return status;
            }
            if (only_id > 0 && row.id != only_id) {
                continue;
            }
            status = on_row(&row, user_data);
            if (status != DB_OK) {
                return status;
            }
            (*out_count)++;
            if (only_id > 0) {
                return DB_OK;
            }
        }
        next += batch;
    }
    return DB_OK;
}

/* 재구축용 콜백: 검사를 통과한 행을 트리에 넣어요. 키는 오름차순이에요. */
static DbStatus index_row(const Row *row, void *user_data) {
    return bptree_insert(user_data, row->id, row->row_index);
}

/* 열기 마지막 단계(3.3절). DB_OK면 storage->index가 트리이거나, 레코드가
 * 손상돼 NULL("인덱스 안 씀")이에요. 손상은 열기 실패가 아니고 scan이 알려요.
 * 메모리 부족과 읽기 오류는 그대로 돌려줘요. */
static DbStatus build_index(Storage *storage) {
    BpTree *tree = NULL;
    if (bptree_create(&tree) != DB_OK) {
        return DB_ERR_NOMEM;
    }

    size_t count = 0;
    DbStatus status = scan_records(storage, storage->row_count, 0, index_row,
                                   tree, &count);
    if (status == DB_OK) {
        storage->index = tree;
        return DB_OK;
    }
    bptree_destroy(tree);
    return status == DB_ERR_CORRUPT ? DB_OK : status;
}

/* ---------- 공개 함수 ---------- */

DbStatus storage_open(const char *path, Storage **out_storage) {
    assert(path != NULL && out_storage != NULL);
    *out_storage = NULL;

    errno = 0;
    FILE *fp = fopen(path, "rb+");
    if (fp == NULL) {
        if (errno != ENOENT) {
            return DB_ERR_IO;
        }
        fp = fopen(path, "wb+");
        if (fp == NULL) {
            return DB_ERR_IO;
        }
        if (!write_header(fp, 0, 1)) {
            fclose(fp);
            remove(path);
            return DB_ERR_IO;
        }
    }

    DbStatus status = DB_OK;
    uint64_t size = 0;
    uint64_t row_count = 0;
    int64_t next_id = 1;

    if (!file_size64(fp, &size)) {
        status = DB_ERR_IO;
    } else if (size == 0) {
        /* 헤더를 쓰기 전에 프로그램이 죽은 파일이에요. 새 파일처럼 시작해요. */
        if (!write_header(fp, 0, 1)) {
            status = DB_ERR_IO;
        }
    } else if (size < SANS_HEADER_SIZE) {
        status = DB_ERR_CORRUPT;
    } else {
        unsigned char header[SANS_HEADER_SIZE];
        size_t got = read_at(fp, 0, header, sizeof(header));
        if (got == SIZE_MAX) {
            status = DB_ERR_IO;
        } else if (got < sizeof(header)) {
            status = DB_ERR_CORRUPT;
        } else {
            status = decode_header(header, &row_count, &next_id);
            /* 남는 바이트는 무시하고, 모자라면 손상이에요. */
            if (status == DB_OK && size < record_offset(row_count)) {
                status = DB_ERR_CORRUPT;
            }
        }
    }

    if (status != DB_OK) {
        fclose(fp);
        return status;
    }

    Storage *storage = malloc(sizeof(*storage));
    if (storage == NULL) {
        fclose(fp);
        return DB_ERR_NOMEM;
    }
    storage->fp = fp;
    storage->row_count = row_count;
    storage->next_id = next_id;
    storage->broken = 0;
    storage->index = NULL;

    status = build_index(storage);
    if (status != DB_OK) {
        fclose(fp);
        free(storage);
        return status;
    }
    *out_storage = storage;
    return DB_OK;
}

DbStatus storage_insert(Storage *storage, const char *name, int64_t age,
                        InsertResult *out_result) {
    assert(storage != NULL && name != NULL && out_result != NULL);
    memset(out_result, 0, sizeof(*out_result));
    if (storage->broken) {
        return DB_ERR_WRITE_UNCERTAIN;
    }

    size_t name_len = strnlen(name, USERS_NAME_MAX + 1);
    /* next_id == row_count + 1이라 행 수 한도가 ID 소진도 함께 막아요. */
    if (name_len > USERS_NAME_MAX || age < INT32_MIN || age > INT32_MAX ||
        storage->row_count >= SANS_MAX_ROWS || storage->next_id == INT64_MAX) {
        return DB_ERR_RANGE;
    }

    int64_t id = storage->next_id;
    uint64_t row_index = storage->row_count;
    unsigned char record[SANS_RECORD_SIZE];
    encode_record(record, id, name, name_len, (int32_t)age);

    /* 4단계: 헤더가 그대로라 실패하면 저장되지 않은 것이 확실해요. */
    if (!write_at(storage->fp, record_offset(row_index), record, sizeof(record))) {
        clearerr(storage->fp);
        return DB_ERR_IO;
    }

    /* 5단계: 실패하면 헤더가 바뀌었는지 알 수 없어요. 다시 시도하지 않아요. */
    if (!write_counts(storage->fp, row_index + 1, id + 1)) {
        storage->broken = 1;
        return DB_ERR_WRITE_UNCERTAIN;
    }

    storage->row_count = row_index + 1;
    storage->next_id = id + 1;

    /* 7단계: 행은 이미 저장됐어요. 트리에 못 넣으면 인덱스를 버리고 이후 id
     * 조회는 전체 순회로 찾아요. 결과는 늘 맞으므로 INSERT는 성공이에요. */
    if (storage->index != NULL &&
        bptree_insert(storage->index, id, row_index) != DB_OK) {
        bptree_destroy(storage->index);
        storage->index = NULL;
    }

    out_result->id = id;
    out_result->row_index = row_index;
    return DB_OK;
}

DbStatus storage_scan(Storage *storage, RowHandler on_row, void *user_data,
                      size_t *out_count) {
    assert(storage != NULL && on_row != NULL && out_count != NULL);
    *out_count = 0;
    if (storage->broken) {
        return DB_ERR_WRITE_UNCERTAIN;
    }

    /* 시작할 때의 row_count까지만 읽어요. */
    return scan_records(storage, storage->row_count, 0, on_row, user_data,
                        out_count);
}

DbStatus storage_find_by_id(Storage *storage, int64_t id, RowHandler on_row,
                            void *user_data, size_t *out_count) {
    assert(storage != NULL && on_row != NULL && out_count != NULL);
    *out_count = 0;
    if (storage->broken) {
        return DB_ERR_WRITE_UNCERTAIN;
    }
    if (id < 1) {
        return DB_OK;
    }
    if (storage->index == NULL) {
        return scan_records(storage, storage->row_count, id, on_row, user_data,
                            out_count);
    }

    uint64_t row_index = 0;
    if (!bptree_find(storage->index, id, &row_index)) {
        return DB_OK;
    }
    assert(row_index < storage->row_count);

    unsigned char record[SANS_RECORD_SIZE];
    size_t got = read_at(storage->fp, record_offset(row_index), record,
                         sizeof(record));
    if (got == SIZE_MAX) {
        return DB_ERR_IO;
    }
    if (got < sizeof(record)) {
        return DB_ERR_CORRUPT; /* 파일이 열린 뒤 줄었어요 */
    }

    Row row;
    DbStatus status = decode_record(record, row_index, &row);
    if (status != DB_OK) {
        return status;
    }
    assert(row.id == id);
    status = on_row(&row, user_data);
    if (status != DB_OK) {
        return status;
    }
    *out_count = 1;
    return DB_OK;
}

DbStatus storage_close(Storage *storage) {
    if (storage == NULL) {
        return DB_OK;
    }
    /* 실패해도 정리는 끝까지 하고, 다시 닫지 않아요. */
    DbStatus status = fclose(storage->fp) == 0 ? DB_OK : DB_ERR_IO;
    bptree_destroy(storage->index);
    free(storage);
    return status;
}

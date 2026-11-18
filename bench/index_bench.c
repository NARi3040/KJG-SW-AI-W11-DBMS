/*
 * index_bench.c - id 인덱스 성능 측정 (docs/phase2-3.md 3.5절, 이슈 #20)
 *
 * 저장 API를 직접 불러 SQL 처리기를 거치지 않아요. 열기(인덱스 구축)와 조회를
 * 따로 재요. 비교군:
 *   ① id + 인덱스   storage_find_by_id()
 *   ② id 선형       storage_scan()으로 모든 행의 id 비교
 *   ③ name 선형     storage_scan()으로 모든 행의 name 비교 (명세가 요구하는 비교)
 * ①과 ②·③이 넘긴 행이 하나라도 다르면 불일치로 세고 종료 코드 1이에요.
 *
 * 트리 노드 수와 메모리를 세려고 storage.c와 bptree.c를 이 파일에 포함해 한
 * 번역 단위로 빌드해요. 측정 대상 코드는 그대로이고, 공개 헤더에 통계 함수를
 * 더하지 않아요(bptree.h 주석).
 *
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -I. -pthread \
 *       bench/index_bench.c -o index_bench
 *   ./index_bench bench.sans [행 수=1000000] [조회 수=1000] [시드=20] [②=1|0]
 *
 * 파일이 없으면 행 수만큼 INSERT해서 만들고(삽입 시간도 출력), 있으면 이 프로그램이
 * 만든 형식(id i의 이름이 user<i>, 나이 i % 100)인지 확인한 뒤 그대로 써요.
 * -pthread는 MinGW-w64의 clock_gettime(winpthreads) 때문이고 Linux에서는 영향이
 * 없어요. 측정 도중 저장 API·닫기·시간 읽기 오류가 나면 시간표를 내지 않고 종료
 * 코드 1이에요. 호출은 성공했는데 결과가 다르면 표와 함께 불일치 수를 내고 1이에요.
 */
#include "../storage.c"
#include "../bptree.c"

#include <inttypes.h>
#include <time.h>

#define OPEN_REPEAT 5
#define ABSENT_COUNT 4
#define MAX_QUERIES 1000000u /* 조회 배열 4개를 합쳐 32MB 이하 */

/* ---------- 시간과 통계 ---------- */

/* 시간을 못 읽으면 측정값을 믿을 수 없어서 바로 끝내요. */
static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "clock_gettime 실패\n");
        exit(1);
    }
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    double mean_ns;
    double median_ns;
    uint64_t min_ns;
    uint64_t max_ns;
} Stats;

/* samples를 정렬해요. */
static Stats summarize(uint64_t *samples, size_t n) {
    Stats s = {0, 0, 0, 0};
    if (n == 0) {
        return s;
    }
    qsort(samples, n, sizeof(samples[0]), cmp_u64);
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)samples[i];
    }
    s.mean_ns = sum / (double)n;
    s.median_ns = n % 2 ? (double)samples[n / 2]
                        : ((double)samples[n / 2 - 1] + (double)samples[n / 2]) / 2;
    s.min_ns = samples[0];
    s.max_ns = samples[n - 1];
    return s;
}

/* 단위를 골라 "12.3us"처럼 써요. */
static void fmt_time(char *out, size_t cap, double ns) {
    if (ns < 1e3) {
        snprintf(out, cap, "%.0fns", ns);
    } else if (ns < 1e6) {
        snprintf(out, cap, "%.2fus", ns / 1e3);
    } else if (ns < 1e9) {
        snprintf(out, cap, "%.2fms", ns / 1e6);
    } else {
        snprintf(out, cap, "%.3fs", ns / 1e9);
    }
}

static void print_stats(const char *label, Stats s) {
    char mean[32], median[32], lo[32], hi[32];
    fmt_time(mean, sizeof(mean), s.mean_ns);
    fmt_time(median, sizeof(median), s.median_ns);
    fmt_time(lo, sizeof(lo), (double)s.min_ns);
    fmt_time(hi, sizeof(hi), (double)s.max_ns);
    printf("| %s | %s | %s | %s | %s |\n", label, mean, median, lo, hi);
}

/* splitmix64: 환경마다 같은 무작위 id를 뽑으려고 rand()를 쓰지 않아요. */
static uint64_t next_random(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15u);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9u;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBu;
    return z ^ (z >> 31);
}

/* ---------- 인자 ---------- */

/* 10진수 전체가 [lo, hi] 안의 숫자일 때만 1이에요. 부호, 빈 문자열, 뒤에 붙은
 * 글자, 범위 초과는 거절해요(strtoull은 "-1"과 "12x"를 받아들여서요). */
static int parse_u64(const char *text, uint64_t lo, uint64_t hi, uint64_t *out) {
    if (text[0] < '0' || text[0] > '9') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

/* ---------- 콜백 ---------- */

typedef struct {
    int64_t want_id;           /* ② 비교할 id */
    const char *want_name;     /* ③ 비교할 이름 */
    size_t matched;
    Row row;                   /* 마지막 일치 행 */
} Probe;

static DbStatus copy_row(const Row *row, void *user_data) {
    Probe *p = user_data;
    p->matched++;
    p->row = *row;
    return DB_OK;
}

static DbStatus match_id(const Row *row, void *user_data) {
    Probe *p = user_data;
    if (row->id == p->want_id) {
        p->matched++;
        p->row = *row;
    }
    return DB_OK;
}

static DbStatus match_name(const Row *row, void *user_data) {
    Probe *p = user_data;
    if (strcmp(row->name, p->want_name) == 0) {
        p->matched++;
        p->row = *row;
    }
    return DB_OK;
}

static DbStatus ignore_row(const Row *row, void *user_data) {
    (void)row;
    (void)user_data;
    return DB_OK;
}

static void make_name(char *out, size_t cap, int64_t id) {
    snprintf(out, cap, "user%" PRId64, id);
}

/* 재사용 파일 검사: prepare()가 만든 형식이 아니면 첫 행에서 멈춰요. */
static DbStatus check_bench_row(const Row *row, void *user_data) {
    (void)user_data;
    char name[32];
    make_name(name, sizeof(name), row->id);
    return strcmp(row->name, name) == 0 && row->age == (int32_t)(row->id % 100)
               ? DB_OK
               : DB_ERR_SCHEMA;
}

static int same_row(const Row *a, const Row *b) {
    return a->id == b->id && a->age == b->age && a->row_index == b->row_index &&
           strcmp(a->name, b->name) == 0;
}

/* ---------- 트리 통계 ---------- */

typedef struct {
    size_t nodes;
    size_t leaves;
    size_t keys; /* 리프 키 수 */
    size_t height;
} TreeShape;

static void walk(const BpNode *node, size_t depth, TreeShape *shape) {
    shape->nodes++;
    if (depth > shape->height) {
        shape->height = depth;
    }
    if (node->leaf) {
        shape->leaves++;
        shape->keys += node->count;
        return;
    }
    for (size_t i = 0; i <= node->count; i++) {
        walk(node->data.children[i], depth + 1, shape);
    }
}

/* ---------- 측정 단계 ---------- */

static const char *status_name(DbStatus s) {
    static const char *names[] = {"DB_OK", "DB_END", "DB_ERR_SYNTAX", "DB_ERR_ARG",
                                  "DB_ERR_SCHEMA", "DB_ERR_TYPE", "DB_ERR_RANGE",
                                  "DB_ERR_NOMEM", "DB_ERR_IO", "DB_ERR_CORRUPT",
                                  "DB_ERR_OUTPUT", "DB_ERR_WRITE_UNCERTAIN"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

/* 닫기 실패(DB_ERR_IO)도 측정 실패로 봐요. 핸들은 성공·실패 모두 해제돼요. */
static int close_ok(Storage *s, const char *when) {
    DbStatus st = storage_close(s);
    if (st != DB_OK) {
        fprintf(stderr, "%s 닫기 실패: %s\n", when, status_name(st));
        return 0;
    }
    return 1;
}

/* 파일이 있으면 행 수와 형식을 확인하고, 없으면 rows건을 넣어 만들어요. 실패하면 0. */
static int prepare(const char *path, uint64_t rows) {
    FILE *probe = fopen(path, "rb");
    if (probe != NULL) {
        fclose(probe);
        Storage *s = NULL;
        DbStatus st = storage_open(path, &s);
        if (st != DB_OK) {
            fprintf(stderr, "기존 파일 열기 실패: %s\n", status_name(st));
            return 0;
        }
        if (s->row_count != rows) {
            fprintf(stderr, "기존 파일 행 수 %" PRIu64 " != 요청 %" PRIu64
                            ". 파일을 지우고 다시 실행하세요\n",
                    s->row_count, rows);
            close_ok(s, "기존 파일");
            return 0;
        }
        size_t count = 0;
        st = storage_scan(s, check_bench_row, NULL, &count);
        if (st != DB_OK) {
            fprintf(stderr, "기존 파일이 측정용 형식(이름 user<id>, 나이 id %% 100)이 "
                            "아니에요(%s, %zu행째). 새 경로로 다시 실행하세요\n",
                    status_name(st), count + 1);
            close_ok(s, "기존 파일");
            return 0;
        }
        if (!close_ok(s, "기존 파일")) {
            return 0;
        }
        printf("데이터: 기존 파일 재사용 (%" PRIu64 "행, 형식 확인함)\n", rows);
        return 1;
    }

    Storage *s = NULL;
    DbStatus st = storage_open(path, &s);
    if (st != DB_OK) {
        fprintf(stderr, "새 파일 만들기 실패: %s\n", status_name(st));
        return 0;
    }
    /* 존재 확인과 열기 사이에 다른 프로세스가 만든 파일이면 건드리지 않아요. */
    if (s->row_count != 0) {
        fprintf(stderr, "새 파일이어야 하는데 이미 %" PRIu64 "행이 있어요. "
                        "INSERT하지 않고 끝내요\n",
                s->row_count);
        close_ok(s, "새 파일");
        return 0;
    }
    uint64_t start = now_ns();
    for (uint64_t i = 1; i <= rows; i++) {
        char name[32];
        InsertResult r;
        make_name(name, sizeof(name), (int64_t)i);
        st = storage_insert(s, name, (int64_t)(i % 100), &r);
        if (st != DB_OK || r.id != (int64_t)i) {
            fprintf(stderr, "INSERT %" PRIu64 " 실패: %s\n", i, status_name(st));
            close_ok(s, "새 파일");
            return 0;
        }
    }
    uint64_t elapsed = now_ns() - start;
    if (!close_ok(s, "새 파일")) {
        return 0;
    }
    char t[32];
    fmt_time(t, sizeof(t), (double)elapsed);
    printf("데이터: %" PRIu64 "행 INSERT %s (행당 %.2fus, INSERT마다 레코드·헤더 flush)\n",
           rows, t, (double)elapsed / 1e3 / (double)rows);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 6) {
        fprintf(stderr, "usage: %s <file.sans> [rows=1000000] [queries=1000] "
                        "[seed=20] [linear_id=1|0]\n",
                argv[0]);
        return 2;
    }
    const char *path = argv[1];
    uint64_t rows = 1000000u;
    uint64_t queries64 = 1000u;
    uint64_t seed = 20u;
    uint64_t linear = 1u;
    if ((argc > 2 && !parse_u64(argv[2], 1, SANS_MAX_ROWS, &rows)) ||
        (argc > 3 && !parse_u64(argv[3], 1, MAX_QUERIES, &queries64)) ||
        (argc > 4 && !parse_u64(argv[4], 0, UINT64_MAX, &seed)) ||
        (argc > 5 && !parse_u64(argv[5], 0, 1, &linear))) {
        fprintf(stderr, "인자 오류: rows는 1~%" PRIu64 ", queries는 1~%u, seed는 0 이상 "
                        "정수, linear_id는 0 또는 1이에요\n",
                (uint64_t)SANS_MAX_ROWS, MAX_QUERIES);
        return 2;
    }
    const size_t queries = (size_t)queries64;
    const int run_linear_id = linear == 1;

    int rc = 1;
    Storage *s = NULL;
    BpTree *tree = NULL;
    int64_t *ids = NULL;
    uint64_t *t_index = NULL, *t_lin_id = NULL, *t_lin_name = NULL;

    printf("# index_bench\n");
    printf("빌드: gcc %s, BPTREE_ORDER=%d, sizeof(BpNode)=%zu, %s\n", __VERSION__,
           BPTREE_ORDER, sizeof(BpNode),
#ifdef NDEBUG
           "NDEBUG"
#else
           "assert 켜짐"
#endif
    );
    printf("조건: 행 %" PRIu64 ", 무작위 id %zu개, 시드 %" PRIu64 "\n", rows, queries, seed);

    if (!prepare(path, rows)) {
        goto done;
    }

    /* 1. 열기(인덱스 구축 포함)와 전체 순회 1번 비용, 파일 없이 트리만 만들기 */
    uint64_t open_ns[OPEN_REPEAT], scan_ns[OPEN_REPEAT], build_ns[OPEN_REPEAT];
    for (int i = 0; i < OPEN_REPEAT; i++) {
        uint64_t t0 = now_ns();
        DbStatus st = storage_open(path, &s);
        open_ns[i] = now_ns() - t0;
        if (st != DB_OK || s->index == NULL) {
            fprintf(stderr, "열기 실패 또는 인덱스 없음: %s\n", status_name(st));
            goto done;
        }
        size_t count = 0;
        t0 = now_ns();
        st = storage_scan(s, ignore_row, NULL, &count);
        scan_ns[i] = now_ns() - t0;
        if (st != DB_OK || count != rows) {
            fprintf(stderr, "scan 실패: %s\n", status_name(st));
            goto done;
        }
        int closed = close_ok(s, "측정");
        s = NULL;
        if (!closed) {
            goto done;
        }

        if (bptree_create(&tree) != DB_OK) {
            fprintf(stderr, "트리 만들기 메모리 부족\n");
            goto done;
        }
        t0 = now_ns();
        for (uint64_t k = 1; k <= rows; k++) {
            if (bptree_insert(tree, (int64_t)k, k - 1) != DB_OK) {
                fprintf(stderr, "트리 넣기 메모리 부족\n");
                goto done;
            }
        }
        build_ns[i] = now_ns() - t0;
        bptree_destroy(tree);
        tree = NULL;
    }

    DbStatus st = storage_open(path, &s);
    if (st != DB_OK || s->index == NULL) {
        fprintf(stderr, "열기 실패 또는 인덱스 없음: %s\n", status_name(st));
        goto done;
    }

    /* 2. 인덱스 모양과 메모리 */
    TreeShape shape = {0, 0, 0, 0};
    if (s->index->root != NULL) {
        walk(s->index->root, 1, &shape);
    }
    size_t bytes = shape.nodes * sizeof(BpNode) + sizeof(BpTree);
    int tree_ok = bptree_check(s->index);
    printf("\n## 인덱스\n\n");
    printf("| 항목 | 값 |\n| --- | --- |\n");
    printf("| 높이 | %zu |\n", shape.height);
    printf("| 노드 수 (리프) | %zu (%zu) |\n", shape.nodes, shape.leaves);
    printf("| 리프 키 수 | %zu |\n", shape.keys);
    printf("| 리프 채움률 | %.1f%% |\n",
           shape.leaves ? 100.0 * (double)shape.keys /
                              ((double)shape.leaves * (BPTREE_ORDER - 1))
                        : 0.0);
    printf("| 노드 메모리 (malloc 부가 비용 제외) | %.1fMiB |\n", (double)bytes / 1048576.0);
    printf("| bptree_check | %s |\n", tree_ok ? "통과" : "실패");

    /* 3. 조회. queries ≤ MAX_QUERIES라 크기 계산이 넘치지 않아요. */
    ids = malloc(queries * sizeof(*ids));
    t_index = malloc(queries * sizeof(*t_index));
    t_lin_id = malloc(queries * sizeof(*t_lin_id));
    t_lin_name = malloc(queries * sizeof(*t_lin_name));
    if (ids == NULL || t_index == NULL || t_lin_id == NULL || t_lin_name == NULL) {
        fprintf(stderr, "메모리 부족\n");
        goto done;
    }
    uint64_t rng = seed;
    for (size_t i = 0; i < queries; i++) {
        ids[i] = (int64_t)(next_random(&rng) % rows) + 1;
    }

    size_t mismatches = 0;
    size_t lin_id_runs = 0;
    for (size_t i = 0; i < queries; i++) {
        char name[32];
        make_name(name, sizeof(name), ids[i]);
        Probe a = {ids[i], name, 0, {0}};
        Probe b = a;
        Probe c = a;
        size_t count = 0;

        /* 저장 API 오류는 측정 실패라 표 없이 끝내요. 호출은 성공했는데 결과가
         * 다르면 불일치로 세요. */
        uint64_t t0 = now_ns();
        st = storage_find_by_id(s, ids[i], copy_row, &a, &count);
        t_index[i] = now_ns() - t0;
        if (st != DB_OK) {
            fprintf(stderr, "① id %" PRId64 " 조회 실패: %s\n", ids[i], status_name(st));
            goto done;
        }
        if (a.matched != 1) {
            mismatches++;
        }

        if (run_linear_id) {
            t0 = now_ns();
            st = storage_scan(s, match_id, &b, &count);
            t_lin_id[i] = now_ns() - t0;
            lin_id_runs++;
            if (st != DB_OK) {
                fprintf(stderr, "② id %" PRId64 " 순회 실패: %s\n", ids[i], status_name(st));
                goto done;
            }
            if (b.matched != 1 || !same_row(&a.row, &b.row)) {
                mismatches++;
            }
        }

        t0 = now_ns();
        st = storage_scan(s, match_name, &c, &count);
        t_lin_name[i] = now_ns() - t0;
        if (st != DB_OK) {
            fprintf(stderr, "③ %s 순회 실패: %s\n", name, status_name(st));
            goto done;
        }
        if (c.matched != 1 || !same_row(&a.row, &c.row)) {
            mismatches++;
        }
    }

    /* 없는 id: 인덱스와 선형 모두 0행 */
    const int64_t absent[ABSENT_COUNT] = {0, -1, (int64_t)rows + 1, INT64_MAX};
    for (size_t i = 0; i < ABSENT_COUNT; i++) {
        Probe a = {absent[i], "", 0, {0}};
        Probe b = a;
        size_t count = 0;
        st = storage_find_by_id(s, absent[i], copy_row, &a, &count);
        if (st == DB_OK) {
            st = storage_scan(s, match_id, &b, &count);
        }
        if (st != DB_OK) {
            fprintf(stderr, "없는 id %" PRId64 " 조회 실패: %s\n", absent[i], status_name(st));
            goto done;
        }
        if (a.matched != 0 || b.matched != 0) {
            mismatches++;
        }
    }
    int closed = close_ok(s, "조회");
    s = NULL;
    if (!closed) {
        goto done;
    }

    printf("\n## 시간 (%d번 반복: 열기·순회·트리 / %zu개: 조회)\n\n", OPEN_REPEAT, queries);
    printf("| 항목 | 평균 | 중앙값 | 최소 | 최대 |\n");
    printf("| --- | --- | --- | --- | --- |\n");
    print_stats("열기 (헤더+읽기+트리 구축)", summarize(open_ns, OPEN_REPEAT));
    print_stats("전체 순회 1번 (scan)", summarize(scan_ns, OPEN_REPEAT));
    print_stats("트리만 만들기 (메모리)", summarize(build_ns, OPEN_REPEAT));
    print_stats("① id + 인덱스", summarize(t_index, queries));
    if (lin_id_runs > 0) {
        print_stats("② id 선형", summarize(t_lin_id, lin_id_runs));
    }
    print_stats("③ name 선형", summarize(t_lin_name, queries));

    printf("\n불일치: %zu (무작위 id %zu개 + 없는 id %d개: 0, -1, %" PRIu64 "+1, INT64_MAX)\n",
           mismatches, queries, ABSENT_COUNT, rows);
    rc = mismatches == 0 && tree_ok ? 0 : 1;

done:
    if (s != NULL && !close_ok(s, "정리")) {
        rc = 1;
    }
    bptree_destroy(tree);
    free(ids);
    free(t_index);
    free(t_lin_id);
    free(t_lin_name);
    /* 표를 파일로 돌렸을 때 쓰기 실패(디스크 가득 등)도 실패로 알려요. */
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fprintf(stderr, "표준 출력 쓰기 실패\n");
        rc = 1;
    }
    return rc;
}

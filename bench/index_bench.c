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
 * 파일이 없으면 행 수만큼 INSERT해서 만들고(삽입 시간도 출력), 있으면 행 수가
 * 같은지 확인한 뒤 그대로 써요. -pthread는 MinGW-w64의 clock_gettime(winpthreads)
 * 때문이고 Linux에서는 영향이 없어요.
 */
#include "../storage.c"
#include "../bptree.c"

#include <inttypes.h>
#include <time.h>

#define OPEN_REPEAT 5
#define ABSENT_COUNT 4

/* ---------- 시간과 통계 ---------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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

static void make_name(char *out, size_t cap, int64_t id) {
    snprintf(out, cap, "user%" PRId64, id);
}

/* 파일이 없으면 rows건을 넣어 만들고, 있으면 행 수를 확인해요. 실패하면 0. */
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
        uint64_t have = s->row_count;
        storage_close(s);
        if (have != rows) {
            fprintf(stderr, "기존 파일 행 수 %" PRIu64 " != 요청 %" PRIu64
                            ". 파일을 지우고 다시 실행하세요\n",
                    have, rows);
            return 0;
        }
        printf("데이터: 기존 파일 재사용 (%" PRIu64 "행)\n", rows);
        return 1;
    }

    Storage *s = NULL;
    DbStatus st = storage_open(path, &s);
    if (st != DB_OK) {
        fprintf(stderr, "새 파일 만들기 실패: %s\n", status_name(st));
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
            storage_close(s);
            return 0;
        }
    }
    uint64_t elapsed = now_ns() - start;
    storage_close(s);
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
    uint64_t rows = argc > 2 ? strtoull(argv[2], NULL, 10) : 1000000u;
    size_t queries = argc > 3 ? (size_t)strtoull(argv[3], NULL, 10) : 1000u;
    uint64_t seed = argc > 4 ? strtoull(argv[4], NULL, 10) : 20u;
    int run_linear_id = argc > 5 ? atoi(argv[5]) != 0 : 1;
    if (rows == 0 || queries == 0) {
        fprintf(stderr, "rows와 queries는 1 이상이어야 해요\n");
        return 2;
    }

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
        return 1;
    }

    /* 1. 열기(인덱스 구축 포함)와 전체 순회 1번 비용 */
    uint64_t open_ns[OPEN_REPEAT], scan_ns[OPEN_REPEAT], build_ns[OPEN_REPEAT];
    for (int i = 0; i < OPEN_REPEAT; i++) {
        Storage *s = NULL;
        uint64_t t0 = now_ns();
        DbStatus st = storage_open(path, &s);
        open_ns[i] = now_ns() - t0;
        if (st != DB_OK || s->index == NULL) {
            fprintf(stderr, "열기 실패 또는 인덱스 없음: %s\n", status_name(st));
            storage_close(s);
            return 1;
        }
        size_t count = 0;
        t0 = now_ns();
        st = storage_scan(s, ignore_row, NULL, &count);
        scan_ns[i] = now_ns() - t0;
        storage_close(s);
        if (st != DB_OK || count != rows) {
            fprintf(stderr, "scan 실패: %s\n", status_name(st));
            return 1;
        }

        /* 파일 없이 트리만 만들기 (키 오름차순, 열기와 같은 순서) */
        BpTree *tree = NULL;
        if (bptree_create(&tree) != DB_OK) {
            return 1;
        }
        t0 = now_ns();
        for (uint64_t k = 1; k <= rows; k++) {
            if (bptree_insert(tree, (int64_t)k, k - 1) != DB_OK) {
                fprintf(stderr, "트리 넣기 메모리 부족\n");
                return 1;
            }
        }
        build_ns[i] = now_ns() - t0;
        bptree_destroy(tree);
    }

    Storage *s = NULL;
    DbStatus st = storage_open(path, &s);
    if (st != DB_OK || s->index == NULL) {
        fprintf(stderr, "열기 실패: %s\n", status_name(st));
        return 1;
    }

    /* 2. 인덱스 모양과 메모리 */
    TreeShape shape = {0, 0, 0, 0};
    if (s->index->root != NULL) {
        walk(s->index->root, 1, &shape);
    }
    size_t bytes = shape.nodes * sizeof(BpNode) + sizeof(BpTree);
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
    int tree_ok = bptree_check(s->index);
    printf("| bptree_check | %s |\n", tree_ok ? "통과" : "실패");

    /* 3. 조회 */
    int64_t *ids = malloc(queries * sizeof(*ids));
    uint64_t *t_index = malloc(queries * sizeof(*t_index));
    uint64_t *t_lin_id = malloc(queries * sizeof(*t_lin_id));
    uint64_t *t_lin_name = malloc(queries * sizeof(*t_lin_name));
    if (ids == NULL || t_index == NULL || t_lin_id == NULL || t_lin_name == NULL) {
        fprintf(stderr, "메모리 부족\n");
        return 1;
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

        uint64_t t0 = now_ns();
        st = storage_find_by_id(s, ids[i], copy_row, &a, &count);
        t_index[i] = now_ns() - t0;
        if (st != DB_OK || a.matched != 1) {
            mismatches++;
        }

        if (run_linear_id) {
            t0 = now_ns();
            st = storage_scan(s, match_id, &b, &count);
            t_lin_id[i] = now_ns() - t0;
            lin_id_runs++;
            if (st != DB_OK || b.matched != 1 || !same_row(&a.row, &b.row)) {
                mismatches++;
            }
        }

        t0 = now_ns();
        st = storage_scan(s, match_name, &c, &count);
        t_lin_name[i] = now_ns() - t0;
        if (st != DB_OK || c.matched != 1 || !same_row(&a.row, &c.row)) {
            mismatches++;
        }
    }

    /* 없는 id: 인덱스와 선형 모두 0행 */
    const int64_t absent[ABSENT_COUNT] = {0, -1, (int64_t)rows + 1, INT64_MAX};
    for (size_t i = 0; i < ABSENT_COUNT; i++) {
        Probe a = {absent[i], "", 0, {0}};
        Probe b = a;
        size_t count = 0;
        if (storage_find_by_id(s, absent[i], copy_row, &a, &count) != DB_OK ||
            a.matched != 0) {
            mismatches++;
        }
        if (storage_scan(s, match_id, &b, &count) != DB_OK || b.matched != 0) {
            mismatches++;
        }
    }
    storage_close(s);

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

    free(ids);
    free(t_index);
    free(t_lin_id);
    free(t_lin_name);
    return mismatches == 0 && tree_ok ? 0 : 1;
}

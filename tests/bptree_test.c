/*
 * bptree_test.c - 메모리 B+ 트리 단위 테스트 (이슈 #18, docs/phase2-3.md 3.2·3.4)
 *
 * bptree.c의 테스트 전용 할당 훅을 쓰려고 BPTREE_TESTING을 정의한 뒤
 * bptree.c를 이 파일에 포함해요. bptree.c를 따로 링크하지 않아요.
 *
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -I. -DBPTREE_ORDER=4 \
 *       -o bptree_test_order4 tests/bptree_test.c
 *   gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -I. \
 *       -o bptree_test tests/bptree_test.c
 *
 * 오름차순 10만 개는 배열 대조와 bptree_check()로 확인해요. 처음 3,000개는
 * 넣을 때마다, 그 뒤로는 997개마다, 마지막에 한 번 검사해요(전체 순회가
 * O(n)이라 모든 넣기에서 부르면 이차 시간이 돼요). 무작위 순서 30만 개는
 * 차수 4·512 모두 내부 노드 분할을 지나요(512도 리프가 588개 이상이라
 * 512개 자식 제한을 넘어요). 할당 실패는 만들기, 빈 트리 첫 넣기, 리프
 * 분할, 내부 루트까지 번지는 분할에서 실패 인덱스를 모두 소진해요.
 * 실패한 넣기는 트리·검색 결과·할당 수를 바꾸지 않아야 해요. 체커가 손상된
 * 개수·키 순서·길잡이·리프 깊이·리프 연결을 거부하는지도 되돌리며 봐요.
 */
#define BPTREE_TESTING
#include "bptree.c"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define BPT_TEST_ASC_COUNT 100000
#define BPT_TEST_SHUFFLE_COUNT 300000
#define BPT_TEST_FAIL_LIMIT 400000
#define BPT_TEST_FAIL_CAP 64
#define BPT_TEST_SENTINEL UINT64_C(0xA5A5A5A5DEADBEEF)

static int bpt_test_checks = 0;
static int bpt_test_failures = 0;

#define BPT_CHECK(cond)                                                      \
    do {                                                                     \
        bpt_test_checks++;                                                   \
        if (!(cond)) {                                                       \
            bpt_test_failures++;                                             \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

/* 키마다 다른 값을 만들어 값이 뒤바뀐 경우를 잡아요. 홀수 배수라 단사예요. */
static uint64_t bpt_test_value(int64_t key) {
    return (uint64_t)key * UINT64_C(0x9E3779B97F4A7C15) +
           UINT64_C(0x0123456789ABCDEF);
}

/* first..last의 모든 키가 같은 값으로 남아 있는지 확인해요. */
static int bpt_test_verify_range(const BpTree *tree, int64_t first, int64_t last) {
    if (last < first) {
        return 1;
    }
    int64_t key = first;
    for (;;) {
        uint64_t out = 0;
        if (bptree_find(tree, key, &out) != 1 || out != bpt_test_value(key)) {
            fprintf(stderr, "  range mismatch at key=%" PRId64 "\n", key);
            return 0;
        }
        if (key == last) {
            break;
        }
        key++;
    }
    return 1;
}

/*
 * key를 넣는 데 필요한 할당을 하나씩 실패시켜요. fail_after=k는 k번의 성공
 * 할당을 허용하므로, k=0,1,...에서 NOMEM을 확인하고 처음 성공하는 k가
 * 필요한 할당 수예요. 실패할 때마다 트리 모양, 이전 키 검색, 할당 수가
 * 그대로인지 확인해요.
 */
static int bpt_test_exhaust_failures(BpTree *tree, int64_t key,
                                     int64_t first_key, size_t *out_required) {
    size_t live_before = bptree_test_live_allocations;
    uint64_t out = BPT_TEST_SENTINEL;
    size_t k = 0;
    int success = 0;

    int absent = (bptree_find(tree, key, &out) == 0);
    BPT_CHECK(absent);
    BPT_CHECK(out == BPT_TEST_SENTINEL);
    BPT_CHECK(bptree_check(tree) == 1);
    if (!absent) {
        return 0;
    }

    for (k = 0; k < BPT_TEST_FAIL_CAP; k++) {
        bptree_test_fail_after = k;
        DbStatus status = bptree_insert(tree, key, bpt_test_value(key));
        if (status == DB_OK) {
            success = 1;
            break;
        }
        BPT_CHECK(status == DB_ERR_NOMEM);
        if (status != DB_ERR_NOMEM) {
            bptree_test_fail_after = SIZE_MAX;
            return 0;
        }
        BPT_CHECK(bptree_test_live_allocations == live_before);
        if (bptree_test_live_allocations != live_before) {
            bptree_test_fail_after = SIZE_MAX;
            return 0;
        }

        bptree_test_fail_after = SIZE_MAX;
        out = BPT_TEST_SENTINEL;
        int still_absent = (bptree_find(tree, key, &out) == 0);
        BPT_CHECK(still_absent);
        BPT_CHECK(out == BPT_TEST_SENTINEL);
        int check_ok = bptree_check(tree);
        BPT_CHECK(check_ok == 1);
        int range_ok = bpt_test_verify_range(tree, first_key, key - 1);
        BPT_CHECK(range_ok);
        if (!still_absent || !check_ok || !range_ok) {
            return 0;
        }
    }

    bptree_test_fail_after = SIZE_MAX;
    BPT_CHECK(success == 1);
    if (!success) {
        return 0;
    }
    BPT_CHECK(k >= 1);
    BPT_CHECK(bptree_test_live_allocations > live_before);
    out = BPT_TEST_SENTINEL;
    BPT_CHECK(bptree_find(tree, key, &out) == 1);
    BPT_CHECK(out == bpt_test_value(key));
    BPT_CHECK(bptree_check(tree) == 1);
    BPT_CHECK(bpt_test_verify_range(tree, first_key, key));
    if (out_required != NULL) {
        *out_required = k;
    }
    return 1;
}

/*
 * first_key부터 오름차순으로 넣다가 min_alloc개 이상의 할당이 필요한
 * 분할을 처음 만나면 그 넣기에서 할당 실패를 모두 소진해요. min_alloc=1은
 * 첫 리프 분할(또는 그다음 리프 분할), 3은 내부 루트까지 번지는 분할이에요.
 * 찾기 전까지 넣은 키는 나중에 bpt_test_exhaust_failures가 전부 검증해요.
 */
static int bpt_test_find_split(BpTree *tree, int64_t first_key, int64_t limit,
                               size_t min_alloc, int64_t *out_key) {
    int64_t key = first_key;
    for (;;) {
        bptree_test_fail_after = 0;
        DbStatus status = bptree_insert(tree, key, bpt_test_value(key));
        if (status == DB_OK) {
            if (key == limit) {
                break;
            }
            key++;
            continue;
        }
        BPT_CHECK(status == DB_ERR_NOMEM);
        if (status != DB_ERR_NOMEM) {
            bptree_test_fail_after = SIZE_MAX;
            return 0;
        }

        bptree_test_fail_after = SIZE_MAX;
        uint64_t probe_out = BPT_TEST_SENTINEL;
        int probe_absent = (bptree_find(tree, key, &probe_out) == 0);
        BPT_CHECK(probe_absent);
        BPT_CHECK(probe_out == BPT_TEST_SENTINEL);
        if (!probe_absent) {
            return 0;
        }

        if (min_alloc > 1) {
            bptree_test_fail_after = min_alloc - 1;
            status = bptree_insert(tree, key, bpt_test_value(key));
            if (status == DB_OK) {
                if (key == limit) {
                    break;
                }
                key++;
                continue;
            }
            BPT_CHECK(status == DB_ERR_NOMEM);
            if (status != DB_ERR_NOMEM) {
                bptree_test_fail_after = SIZE_MAX;
                return 0;
            }
        }

        bptree_test_fail_after = SIZE_MAX;
        if (!bpt_test_exhaust_failures(tree, key, 1, NULL)) {
            return 0;
        }
        *out_key = key;
        return 1;
    }

    bptree_test_fail_after = SIZE_MAX;
    return 0;
}

/* 만들기 실패와 빈 트리 계약을 확인해요. */
static void bpt_test_create_and_empty(void) {
    size_t live_before = bptree_test_live_allocations;
    bptree_test_fail_after = SIZE_MAX;

    BpTree *tree = (BpTree *)(void *)&bpt_test_checks;
    bptree_test_fail_after = 0;
    BPT_CHECK(bptree_create(&tree) == DB_ERR_NOMEM);
    BPT_CHECK(tree == NULL);
    BPT_CHECK(bptree_test_live_allocations == live_before);

    tree = NULL;
    bptree_test_fail_after = 1;
    BPT_CHECK(bptree_create(&tree) == DB_OK);
    BPT_CHECK(tree != NULL);
    bptree_test_fail_after = SIZE_MAX;
    if (tree == NULL) {
        return;
    }

    BPT_CHECK(bptree_check(tree) == 1);
    uint64_t out = BPT_TEST_SENTINEL;
    BPT_CHECK(bptree_find(tree, 0, &out) == 0);
    BPT_CHECK(out == BPT_TEST_SENTINEL);

    bptree_destroy(tree);
    BPT_CHECK(bptree_test_live_allocations == 0);
    bptree_destroy(NULL);
    BPT_CHECK(bptree_test_live_allocations == 0);
}

/* 빈 트리의 첫 넣기(리프 하나 할당)가 실패해도 트리는 빈 채로 남아요. */
static void bpt_test_empty_insert(void) {
    bptree_test_fail_after = SIZE_MAX;

    BpTree *tree = NULL;
    DbStatus status = bptree_create(&tree);
    BPT_CHECK(status == DB_OK);
    if (status != DB_OK || tree == NULL) {
        return;
    }
    size_t live_before = bptree_test_live_allocations;

    uint64_t out = BPT_TEST_SENTINEL;
    bptree_test_fail_after = 0;
    BPT_CHECK(bptree_insert(tree, 42, bpt_test_value(42)) == DB_ERR_NOMEM);
    BPT_CHECK(bptree_test_live_allocations == live_before);
    bptree_test_fail_after = SIZE_MAX;

    BPT_CHECK(bptree_check(tree) == 1);
    BPT_CHECK(bptree_find(tree, 42, &out) == 0);
    BPT_CHECK(out == BPT_TEST_SENTINEL);

    BPT_CHECK(bptree_insert(tree, 42, bpt_test_value(42)) == DB_OK);
    out = BPT_TEST_SENTINEL;
    BPT_CHECK(bptree_find(tree, 42, &out) == 1);
    BPT_CHECK(out == bpt_test_value(42));
    BPT_CHECK(bptree_check(tree) == 1);

    bptree_destroy(tree);
    BPT_CHECK(bptree_test_live_allocations == 0);
}

/* 오름차순 10만 개: 배열 대조, 정해진 간격의 불변식 검사, 전체 값 검증. */
static void bpt_test_ascending(void) {
    BPT_CHECK(bptree_test_live_allocations == 0);
    bptree_test_fail_after = SIZE_MAX;

    uint64_t *oracle = malloc(BPT_TEST_ASC_COUNT * sizeof(*oracle));
    BPT_CHECK(oracle != NULL);
    if (oracle == NULL) {
        return;
    }

    BpTree *tree = NULL;
    DbStatus status = bptree_create(&tree);
    BPT_CHECK(status == DB_OK);
    if (status != DB_OK || tree == NULL) {
        free(oracle);
        return;
    }

    int ok = 1;
    for (size_t i = 0; i < BPT_TEST_ASC_COUNT; i++) {
        int64_t key = (int64_t)i + 1;
        oracle[i] = bpt_test_value(key);
        status = bptree_insert(tree, key, oracle[i]);
        BPT_CHECK(status == DB_OK);
        if (status != DB_OK) {
            ok = 0;
            break;
        }
        if (i < 3000 || ((i + 1) % 997 == 0)) {
            BPT_CHECK(bptree_check(tree) == 1);
        }
    }
    BPT_CHECK(bptree_check(tree) == 1);

    if (ok) {
        for (size_t i = 0; i < BPT_TEST_ASC_COUNT; i++) {
            int64_t key = (int64_t)i + 1;
            uint64_t out = BPT_TEST_SENTINEL;
            BPT_CHECK(bptree_find(tree, key, &out) == 1);
            BPT_CHECK(out == oracle[i]);
        }

        const int64_t absent[] = {
            0,
            -1,
            -42,
            INT64_MIN,
            INT64_MAX,
            (int64_t)BPT_TEST_ASC_COUNT + 1,
            (int64_t)BPT_TEST_ASC_COUNT + 100,
        };
        for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
            uint64_t out = BPT_TEST_SENTINEL;
            BPT_CHECK(bptree_find(tree, absent[i], &out) == 0);
            BPT_CHECK(out == BPT_TEST_SENTINEL);
        }

        uint64_t out = BPT_TEST_SENTINEL;
        BPT_CHECK(bptree_find(tree, 1, &out) == 1);
        BPT_CHECK(out == oracle[0]);
        out = BPT_TEST_SENTINEL;
        BPT_CHECK(bptree_find(tree, BPT_TEST_ASC_COUNT, &out) == 1);
        BPT_CHECK(out == oracle[BPT_TEST_ASC_COUNT - 1]);
    }

    bptree_destroy(tree);
    BPT_CHECK(bptree_test_live_allocations == 0);
    free(oracle);
}

/* 0·음수·int64 양 끝값을 실제 키로 넣고 찾아요. */
static void bpt_test_signed_extrema(void) {
    BPT_CHECK(bptree_test_live_allocations == 0);
    bptree_test_fail_after = SIZE_MAX;

    BpTree *tree = NULL;
    DbStatus status = bptree_create(&tree);
    BPT_CHECK(status == DB_OK);
    if (status != DB_OK || tree == NULL) {
        return;
    }

    const int64_t keys[] = {
        INT64_MAX,
        0,
        INT64_MIN,
        -1,
        INT64_MAX - 1,
        1,
        INT64_MIN + 1,
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        BPT_CHECK(bptree_insert(tree, keys[i], bpt_test_value(keys[i])) == DB_OK);
        BPT_CHECK(bptree_check(tree) == 1);
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        uint64_t out = BPT_TEST_SENTINEL;
        BPT_CHECK(bptree_find(tree, keys[i], &out) == 1);
        BPT_CHECK(out == bpt_test_value(keys[i]));
    }

    const int64_t absent[] = {
        INT64_MIN + 2,
        INT64_MAX - 2,
        -2,
        2,
    };
    for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        uint64_t out = BPT_TEST_SENTINEL;
        BPT_CHECK(bptree_find(tree, absent[i], &out) == 0);
        BPT_CHECK(out == BPT_TEST_SENTINEL);
    }

    bptree_destroy(tree);
    BPT_CHECK(bptree_test_live_allocations == 0);
}

/* 결정적인 xorshift64* 난수로 피셔-예이츠 섞기를 해요. */
static uint64_t bpt_test_rng_state = UINT64_C(0x243F6A8885A308D3);

static uint64_t bpt_test_rng_next(void) {
    uint64_t x = bpt_test_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    bpt_test_rng_state = x;
    return x * UINT64_C(2685821657736338717);
}

/*
 * 무작위 순서 30만 개. 차수 4에서는 내부 노드가 여러 단계로 갈라지고,
 * 512에서도 리프가 512개를 넘어 내부 루트가 갈라져요. 섞은 뒤에도 각 키의
 * 값이 그대로인지 전부 찾아 확인해요.
 */
static void bpt_test_shuffled(void) {
    BPT_CHECK(bptree_test_live_allocations == 0);
    bptree_test_fail_after = SIZE_MAX;

    size_t count = BPT_TEST_SHUFFLE_COUNT;
    int64_t *keys = malloc(count * sizeof(*keys));
    uint64_t *values = malloc(count * sizeof(*values));
    BPT_CHECK(keys != NULL);
    BPT_CHECK(values != NULL);
    if (keys == NULL || values == NULL) {
        free(keys);
        free(values);
        return;
    }

    int64_t base = -(int64_t)(count / 2);
    for (size_t i = 0; i < count; i++) {
        keys[i] = base + (int64_t)i;
        values[i] = bpt_test_value(keys[i]);
    }

    bpt_test_rng_state = UINT64_C(0x243F6A8885A308D3);
    for (size_t i = count; i > 1; i--) {
        size_t j = (size_t)(bpt_test_rng_next() % (uint64_t)i);
        int64_t tmp_key = keys[i - 1];
        keys[i - 1] = keys[j];
        keys[j] = tmp_key;
        uint64_t tmp_value = values[i - 1];
        values[i - 1] = values[j];
        values[j] = tmp_value;
    }

    BpTree *tree = NULL;
    DbStatus status = bptree_create(&tree);
    BPT_CHECK(status == DB_OK);
    if (status != DB_OK || tree == NULL) {
        free(keys);
        free(values);
        return;
    }

    int ok = 1;
    for (size_t i = 0; i < count; i++) {
        status = bptree_insert(tree, keys[i], values[i]);
        BPT_CHECK(status == DB_OK);
        if (status != DB_OK) {
            ok = 0;
            break;
        }
        if ((i + 1) % 100000 == 0) {
            BPT_CHECK(bptree_check(tree) == 1);
        }
    }
    BPT_CHECK(bptree_check(tree) == 1);

    if (ok) {
        for (size_t i = 0; i < count; i++) {
            uint64_t out = BPT_TEST_SENTINEL;
            BPT_CHECK(bptree_find(tree, keys[i], &out) == 1);
            BPT_CHECK(out == values[i]);
        }

        const int64_t absent[] = {
            base - 1,
            base + (int64_t)count,
            INT64_MIN,
            INT64_MAX,
        };
        for (size_t i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
            uint64_t out = BPT_TEST_SENTINEL;
            BPT_CHECK(bptree_find(tree, absent[i], &out) == 0);
            BPT_CHECK(out == BPT_TEST_SENTINEL);
        }
    }

    bptree_destroy(tree);
    BPT_CHECK(bptree_test_live_allocations == 0);
    free(keys);
    free(values);
}

static BpNode *bpt_test_leftmost_leaf(BpNode *node) {
    while (node != NULL && !node->leaf) {
        node = node->data.children[0];
    }
    return node;
}

/*
 * 체커가 소비자에게 보이는 손상을 거부하는지 확인해요. 이 시험만 BpNode
 * 내부를 보고, 모든 변형은 검사 직후 되돌려요. 트리는 내부 루트가 갈라진
 * 뒤(높이 3)라 리프가 깊이 2에 있고 리프 연결도 여러 개예요.
 */
static void bpt_test_checker_corruptions(BpTree *tree) {
    BPT_CHECK(tree != NULL);
    if (tree == NULL) {
        return;
    }
    BpNode *root = tree->root;
    BPT_CHECK(root != NULL);
    if (root == NULL) {
        return;
    }
    BPT_CHECK(bptree_check(tree) == 1);
    BPT_CHECK(root->leaf == 0);
    BPT_CHECK(root->count >= 1);
    if (root->leaf || root->count < 1) {
        return;
    }

    BpNode *leaf1 = bpt_test_leftmost_leaf(root);
    BPT_CHECK(leaf1 != NULL && leaf1->leaf);
    if (leaf1 == NULL || !leaf1->leaf) {
        return;
    }
    BpNode *leaf2 = leaf1->next;
    BPT_CHECK(leaf2 != NULL && leaf2->leaf);
    if (leaf2 == NULL || !leaf2->leaf) {
        return;
    }
    BpNode *leaf3 = leaf2->next;
    BPT_CHECK(leaf3 != NULL && leaf3->leaf);
    if (leaf3 == NULL || !leaf3->leaf) {
        return;
    }

    /* 노드 개수: 0, 최소 미만, 최대 초과를 모두 거부해야 해요. */
    size_t saved_count = leaf1->count;
    leaf1->count = 0;
    BPT_CHECK(bptree_check(tree) == 0);
    leaf1->count = saved_count;
    BPT_CHECK(bptree_check(tree) == 1);

    BPT_CHECK(saved_count >= 2);
    if (saved_count >= 2) {
        leaf1->count = 1;
        BPT_CHECK(bptree_check(tree) == 0);
        leaf1->count = saved_count;
        BPT_CHECK(bptree_check(tree) == 1);
    }

    leaf1->count = (size_t)BP_MAX_KEYS + 1;
    BPT_CHECK(bptree_check(tree) == 0);
    leaf1->count = saved_count;
    BPT_CHECK(bptree_check(tree) == 1);

    /* 내부 노드 개수도 같아요. 루트를 잠깐 비워요. */
    size_t saved_root_count = root->count;
    root->count = 0;
    BPT_CHECK(bptree_check(tree) == 0);
    root->count = saved_root_count;
    BPT_CHECK(bptree_check(tree) == 1);

    /* 키 순서: 리프의 두 키를 같게 만들어요. */
    BPT_CHECK(leaf1->count >= 2);
    if (leaf1->count >= 2) {
        int64_t saved_key = leaf1->keys[1];
        leaf1->keys[1] = leaf1->keys[0];
        BPT_CHECK(bptree_check(tree) == 0);
        leaf1->keys[1] = saved_key;
        BPT_CHECK(bptree_check(tree) == 1);
    }

    /* 길잡이 키: 마지막 길잡이를 오른쪽 첫 키와 달라지게 해요. */
    size_t separator_at = root->count - 1;
    int64_t saved_separator = root->keys[separator_at];
    root->keys[separator_at] = saved_separator + 1;
    BPT_CHECK(bptree_check(tree) == 0);
    root->keys[separator_at] = saved_separator;
    BPT_CHECK(bptree_check(tree) == 1);

    /* 리프 연결: 끊고, 순서도 바꿔 봐요. */
    BpNode *saved_next1 = leaf1->next;
    leaf1->next = NULL;
    BPT_CHECK(bptree_check(tree) == 0);
    leaf1->next = saved_next1;
    BPT_CHECK(bptree_check(tree) == 1);

    BpNode *saved_next3 = leaf3->next;
    leaf1->next = leaf3;
    leaf3->next = leaf2;
    BPT_CHECK(bptree_check(tree) == 0);
    leaf1->next = saved_next1;
    leaf3->next = saved_next3;
    BPT_CHECK(bptree_check(tree) == 1);

    /* 리프 깊이: 오른쪽 내부 서브트리를 그 첫 리프로 바꿔 깊이를 섞어요. */
    BpNode *right_internal = root->data.children[1];
    BPT_CHECK(right_internal != NULL && !right_internal->leaf);
    if (right_internal != NULL && !right_internal->leaf) {
        BpNode *right_leaf = bpt_test_leftmost_leaf(right_internal);
        BPT_CHECK(right_leaf != NULL && right_leaf->leaf);
        if (right_leaf != NULL && right_leaf->leaf) {
            BpNode *saved_child = root->data.children[1];
            int64_t saved_key0 = root->keys[0];
            root->data.children[1] = right_leaf;
            root->keys[0] = right_leaf->keys[0];
            BPT_CHECK(bptree_check(tree) == 0);
            root->data.children[1] = saved_child;
            root->keys[0] = saved_key0;
            BPT_CHECK(bptree_check(tree) == 1);
        }
    }
}

/*
 * 할당 실패: 첫 리프 분할(새 루트 포함), 부모에 빈칸이 있는 다음 리프
 * 분할, 내부 루트까지 번지는 분할을 골라 모든 실패 인덱스를 소진해요.
 * 차수 512에서는 첫 내부 루트 분할까지 오름차순으로만 내려가요.
 */
static void bpt_test_failure_injection(void) {
    BPT_CHECK(bptree_test_live_allocations == 0);
    bptree_test_fail_after = SIZE_MAX;

    BpTree *tree = NULL;
    DbStatus status = bptree_create(&tree);
    BPT_CHECK(status == DB_OK);
    if (status != DB_OK || tree == NULL) {
        return;
    }

    /* 키 1은 정상으로 넣어 두고, find_split이 첫 리프 분할부터 찾게 해요. */
    bptree_test_fail_after = SIZE_MAX;
    DbStatus seed_status = bptree_insert(tree, 1, bpt_test_value(1));
    BPT_CHECK(seed_status == DB_OK);
    if (seed_status != DB_OK) {
        bptree_destroy(tree);
        return;
    }

    int64_t first_split = 0;
    int64_t next_split = 0;
    int64_t cascade = 0;

    int found_first =
        bpt_test_find_split(tree, 2, BPT_TEST_FAIL_LIMIT, 1, &first_split);
    BPT_CHECK(found_first == 1);
    if (found_first) {
        uint64_t out = BPT_TEST_SENTINEL;
        BPT_CHECK(bptree_find(tree, first_split, &out) == 1);
        BPT_CHECK(out == bpt_test_value(first_split));
    }

    int found_next = 0;
    if (found_first) {
        found_next = bpt_test_find_split(tree, first_split + 1,
                                         BPT_TEST_FAIL_LIMIT, 1, &next_split);
        BPT_CHECK(found_next == 1);
    }

    int found_cascade = 0;
    if (found_next) {
        found_cascade = bpt_test_find_split(tree, next_split + 1,
                                            BPT_TEST_FAIL_LIMIT, 3, &cascade);
        BPT_CHECK(found_cascade == 1);
    }

    if (found_first && found_next && found_cascade) {
        BPT_CHECK(first_split < next_split);
        BPT_CHECK(next_split < cascade);
        BPT_CHECK(bptree_check(tree) == 1);
        BPT_CHECK(bpt_test_verify_range(tree, 1, cascade));
        bpt_test_checker_corruptions(tree);
    }

    bptree_destroy(tree);
    BPT_CHECK(bptree_test_live_allocations == 0);
}

int main(void) {
    bpt_test_create_and_empty();
    bpt_test_empty_insert();
    bpt_test_ascending();
    bpt_test_signed_extrema();
    bpt_test_shuffled();
    bpt_test_failure_injection();

    BPT_CHECK(bptree_test_live_allocations == 0);
    printf("bptree_test: %d checks, %d failed (order %d)\n", bpt_test_checks,
           bpt_test_failures, BPTREE_ORDER);
    return bpt_test_failures == 0 ? 0 : 1;
}

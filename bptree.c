/*
 * bptree.c - 메모리 B+ 트리. 파일·SQL·잠금은 호출자가 맡아요.
 * 노드는 넘침 한 칸을 포함해 두고, 분할에 필요한 할당이 모두 성공한 뒤에만 써요.
 */
#include "bptree.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define BP_MAX_KEYS (BPTREE_ORDER - 1)
/* 최소 자식 수가 2이므로 64비트 키 공간에서 경로는 65칸이면 충분해요. */
#define BP_MAX_HEIGHT (sizeof(int64_t) * CHAR_BIT + 1)

typedef struct BpNode BpNode;
struct BpNode {
    size_t count;
    int leaf;
    int64_t keys[BPTREE_ORDER];
    union {
        uint64_t values[BPTREE_ORDER];
        BpNode *children[BPTREE_ORDER + 1];
    } data;
    BpNode *next;
};

struct BpTree {
    BpNode *root;
};

#ifdef BPTREE_TESTING
/* 공개 헤더에는 노출하지 않는 실패 주입·누수 검사 훅이에요. */
static size_t bptree_test_fail_after = SIZE_MAX;
static size_t bptree_test_live_allocations = 0;

static void *bp_alloc(size_t size) {
    if (bptree_test_fail_after == 0) {
        return NULL;
    }
    void *memory = malloc(size);
    if (memory != NULL) {
        if (bptree_test_fail_after != SIZE_MAX) {
            bptree_test_fail_after--;
        }
        bptree_test_live_allocations++;
    }
    return memory;
}

static void bp_free(void *memory) {
    if (memory != NULL) {
        assert(bptree_test_live_allocations > 0);
        bptree_test_live_allocations--;
        free(memory);
    }
}
#else
#define bp_alloc malloc
#define bp_free free
#endif

static BpNode *bp_new_node(int leaf) {
    BpNode *node = bp_alloc(sizeof(*node));
    if (node != NULL) {
        node->count = 0;
        node->leaf = leaf;
        node->next = NULL;
    }
    return node;
}

DbStatus bptree_create(BpTree **out_tree) {
    assert(out_tree != NULL);
    *out_tree = bp_alloc(sizeof(**out_tree));
    if (*out_tree == NULL) {
        return DB_ERR_NOMEM;
    }
    (*out_tree)->root = NULL;
    return DB_OK;
}

static void bp_destroy_node(BpNode *node) {
    if (node == NULL) {
        return;
    }
    if (!node->leaf) {
        for (size_t i = 0; i <= node->count; i++) {
            bp_destroy_node(node->data.children[i]);
        }
    }
    bp_free(node);
}

void bptree_destroy(BpTree *tree) {
    if (tree != NULL) {
        bp_destroy_node(tree->root);
        bp_free(tree);
    }
}

/* 내부 길잡이 키와 같으면 오른쪽 자식으로 내려가요. */
static size_t bp_child_index(const BpNode *node, int64_t key) {
    size_t lo = 0;
    size_t hi = node->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (node->keys[mid] <= key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static size_t bp_key_index(const BpNode *node, int64_t key) {
    size_t lo = 0;
    size_t hi = node->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (node->keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int bptree_find(const BpTree *tree, int64_t key, uint64_t *out_value) {
    assert(tree != NULL);
    assert(out_value != NULL);
    const BpNode *node = tree->root;
    if (node == NULL) {
        return 0;
    }
    while (!node->leaf) {
        node = node->data.children[bp_child_index(node, key)];
    }
    size_t at = bp_key_index(node, key);
    if (at == node->count || node->keys[at] != key) {
        return 0;
    }
    *out_value = node->data.values[at];
    return 1;
}

static void bp_insert_leaf(BpNode *leaf, size_t at, int64_t key,
                           uint64_t value) {
    size_t tail = leaf->count - at;
    memmove(leaf->keys + at + 1, leaf->keys + at,
            tail * sizeof(*leaf->keys));
    memmove(leaf->data.values + at + 1, leaf->data.values + at,
            tail * sizeof(*leaf->data.values));
    leaf->keys[at] = key;
    leaf->data.values[at] = value;
    leaf->count++;
}

static void bp_insert_child(BpNode *node, size_t at, int64_t separator,
                            BpNode *right) {
    size_t tail = node->count - at;
    memmove(node->keys + at + 1, node->keys + at,
            tail * sizeof(*node->keys));
    memmove(node->data.children + at + 2, node->data.children + at + 1,
            tail * sizeof(*node->data.children));
    node->keys[at] = separator;
    node->data.children[at + 1] = right;
    node->count++;
}

/* 리프는 오른쪽 첫 키를 복사하고, 내부 노드는 가운데 키를 빼서 올려요. */
static int64_t bp_split(BpNode *left, BpNode *right) {
    size_t middle = left->count / 2;
    right->leaf = left->leaf;
    if (left->leaf) {
        right->count = left->count - middle;
        memcpy(right->keys, left->keys + middle,
               right->count * sizeof(*right->keys));
        memcpy(right->data.values, left->data.values + middle,
               right->count * sizeof(*right->data.values));
        left->count = middle;
        right->next = left->next;
        left->next = right;
        return right->keys[0];
    }
    int64_t separator = left->keys[middle];
    right->count = left->count - middle - 1;
    memcpy(right->keys, left->keys + middle + 1,
           right->count * sizeof(*right->keys));
    memcpy(right->data.children, left->data.children + middle + 1,
           (right->count + 1) * sizeof(*right->data.children));
    left->count = middle;
    return separator;
}

DbStatus bptree_insert(BpTree *tree, int64_t key, uint64_t value) {
    assert(tree != NULL);
    if (tree->root == NULL) {
        BpNode *leaf = bp_new_node(1);
        if (leaf == NULL) {
            return DB_ERR_NOMEM;
        }
        bp_insert_leaf(leaf, 0, key, value);
        tree->root = leaf;
        return DB_OK;
    }

    struct {
        BpNode *node;
        size_t child;
    } path[BP_MAX_HEIGHT];
    size_t depth = 0;
    BpNode *node = tree->root;
    while (!node->leaf) {
        assert(depth < BP_MAX_HEIGHT);
        size_t child = bp_child_index(node, key);
        path[depth].node = node;
        path[depth].child = child;
        depth++;
        node = node->data.children[child];
    }
    size_t at = bp_key_index(node, key);
    assert(at == node->count || node->keys[at] != key);
    if (node->count < BP_MAX_KEYS) {
        bp_insert_leaf(node, at, key, value);
        return DB_OK;
    }

    /* 가득 찬 조상만 분할해요. 루트까지 이어지면 새 루트도 먼저 받아요. */
    size_t stop = depth;
    while (stop > 0 && path[stop - 1].node->count == BP_MAX_KEYS) {
        stop--;
    }
    size_t needed = 1 + depth - stop + (stop == 0);
    BpNode *spares[BP_MAX_HEIGHT + 1];
    for (size_t i = 0; i < needed; i++) {
        spares[i] = bp_new_node(0);
        if (spares[i] == NULL) {
            while (i > 0) {
                bp_free(spares[--i]);
            }
            return DB_ERR_NOMEM;
        }
    }

    /* 여기부터 할당·실패 경로가 없어요. */
    bp_insert_leaf(node, at, key, value);
    size_t used = 0;
    while (node->count > BP_MAX_KEYS) {
        BpNode *right = spares[used++];
        int64_t separator = bp_split(node, right);
        if (depth == 0) {
            BpNode *root = spares[used++];
            root->count = 1;
            root->keys[0] = separator;
            root->data.children[0] = node;
            root->data.children[1] = right;
            tree->root = root;
            break;
        }
        depth--;
        node = path[depth].node;
        bp_insert_child(node, path[depth].child, separator, right);
    }
    assert(used == needed);
    return DB_OK;
}

typedef struct {
    const BpNode *previous_leaf;
    size_t leaf_depth;
} BpCheck;

static int bp_check_node(const BpNode *node, int root, size_t depth,
                          BpCheck *check, int64_t *minimum, int64_t *maximum) {
    if (node == NULL || depth >= BP_MAX_HEIGHT ||
        (node->leaf != 0 && node->leaf != 1) ||
        node->count == 0 || node->count > BP_MAX_KEYS) {
        return 0;
    }
    size_t min_count = node->leaf ? BPTREE_ORDER / 2 : BP_MAX_KEYS / 2;
    if (!root && node->count < min_count) {
        return 0;
    }
    for (size_t i = 1; i < node->count; i++) {
        if (node->keys[i - 1] >= node->keys[i]) {
            return 0;
        }
    }
    if (node->leaf) {
        if (check->previous_leaf == NULL) {
            check->leaf_depth = depth;
        } else if (depth != check->leaf_depth ||
                   check->previous_leaf->next != node) {
            return 0;
        }
        check->previous_leaf = node;
        *minimum = node->keys[0];
        *maximum = node->keys[node->count - 1];
        return 1;
    }
    int64_t previous_max = 0;
    for (size_t i = 0; i <= node->count; i++) {
        int64_t child_min;
        int64_t child_max;
        if (!bp_check_node(node->data.children[i], 0, depth + 1,
                           check, &child_min, &child_max)) {
            return 0;
        }
        if (i == 0) {
            *minimum = child_min;
        } else if (previous_max >= child_min ||
                   node->keys[i - 1] != child_min) {
            return 0;
        }
        previous_max = child_max;
    }
    *maximum = previous_max;
    return 1;
}

int bptree_check(const BpTree *tree) {
    assert(tree != NULL);
    if (tree->root == NULL) {
        return 1;
    }
    BpCheck check = {NULL, 0};
    int64_t minimum;
    int64_t maximum;
    return bp_check_node(tree->root, 1, 0, &check, &minimum, &maximum) &&
           check.previous_leaf->next == NULL;
}

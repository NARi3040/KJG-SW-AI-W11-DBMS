/*
 * bptree.h - 메모리 B+ 트리 인덱스 (C11)
 *
 * 키는 users.id, 값은 그 행의 row_index예요. 저장 모듈(storage.c)만 쓰고,
 * 다른 모듈은 storage_find_by_id()로 찾아요. 트리는 파일에 쓰지 않고
 * storage_open()에서 다시 만들어요. 잠금은 없고 호출자가 맞춰요.
 */
#ifndef BPTREE_H
#define BPTREE_H

#include <stdint.h>

#include "dbms.h"

#ifndef BPTREE_ORDER
#define BPTREE_ORDER 512 /* 노드 하나의 최대 자식 수. 테스트는 -DBPTREE_ORDER=4 */
#endif

typedef struct BpTree BpTree;

/* 빈 트리를 만들어요. DB_OK 또는 DB_ERR_NOMEM이고, 실패하면 *out_tree는 NULL이에요. */
DbStatus bptree_create(BpTree **out_tree);

/* NULL이면 아무것도 하지 않아요. 호출 뒤 포인터는 무효예요. */
void bptree_destroy(BpTree *tree);

/* DB_OK 또는 DB_ERR_NOMEM이에요. DB_ERR_NOMEM이면 트리는 호출 전 그대로예요.
 * 이미 있는 키는 넘기지 않아요. 중복 처리 경로 없이 assert만 둬요. */
DbStatus bptree_insert(BpTree *tree, int64_t key, uint64_t value);

/* 찾으면 1을 돌려주고 *out_value를 채워요. 없으면 0이고 *out_value는 그대로예요. */
int bptree_find(const BpTree *tree, int64_t key, uint64_t *out_value);

/* 노드 안 키 정렬, 노드별 키 개수 범위, 모든 리프 같은 깊이, 리프 연결 순서가
 * 전체 정렬 순서와 같음이 모두 맞으면 1이에요. 테스트용이에요. */
int bptree_check(const BpTree *tree);

#endif /* BPTREE_H */

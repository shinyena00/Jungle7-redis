/*
 * store.c — 해시 테이블 (FNV-1a, 체이닝, 점진적 rehash)
 *
 * 공개 함수 모양은 include/store.h 그대로 둔다. 이 파일 안쪽만 자유.
 *
 *   store_set ──┬─ step_rehash: 옛 테이블 칸 하나를 새 테이블로 (R6)
 *               ├─ locate_link_in_store(key) ← 두 테이블 다 보고, 만료 노드도 찾음
 *               ├─ 있으면 바꿔치기
 *               └─ 없으면 새 노드 → start_rehash_if_full → 넣는 테이블 맨 앞
 *   store_get ──┐
 *   store_del ──┼─ step_rehash → locate_live_link (만료 노드를 만나면 지우고 "없음")
 *   store_expire┘
 *   store_purge_expired ── 두 테이블의 칸마다 remove_expired_in_chain
 *
 * rehash 중에는 테이블이 두 개다. tables[0] = 옛 테이블, tables[1] = 새 테이블.
 * 새 키는 tables[1]에만 넣고, 옛 테이블은 rehash_index 칸부터 차례로 비운다.
 * 다 비면 tables[1]이 tables[0]이 된다.
 */
#include "store.h"

#include <stdlib.h>
#include <string.h>

/* ── 자료 구조 ─────────────────────────────────────────────── */

typedef struct entry {
    char * key;
    size_t klen;
    char * val;
    size_t vlen;
    int64_t expire_at_ms;   /* STORE_NO_EXPIRE면 영구 */
    struct entry * next;
} entry_t;

/* buckets가 NULL이면 비어 있는 자리 (rehash 중이 아닐 때 tables[1]) */
typedef struct table {
    entry_t ** buckets;
    size_t nbuckets;
} table_t;

struct store {
    table_t tables[2];
    size_t rehash_index;    /* 옛 테이블에서 다음에 옮길 칸. rehash 중에만 의미 */
    size_t count;           /* 두 테이블을 합친 노드 수 (만료됐지만 안 지운 것 포함) */
};

/* FNV-1a 64비트 표준 상수 */
#define FNV1A_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV1A_PRIME        UINT64_C(1099511628211)

/* 한 번에 건너뛸 수 있는 빈 칸 수. 빈 칸이 아주 많아도 요청 하나가
 * 오래 붙잡히지 않게 한다 (실제 Redis와 같은 값). */
#define MAX_EMPTY_VISITS 10

/* ── 도우미 함수 원형 ──────────────────────────────────────── */

static store_t * create_store (size_t nbuckets);
static void destroy_store (store_t * store);
static void destroy_table (table_t * table);
static void destroy_chain (entry_t * head);
static void destroy_entry (entry_t * entry);
static char * copy_bytes (const char * source, size_t length);
static void step_rehash (store_t * store);
static int is_rehashing (const store_t * store);
static entry_t ** locate_next_chain (store_t * store);
static void move_chain (store_t * store, entry_t ** bucket);
static entry_t ** locate_bucket (table_t * table,
                                 const char * key, size_t klen);
static uint64_t fnv1a (const char * data, size_t length);
static void push_front_bucket (entry_t ** bucket, entry_t * entry);
static void finish_rehash (store_t * store);
static entry_t ** locate_link_in_store (store_t * store,
                                        const char * key, size_t klen);
static entry_t ** locate_link (entry_t ** bucket,
                               const char * key, size_t klen);
static int entry_has_key (const entry_t * entry,
                          const char * key, size_t klen);
static void replace_value (entry_t * entry,
                           char * val, size_t vlen, int64_t expire_at_ms);
static entry_t * create_entry (const char * key, size_t klen,
                               char * val, size_t vlen, int64_t expire_at_ms);
static void start_rehash_if_full (store_t * store);
static void insert_entry (store_t * store, entry_t * entry);
static entry_t ** locate_live_link (store_t * store,
                                    const char * key, size_t klen,
                                    int64_t now_ms);
static int is_expired (const entry_t * entry, int64_t now_ms);
static void remove_entry (store_t * store, entry_t ** link);
static size_t remove_expired_in_table (store_t * store, table_t * table,
                                       int64_t now_ms);
static size_t remove_expired_in_chain (store_t * store, entry_t ** bucket,
                                       int64_t now_ms);

/* ── 공개 함수 ─────────────────────────────────────────────── */

/* nbuckets가 0이면 칸 계산(% nbuckets)이 0으로 나누기가 되므로 거절한다. */
store_t * store_new (size_t nbuckets) {
    if (nbuckets == 0) {
        return NULL;
    }
    return create_store(nbuckets);
}

/* free(NULL)처럼 NULL을 받아도 아무 일 없이 돌아간다. */
void store_free (store_t * s) {
    if (s == NULL) {
        return;
    }
    destroy_store(s);
}

/*
 * 새 값 복사본을 "먼저" 만든다. malloc이 실패해도 원래 값·기간이 남게.
 * step_rehash는 노드 자리만 옮기고 값·기간은 안 바꾸므로 그 뒤에 실패해도 된다.
 * 이미 있는 키는 만료됐어도 같은 노드를 재사용해 바꿔치기한다. (u14)
 */
int store_set (store_t * s, const char * key, size_t klen,
               const char * val, size_t vlen, int64_t expire_at_ms) {
    char * val_copy = copy_bytes(val, vlen);
    if (val_copy == NULL) {
        return -1;
    }

    step_rehash(s);
    entry_t ** link = locate_link_in_store(s, key, klen);
    if (link != NULL) {
        replace_value(*link, val_copy, vlen, expire_at_ms);
        return 0;
    }

    entry_t * created = create_entry(key, klen, val_copy, vlen, expire_at_ms);
    if (created == NULL) {
        free(val_copy);
        return -1;
    }
    start_rehash_if_full(s);
    insert_entry(s, created);
    return 0;
}

/*
 * *val은 다음 store_* 호출 전까지만 유효하다 (store.h 약속).
 * rehash는 노드를 옮길 뿐 값을 복사·해제하지 않으므로 값 주소는 그대로다.
 */
int store_get (store_t * s, const char * key, size_t klen, int64_t now_ms,
               const char ** val, size_t * vlen) {
    step_rehash(s);
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

    *val = (*link)->val;
    *vlen = (*link)->vlen;
    return 1;
}

/* 이미 만료된 키는 "없음"(0). 만료 노드는 locate_live_link가 치운다. (u18) */
int store_del (store_t * s, const char * key, size_t klen, int64_t now_ms) {
    step_rehash(s);
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

    remove_entry(s, link);
    return 1;
}

/* 값은 그대로, 만료 시각만 바꾼다. 만료된 키를 되살리지 않는다. (u16, u18) */
int store_expire (store_t * s, const char * key, size_t klen,
                  int64_t expire_at_ms, int64_t now_ms) {
    step_rehash(s);
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

    (*link)->expire_at_ms = expire_at_ms;
    return 1;
}

/*
 * 지운 게 없으면 0. 호출자가 "다시 해 봐야 소용없다"를 알 수 있게.
 * rehash를 진행하지 않는다: 만료 안 된 노드는 건드리지 않아야
 * 방금 GET한 값의 주소가 그대로 남는다 (server.c 🔒 조건 2).
 */
int store_purge_expired (store_t * s, int64_t now_ms) {
    size_t removed = remove_expired_in_table(s, &s->tables[0], now_ms)
                   + remove_expired_in_table(s, &s->tables[1], now_ms);
    return removed > 0;
}

/* ── 도우미 함수 (처음 불리는 순서) ────────────────────────── */

/* calloc이라 모든 칸이 NULL(빈 체인)로 시작한다. */
static store_t * create_store (size_t nbuckets) {
    store_t * store = malloc(sizeof(*store));
    entry_t ** buckets = calloc(nbuckets, sizeof(*buckets));
    if (store == NULL || buckets == NULL) {
        free(store);
        free(buckets);
        return NULL;
    }
    *store = (store_t){
        .tables = {{ .buckets = buckets, .nbuckets = nbuckets }},
        .rehash_index = 0,
        .count = 0,
    };
    return store;
}

static void destroy_store (store_t * store) {
    destroy_table(&store->tables[0]);
    destroy_table(&store->tables[1]);
    free(store);
}

/* 비어 있는 자리(buckets == NULL, nbuckets == 0)여도 안전하다. */
static void destroy_table (table_t * table) {
    for (size_t i = 0; i < table->nbuckets; i++) {
        destroy_chain(table->buckets[i]);
    }
    free(table->buckets);
}

/* 지우기 전에 next를 먼저 챙긴다. 지운 노드의 next는 읽으면 안 되므로. */
static void destroy_chain (entry_t * head) {
    entry_t * entry = head;
    while (entry != NULL) {
        entry_t * next = entry->next;
        destroy_entry(entry);
        entry = next;
    }
}

static void destroy_entry (entry_t * entry) {
    free(entry->key);
    free(entry->val);
    free(entry);
}

/* 길이 0이어도 NULL이 아닌 주소를 돌려준다. NULL은 메모리 부족만 뜻하게. */
static char * copy_bytes (const char * source, size_t length) {
    char * copy = malloc(length > 0 ? length : 1);
    if (copy == NULL) {
        return NULL;
    }
    if (length > 0) {
        memcpy(copy, source, length);
    }
    return copy;
}

/*
 * 요청 하나에 옛 테이블의 "키가 든 칸" 하나만 옮긴다. 한 번에 다 옮기면
 * 그동안 스레드 하나가 멈추기 때문 (R6). 빈 칸은 MAX_EMPTY_VISITS개까지만 본다.
 */
static void step_rehash (store_t * store) {
    if (!is_rehashing(store)) {
        return;
    }
    entry_t ** bucket = locate_next_chain(store);
    if (bucket != NULL) {
        move_chain(store, bucket);
        store->rehash_index++;
    }
    if (store->rehash_index == store->tables[0].nbuckets) {
        finish_rehash(store);
    }
}

static int is_rehashing (const store_t * store) {
    return store->tables[1].buckets != NULL;
}

/* 빈 칸은 건너뛰며 rehash_index를 옮긴다. 끝에 닿거나 빈 칸만
 * MAX_EMPTY_VISITS개 보면 NULL (이번 요청은 여기까지). */
static entry_t ** locate_next_chain (store_t * store) {
    table_t * old = &store->tables[0];
    for (int visited = 0; visited < MAX_EMPTY_VISITS; visited++) {
        if (store->rehash_index == old->nbuckets) {
            return NULL;
        }
        entry_t ** bucket = &old->buckets[store->rehash_index];
        if (*bucket != NULL) {
            return bucket;
        }
        store->rehash_index++;
    }
    return NULL;
}

/* 노드를 복사하지 않고 이어 붙이는 자리만 바꾼다. 그래서 malloc이 없어
 * 실패할 일이 없고, 키·값 주소도 그대로다. 해시값은 저장하지 않아서 다시 계산한다. */
static void move_chain (store_t * store, entry_t ** bucket) {
    while (*bucket != NULL) {
        entry_t * entry = *bucket;
        *bucket = entry->next;
        push_front_bucket(locate_bucket(&store->tables[1],
                                        entry->key, entry->klen),
                          entry);
    }
}

/* 칸 값이 아니라 칸의 주소를 돌려준다. 맨 앞 노드를 바꿔 끼울 수 있게. */
static entry_t ** locate_bucket (table_t * table,
                                 const char * key, size_t klen) {
    return &table->buckets[fnv1a(key, klen) % table->nbuckets];
}

/* strlen이 아니라 length만큼 돈다. '\0'이 든 키도 해시할 수 있게. (R1) */
static uint64_t fnv1a (const char * data, size_t length) {
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char)data[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

static void push_front_bucket (entry_t ** bucket, entry_t * entry) {
    entry->next = *bucket;
    *bucket = entry;
}

/* 옛 테이블은 이 시점에 모든 칸이 비어 있어서 칸 배열만 해제하면 된다. */
static void finish_rehash (store_t * store) {
    free(store->tables[0].buckets);
    store->tables[0] = store->tables[1];
    store->tables[1] = (table_t){ .buckets = NULL, .nbuckets = 0 };
    store->rehash_index = 0;
}

/*
 * rehash 중이면 옛 테이블에 없을 때 새 테이블도 본다.
 * 한 키는 두 테이블 중 한 곳에만 있다 (새 키는 새 테이블에만 넣으므로).
 */
static entry_t ** locate_link_in_store (store_t * store,
                                        const char * key, size_t klen) {
    entry_t ** link = locate_link(locate_bucket(&store->tables[0], key, klen),
                                  key, klen);
    if (link != NULL || !is_rehashing(store)) {
        return link;
    }
    return locate_link(locate_bucket(&store->tables[1], key, klen), key, klen);
}

/*
 * 키를 가진 노드를 "가리키고 있는 포인터"의 주소. 없으면 NULL.
 * 맨 앞이면 칸 자신(bucket), 아니면 앞 노드의 &next가 된다.
 * 그래서 맨 앞·가운데·끝을 나누지 않고 *link 하나로 빼낼 수 있다.
 */
static entry_t ** locate_link (entry_t ** bucket,
                               const char * key, size_t klen) {
    for (entry_t ** link = bucket; *link != NULL; link = &(*link)->next) {
        if (entry_has_key(*link, key, klen)) {
            return link;
        }
    }
    return NULL;
}

/* 길이 먼저, 그다음 memcmp. strcmp는 '\0'이 든 키를 구분 못 한다. (u08) */
static int entry_has_key (const entry_t * entry,
                          const char * key, size_t klen) {
    return entry->klen == klen && memcmp(entry->key, key, klen) == 0;
}

/* val: 넘겨받는다. 기간도 통째로 바뀌어 새로 시작한다. */
static void replace_value (entry_t * entry,
                           char * val, size_t vlen, int64_t expire_at_ms) {
    free(entry->val);
    entry->val = val;
    entry->vlen = vlen;
    entry->expire_at_ms = expire_at_ms;
}

/* val: 성공하면 넘겨받는다. 실패하면 호출자가 free */
static entry_t * create_entry (const char * key, size_t klen,
                               char * val, size_t vlen, int64_t expire_at_ms) {
    entry_t * entry = malloc(sizeof(*entry));
    char * key_copy = copy_bytes(key, klen);
    if (entry == NULL || key_copy == NULL) {
        free(entry);
        free(key_copy);
        return NULL;
    }
    *entry = (entry_t){
        .key = key_copy,
        .klen = klen,
        .val = val,
        .vlen = vlen,
        .expire_at_ms = expire_at_ms,
        .next = NULL,
    };
    return entry;
}

/*
 * 적재율(노드 수 / 칸 수)이 1이면 칸 수를 두 배로 늘린 새 테이블을 만든다.
 * 칸 배열을 못 만들면 늘리지 않고 넘어간다: 체인이 길어질 뿐 저장은 되고,
 * 다음 새 키 때 다시 시도한다. 그래서 SET이 이 일로 실패하지 않는다.
 */
static void start_rehash_if_full (store_t * store) {
    if (is_rehashing(store) || store->count < store->tables[0].nbuckets) {
        return;
    }
    size_t nbuckets = store->tables[0].nbuckets * 2;
    entry_t ** buckets = calloc(nbuckets, sizeof(*buckets));
    if (buckets == NULL) {
        return;
    }
    store->tables[1] = (table_t){ .buckets = buckets, .nbuckets = nbuckets };
    store->rehash_index = 0;
}

/* rehash 중이면 새 테이블에 넣는다. 옛 테이블에 넣으면 이미 지나간 칸에
 * 들어가 영영 안 옮겨질 수 있다. */
static void insert_entry (store_t * store, entry_t * entry) {
    table_t * table = is_rehashing(store) ? &store->tables[1]
                                          : &store->tables[0];
    push_front_bucket(locate_bucket(table, entry->key, entry->klen), entry);
    store->count++;
}

/*
 * 게으른 만료: 찾은 노드가 만료됐으면 이 자리에서 지우고 NULL을 돌려준다.
 * 그래서 이름은 locate_지만 체인을 바꿀 수 있다. (R2)
 */
static entry_t ** locate_live_link (store_t * store,
                                    const char * key, size_t klen,
                                    int64_t now_ms) {
    entry_t ** link = locate_link_in_store(store, key, klen);
    if (link == NULL) {
        return NULL;
    }
    if (is_expired(*link, now_ms)) {
        remove_entry(store, link);
        return NULL;
    }
    return link;
}

/* 정각(now == expire_at)엔 아직 보인다. 넘어야 만료. (R2, u12) */
static int is_expired (const entry_t * entry, int64_t now_ms) {
    return entry->expire_at_ms != STORE_NO_EXPIRE
        && now_ms > entry->expire_at_ms;
}

/* 앞쪽 포인터가 다음 노드를 가리키게 다시 이은 다음 해제한다. (u05~u07) */
static void remove_entry (store_t * store, entry_t ** link) {
    entry_t * entry = *link;
    *link = entry->next;
    destroy_entry(entry);
    store->count--;
}

/* 비어 있는 자리(nbuckets == 0)면 0. */
static size_t remove_expired_in_table (store_t * store, table_t * table,
                                       int64_t now_ms) {
    size_t removed = 0;
    for (size_t i = 0; i < table->nbuckets; i++) {
        removed += remove_expired_in_chain(store, &table->buckets[i], now_ms);
    }
    return removed;
}

/* 지운 자리에는 다음 노드가 당겨 오므로, 지웠을 때는 link를 옮기지 않는다. */
static size_t remove_expired_in_chain (store_t * store, entry_t ** bucket,
                                       int64_t now_ms) {
    size_t removed = 0;
    entry_t ** link = bucket;
    while (*link != NULL) {
        if (is_expired(*link, now_ms)) {
            remove_entry(store, link);
            removed++;
        } else {
            link = &(*link)->next;
        }
    }
    return removed;
}

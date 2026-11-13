/*
 * store.c — 조각 1 해시 테이블 (FNV-1a, 체이닝, 칸 고정)
 *
 * 공개 함수 모양은 include/store.h 그대로 둔다. 이 파일 안쪽만 자유.
 *
 *   store_set ──┬─ locate_bucket: fnv1a(key) % nbuckets → 칸
 *               ├─ locate_link(칸, key)  ← 만료된 노드도 찾는다
 *               └─ 새 값 복사본 먼저 malloc → 바꿔치기 or 맨 앞에 새 노드
 *   store_get ──┐
 *   store_del ──┼─ locate_live_link → link (노드를 가리키는 포인터의 주소)
 *   store_expire┘     (만료된 노드를 만나면 지우고 "없음")
 *   store_purge_expired ── 칸마다 remove_expired_in_chain (메모리 부족 때만)
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

struct store {
    entry_t ** buckets;
    size_t nbuckets;
};

/* FNV-1a 64비트 표준 상수 */
#define FNV1A_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV1A_PRIME        UINT64_C(1099511628211)

/* ── 도우미 함수 원형 ──────────────────────────────────────── */

static store_t * create_store (size_t nbuckets);
static void destroy_store (store_t * store);
static void destroy_chain (entry_t * head);
static void destroy_entry (entry_t * entry);
static entry_t ** locate_bucket (store_t * store,
                                 const char * key, size_t klen);
static uint64_t fnv1a (const char * data, size_t length);
static char * copy_bytes (const char * source, size_t length);
static entry_t ** locate_link (entry_t ** bucket,
                               const char * key, size_t klen);
static int entry_has_key (const entry_t * entry,
                          const char * key, size_t klen);
static void replace_value (entry_t * entry,
                           char * val, size_t vlen, int64_t expire_at_ms);
static entry_t * create_entry (const char * key, size_t klen,
                               char * val, size_t vlen, int64_t expire_at_ms);
static void push_front_bucket (entry_t ** bucket, entry_t * entry);
static entry_t ** locate_live_link (store_t * store,
                                    const char * key, size_t klen,
                                    int64_t now_ms);
static int is_expired (const entry_t * entry, int64_t now_ms);
static void remove_entry (entry_t ** link);
static size_t remove_expired_in_chain (entry_t ** bucket, int64_t now_ms);

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
 * 이미 있는 키는 만료됐어도 같은 노드를 재사용해 바꿔치기한다. (u14)
 */
int store_set (store_t * s, const char * key, size_t klen,
               const char * val, size_t vlen, int64_t expire_at_ms) {
    char * val_copy = copy_bytes(val, vlen);
    if (val_copy == NULL) {
        return -1;
    }

    entry_t ** bucket = locate_bucket(s, key, klen);
    entry_t ** link = locate_link(bucket, key, klen);
    if (link != NULL) {
        replace_value(*link, val_copy, vlen, expire_at_ms);
        return 0;
    }

    entry_t * created = create_entry(key, klen, val_copy, vlen, expire_at_ms);
    if (created == NULL) {
        free(val_copy);
        return -1;
    }
    push_front_bucket(bucket, created);
    return 0;
}

/* *val은 다음 store_* 호출 전까지만 유효하다 (store.h 약속). */
int store_get (store_t * s, const char * key, size_t klen, int64_t now_ms,
               const char ** val, size_t * vlen) {
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
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

    remove_entry(link);
    return 1;
}

/* 값은 그대로, 만료 시각만 바꾼다. 만료된 키를 되살리지 않는다. (u16, u18) */
int store_expire (store_t * s, const char * key, size_t klen,
                  int64_t expire_at_ms, int64_t now_ms) {
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

    (*link)->expire_at_ms = expire_at_ms;
    return 1;
}

/* 지운 게 없으면 0. 호출자가 "다시 해 봐야 소용없다"를 알 수 있게. */
int store_purge_expired (store_t * s, int64_t now_ms) {
    size_t removed = 0;
    for (size_t i = 0; i < s->nbuckets; i++) {
        removed += remove_expired_in_chain(&s->buckets[i], now_ms);
    }
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
    store->buckets = buckets;
    store->nbuckets = nbuckets;
    return store;
}

static void destroy_store (store_t * store) {
    for (size_t i = 0; i < store->nbuckets; i++) {
        destroy_chain(store->buckets[i]);
    }
    free(store->buckets);
    free(store);
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

/* 칸 값이 아니라 칸의 주소를 돌려준다. 맨 앞 노드를 바꿔 끼울 수 있게. */
static entry_t ** locate_bucket (store_t * store,
                                 const char * key, size_t klen) {
    return &store->buckets[fnv1a(key, klen) % store->nbuckets];
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

static void push_front_bucket (entry_t ** bucket, entry_t * entry) {
    entry->next = *bucket;
    *bucket = entry;
}

/*
 * 게으른 만료: 찾은 노드가 만료됐으면 이 자리에서 지우고 NULL을 돌려준다.
 * 그래서 이름은 locate_지만 체인을 바꿀 수 있다. (R2)
 */
static entry_t ** locate_live_link (store_t * store,
                                    const char * key, size_t klen,
                                    int64_t now_ms) {
    entry_t ** bucket = locate_bucket(store, key, klen);
    entry_t ** link = locate_link(bucket, key, klen);
    if (link == NULL) {
        return NULL;
    }
    if (is_expired(*link, now_ms)) {
        remove_entry(link);
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
static void remove_entry (entry_t ** link) {
    entry_t * entry = *link;
    *link = entry->next;
    destroy_entry(entry);
}

/* 지운 자리에는 다음 노드가 당겨 오므로, 지웠을 때는 link를 옮기지 않는다. */
static size_t remove_expired_in_chain (entry_t ** bucket, int64_t now_ms) {
    size_t removed = 0;
    entry_t ** link = bucket;
    while (*link != NULL) {
        if (is_expired(*link, now_ms)) {
            remove_entry(link);
            removed++;
        } else {
            link = &(*link)->next;
        }
    }
    return removed;
}

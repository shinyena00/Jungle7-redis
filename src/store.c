<<<<<<< HEAD
=======
/*
 * store.c — FNV-1a와 체이닝을 쓰는 해시 테이블.
 * 크기를 늘리는 동안 새 키는 current에만 넣고, 기존 키는 양쪽에서 찾는다.
 * previous의 노드는 복사 없이 옮기므로 값과 만료 시각을 그대로 유지한다.
 */
>>>>>>> 96db220 (feat: server.c)
#include "store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct entry {
    char * key;
    size_t key_length;
    char * value;
    size_t value_length;
<<<<<<< HEAD
    int64_t expire_at_ms;
    struct entry * next;
} entry_t;

struct store {
    size_t bucket_count;
    entry_t ** buckets;
};

static store_t * create_store(size_t bucket_count);
static void destroy_store(store_t * store);
static void destroy_entry(entry_t * entry);
static entry_t ** find_entry_link(store_t * store, const char * key,
                                  size_t key_length);
static size_t find_bucket_index(const store_t * store, const char * key,
                                size_t key_length);
static bool entry_has_key(const entry_t * entry, const char * key,
                          size_t key_length);
static entry_t * replace_entry_value(entry_t * entry, const char * value,
                                     size_t value_length,
                                     int64_t expire_at_ms);
static char * copy_bytes(const char * source, size_t length);
static entry_t * create_entry(const char * key, size_t key_length,
                              const char * value, size_t value_length,
                              int64_t expire_at_ms);
static bool is_expired(const entry_t * entry, int64_t now_ms);
static void destroy_entry_at(entry_t ** link);
static entry_t ** find_live_entry_link(store_t * store, const char * key,
                                       size_t key_length, int64_t now_ms);
=======
    uint64_t hash;
    int64_t expire_at_ms;   /* STORE_NO_EXPIRE면 영구 */
    struct entry * next;
} entry_t;

typedef struct {
    entry_t ** buckets;
    size_t bucket_count;
} table_t;

struct store {
    table_t current;
    table_t previous;
    size_t entry_count;
    size_t rehash_bucket;
};

/* 긴 체인과 빈 버킷이 이어져도 한 호출의 이동량을 제한한다. */
#define REHASH_STEPS_PER_OPERATION 2

/* FNV-1a 64비트 표준 상수 */
#define FNV1A_OFFSET_BASIS UINT64_C(14695981039346656037)
#define FNV1A_PRIME        UINT64_C(1099511628211)
>>>>>>> 96db220 (feat: server.c)

/* ── 공개 함수 ── */

<<<<<<< HEAD
=======
static store_t * create_store (size_t bucket_count);
static entry_t ** create_buckets (size_t bucket_count);
static void destroy_store (store_t * store);
static void destroy_table (table_t * table);
static void destroy_chain (entry_t * head);
static void destroy_entry (entry_t * entry);
static char * copy_bytes (const char * source, size_t length);
static uint64_t fnv1a (const char * data, size_t length);
static entry_t ** locate_store_link (store_t * store, const char * key,
                                      size_t key_length, uint64_t hash);
static entry_t ** locate_bucket (table_t * table, uint64_t hash);
static entry_t ** locate_link (entry_t ** bucket,
                               const char * key, size_t key_length);
static int has_key (const entry_t * entry, const char * key,
                    size_t key_length);
static void replace_value (entry_t * entry, char * value, size_t value_length,
                           int64_t expire_at_ms);
static entry_t * create_entry (const char * key, size_t key_length,
                               char * value, size_t value_length,
                               int64_t expire_at_ms);
static void try_start_rehash (store_t * store);
static void advance_rehash (store_t * store);
static void move_rehash_step (store_t * store);
static void push_front_bucket (entry_t ** bucket, entry_t * entry);
static entry_t ** locate_live_link (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms);
static int is_expired (const entry_t * entry, int64_t now_ms);
static void remove_entry (entry_t ** link);
static size_t purge_table (table_t * table, int64_t now_ms);
static size_t remove_expired_in_chain (entry_t ** bucket, int64_t now_ms);

/* ── 공개 함수 ─────────────────────────────────────────────── */

/* nbuckets가 0이면 칸 계산(% nbuckets)이 0으로 나누기가 되므로 거절한다. */
>>>>>>> 96db220 (feat: server.c)
store_t * store_new (size_t nbuckets) {
    if (nbuckets == 0) {
        return NULL;
    }

    return create_store(nbuckets);
}

void store_free (store_t * s) {
    if (s == NULL) {
        return;
    }

    destroy_store(s);
}

int store_set (store_t * s, const char * key, size_t klen,
               const char * val, size_t vlen, int64_t expire_at_ms) {
<<<<<<< HEAD
    entry_t ** link = find_entry_link(s, key, klen);
    entry_t * entry = *link;
    if (entry != NULL) {
        entry = replace_entry_value(entry, val, vlen, expire_at_ms);
    } else {
        entry = create_entry(key, klen, val, vlen, expire_at_ms);
    }
    if (entry == NULL) {
        return -1;
    }

    *link = entry;
=======
    char * value_copy = copy_bytes(val, vlen);
    if (value_copy == NULL) {
        return -1;
    }

    uint64_t hash = fnv1a(key, klen);
    entry_t ** link = locate_store_link(s, key, klen, hash);
    if (link != NULL) {
        replace_value(*link, value_copy, vlen, expire_at_ms);
        advance_rehash(s);
        return 0;
    }

    entry_t * created = create_entry(key, klen, value_copy, vlen, expire_at_ms);
    if (created == NULL) {
        free(value_copy);
        return -1;
    }
    created->hash = hash;
    try_start_rehash(s);
    push_front_bucket(locate_bucket(&s->current, hash), created);
    s->entry_count++;
    advance_rehash(s);
>>>>>>> 96db220 (feat: server.c)
    return 0;
}

int store_get (store_t * s, const char * key, size_t klen, int64_t now_ms,
               const char ** val, size_t * vlen) {
    entry_t ** link = find_entry_link(s, key, klen);
    entry_t * entry = *link;
    if (entry == NULL) {
        return 0;
    }
    if (is_expired(entry, now_ms)) {
        destroy_entry_at(link);
        return 0;
    }

<<<<<<< HEAD
    *val = entry->value;
    *vlen = entry->value_length;
=======
    *val = (*link)->value;
    *vlen = (*link)->value_length;
>>>>>>> 96db220 (feat: server.c)
    return 1;
}

int store_del (store_t * s, const char * key, size_t klen, int64_t now_ms) {
    entry_t ** link = find_live_entry_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

<<<<<<< HEAD
    destroy_entry_at(link);
=======
    remove_entry(link);
    s->entry_count--;
>>>>>>> 96db220 (feat: server.c)
    return 1;
}

int store_expire (store_t * s, const char * key, size_t klen,
                  int64_t expire_at_ms, int64_t now_ms) {
    entry_t ** link = find_live_entry_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

    (*link)->expire_at_ms = expire_at_ms;
    return 1;
}

<<<<<<< HEAD
=======
/* 지운 게 없으면 0. 호출자가 "다시 해 봐야 소용없다"를 알 수 있게. */
int store_purge_expired (store_t * s, int64_t now_ms) {
    size_t removed = purge_table(&s->current, now_ms);
    removed += purge_table(&s->previous, now_ms);
    s->entry_count -= removed;
    return removed > 0;
}

/* ── 도우미 함수 (처음 불리는 순서) ────────────────────────── */

/* calloc이라 모든 칸이 NULL(빈 체인)로 시작한다. */
>>>>>>> 96db220 (feat: server.c)
static store_t * create_store (size_t bucket_count) {
    store_t * store = malloc(sizeof(*store));
    if (store == NULL) {
        return NULL;
    }

<<<<<<< HEAD
    store->buckets = calloc(bucket_count, sizeof(*store->buckets));
    if (store->buckets == NULL) {
        free(store);
        return NULL;
    }

    store->bucket_count = bucket_count;
    return store;
}

static void destroy_store (store_t * store) {
    for (size_t i = 0; i < store->bucket_count; i++) {
        entry_t * entry = store->buckets[i];
        while (entry != NULL) {
            entry_t * next = entry->next;
            destroy_entry(entry);
            entry = next;
        }
    }

    free(store->buckets);
    free(store);
}

=======
    entry_t ** buckets = create_buckets(bucket_count);
    if (buckets == NULL) {
        free(store);
        return NULL;
    }
    *store = (store_t) {
        .current = {buckets, bucket_count},
    };
    return store;
}

static entry_t ** create_buckets (size_t bucket_count) {
    if (bucket_count > SIZE_MAX / sizeof(entry_t *)) {
        return NULL;
    }
    return calloc(bucket_count, sizeof(entry_t *));
}

static void destroy_store (store_t * store) {
    destroy_table(&store->current);
    destroy_table(&store->previous);
    free(store);
}

static void destroy_table (table_t * table) {
    for (size_t i = 0; i < table->bucket_count; i++) {
        destroy_chain(table->buckets[i]);
    }
    free(table->buckets);
}

/* 지운 노드의 next를 읽을 수 없으므로 미리 챙긴다. */
static void destroy_chain (entry_t * head) {
    entry_t * entry = head;
    while (entry != NULL) {
        entry_t * next = entry->next;
        destroy_entry(entry);
        entry = next;
    }
}

>>>>>>> 96db220 (feat: server.c)
static void destroy_entry (entry_t * entry) {
    free(entry->key);
    free(entry->value);
    free(entry);
}

<<<<<<< HEAD
/* 일치하는 노드를 가리키는 포인터의 주소, 없으면 체인 끝의 빈 포인터 주소. */
static entry_t ** find_entry_link (store_t * store, const char * key,
                                   size_t key_length) {
    size_t index = find_bucket_index(store, key, key_length);
    entry_t ** link = &store->buckets[index];

    while (*link != NULL) {
        entry_t * entry = *link;
        if (entry_has_key(entry, key, key_length)) {
            return link;
        }
        link = &entry->next;
    }

    return link;
}

static size_t find_bucket_index (const store_t * store, const char * key,
                                 size_t key_length) {
    uint64_t hash = UINT64_C(14695981039346656037);
    const uint64_t fnv_prime = UINT64_C(1099511628211);

    for (size_t i = 0; i < key_length; i++) {
        hash ^= (unsigned char)key[i];
        hash *= fnv_prime;
    }

    return (size_t)(hash % store->bucket_count);
}

static bool entry_has_key (const entry_t * entry, const char * key,
                           size_t key_length) {
    return entry->key_length == key_length &&
           (key_length == 0 || memcmp(entry->key, key, key_length) == 0);
}

static entry_t * replace_entry_value (entry_t * entry, const char * value,
                                      size_t value_length,
                                      int64_t expire_at_ms) {
    char * copy = copy_bytes(value, value_length);
=======
/* 길이 0이어도 NULL이 아닌 주소를 돌려준다. NULL은 메모리 부족만 뜻하게. */
static char * copy_bytes (const char * source, size_t length) {
    char * copy = malloc(length > 0 ? length : 1);
>>>>>>> 96db220 (feat: server.c)
    if (copy == NULL) {
        return NULL;
    }

    free(entry->value);
    entry->value = copy;
    entry->value_length = value_length;
    entry->expire_at_ms = expire_at_ms;
    return entry;
}

static char * copy_bytes (const char * source, size_t length) {
    char * copy = malloc(length == 0 ? 1 : length);
    if (copy == NULL) {
        return NULL;
    }

    if (length > 0) {
        memcpy(copy, source, length);
    }
    return copy;
}

<<<<<<< HEAD
static entry_t * create_entry (const char * key, size_t key_length,
                               const char * value, size_t value_length,
                               int64_t expire_at_ms) {
    entry_t * entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return NULL;
    }

    entry->key = copy_bytes(key, key_length);
    if (entry->key == NULL) {
        destroy_entry(entry);
        return NULL;
    }

    entry->value = copy_bytes(value, value_length);
    if (entry->value == NULL) {
        destroy_entry(entry);
        return NULL;
    }

    entry->key_length = key_length;
    entry->value_length = value_length;
    entry->expire_at_ms = expire_at_ms;
    return entry;
}

static bool is_expired (const entry_t * entry, int64_t now_ms) {
    return entry->expire_at_ms != STORE_NO_EXPIRE &&
           now_ms > entry->expire_at_ms;
}

static void destroy_entry_at (entry_t ** link) {
=======
/* strlen 대신 길이만큼 돌아 '\0'이 든 키도 해시한다. (R1) */
static uint64_t fnv1a (const char * data, size_t length) {
    uint64_t hash = FNV1A_OFFSET_BASIS;
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) data[i];
        hash *= FNV1A_PRIME;
    }
    return hash;
}

static entry_t ** locate_store_link (store_t * store, const char * key,
                                      size_t key_length, uint64_t hash) {
    entry_t ** bucket = locate_bucket(&store->current, hash);
    entry_t ** link = locate_link(bucket, key, key_length);
    if (link != NULL || store->previous.buckets == NULL) {
        return link;
    }
    if (hash % store->previous.bucket_count < store->rehash_bucket) {
        return NULL;
    }
    bucket = locate_bucket(&store->previous, hash);
    return locate_link(bucket, key, key_length);
}

/* 맨 앞 노드도 바꿔 끼울 수 있도록 버킷의 주소를 돌려준다. */
static entry_t ** locate_bucket (table_t * table, uint64_t hash) {
    return &table->buckets[hash % table->bucket_count];
}

/*
 * 키를 가진 노드를 "가리키고 있는 포인터"의 주소. 없으면 NULL.
 * 맨 앞이면 칸 자신(bucket), 아니면 앞 노드의 &next가 된다.
 * 그래서 맨 앞·가운데·끝을 나누지 않고 *link 하나로 빼낼 수 있다.
 */
static entry_t ** locate_link (entry_t ** bucket,
                               const char * key, size_t key_length) {
    for (entry_t ** link = bucket; *link != NULL; link = &(*link)->next) {
        if (has_key(*link, key, key_length)) {
            return link;
        }
    }
    return NULL;
}

/* strcmp는 '\0'이 든 키를 구분하지 못한다. (u08) */
static int has_key (const entry_t * entry, const char * key,
                    size_t key_length) {
    return entry->key_length == key_length
        && memcmp(entry->key, key, key_length) == 0;
}

/* value: 넘겨받는다. */
static void replace_value (entry_t * entry, char * value, size_t value_length,
                           int64_t expire_at_ms) {
    free(entry->value);
    entry->value = value;
    entry->value_length = value_length;
    entry->expire_at_ms = expire_at_ms;
}

/* value: 성공하면 넘겨받는다. 실패하면 호출자가 free. */
static entry_t * create_entry (const char * key, size_t key_length,
                               char * value, size_t value_length,
                               int64_t expire_at_ms) {
    entry_t * entry = malloc(sizeof(*entry));
    if (entry == NULL) {
        return NULL;
    }
    char * key_copy = copy_bytes(key, key_length);
    if (key_copy == NULL) {
        free(entry);
        return NULL;
    }
    *entry = (entry_t) {
        .key = key_copy,
        .key_length = key_length,
        .value = value,
        .value_length = value_length,
        .expire_at_ms = expire_at_ms,
    };
    return entry;
}

/* 새 버킷 배열 할당이 실패해도 이미 확보한 새 노드는 넣을 수 있다. */
static void try_start_rehash (store_t * store) {
    size_t bucket_count = store->current.bucket_count;
    if (store->previous.buckets != NULL || store->entry_count < bucket_count
        || bucket_count > SIZE_MAX / 2) {
        return;
    }

    size_t new_bucket_count = bucket_count * 2;
    entry_t ** buckets = create_buckets(new_bucket_count);
    if (buckets == NULL) {
        return;
    }
    store->previous = store->current;
    store->current = (table_t) {buckets, new_bucket_count};
    store->rehash_bucket = 0;
}

static void advance_rehash (store_t * store) {
    for (size_t i = 0; i < REHASH_STEPS_PER_OPERATION; i++) {
        if (store->previous.buckets == NULL) {
            return;
        }
        move_rehash_step(store);
    }
}

/* 빈 칸 하나를 지나가거나, 체인에서 노드 하나만 옮긴다. */
static void move_rehash_step (store_t * store) {
    entry_t ** bucket = &store->previous.buckets[store->rehash_bucket];
    if (*bucket != NULL) {
        entry_t * entry = *bucket;
        *bucket = entry->next;
        push_front_bucket(locate_bucket(&store->current, entry->hash), entry);
    }
    if (*bucket == NULL) {
        store->rehash_bucket++;
    }
    if (store->rehash_bucket == store->previous.bucket_count) {
        free(store->previous.buckets);
        store->previous = (table_t) {0};
        store->rehash_bucket = 0;
    }
}

static void push_front_bucket (entry_t ** bucket, entry_t * entry) {
    entry->next = *bucket;
    *bucket = entry;
}

/*
 * 게으른 만료: 찾은 노드가 만료됐으면 이 자리에서 지우고 NULL을 돌려준다.
 * 그래서 이름은 locate_지만 체인을 바꿀 수 있다. (R2)
 */
static entry_t ** locate_live_link (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms) {
    advance_rehash(store);
    uint64_t hash = fnv1a(key, key_length);
    entry_t ** link = locate_store_link(store, key, key_length, hash);
    if (link == NULL) {
        return NULL;
    }
    if (is_expired(*link, now_ms)) {
        remove_entry(link);
        store->entry_count--;
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
>>>>>>> 96db220 (feat: server.c)
    entry_t * entry = *link;
    *link = entry->next;
    destroy_entry(entry);
}

<<<<<<< HEAD
static entry_t ** find_live_entry_link (store_t * store, const char * key,
                                        size_t key_length, int64_t now_ms) {
    entry_t ** link = find_entry_link(store, key, key_length);
    if (*link == NULL || is_expired(*link, now_ms)) {
        return NULL;
=======
static size_t purge_table (table_t * table, int64_t now_ms) {
    size_t removed = 0;
    for (size_t i = 0; i < table->bucket_count; i++) {
        removed += remove_expired_in_chain(&table->buckets[i], now_ms);
    }
    return removed;
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
>>>>>>> 96db220 (feat: server.c)
    }

    return link;
}

#include "store.h"

#include <stdlib.h>
#include <string.h>

typedef struct entry {
    char * key;              /* 저장소가 가진 복사본 (R1) */
    size_t key_length;
    char * value;            /* 저장소가 가진 복사본 (R1) */
    size_t value_length;
    int64_t expire_at_ms;    /* STORE_NO_EXPIRE면 영구 (R2) */
    struct entry * next;
} entry_t;

struct store {
    entry_t ** buckets;      /* buckets[i]는 i번 칸 체인의 맨 앞, 빈 칸은 NULL */
    size_t nbuckets;
};

static void destroy_chain (entry_t * entry);
static void destroy_entry (entry_t * entry);
static char * copy_bytes (const char * source, size_t length);
static entry_t ** locate_bucket (store_t * store, const char * key,
                                 size_t key_length);
static uint64_t fnv1a (const char * data, size_t length);
static entry_t ** locate_link (entry_t ** bucket, const char * key,
                               size_t key_length);
static int entry_has_key (const entry_t * entry, const char * key,
                          size_t key_length);
static void replace_value (entry_t * entry, char * value,
                           size_t value_length, int64_t expire_at_ms);
static entry_t * create_entry (const char * key, size_t key_length,
                               char * value, size_t value_length,
                               int64_t expire_at_ms);
static void push_entry (entry_t ** bucket, entry_t * entry);
static entry_t ** locate_live_link (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms);
static int is_expired (const entry_t * entry, int64_t now_ms);
static void remove_entry (entry_t ** link);

/* ── 공개 함수 ── */

store_t * store_new (size_t nbuckets) {
    if (nbuckets == 0) {
        return NULL;
    }
    store_t * store = malloc(sizeof(*store));
    if (store == NULL) {
        return NULL;
    }
    /* calloc: 모든 칸을 NULL(빈 칸)로 시작하기 위해 */
    store->buckets = calloc(nbuckets, sizeof(*store->buckets));
    if (store->buckets == NULL) {
        free(store);
        return NULL;
    }
    store->nbuckets = nbuckets;
    return store;
}

void store_free (store_t * s) {
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->nbuckets; i++) {
        destroy_chain(s->buckets[i]);
    }
    free(s->buckets);
    free(s);
}

/* 만료를 보지 않는다: 만료됐지만 안 지워진 entry도 같은 자리에서 새 값으로 (u14) */
int store_set (store_t * s, const char * key, size_t klen,
               const char * val, size_t vlen, int64_t expire_at_ms) {
    /* 실패할 수 있는 복사를 먼저 해서, 실패해도 기존 값·기간이 그대로 남게 */
    char * value_copy = copy_bytes(val, vlen);
    if (value_copy == NULL) {
        return -1;
    }
    entry_t ** bucket = locate_bucket(s, key, klen);
    entry_t ** link = locate_link(bucket, key, klen);
    if (link != NULL) {
        replace_value(*link, value_copy, vlen, expire_at_ms);
        return 0;
    }
    entry_t * entry = create_entry(key, klen, value_copy, vlen, expire_at_ms);
    if (entry == NULL) {
        free(value_copy);
        return -1;
    }
    push_entry(bucket, entry);
    return 0;
}

int store_get (store_t * s, const char * key, size_t klen, int64_t now_ms,
               const char ** val, size_t * vlen) {
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }
    *val = (*link)->value;
    *vlen = (*link)->value_length;
    return 1;
}

int store_del (store_t * s, const char * key, size_t klen, int64_t now_ms) {
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }
    remove_entry(link);
    return 1;
}

int store_expire (store_t * s, const char * key, size_t klen,
                  int64_t expire_at_ms, int64_t now_ms) {
    /* 만료된 키에 기간을 걸면 되살아나므로, 살아 있는 키에만 건다 (u18) */
    entry_t ** link = locate_live_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }
    (*link)->expire_at_ms = expire_at_ms;
    return 1;
}

/* ── 도우미 ── */

/* entry부터 체인 끝까지 전부 해제. entry가 NULL이면(빈 칸) 아무것도 안 함 */
static void destroy_chain (entry_t * entry) {
    while (entry != NULL) {
        /* 해제하면 entry->next를 못 읽으니 먼저 잡아 둔다 */
        entry_t * next = entry->next;
        destroy_entry(entry);
        entry = next;
    }
}

static void destroy_entry (entry_t * entry) {
    free(entry->key);
    free(entry->value);
    free(entry);
}

/* 실패하면 NULL. 길이 0이어도 NULL과 구분되게 1바이트를 잡는다 */
static char * copy_bytes (const char * source, size_t length) {
    char * copy = malloc(length > 0 ? length : 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, source, length);
    return copy;
}

/* 키가 들어갈 칸의 주소. 칸 안의 "맨 앞 entry 포인터"를 바꿔야
 * 맨 앞에 끼우거나 빼낼 수 있어서, 칸 값이 아니라 칸의 주소를 돌려준다. */
static entry_t ** locate_bucket (store_t * store, const char * key,
                                 size_t key_length) {
    size_t index = (size_t)(fnv1a(key, key_length) % store->nbuckets);
    return &store->buckets[index];
}

/* FNV-1a 64비트 (R1). 길이로 끝을 알기 때문에 '\0'도 평범한 바이트로 섞인다.
 * XOR로 바이트를 섞고, 소수를 곱해 그 변화를 모든 자리로 퍼뜨린다. */
static uint64_t fnv1a (const char * data, size_t length) {
    uint64_t hash = UINT64_C(14695981039346656037);   /* offset basis */
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char)data[i];
        hash *= UINT64_C(1099511628211);              /* FNV prime */
    }
    return hash;
}

/* 키를 가진 entry를 "가리키고 있는 포인터"의 주소. 없으면 NULL.
 * 맨 앞이면 칸 자신(bucket), 아니면 앞 entry의 next 칸의 주소가 된다.
 * 이렇게 하면 맨 앞·가운데·맨 끝을 구분하지 않고 같은 방법으로 빼낼 수 있다. */
static entry_t ** locate_link (entry_t ** bucket, const char * key,
                               size_t key_length) {
    for (entry_t ** link = bucket; *link != NULL; link = &(*link)->next) {
        if (entry_has_key(*link, key, key_length)) {
            return link;
        }
    }
    return NULL;
}

/* strcmp는 '\0'에서 멈춰 "a\0b"와 "a\0c"를 같게 보므로 길이 + memcmp (u08) */
static int entry_has_key (const entry_t * entry, const char * key,
                          size_t key_length) {
    return entry->key_length == key_length
        && memcmp(entry->key, key, key_length) == 0;
}

/* value: 넘겨받는다. 옛 값은 여기서 해제 */
static void replace_value (entry_t * entry, char * value,
                           size_t value_length, int64_t expire_at_ms) {
    free(entry->value);
    entry->value = value;
    entry->value_length = value_length;
    entry->expire_at_ms = expire_at_ms;
}

/* value: 성공하면 넘겨받는다. 실패하면(NULL) 호출자가 free */
static entry_t * create_entry (const char * key, size_t key_length,
                               char * value, size_t value_length,
                               int64_t expire_at_ms) {
    entry_t * entry = malloc(sizeof(*entry));
    if (entry == NULL) {
        return NULL;
    }
    entry->key = copy_bytes(key, key_length);
    if (entry->key == NULL) {
        free(entry);
        return NULL;
    }
    entry->key_length = key_length;
    entry->value = value;
    entry->value_length = value_length;
    entry->expire_at_ms = expire_at_ms;
    entry->next = NULL;
    return entry;
}

/* 맨 앞에 끼운다: 체인 끝까지 갈 필요가 없어 O(1) */
static void push_entry (entry_t ** bucket, entry_t * entry) {
    entry->next = *bucket;
    *bucket = entry;
}

/* 살아 있는 entry의 link. 없거나 만료면 NULL.
 * 만료된 entry를 만나면 그 자리에서 지운다: 게으른 만료 (R2) */
static entry_t ** locate_live_link (store_t * store, const char * key,
                                    size_t key_length, int64_t now_ms) {
    entry_t ** bucket = locate_bucket(store, key, key_length);
    entry_t ** link = locate_link(bucket, key, key_length);
    if (link == NULL) {
        return NULL;
    }
    if (is_expired(*link, now_ms)) {
        remove_entry(link);
        return NULL;
    }
    return link;
}

/* 정각에는 아직 살아 있다: now > expire_at 이어야 만료 (u12) */
static int is_expired (const entry_t * entry, int64_t now_ms) {
    return entry->expire_at_ms != STORE_NO_EXPIRE
        && now_ms > entry->expire_at_ms;
}

/* 앞쪽이 뒤 entry를 가리키게 다시 이은 다음 해제 (u05~u07) */
static void remove_entry (entry_t ** link) {
    entry_t * entry = *link;
    *link = entry->next;
    destroy_entry(entry);
}

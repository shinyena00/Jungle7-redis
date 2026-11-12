#include "store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct entry {
    char * key;
    size_t key_length;
    char * value;
    size_t value_length;
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

/* ── 공개 함수 ── */

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

    *val = entry->value;
    *vlen = entry->value_length;
    return 1;
}

int store_del (store_t * s, const char * key, size_t klen, int64_t now_ms) {
    entry_t ** link = find_live_entry_link(s, key, klen, now_ms);
    if (link == NULL) {
        return 0;
    }

    destroy_entry_at(link);
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

static store_t * create_store (size_t bucket_count) {
    store_t * store = malloc(sizeof(*store));
    if (store == NULL) {
        return NULL;
    }

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

static void destroy_entry (entry_t * entry) {
    free(entry->key);
    free(entry->value);
    free(entry);
}

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
    entry_t * entry = *link;
    *link = entry->next;
    destroy_entry(entry);
}

static entry_t ** find_live_entry_link (store_t * store, const char * key,
                                        size_t key_length, int64_t now_ms) {
    entry_t ** link = find_entry_link(store, key, key_length);
    if (*link == NULL || is_expired(*link, now_ms)) {
        return NULL;
    }

    return link;
}

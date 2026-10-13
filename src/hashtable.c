#include "hashtable.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HT_INITIAL_CAPACITY 64

/**
 * Singly linked collision chain node.
 */
typedef struct ht_entry {
    char *key;
    char *value;
    struct ht_entry *next;
} ht_entry_t;

/**
 * Hash table instance definition.
 */
struct hashtable {
    ht_entry_t **buckets;
    size_t num_buckets;
    size_t num_entries;
    pthread_rwlock_t rwlock;
};

/**
 * 64-bit Fowler-Noll-Vo (FNV-1a) hash function.
 * Fast, highly uniform distribution, and zero external dependencies.
 */
static uint64_t hash_fnv1a(const char *str) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    while (*str != '\0') {
        hash ^= (uint64_t)(unsigned char)(*str++);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

hashtable_t *hashtable_create(size_t initial_capacity) {
    size_t capacity = (initial_capacity < HT_INITIAL_CAPACITY) ? HT_INITIAL_CAPACITY : initial_capacity;

    hashtable_t *ht = malloc(sizeof(hashtable_t));
    if (ht == NULL) {
        return NULL;
    }

    ht->buckets = calloc(capacity, sizeof(ht_entry_t *));
    if (ht->buckets == NULL) {
        free(ht);
        return NULL;
    }

    ht->num_buckets = capacity;
    ht->num_entries = 0;

    if (pthread_rwlock_init(&ht->rwlock, NULL) != 0) {
        free(ht->buckets);
        free(ht);
        return NULL;
    }

    return ht;
}

void hashtable_destroy(hashtable_t *ht) {
    if (ht == NULL) {
        return;
    }

    /* Acquire write lock to ensure no readers/writers remain */
    (void)pthread_rwlock_wrlock(&ht->rwlock);

    for (size_t i = 0; i < ht->num_buckets; i++) {
        ht_entry_t *curr = ht->buckets[i];
        while (curr != NULL) {
            ht_entry_t *next = curr->next;
            free(curr->key);
            free(curr->value);
            free(curr);
            curr = next;
        }
    }

    free(ht->buckets);
    ht->buckets = NULL;
    ht->num_entries = 0;
    ht->num_buckets = 0;

    (void)pthread_rwlock_unlock(&ht->rwlock);
    (void)pthread_rwlock_destroy(&ht->rwlock);
    free(ht);
}

/**
 * Internal rehashing function with overflow protection.
 * Must be called while holding rwlock in write mode.
 * Reallocates the bucket array to double the capacity and relinks existing nodes.
 */
static int hashtable_resize_internal(hashtable_t *ht) {
    /* Protect against size_t overflow before doubling capacity */
    if (ht->num_buckets > SIZE_MAX / 2) {
        return -1;
    }

    size_t new_capacity = ht->num_buckets * 2;
    if (new_capacity > SIZE_MAX / sizeof(ht_entry_t *)) {
        return -1;
    }

    ht_entry_t **new_buckets = calloc(new_capacity, sizeof(ht_entry_t *));
    if (new_buckets == NULL) {
        /* On memory exhaustion, gracefully degrade without resizing or corrupting */
        return -1;
    }

    for (size_t i = 0; i < ht->num_buckets; i++) {
        ht_entry_t *curr = ht->buckets[i];
        while (curr != NULL) {
            ht_entry_t *next = curr->next;
            size_t new_idx = (size_t)(hash_fnv1a(curr->key) % new_capacity);

            /* Relink existing node into new bucket chain */
            curr->next = new_buckets[new_idx];
            new_buckets[new_idx] = curr;

            curr = next;
        }
    }

    free(ht->buckets);
    ht->buckets = new_buckets;
    ht->num_buckets = new_capacity;
    return 0;
}

int hashtable_set(hashtable_t *ht, const char *key, const char *value) {
    if (ht == NULL || key == NULL || value == NULL) {
        return -1;
    }

    size_t key_len = strlen(key);
    size_t val_len = strlen(value);
    if (key_len == 0 || key_len > HT_KEY_MAX_LEN || val_len > HT_VAL_MAX_LEN) {
        return -1;
    }

    if (pthread_rwlock_wrlock(&ht->rwlock) != 0) {
        return -1;
    }

    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];

    /* 1. Check if key already exists (UPDATE in-place).
     * Overwriting an existing key does NOT increase entry count, so no resize is needed.
     */
    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            char *new_val = malloc(val_len + 1);
            if (new_val == NULL) {
                (void)pthread_rwlock_unlock(&ht->rwlock);
                return -1;
            }
            memcpy(new_val, value, val_len + 1);
            free(curr->value);
            curr->value = new_val;
            (void)pthread_rwlock_unlock(&ht->rwlock);
            return 0;
        }
        curr = curr->next;
    }

    /* 2. Key does not exist: this is a new INSERTION.
     * Check load factor safely with overflow protection before inserting.
     * Trigger resize if (num_entries + 1) / num_buckets > 0.75.
     */
    bool needs_resize = false;
    if (ht->num_entries >= ht->num_buckets) {
        needs_resize = true;
    } else if (ht->num_entries + 1 > SIZE_MAX / 4 || ht->num_buckets > SIZE_MAX / 3) {
        if ((double)(ht->num_entries + 1) / (double)ht->num_buckets > 0.75) {
            needs_resize = true;
        }
    } else {
        if ((ht->num_entries + 1) * 4 > ht->num_buckets * 3) {
            needs_resize = true;
        }
    }

    if (needs_resize) {
        if (hashtable_resize_internal(ht) == 0) {
            /* Recompute bucket index since num_buckets has changed */
            idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
        }
    }

    /* 3. Insert new key-value entry at head of bucket chain */
    ht_entry_t *new_entry = malloc(sizeof(ht_entry_t));
    if (new_entry == NULL) {
        (void)pthread_rwlock_unlock(&ht->rwlock);
        return -1;
    }

    new_entry->key = malloc(key_len + 1);
    new_entry->value = malloc(val_len + 1);
    if (new_entry->key == NULL || new_entry->value == NULL) {
        free(new_entry->key);
        free(new_entry->value);
        free(new_entry);
        (void)pthread_rwlock_unlock(&ht->rwlock);
        return -1;
    }

    memcpy(new_entry->key, key, key_len + 1);
    memcpy(new_entry->value, value, val_len + 1);

    new_entry->next = ht->buckets[idx];
    ht->buckets[idx] = new_entry;
    ht->num_entries++;

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return 0;
}

bool hashtable_get(hashtable_t *ht, const char *key, char *out_buf, size_t out_buf_size) {
    if (ht == NULL || key == NULL || out_buf == NULL || out_buf_size == 0) {
        return false;
    }

    if (pthread_rwlock_rdlock(&ht->rwlock) != 0) {
        return false;
    }

    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];

    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            size_t val_len = strlen(curr->value);
            if (val_len >= out_buf_size) {
                (void)pthread_rwlock_unlock(&ht->rwlock);
                return false;
            }
            memcpy(out_buf, curr->value, val_len + 1);
            (void)pthread_rwlock_unlock(&ht->rwlock);
            return true;
        }
        curr = curr->next;
    }

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return false;
}

bool hashtable_del(hashtable_t *ht, const char *key) {
    if (ht == NULL || key == NULL) {
        return false;
    }

    if (pthread_rwlock_wrlock(&ht->rwlock) != 0) {
        return false;
    }

    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];
    ht_entry_t *prev = NULL;

    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            if (prev == NULL) {
                ht->buckets[idx] = curr->next;
            } else {
                prev->next = curr->next;
            }

            free(curr->key);
            free(curr->value);
            free(curr);
            ht->num_entries--;

            (void)pthread_rwlock_unlock(&ht->rwlock);
            return true;
        }
        prev = curr;
        curr = curr->next;
    }

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return false;
}

bool hashtable_exists(hashtable_t *ht, const char *key) {
    if (ht == NULL || key == NULL) {
        return false;
    }

    if (pthread_rwlock_rdlock(&ht->rwlock) != 0) {
        return false;
    }

    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];

    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            (void)pthread_rwlock_unlock(&ht->rwlock);
            return true;
        }
        curr = curr->next;
    }

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return false;
}

size_t hashtable_count(hashtable_t *ht) {
    if (ht == NULL) {
        return 0;
    }

    if (pthread_rwlock_rdlock(&ht->rwlock) != 0) {
        return 0;
    }

    size_t count = ht->num_entries;
    (void)pthread_rwlock_unlock(&ht->rwlock);
    return count;
}

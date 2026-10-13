#define _POSIX_C_SOURCE 200809L

#include "hashtable.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HT_INITIAL_CAPACITY 64

/**
 * Hash table collision chain and LRU doubly-linked list node.
 */
typedef struct ht_entry {
    char *key;
    char *value;
    int64_t expire_at_ms;        /* Millisecond epoch deadline; <= 0 means no expiration */
    struct ht_entry *next;       /* Bucket collision chain pointer */
    struct ht_entry *lru_prev;   /* Doubly linked LRU list previous */
    struct ht_entry *lru_next;   /* Doubly linked LRU list next */
} ht_entry_t;

/**
 * Hash table instance definition with thread-safe rwlock and LRU cache tracking.
 */
struct hashtable {
    ht_entry_t **buckets;
    size_t num_buckets;
    size_t num_entries;
    size_t max_capacity;         /* 0 = unbounded; >0 = capacity limit triggering LRU eviction */
    ht_entry_t *lru_head;        /* Most recently used entry */
    ht_entry_t *lru_tail;        /* Least recently used entry */
    pthread_mutex_t lru_lock;    /* Dedicated mutex protecting LRU doubly-linked list pointers */
    pthread_rwlock_t rwlock;     /* Read/Write lock protecting bucket arrays and key/value memory */
};

/* Test simulation timestamp for deterministic TTL testing without sleeping */
static int64_t g_mock_time_ms = 0;

void hashtable_set_test_time_ms(int64_t mock_time_ms) {
    g_mock_time_ms = mock_time_ms;
}

static int64_t get_current_time_ms(void) {
    if (g_mock_time_ms > 0) {
        return g_mock_time_ms;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static inline bool is_entry_expired(const ht_entry_t *entry, int64_t now_ms) {
    return (entry != NULL && entry->expire_at_ms > 0 && now_ms >= entry->expire_at_ms);
}

/**
 * 64-bit Fowler-Noll-Vo (FNV-1a) hash function.
 */
static uint64_t hash_fnv1a(const char *str) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    while (*str != '\0') {
        hash ^= (uint64_t)(unsigned char)(*str++);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/* Internal LRU manipulation helpers (must be called while holding ht->lru_lock) */
static void lru_detach_locked(hashtable_t *ht, ht_entry_t *entry) {
    if (entry->lru_prev != NULL) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else {
        ht->lru_head = entry->lru_next;
    }

    if (entry->lru_next != NULL) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else {
        ht->lru_tail = entry->lru_prev;
    }

    entry->lru_prev = NULL;
    entry->lru_next = NULL;
}

static void lru_attach_head_locked(hashtable_t *ht, ht_entry_t *entry) {
    entry->lru_prev = NULL;
    entry->lru_next = ht->lru_head;
    if (ht->lru_head != NULL) {
        ht->lru_head->lru_prev = entry;
    }
    ht->lru_head = entry;
    if (ht->lru_tail == NULL) {
        ht->lru_tail = entry;
    }
}

static void lru_touch_locked(hashtable_t *ht, ht_entry_t *entry) {
    if (ht->lru_head == entry) {
        return;
    }
    lru_detach_locked(ht, entry);
    lru_attach_head_locked(ht, entry);
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
    ht->max_capacity = 0;
    ht->lru_head = NULL;
    ht->lru_tail = NULL;

    if (pthread_mutex_init(&ht->lru_lock, NULL) != 0) {
        free(ht->buckets);
        free(ht);
        return NULL;
    }

    if (pthread_rwlock_init(&ht->rwlock, NULL) != 0) {
        (void)pthread_mutex_destroy(&ht->lru_lock);
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
    ht->lru_head = NULL;
    ht->lru_tail = NULL;

    (void)pthread_rwlock_unlock(&ht->rwlock);
    (void)pthread_rwlock_destroy(&ht->rwlock);
    (void)pthread_mutex_destroy(&ht->lru_lock);
    free(ht);
}

static int hashtable_resize_internal(hashtable_t *ht) {
    if (ht->num_buckets > SIZE_MAX / 2) {
        return -1;
    }

    size_t new_capacity = ht->num_buckets * 2;
    if (new_capacity > SIZE_MAX / sizeof(ht_entry_t *)) {
        return -1;
    }

    ht_entry_t **new_buckets = calloc(new_capacity, sizeof(ht_entry_t *));
    if (new_buckets == NULL) {
        return -1;
    }

    for (size_t i = 0; i < ht->num_buckets; i++) {
        ht_entry_t *curr = ht->buckets[i];
        while (curr != NULL) {
            ht_entry_t *next = curr->next;
            size_t new_idx = (size_t)(hash_fnv1a(curr->key) % new_capacity);

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

int hashtable_set_ttl(hashtable_t *ht, const char *key, const char *value, int ttl_sec) {
    if (ht == NULL || key == NULL || value == NULL) {
        return -1;
    }

    size_t key_len = strlen(key);
    size_t val_len = strlen(value);
    if (key_len == 0 || key_len > HT_KEY_MAX_LEN || val_len > HT_VAL_MAX_LEN) {
        return -1;
    }

    int64_t now_ms = get_current_time_ms();
    int64_t expire_at_ms = (ttl_sec > 0) ? (now_ms + (int64_t)ttl_sec * 1000) : 0;

    if (pthread_rwlock_wrlock(&ht->rwlock) != 0) {
        return -1;
    }

    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];

    /* 1. Check if key already exists (UPDATE in-place) */
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
            curr->expire_at_ms = expire_at_ms;

            if (ht->max_capacity > 0) {
                (void)pthread_mutex_lock(&ht->lru_lock);
                lru_touch_locked(ht, curr);
                (void)pthread_mutex_unlock(&ht->lru_lock);
            }

            (void)pthread_rwlock_unlock(&ht->rwlock);
            return 0;
        }
        curr = curr->next;
    }

    /* 2. Key does not exist: this is an INSERTION.
     * Check if LRU eviction is needed prior to insertion.
     */
    if (ht->max_capacity > 0 && ht->num_entries >= ht->max_capacity) {
        (void)pthread_mutex_lock(&ht->lru_lock);
        ht_entry_t *victim = ht->lru_tail;
        if (victim != NULL) {
            lru_detach_locked(ht, victim);
        }
        (void)pthread_mutex_unlock(&ht->lru_lock);

        if (victim != NULL) {
            /* Unlink victim from bucket collision chain */
            size_t v_idx = (size_t)(hash_fnv1a(victim->key) % ht->num_buckets);
            ht_entry_t *v_curr = ht->buckets[v_idx];
            ht_entry_t *v_prev = NULL;
            while (v_curr != NULL) {
                if (v_curr == victim) {
                    if (v_prev == NULL) {
                        ht->buckets[v_idx] = v_curr->next;
                    } else {
                        v_prev->next = v_curr->next;
                    }
                    break;
                }
                v_prev = v_curr;
                v_curr = v_curr->next;
            }

            free(victim->key);
            free(victim->value);
            free(victim);
            ht->num_entries--;

            /* Recalculate target bucket index in case victim belonged to the same bucket */
            idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
        }
    }

    /* Check load factor and resize if needed (only if unbounded or under capacity) */
    bool needs_resize = false;
    if (ht->max_capacity == 0 || ht->num_buckets < ht->max_capacity * 2) {
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
    }

    if (needs_resize) {
        if (hashtable_resize_internal(ht) == 0) {
            idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
        }
    }

    /* 3. Insert new key-value entry */
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
    new_entry->expire_at_ms = expire_at_ms;
    new_entry->lru_prev = NULL;
    new_entry->lru_next = NULL;

    new_entry->next = ht->buckets[idx];
    ht->buckets[idx] = new_entry;

    if (ht->max_capacity > 0) {
        (void)pthread_mutex_lock(&ht->lru_lock);
        lru_attach_head_locked(ht, new_entry);
        (void)pthread_mutex_unlock(&ht->lru_lock);
    }

    ht->num_entries++;

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return 0;
}

int hashtable_set(hashtable_t *ht, const char *key, const char *value) {
    return hashtable_set_ttl(ht, key, value, 0);
}

bool hashtable_get(hashtable_t *ht, const char *key, char *out_buf, size_t out_buf_size) {
    if (ht == NULL || key == NULL || out_buf == NULL || out_buf_size == 0) {
        return false;
    }

    if (pthread_rwlock_rdlock(&ht->rwlock) != 0) {
        return false;
    }

    int64_t now_ms = get_current_time_ms();
    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];

    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            /* Check TTL expiration */
            if (is_entry_expired(curr, now_ms)) {
                (void)pthread_rwlock_unlock(&ht->rwlock);
                return false;
            }

            size_t val_len = strlen(curr->value);
            if (val_len >= out_buf_size) {
                (void)pthread_rwlock_unlock(&ht->rwlock);
                return false;
            }
            memcpy(out_buf, curr->value, val_len + 1);

            if (ht->max_capacity > 0) {
                (void)pthread_mutex_lock(&ht->lru_lock);
                lru_touch_locked(ht, curr);
                (void)pthread_mutex_unlock(&ht->lru_lock);
            }

            (void)pthread_rwlock_unlock(&ht->rwlock);
            return true;
        }
        curr = curr->next;
    }

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return false;
}

int64_t hashtable_get_ttl(hashtable_t *ht, const char *key) {
    if (ht == NULL || key == NULL) {
        return -2;
    }

    if (pthread_rwlock_rdlock(&ht->rwlock) != 0) {
        return -2;
    }

    int64_t now_ms = get_current_time_ms();
    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];

    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            if (is_entry_expired(curr, now_ms)) {
                (void)pthread_rwlock_unlock(&ht->rwlock);
                return -2; /* Expired / Not Found */
            }

            if (curr->expire_at_ms <= 0) {
                (void)pthread_rwlock_unlock(&ht->rwlock);
                return -1; /* Persistent key */
            }

            int64_t rem_ms = curr->expire_at_ms - now_ms;
            int64_t rem_sec = (rem_ms + 999) / 1000;
            if (rem_sec < 0) {
                rem_sec = 0;
            }

            (void)pthread_rwlock_unlock(&ht->rwlock);
            return rem_sec;
        }
        curr = curr->next;
    }

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return -2; /* Not found */
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
            if (ht->max_capacity > 0) {
                (void)pthread_mutex_lock(&ht->lru_lock);
                lru_detach_locked(ht, curr);
                (void)pthread_mutex_unlock(&ht->lru_lock);
            }

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

    int64_t now_ms = get_current_time_ms();
    size_t idx = (size_t)(hash_fnv1a(key) % ht->num_buckets);
    ht_entry_t *curr = ht->buckets[idx];

    while (curr != NULL) {
        if (strcmp(curr->key, key) == 0) {
            if (is_entry_expired(curr, now_ms)) {
                (void)pthread_rwlock_unlock(&ht->rwlock);
                return false;
            }

            if (ht->max_capacity > 0) {
                (void)pthread_mutex_lock(&ht->lru_lock);
                lru_touch_locked(ht, curr);
                (void)pthread_mutex_unlock(&ht->lru_lock);
            }

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

    int64_t now_ms = get_current_time_ms();
    size_t active_count = 0;

    for (size_t i = 0; i < ht->num_buckets; i++) {
        ht_entry_t *curr = ht->buckets[i];
        while (curr != NULL) {
            if (!is_entry_expired(curr, now_ms)) {
                active_count++;
            }
            curr = curr->next;
        }
    }

    (void)pthread_rwlock_unlock(&ht->rwlock);
    return active_count;
}

void hashtable_set_max_capacity(hashtable_t *ht, size_t max_capacity) {
    if (ht == NULL) {
        return;
    }

    (void)pthread_rwlock_wrlock(&ht->rwlock);
    ht->max_capacity = max_capacity;

    /* Evict excess entries if existing entry count exceeds new limit */
    if (max_capacity > 0) {
        while (ht->num_entries > max_capacity && ht->lru_tail != NULL) {
            (void)pthread_mutex_lock(&ht->lru_lock);
            ht_entry_t *victim = ht->lru_tail;
            if (victim != NULL) {
                lru_detach_locked(ht, victim);
            }
            (void)pthread_mutex_unlock(&ht->lru_lock);

            if (victim != NULL) {
                size_t v_idx = (size_t)(hash_fnv1a(victim->key) % ht->num_buckets);
                ht_entry_t *v_curr = ht->buckets[v_idx];
                ht_entry_t *v_prev = NULL;
                while (v_curr != NULL) {
                    if (v_curr == victim) {
                        if (v_prev == NULL) {
                            ht->buckets[v_idx] = v_curr->next;
                        } else {
                            v_prev->next = v_curr->next;
                        }
                        break;
                    }
                    v_prev = v_curr;
                    v_curr = v_curr->next;
                }
                free(victim->key);
                free(victim->value);
                free(victim);
                ht->num_entries--;
            }
        }
    }

    (void)pthread_rwlock_unlock(&ht->rwlock);
}

size_t hashtable_get_max_capacity(hashtable_t *ht) {
    if (ht == NULL) {
        return 0;
    }

    (void)pthread_rwlock_rdlock(&ht->rwlock);
    size_t cap = ht->max_capacity;
    (void)pthread_rwlock_unlock(&ht->rwlock);
    return cap;
}

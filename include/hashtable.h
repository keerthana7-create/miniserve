#ifndef MINISERVE_HASHTABLE_H
#define MINISERVE_HASHTABLE_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Maximum key and value limits according to MiniServe specification.
 */
#define HT_KEY_MAX_LEN   64
#define HT_VAL_MAX_LEN   512

/**
 * Opaque handle to the hash table storage engine.
 */
typedef struct hashtable hashtable_t;

/**
 * Create a new, thread-safe hash table.
 *
 * @param initial_capacity Initial number of buckets (will be normalized, e.g. min 16).
 * @return Pointer to newly allocated hashtable_t, or NULL on memory allocation failure.
 */
hashtable_t *hashtable_create(size_t initial_capacity);

/**
 * Destroy the hash table and release all associated resources (keys, values, nodes, lock).
 *
 * Ownership / Concurrency contract:
 * The caller must ensure that all threads performing operations on this table
 * have finished before calling hashtable_destroy().
 *
 * @param ht Pointer to hash table instance.
 */
void hashtable_destroy(hashtable_t *ht);

/**
 * Store a key-value pair in the hash table (insert or update).
 * If the key already exists, its previous value is replaced.
 * Automatically triggers dynamic resizing when load factor exceeds 0.75.
 *
 * @param ht Pointer to hash table instance.
 * @param key Null-terminated key string (length must be <= HT_KEY_MAX_LEN).
 * @param value Null-terminated value string (length must be <= HT_VAL_MAX_LEN).
 * @return 0 on success, -1 on memory allocation or invalid argument failure.
 */
int hashtable_set(hashtable_t *ht, const char *key, const char *value);

/**
 * Retrieve the value associated with a key into a caller-supplied buffer.
 * Copies the value while holding a shared reader lock (pthread_rwlock_rdlock),
 * ensuring thread-safety without requiring heap allocation.
 *
 * @param ht Pointer to hash table instance.
 * @param key Null-terminated key string.
 * @param out_buf Buffer where the value will be copied.
 * @param out_buf_size Size in bytes of out_buf (recommended: HT_VAL_MAX_LEN + 1).
 * @return true if key was found, false if not found or buffer too small.
 */
bool hashtable_get(hashtable_t *ht, const char *key, char *out_buf, size_t out_buf_size);

/**
 * Delete a key-value pair from the hash table.
 *
 * @param ht Pointer to hash table instance.
 * @param key Null-terminated key string.
 * @return true if key existed and was deleted, false if not found.
 */
bool hashtable_del(hashtable_t *ht, const char *key);

#include <stdint.h>

/**
 * Check whether a key exists in the hash table.
 *
 * @param ht Pointer to hash table instance.
 * @param key Null-terminated key string.
 * @return true if key exists, false otherwise.
 */
bool hashtable_exists(hashtable_t *ht, const char *key);

/**
 * Get the current number of key-value pairs stored in the table.
 *
 * @param ht Pointer to hash table instance.
 * @return Number of entries.
 */
size_t hashtable_count(hashtable_t *ht);

/**
 * Store a key-value pair with an optional Time-To-Live (TTL) expiration in seconds.
 * If ttl_sec <= 0, the key is persistent (no expiration).
 *
 * @param ht Pointer to hash table instance.
 * @param key Null-terminated key string.
 * @param value Null-terminated value string.
 * @param ttl_sec TTL in seconds (> 0 for expiring key, <= 0 for persistent key).
 * @return 0 on success, -1 on failure.
 */
int hashtable_set_ttl(hashtable_t *ht, const char *key, const char *value, int ttl_sec);

/**
 * Retrieve the remaining Time-To-Live (TTL) for a key in seconds.
 *
 * @param ht Pointer to hash table instance.
 * @param key Null-terminated key string.
 * @return Remaining TTL in seconds (>= 0),
 *         -1 if the key exists but has no expiration (persistent),
 *         -2 if the key does not exist or has expired.
 */
int64_t hashtable_get_ttl(hashtable_t *ht, const char *key);

/**
 * Configure the maximum capacity for LRU eviction.
 * If max_capacity == 0, the table grows unboundedly (no eviction).
 * If max_capacity > 0 and the table reaches this limit upon insertion,
 * the least recently used key is evicted automatically in O(1) time.
 *
 * @param ht Pointer to hash table instance.
 * @param max_capacity Maximum number of entries before LRU eviction.
 */
void hashtable_set_max_capacity(hashtable_t *ht, size_t max_capacity);

/**
 * Get the configured maximum LRU capacity (0 = unbounded).
 *
 * @param ht Pointer to hash table instance.
 * @return Maximum capacity limit.
 */
size_t hashtable_get_max_capacity(hashtable_t *ht);

/**
 * Test hook for deterministic TTL expiration testing without sleeping.
 * If mock_time_ms > 0, the hash table uses this millisecond timestamp.
 * If mock_time_ms <= 0, real system CLOCK_MONOTONIC is used.
 *
 * @param mock_time_ms Mock timestamp in milliseconds.
 */
void hashtable_set_test_time_ms(int64_t mock_time_ms);

#endif /* MINISERVE_HASHTABLE_H */

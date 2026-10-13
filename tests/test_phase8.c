#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"
#include "protocol.h"

static void test_ttl_deterministic_expiration(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    /* Use mock time at T = 10,000 ms */
    hashtable_set_test_time_ms(10000);

    /* Insert key with TTL = 5 seconds (expires at T = 15,000 ms) */
    assert(hashtable_set_ttl(ht, "session:token", "secret_value", 5) == 0);

    /* Insert persistent key without TTL */
    assert(hashtable_set(ht, "config:mode", "production") == 0);

    assert(hashtable_count(ht) == 2);
    assert(hashtable_exists(ht, "session:token"));
    assert(hashtable_exists(ht, "config:mode"));

    char out[128];
    assert(hashtable_get(ht, "session:token", out, sizeof(out)));
    assert(strcmp(out, "secret_value") == 0);

    /* Check remaining TTL */
    int64_t ttl = hashtable_get_ttl(ht, "session:token");
    assert(ttl == 5);

    int64_t perm_ttl = hashtable_get_ttl(ht, "config:mode");
    assert(perm_ttl == -1); /* -1 indicates persistent */

    /* Advance time to T = 13,000 ms (still valid, 2 seconds left) */
    hashtable_set_test_time_ms(13000);
    assert(hashtable_exists(ht, "session:token"));
    assert(hashtable_get(ht, "session:token", out, sizeof(out)));
    assert(strcmp(out, "secret_value") == 0);
    assert(hashtable_get_ttl(ht, "session:token") == 2);

    /* Advance time past expiration to T = 15,001 ms */
    hashtable_set_test_time_ms(15001);

    /* The session key must now be expired */
    assert(!hashtable_exists(ht, "session:token"));
    assert(!hashtable_get(ht, "session:token", out, sizeof(out)));
    assert(hashtable_get_ttl(ht, "session:token") == -2); /* -2 indicates not found/expired */

    /* Persistent key must remain valid */
    assert(hashtable_exists(ht, "config:mode"));
    assert(hashtable_get(ht, "config:mode", out, sizeof(out)));
    assert(strcmp(out, "production") == 0);

    /* Count should only report active (non-expired) keys */
    assert(hashtable_count(ht) == 1);

    /* Overwriting an expired key revives it with new expiration */
    hashtable_set_test_time_ms(16000);
    assert(hashtable_set_ttl(ht, "session:token", "new_secret", 10) == 0);
    assert(hashtable_exists(ht, "session:token"));
    assert(hashtable_get(ht, "session:token", out, sizeof(out)));
    assert(strcmp(out, "new_secret") == 0);
    assert(hashtable_get_ttl(ht, "session:token") == 10);
    assert(hashtable_count(ht) == 2);

    /* Reset mock time back to real time */
    hashtable_set_test_time_ms(0);
    hashtable_destroy(ht);
    printf("  [PASS] test_ttl_deterministic_expiration\n");
}

static void test_lru_eviction_and_boundaries(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    /* Configure max capacity = 3 */
    hashtable_set_max_capacity(ht, 3);
    assert(hashtable_get_max_capacity(ht) == 3);

    /* Insert 3 items */
    assert(hashtable_set(ht, "k1", "v1") == 0);
    assert(hashtable_set(ht, "k2", "v2") == 0);
    assert(hashtable_set(ht, "k3", "v3") == 0);
    assert(hashtable_count(ht) == 3);

    /* Order MRU -> LRU is now: k3, k2, k1 */

    /* Touch k1 via get: k1 becomes MRU. Order: k1, k3, k2 */
    char out[64];
    assert(hashtable_get(ht, "k1", out, sizeof(out)));
    assert(strcmp(out, "v1") == 0);

    /* Insert k4 -> should evict LRU (k2) */
    assert(hashtable_set(ht, "k4", "v4") == 0);
    assert(hashtable_count(ht) == 3);

    assert(!hashtable_exists(ht, "k2")); /* k2 was evicted */
    assert(hashtable_exists(ht, "k1"));
    assert(hashtable_exists(ht, "k3"));
    assert(hashtable_exists(ht, "k4"));

    /* exists() calls above each promote their key: 106→k1 MRU, 107→k3 MRU, 108→k4 MRU.
     * Order MRU -> LRU is now: k4, k3, k1 */

    /* Touch k3 via exists: k3 becomes MRU. Order: k3, k4, k1 */
    assert(hashtable_exists(ht, "k3"));

    /* Insert k5 -> should evict LRU (k1) */
    assert(hashtable_set(ht, "k5", "v5") == 0);
    assert(hashtable_count(ht) == 3);

    assert(!hashtable_exists(ht, "k1")); /* k1 was evicted */
    assert(hashtable_exists(ht, "k3"));
    assert(hashtable_exists(ht, "k4"));
    assert(hashtable_exists(ht, "k5"));

    /* Test dynamic reduction of capacity: reduce to 2 */
    /* exists() calls above each promote their key: 120→k3 MRU, 121→k4 MRU, 122→k5 MRU.
     * Order MRU -> LRU is: k5, k4, k3. Reducing to 2 evicts LRU (k3). */
    hashtable_set_max_capacity(ht, 2);
    assert(hashtable_get_max_capacity(ht) == 2);
    assert(hashtable_count(ht) == 2);
    assert(!hashtable_exists(ht, "k3")); /* k3 was evicted (was LRU) */
    assert(hashtable_exists(ht, "k4"));
    assert(hashtable_exists(ht, "k5"));

    /* Test explicit deletion updates LRU list safely */
    assert(hashtable_del(ht, "k4"));
    assert(hashtable_count(ht) == 1);
    assert(!hashtable_exists(ht, "k4"));

    /* Capacity now has room for 1 more without eviction */
    assert(hashtable_set(ht, "k6", "v6") == 0);
    assert(hashtable_count(ht) == 2);
    assert(hashtable_exists(ht, "k5"));
    assert(hashtable_exists(ht, "k6"));

    hashtable_destroy(ht);
    printf("  [PASS] test_lru_eviction_and_boundaries\n");
}

static void test_protocol_setex_and_ttl(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);
    command_response_t res;

    hashtable_set_test_time_ms(20000);

    /* Valid SETEX */
    protocol_process_line("SETEX auth_key 60 auth_token_xyz\r\n", ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);
    assert(!res.close_connection);

    /* Check GET */
    protocol_process_line("GET auth_key\r\n", ht, &res);
    assert(strcmp(res.response, "VALUE auth_token_xyz\r\n") == 0);

    /* Check TTL */
    protocol_process_line("TTL auth_key\r\n", ht, &res);
    assert(strcmp(res.response, "TTL 60\r\n") == 0);

    /* SET persistent key */
    protocol_process_line("SET permanent_key perm_val\r\n", ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);

    protocol_process_line("TTL permanent_key\r\n", ht, &res);
    assert(strcmp(res.response, "-1\r\n") == 0);

    /* TTL on missing key */
    protocol_process_line("TTL non_existent\r\n", ht, &res);
    assert(strcmp(res.response, "NOT_FOUND\r\n") == 0);

    /* Advance time past expiration (61 seconds) */
    hashtable_set_test_time_ms(20000 + 61000);

    protocol_process_line("GET auth_key\r\n", ht, &res);
    assert(strcmp(res.response, "NOT_FOUND\r\n") == 0);

    protocol_process_line("EXISTS auth_key\r\n", ht, &res);
    assert(strcmp(res.response, "0\r\n") == 0);

    protocol_process_line("TTL auth_key\r\n", ht, &res);
    assert(strcmp(res.response, "NOT_FOUND\r\n") == 0);

    /* Protocol malformed SETEX tests */
    protocol_process_line("SETEX\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("SETEX key_only\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("SETEX key 0 val\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED Invalid TTL") != NULL);

    protocol_process_line("SETEX key -5 val\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED Invalid TTL") != NULL);

    protocol_process_line("SETEX key not_a_number val\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED Invalid TTL") != NULL);

    protocol_process_line("SETEX key 60\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED Missing value") != NULL);

    /* Protocol malformed TTL tests */
    protocol_process_line("TTL\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED Missing key") != NULL);

    protocol_process_line("TTL key extra_arg\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED Unexpected trailing arguments") != NULL);

    hashtable_set_test_time_ms(0);
    hashtable_destroy(ht);
    printf("  [PASS] test_protocol_setex_and_ttl\n");
}

#define LRU_THREAD_COUNT 4
#define LRU_OPS_PER_THREAD 200

typedef struct {
    hashtable_t *ht;
    int thread_id;
} lru_thread_ctx_t;

static void *lru_concurrent_worker(void *arg) {
    lru_thread_ctx_t *ctx = (lru_thread_ctx_t *)arg;
    char key[64];
    char val[64];
    char out[128];

    for (int i = 0; i < LRU_OPS_PER_THREAD; i++) {
        snprintf(key, sizeof(key), "lru_k_%d", (i * 7 + ctx->thread_id) % 25);
        snprintf(val, sizeof(val), "val_t%d_i%d", ctx->thread_id, i);

        if (i % 2 == 0) {
            (void)hashtable_set(ctx->ht, key, val);
        } else {
            (void)hashtable_get(ctx->ht, key, out, sizeof(out));
            (void)hashtable_exists(ctx->ht, key);
        }
    }
    return NULL;
}

static void test_lru_concurrent_safety(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    /* Bounded LRU capacity to force frequent evictions during concurrent operations */
    hashtable_set_max_capacity(ht, 10);

    pthread_t threads[LRU_THREAD_COUNT];
    lru_thread_ctx_t contexts[LRU_THREAD_COUNT];

    for (int i = 0; i < LRU_THREAD_COUNT; i++) {
        contexts[i].ht = ht;
        contexts[i].thread_id = i;
        int rc = pthread_create(&threads[i], NULL, lru_concurrent_worker, &contexts[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < LRU_THREAD_COUNT; i++) {
        int rc = pthread_join(threads[i], NULL);
        assert(rc == 0);
    }

    assert(hashtable_count(ht) <= 10);

    hashtable_destroy(ht);
    printf("  [PASS] test_lru_concurrent_safety (%d threads, bounded LRU)\n", LRU_THREAD_COUNT);
}

int main(void) {
    printf("Running Phase 8 TTL Expiration & LRU Eviction Unit Tests:\n");
    test_ttl_deterministic_expiration();
    test_lru_eviction_and_boundaries();
    test_protocol_setex_and_ttl();
    test_lru_concurrent_safety();
    printf("All Phase 8 tests passed successfully!\n");
    return EXIT_SUCCESS;
}

#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"

static void test_hashtable_basic(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);
    assert(hashtable_count(ht) == 0);

    assert(hashtable_set(ht, "user:1", "Alice") == 0);
    assert(hashtable_set(ht, "user:2", "Bob") == 0);
    assert(hashtable_count(ht) == 2);

    assert(hashtable_exists(ht, "user:1"));
    assert(hashtable_exists(ht, "user:2"));
    assert(!hashtable_exists(ht, "user:3"));

    char buf[128];
    assert(hashtable_get(ht, "user:1", buf, sizeof(buf)));
    assert(strcmp(buf, "Alice") == 0);
    assert(hashtable_get(ht, "user:2", buf, sizeof(buf)));
    assert(strcmp(buf, "Bob") == 0);
    assert(!hashtable_get(ht, "user:3", buf, sizeof(buf)));

    assert(hashtable_set(ht, "user:1", "Alicia") == 0);
    assert(hashtable_count(ht) == 2);
    assert(hashtable_get(ht, "user:1", buf, sizeof(buf)));
    assert(strcmp(buf, "Alicia") == 0);

    assert(hashtable_del(ht, "user:2"));
    assert(hashtable_count(ht) == 1);
    assert(!hashtable_exists(ht, "user:2"));
    assert(!hashtable_get(ht, "user:2", buf, sizeof(buf)));

    assert(!hashtable_del(ht, "user:2"));
    assert(!hashtable_del(ht, "unknown"));

    hashtable_destroy(ht);
    printf("  [PASS] test_hashtable_basic\n");
}

static void test_hashtable_overwrite_count(void) {
    /* Initialize with capacity 16 */
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    /* Insert 12 items (at the 0.75 threshold: 12 / 16 == 0.75) */
    char key[64];
    char val[64];
    for (int i = 0; i < 12; i++) {
        snprintf(key, sizeof(key), "k_%02d", i);
        snprintf(val, sizeof(val), "orig_%02d", i);
        assert(hashtable_set(ht, key, val) == 0);
        assert(hashtable_count(ht) == (size_t)(i + 1));
    }

    assert(hashtable_count(ht) == 12);

    /* Overwrite all 12 items repeatedly: verify entry count NEVER increases */
    for (int round = 1; round <= 5; round++) {
        for (int i = 0; i < 12; i++) {
            snprintf(key, sizeof(key), "k_%02d", i);
            snprintf(val, sizeof(val), "updated_round_%d_%02d", round, i);
            assert(hashtable_set(ht, key, val) == 0);
            assert(hashtable_count(ht) == 12); /* Must remain exactly 12 */
        }
    }

    /* Verify all values were updated in-place correctly */
    for (int i = 0; i < 12; i++) {
        snprintf(key, sizeof(key), "k_%02d", i);
        char expected[64];
        snprintf(expected, sizeof(expected), "updated_round_5_%02d", i);

        char out[64];
        assert(hashtable_get(ht, key, out, sizeof(out)));
        assert(strcmp(out, expected) == 0);
    }

    hashtable_destroy(ht);
    printf("  [PASS] test_hashtable_overwrite_count\n");
}

static void test_hashtable_resizing(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    const int NUM_ITEMS = 300;
    char key[64];
    char val[128];

    for (int i = 0; i < NUM_ITEMS; i++) {
        snprintf(key, sizeof(key), "item_key_%05d", i);
        snprintf(val, sizeof(val), "item_val_%05d", i);
        assert(hashtable_set(ht, key, val) == 0);
    }

    assert(hashtable_count(ht) == (size_t)NUM_ITEMS);

    for (int i = 0; i < NUM_ITEMS; i++) {
        snprintf(key, sizeof(key), "item_key_%05d", i);
        char expected[128];
        snprintf(expected, sizeof(expected), "item_val_%05d", i);

        char out[128];
        assert(hashtable_get(ht, key, out, sizeof(out)));
        assert(strcmp(out, expected) == 0);
    }

    for (int i = 0; i < NUM_ITEMS; i += 2) {
        snprintf(key, sizeof(key), "item_key_%05d", i);
        assert(hashtable_del(ht, key));
    }
    assert(hashtable_count(ht) == (size_t)(NUM_ITEMS / 2));

    for (int i = 0; i < NUM_ITEMS; i++) {
        snprintf(key, sizeof(key), "item_key_%05d", i);
        char out[128];
        if (i % 2 == 0) {
            assert(!hashtable_get(ht, key, out, sizeof(out)));
        } else {
            assert(hashtable_get(ht, key, out, sizeof(out)));
        }
    }

    hashtable_destroy(ht);
    printf("  [PASS] test_hashtable_resizing\n");
}

static void test_hashtable_boundary(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    char max_key[65];
    memset(max_key, 'k', 64);
    max_key[64] = '\0';

    char max_val[513];
    memset(max_val, 'v', 512);
    max_val[512] = '\0';

    assert(hashtable_set(ht, max_key, max_val) == 0);

    char out[513];
    assert(hashtable_get(ht, max_key, out, sizeof(out)));
    assert(strcmp(out, max_val) == 0);

    char oversized_key[66];
    memset(oversized_key, 'k', 65);
    oversized_key[65] = '\0';
    assert(hashtable_set(ht, oversized_key, "val") == -1);

    char oversized_val[514];
    memset(oversized_val, 'v', 513);
    oversized_val[513] = '\0';
    assert(hashtable_set(ht, "k", oversized_val) == -1);

    assert(hashtable_set(ht, "", "val") == -1);
    assert(hashtable_set(ht, NULL, "val") == -1);
    assert(hashtable_set(ht, "key", NULL) == -1);

    hashtable_destroy(ht);
    printf("  [PASS] test_hashtable_boundary\n");
}

#define CONCURRENT_READERS 4
#define CONCURRENT_WRITERS 2
#define OPS_PER_THREAD 150

typedef struct {
    hashtable_t *ht;
    int thread_id;
} thread_test_ctx_t;

static void *reader_worker(void *arg) {
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)arg;
    char buf[128];
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        char key[64];
        snprintf(key, sizeof(key), "shared_key_%d", (i + ctx->thread_id) % 50);
        (void)hashtable_get(ctx->ht, key, buf, sizeof(buf));
        (void)hashtable_exists(ctx->ht, key);
        (void)hashtable_count(ctx->ht);
    }
    return NULL;
}

static void *writer_worker(void *arg) {
    thread_test_ctx_t *ctx = (thread_test_ctx_t *)arg;
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        char key[64];
        char val[64];
        snprintf(key, sizeof(key), "shared_key_%d", i % 50);
        snprintf(val, sizeof(val), "val_%d_%d", ctx->thread_id, i);

        int rc = hashtable_set(ctx->ht, key, val);
        assert(rc == 0);

        if (i % 5 == 0) {
            (void)hashtable_del(ctx->ht, key);
        }
    }
    return NULL;
}

static void test_hashtable_concurrency(void) {
    hashtable_t *ht = hashtable_create(32);
    assert(ht != NULL);

    pthread_t r_threads[CONCURRENT_READERS];
    pthread_t w_threads[CONCURRENT_WRITERS];
    thread_test_ctx_t r_ctx[CONCURRENT_READERS];
    thread_test_ctx_t w_ctx[CONCURRENT_WRITERS];

    for (int i = 0; i < CONCURRENT_WRITERS; i++) {
        w_ctx[i].ht = ht;
        w_ctx[i].thread_id = i;
        int rc = pthread_create(&w_threads[i], NULL, writer_worker, &w_ctx[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < CONCURRENT_READERS; i++) {
        r_ctx[i].ht = ht;
        r_ctx[i].thread_id = i;
        int rc = pthread_create(&r_threads[i], NULL, reader_worker, &r_ctx[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < CONCURRENT_WRITERS; i++) {
        int rc = pthread_join(w_threads[i], NULL);
        assert(rc == 0);
    }
    for (int i = 0; i < CONCURRENT_READERS; i++) {
        int rc = pthread_join(r_threads[i], NULL);
        assert(rc == 0);
    }

    hashtable_destroy(ht);
    printf("  [PASS] test_hashtable_concurrency (%d readers, %d writers)\n",
           CONCURRENT_READERS, CONCURRENT_WRITERS);
}

int main(void) {
    printf("Running Hash Table Unit Tests:\n");
    test_hashtable_basic();
    test_hashtable_overwrite_count();
    test_hashtable_resizing();
    test_hashtable_boundary();
    test_hashtable_concurrency();
    printf("All Hash Table tests passed successfully!\n");
    return EXIT_SUCCESS;
}

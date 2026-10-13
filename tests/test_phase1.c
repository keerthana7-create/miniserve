#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"
#include "util.h"

static void test_parse_int(void) {
    int out = 0;

    /* Valid integers */
    assert(parse_int("0", &out, 0, 100) && out == 0);
    assert(parse_int("42", &out, 0, 100) && out == 42);
    assert(parse_int("-15", &out, -50, 50) && out == -15);
    assert(parse_int("100", &out, 0, 100) && out == 100);

    /* Out of bounds */
    assert(!parse_int("101", &out, 0, 100));
    assert(!parse_int("-1", &out, 0, 100));

    /* Malformed inputs */
    assert(!parse_int("", &out, 0, 100));
    assert(!parse_int("  42", &out, 0, 100));     /* Leading whitespace rejected */
    assert(!parse_int("42abc", &out, 0, 100));   /* Trailing garbage rejected */
    assert(!parse_int("abc", &out, 0, 100));     /* Non-numeric */
    assert(!parse_int("999999999999999999999", &out, 0, 100)); /* Overflow */
    assert(!parse_int(NULL, &out, 0, 100));      /* NULL safety */

    /* Range inversion test (min > max) */
    assert(!parse_int("50", &out, 100, 10));

    printf("  [PASS] test_parse_int\n");
}

static void test_parse_port(void) {
    uint16_t port = 0;

    /* Valid ports */
    assert(parse_port("80", &port) && port == 80);
    assert(parse_port("8080", &port) && port == 8080);
    assert(parse_port("65535", &port) && port == 65535);
    assert(parse_port("1", &port) && port == 1);

    /* Invalid ports */
    assert(!parse_port("0", &port));         /* Port 0 invalid */
    assert(!parse_port("65536", &port));     /* Exceeds 16-bit port limit */
    assert(!parse_port("-80", &port));
    assert(!parse_port("http", &port));
    assert(!parse_port(NULL, &port));

    printf("  [PASS] test_parse_port\n");
}

static void test_logger_invalid_level(void) {
    /* Below minimum bound (LOG_LEVEL_DEBUG is 0) */
    assert(logger_log((log_level_t)-1, __FILE__, __LINE__, "invalid negative level") == -1);
    assert(logger_log((log_level_t)-50, __FILE__, __LINE__, "invalid negative level") == -1);

    /* Above maximum bound (LOG_LEVEL_FATAL is 4) */
    assert(logger_log((log_level_t)5, __FILE__, __LINE__, "invalid level 5") == -1);
    assert(logger_log((log_level_t)100, __FILE__, __LINE__, "invalid level 100") == -1);

    printf("  [PASS] test_logger_invalid_level\n");
}

static void test_logger(void) {
    const char *tmp_log = "test_phase1.log";

    /* Test invalid log path failure */
    assert(logger_init("/this_directory_does_not_exist_at_all_12345/test.log", LOG_LEVEL_DEBUG, false) == -1);

    /* Pre-init test: LOG_INFO before logger_init must not crash, outputs to stderr only, returns 0 */
    assert(LOG_INFO("Pre-init message: uninitialized logger test") == 0);

    /* Initialize logger writing to file without stderr mirroring */
    assert(logger_init(tmp_log, LOG_LEVEL_DEBUG, false) == 0);

    /* Log exactly 4 messages to the file */
    assert(LOG_DEBUG("Phase 1 test debug message: %d", 123) == 0);
    assert(LOG_INFO("Phase 1 test info message: %s", "hello") == 0);
    assert(LOG_WARN("Phase 1 test warning message") == 0);
    assert(LOG_ERROR("Phase 1 test error message") == 0);

    assert(logger_close() == 0);

    /* Post-close test: LOG_INFO after logger_close must not crash, outputs to stderr only, returns 0 */
    assert(LOG_INFO("Post-close message: uninitialized logger test") == 0);

    /* Verify file was created and contains exactly 4 logged messages */
    FILE *f = fopen(tmp_log, "r");
    assert(f != NULL);

    char line[512];
    int line_count = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        line_count++;
        assert(strstr(line, "Phase 1 test") != NULL);
    }
    assert(line_count == 4);
    assert(fclose(f) == 0);

    /* Clean up temporary test file */
    assert(unlink(tmp_log) == 0);

    printf("  [PASS] test_logger\n");
}

#define CONCURRENT_THREADS 4
#define CONCURRENT_ITERATIONS 50

typedef struct {
    int thread_id;
    int iterations;
} concurrent_thread_arg_t;

static void *concurrent_logger_worker(void *arg) {
    concurrent_thread_arg_t *targ = (concurrent_thread_arg_t *)arg;
    for (int i = 0; i < targ->iterations; i++) {
        int rc = LOG_INFO("Concurrent worker %d iter %d", targ->thread_id, i);
        assert(rc == 0);
    }
    return NULL;
}

static void test_logger_concurrency(void) {
    const char *tmp_conc_log = "test_concurrent.log";
    assert(logger_init(tmp_conc_log, LOG_LEVEL_DEBUG, false) == 0);

    pthread_t threads[CONCURRENT_THREADS];
    concurrent_thread_arg_t args[CONCURRENT_THREADS];

    /* Spawn concurrent logging threads, verifying return values */
    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        args[i].thread_id = i;
        args[i].iterations = CONCURRENT_ITERATIONS;
        int rc = pthread_create(&threads[i], NULL, concurrent_logger_worker, &args[i]);
        assert(rc == 0);
    }

    /* Await completion of all threads, verifying return values */
    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        int rc = pthread_join(threads[i], NULL);
        assert(rc == 0);
    }

    assert(logger_close() == 0);

    /* Verify every entry from every thread was cleanly serialized without corruption */
    FILE *f = fopen(tmp_conc_log, "r");
    assert(f != NULL);

    char line[512];
    int total_lines = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        total_lines++;
        assert(strstr(line, "Concurrent worker") != NULL);
    }
    assert(total_lines == CONCURRENT_THREADS * CONCURRENT_ITERATIONS);
    assert(fclose(f) == 0);

    assert(unlink(tmp_conc_log) == 0);

    printf("  [PASS] test_logger_concurrency (%d threads, %d entries total)\n",
           CONCURRENT_THREADS, total_lines);
}

int main(void) {
    printf("Running Phase 1 Unit Tests:\n");
    test_parse_int();
    test_parse_port();
    test_logger_invalid_level();
    test_logger();
    test_logger_concurrency();
    printf("All Phase 1 tests passed successfully!\n");
    return EXIT_SUCCESS;
}

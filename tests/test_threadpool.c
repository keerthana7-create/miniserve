#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "logger.h"
#include "threadpool.h"

static void test_conn_queue_basic(void) {
    conn_queue_t queue;
    assert(conn_queue_init(&queue, 4) == 0);
    assert(queue.capacity == 4);
    assert(queue.count == 0);

    assert(conn_queue_push(&queue, 101) == 0);
    assert(conn_queue_push(&queue, 102) == 0);
    assert(conn_queue_push(&queue, 103) == 0);
    assert(queue.count == 3);

    assert(conn_queue_pop(&queue) == 101);
    assert(conn_queue_pop(&queue) == 102);
    assert(conn_queue_pop(&queue) == 103);
    assert(queue.count == 0);

    conn_queue_destroy(&queue);
    printf("  [PASS] test_conn_queue_basic\n");
}

static void test_conn_queue_try_push(void) {
    conn_queue_t queue;
    assert(conn_queue_init(&queue, 2) == 0);

    assert(conn_queue_try_push(&queue, 10) == 0);
    assert(conn_queue_try_push(&queue, 20) == 0);
    /* Queue full: try_push returns 1 */
    assert(conn_queue_try_push(&queue, 30) == 1);

    assert(conn_queue_pop(&queue) == 10);
    /* Slot freed: try_push succeeds */
    assert(conn_queue_try_push(&queue, 30) == 0);

    assert(conn_queue_pop(&queue) == 20);
    assert(conn_queue_pop(&queue) == 30);
    assert(queue.count == 0);

    conn_queue_destroy(&queue);
    printf("  [PASS] test_conn_queue_try_push\n");
}

#define CONC_ITEMS_PER_PRODUCER 200
#define CONC_NUM_PRODUCERS 4
#define CONC_NUM_CONSUMERS 4

typedef struct {
    conn_queue_t *queue;
    int id;
} conc_worker_arg_t;

static void *producer_worker(void *arg) {
    conc_worker_arg_t *carg = (conc_worker_arg_t *)arg;
    for (int i = 0; i < CONC_ITEMS_PER_PRODUCER; i++) {
        int val = (carg->id * CONC_ITEMS_PER_PRODUCER) + i + 1;
        assert(conn_queue_push(carg->queue, val) == 0);
    }
    return NULL;
}

static pthread_mutex_t g_consumer_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_received_items[CONC_NUM_PRODUCERS * CONC_ITEMS_PER_PRODUCER + 1];
static int g_total_consumed = 0;

static void *consumer_worker(void *arg) {
    conn_queue_t *queue = (conn_queue_t *)arg;
    while (true) {
        int val = conn_queue_pop(queue);
        if (val < 0) {
            break;
        }
        pthread_mutex_lock(&g_consumer_lock);
        assert(val > 0 && val <= CONC_NUM_PRODUCERS * CONC_ITEMS_PER_PRODUCER);
        assert(!g_received_items[val]);
        g_received_items[val] = true;
        g_total_consumed++;
        pthread_mutex_unlock(&g_consumer_lock);
    }
    return NULL;
}

static void test_conn_queue_concurrency(void) {
    memset(g_received_items, 0, sizeof(g_received_items));
    g_total_consumed = 0;

    conn_queue_t queue;
    assert(conn_queue_init(&queue, 8) == 0);

    pthread_t producers[CONC_NUM_PRODUCERS];
    pthread_t consumers[CONC_NUM_CONSUMERS];
    conc_worker_arg_t producer_args[CONC_NUM_PRODUCERS];

    for (int i = 0; i < CONC_NUM_CONSUMERS; i++) {
        assert(pthread_create(&consumers[i], NULL, consumer_worker, &queue) == 0);
    }

    for (int i = 0; i < CONC_NUM_PRODUCERS; i++) {
        producer_args[i].queue = &queue;
        producer_args[i].id = i;
        assert(pthread_create(&producers[i], NULL, producer_worker, &producer_args[i]) == 0);
    }

    for (int i = 0; i < CONC_NUM_PRODUCERS; i++) {
        assert(pthread_join(producers[i], NULL) == 0);
    }

    /* Wait until all items have been consumed */
    while (true) {
        pthread_mutex_lock(&g_consumer_lock);
        int consumed = g_total_consumed;
        pthread_mutex_unlock(&g_consumer_lock);
        if (consumed == CONC_NUM_PRODUCERS * CONC_ITEMS_PER_PRODUCER) {
            break;
        }
        usleep(1000);
    }

    /* Shutdown queue to unblock consumer workers */
    conn_queue_shutdown(&queue);

    for (int i = 0; i < CONC_NUM_CONSUMERS; i++) {
        assert(pthread_join(consumers[i], NULL) == 0);
    }

    assert(g_total_consumed == CONC_NUM_PRODUCERS * CONC_ITEMS_PER_PRODUCER);
    conn_queue_destroy(&queue);
    printf("  [PASS] test_conn_queue_concurrency (%d items processed concurrently)\n", g_total_consumed);
}

typedef struct {
    conn_queue_t *queue;
    int client_fd;
    int result;
} blocked_pusher_arg_t;

static void *blocked_pusher_worker(void *arg) {
    blocked_pusher_arg_t *barg = (blocked_pusher_arg_t *)arg;
    barg->result = conn_queue_push(barg->queue, barg->client_fd);
    return NULL;
}

static void test_conn_queue_shutdown_unblocks_producer_and_consumer(void) {
    conn_queue_t queue;
    assert(conn_queue_init(&queue, 1) == 0);

    /* Fill queue */
    assert(conn_queue_push(&queue, 501) == 0);

    /* Pusher thread tries to push into full queue; blocks on not_full */
    blocked_pusher_arg_t barg;
    barg.queue = &queue;
    barg.client_fd = 502;
    barg.result = 0;

    pthread_t pusher_tid;
    assert(pthread_create(&pusher_tid, NULL, blocked_pusher_worker, &barg) == 0);
    usleep(10000); /* 10ms wait to ensure pusher is blocked */

    /* Shutdown queue: must wake up blocked pusher and return -1 */
    conn_queue_shutdown(&queue);
    assert(pthread_join(pusher_tid, NULL) == 0);
    assert(barg.result == -1);

    /* Consumer pop on shutdown queue returns -1 immediately */
    assert(conn_queue_pop(&queue) == -1);

    conn_queue_destroy(&queue);
    printf("  [PASS] test_conn_queue_shutdown_unblocks_producer_and_consumer\n");
}

static pthread_mutex_t g_task_counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_tasks_executed = 0;

static void dummy_worker_task(int client_fd, int worker_id, void *arg) {
    (void)worker_id;
    (void)arg;
    /* Verify valid descriptor passed to worker */
    assert(client_fd >= 0);

    pthread_mutex_lock(&g_task_counter_lock);
    g_tasks_executed++;
    pthread_mutex_unlock(&g_task_counter_lock);

    /* Worker thread loop will close client_fd */
}

static void test_threadpool_basic_tasks(void) {
    g_tasks_executed = 0;

    threadpool_t pool;
    assert(threadpool_init(&pool, 4, 16, dummy_worker_task, NULL) == 0);
    assert(pool.is_started);
    assert(pool.threads_created == 4);

    /* Create dummy socket pairs to simulate client descriptors */
    const int NUM_TASKS = 20;
    int fds[NUM_TASKS];
    for (int i = 0; i < NUM_TASKS; i++) {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        close(sv[1]); /* Close remote peer */
        fds[i] = sv[0];
        assert(threadpool_submit(&pool, fds[i]) == 0);
    }

    /* Wait until all tasks execute */
    while (true) {
        pthread_mutex_lock(&g_task_counter_lock);
        int count = g_tasks_executed;
        pthread_mutex_unlock(&g_task_counter_lock);
        if (count == NUM_TASKS) {
            break;
        }
        usleep(1000);
    }

    threadpool_shutdown(&pool);
    assert(!pool.is_started);
    printf("  [PASS] test_threadpool_basic_tasks (%d tasks executed)\n", NUM_TASKS);
}

static void test_threadpool_shutdown_with_idle_workers(void) {
    threadpool_t pool;
    assert(threadpool_init(&pool, 8, 32, dummy_worker_task, NULL) == 0);
    assert(pool.is_started);

    /* Sleep briefly while all workers wait on empty queue */
    usleep(10000);

    /* Shutdown must promptly unblock all 8 workers and join them cleanly */
    threadpool_shutdown(&pool);
    assert(!pool.is_started);
    printf("  [PASS] test_threadpool_shutdown_with_idle_workers\n");
}

static void test_threadpool_invalid_params(void) {
    threadpool_t pool;
    assert(threadpool_init(NULL, 4, 16, dummy_worker_task, NULL) == -1);
    assert(threadpool_init(&pool, 0, 16, dummy_worker_task, NULL) == -1);
    assert(threadpool_init(&pool, 4, 0, dummy_worker_task, NULL) == -1);
    assert(threadpool_init(&pool, 4, 16, NULL, NULL) == -1);
    assert(threadpool_submit(NULL, 1) == -1);
    threadpool_shutdown(NULL);

    conn_queue_t queue;
    assert(conn_queue_init(NULL, 10) == -1);
    assert(conn_queue_init(&queue, 0) == -1);
    assert(conn_queue_push(NULL, 1) == -1);
    assert(conn_queue_push(&queue, -1) == -1);
    assert(conn_queue_pop(NULL) == -1);
    conn_queue_shutdown(NULL);
    conn_queue_destroy(NULL);

    printf("  [PASS] test_threadpool_invalid_params\n");
}

int main(void) {
    printf("Running Phase 4 Thread Pool and Bounded Queue Unit Tests:\n");

    /* Initialize logger to discard debug noise */
    logger_init(NULL, LOG_LEVEL_WARN, false);

    test_conn_queue_basic();
    test_conn_queue_try_push();
    test_conn_queue_concurrency();
    test_conn_queue_shutdown_unblocks_producer_and_consumer();
    test_threadpool_basic_tasks();
    test_threadpool_shutdown_with_idle_workers();
    test_threadpool_invalid_params();

    logger_close();
    printf("All Thread Pool and Bounded Queue unit tests passed successfully!\n");
    return EXIT_SUCCESS;
}

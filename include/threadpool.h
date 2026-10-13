#ifndef MINISERVE_THREADPOOL_H
#define MINISERVE_THREADPOOL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Bounded circular FIFO queue of accepted client socket descriptors.
 */
typedef struct {
    int *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    bool shutdown;
} conn_queue_t;

/**
 * Worker thread routine callback.
 *
 * @param client_fd Socket descriptor for the accepted connection.
 * @param worker_id Index of the worker thread executing this task (0 .. num_threads-1).
 * @param arg User-supplied argument passed to threadpool_init().
 */
typedef void (*worker_task_fn)(int client_fd, int worker_id, void *arg);

/**
 * Context structure passed to each worker thread.
 */
typedef struct {
    void *pool_ptr;
    int worker_id;
} worker_context_t;

/**
 * Thread pool structure managing a fixed pool of worker threads and a bounded connection queue.
 */
typedef struct {
    conn_queue_t queue;
    pthread_t *threads;
    worker_context_t *contexts;
    size_t num_threads;
    size_t threads_created;
    worker_task_fn task_fn;
    void *task_arg;
    bool is_started;
} threadpool_t;

/**
 * Initialize a bounded connection queue with fixed capacity.
 *
 * @param queue Pointer to queue structure.
 * @param capacity Maximum number of descriptors the queue can hold (must be >= 1).
 * @return 0 on success, -1 on failure.
 */
int conn_queue_init(conn_queue_t *queue, size_t capacity);

/**
 * Push a client socket descriptor into the bounded queue.
 * Blocks if queue is full until space becomes available or shutdown is requested.
 *
 * @param queue Pointer to queue structure.
 * @param client_fd Socket descriptor to enqueue.
 * @return 0 on success, -1 if queue is shutdown or on error.
 */
int conn_queue_push(conn_queue_t *queue, int client_fd);

/**
 * Non-blocking attempt to push a client socket descriptor into the queue.
 *
 * @param queue Pointer to queue structure.
 * @param client_fd Socket descriptor to enqueue.
 * @return 0 on success, 1 if queue is currently full, -1 on shutdown/error.
 */
int conn_queue_try_push(conn_queue_t *queue, int client_fd);

/**
 * Pop a client socket descriptor from the bounded queue.
 * Blocks if queue is empty until an item is available or shutdown is requested.
 *
 * @param queue Pointer to queue structure.
 * @return Non-negative socket descriptor on success, -1 if shutdown and empty or on error.
 */
int conn_queue_pop(conn_queue_t *queue);

/**
 * Signal shutdown on the connection queue and wake all waiting producers and consumers.
 * Drains and closes all remaining unserviced descriptors currently in the queue.
 *
 * @param queue Pointer to queue structure.
 */
void conn_queue_shutdown(conn_queue_t *queue);

/**
 * Clean up queue resources, free buffer, and destroy synchronization primitives.
 *
 * @param queue Pointer to queue structure.
 */
void conn_queue_destroy(conn_queue_t *queue);

/**
 * Initialize and start a fixed-size worker thread pool.
 *
 * @param pool Pointer to threadpool structure.
 * @param num_threads Number of worker threads to spawn (must be >= 1).
 * @param queue_capacity Maximum capacity of bounded connection queue (must be >= 1).
 * @param task_fn Callback function executed by workers for each dequeued connection.
 * @param task_arg User argument passed to task_fn.
 * @return 0 on success, -1 on failure.
 */
int threadpool_init(threadpool_t *pool, size_t num_threads, size_t queue_capacity,
                    worker_task_fn task_fn, void *task_arg);

/**
 * Submit an accepted client socket descriptor to the thread pool's bounded queue.
 *
 * @param pool Pointer to threadpool structure.
 * @param client_fd Accepted socket descriptor.
 * @return 0 on success, -1 on failure or shutdown.
 */
int threadpool_submit(threadpool_t *pool, int client_fd);

/**
 * Shutdown the thread pool: signals shutdown to queue, unblocks waiting threads,
 * drains/closes pending queue descriptors, joins all worker threads, and releases resources.
 *
 * @param pool Pointer to threadpool structure.
 */
void threadpool_shutdown(threadpool_t *pool);

#endif /* MINISERVE_THREADPOOL_H */

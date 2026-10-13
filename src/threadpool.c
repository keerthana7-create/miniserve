#include "threadpool.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logger.h"

int conn_queue_init(conn_queue_t *queue, size_t capacity) {
    if (queue == NULL || capacity == 0) {
        return -1;
    }

    queue->buffer = calloc(capacity, sizeof(int));
    if (queue->buffer == NULL) {
        LOG_FATAL("Failed to allocate connection queue buffer of capacity %zu", capacity);
        return -1;
    }

    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->shutdown = false;

    int rc = pthread_mutex_init(&queue->lock, NULL);
    if (rc != 0) {
        LOG_FATAL("pthread_mutex_init failed for queue: %s", strerror(rc));
        free(queue->buffer);
        queue->buffer = NULL;
        return -1;
    }

    rc = pthread_cond_init(&queue->not_empty, NULL);
    if (rc != 0) {
        LOG_FATAL("pthread_cond_init(not_empty) failed: %s", strerror(rc));
        pthread_mutex_destroy(&queue->lock);
        free(queue->buffer);
        queue->buffer = NULL;
        return -1;
    }

    rc = pthread_cond_init(&queue->not_full, NULL);
    if (rc != 0) {
        LOG_FATAL("pthread_cond_init(not_full) failed: %s", strerror(rc));
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->lock);
        free(queue->buffer);
        queue->buffer = NULL;
        return -1;
    }

    return 0;
}

int conn_queue_push(conn_queue_t *queue, int client_fd) {
    if (queue == NULL || client_fd < 0) {
        return -1;
    }

    int rc = pthread_mutex_lock(&queue->lock);
    if (rc != 0) {
        LOG_ERROR("pthread_mutex_lock failed in conn_queue_push: %s", strerror(rc));
        return -1;
    }

    /* Wait while queue is full; handle spurious wakeups */
    while (queue->count == queue->capacity && !queue->shutdown) {
        rc = pthread_cond_wait(&queue->not_full, &queue->lock);
        if (rc != 0) {
            LOG_ERROR("pthread_cond_wait failed in conn_queue_push: %s", strerror(rc));
            pthread_mutex_unlock(&queue->lock);
            return -1;
        }
    }

    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        return -1;
    }

    queue->buffer[queue->tail] = client_fd;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    rc = pthread_cond_signal(&queue->not_empty);
    if (rc != 0) {
        LOG_WARN("pthread_cond_signal failed in conn_queue_push: %s", strerror(rc));
    }

    pthread_mutex_unlock(&queue->lock);
    return 0;
}

int conn_queue_try_push(conn_queue_t *queue, int client_fd) {
    if (queue == NULL || client_fd < 0) {
        return -1;
    }

    int rc = pthread_mutex_lock(&queue->lock);
    if (rc != 0) {
        return -1;
    }

    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        return -1;
    }

    if (queue->count == queue->capacity) {
        pthread_mutex_unlock(&queue->lock);
        return 1; /* Queue is currently full */
    }

    queue->buffer[queue->tail] = client_fd;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

int conn_queue_pop(conn_queue_t *queue) {
    if (queue == NULL) {
        return -1;
    }

    int rc = pthread_mutex_lock(&queue->lock);
    if (rc != 0) {
        LOG_ERROR("pthread_mutex_lock failed in conn_queue_pop: %s", strerror(rc));
        return -1;
    }

    /* Wait while queue is empty; handle spurious wakeups */
    while (queue->count == 0 && !queue->shutdown) {
        rc = pthread_cond_wait(&queue->not_empty, &queue->lock);
        if (rc != 0) {
            LOG_ERROR("pthread_cond_wait failed in conn_queue_pop: %s", strerror(rc));
            pthread_mutex_unlock(&queue->lock);
            return -1;
        }
    }

    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        return -1;
    }

    int client_fd = queue->buffer[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    rc = pthread_cond_signal(&queue->not_full);
    if (rc != 0) {
        LOG_WARN("pthread_cond_signal failed in conn_queue_pop: %s", strerror(rc));
    }

    pthread_mutex_unlock(&queue->lock);
    return client_fd;
}

void conn_queue_shutdown(conn_queue_t *queue) {
    if (queue == NULL) {
        return;
    }

    int rc = pthread_mutex_lock(&queue->lock);
    if (rc != 0) {
        return;
    }

    queue->shutdown = true;

    /* Wake all waiting workers and producers */
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);

    /* Drain and close any remaining unserviced descriptors */
    while (queue->count > 0) {
        int fd = queue->buffer[queue->head];
        queue->head = (queue->head + 1) % queue->capacity;
        queue->count--;
        if (fd >= 0) {
            close(fd);
        }
    }

    pthread_mutex_unlock(&queue->lock);
}

void conn_queue_destroy(conn_queue_t *queue) {
    if (queue == NULL) {
        return;
    }

    conn_queue_shutdown(queue);

    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->lock);

    if (queue->buffer != NULL) {
        free(queue->buffer);
        queue->buffer = NULL;
    }
}

static void *threadpool_worker_thread(void *arg) {
    worker_context_t *ctx = (worker_context_t *)arg;
    threadpool_t *pool = (threadpool_t *)ctx->pool_ptr;
    int worker_id = ctx->worker_id;

    while (true) {
        int client_fd = conn_queue_pop(&pool->queue);
        if (client_fd < 0) {
            /* Queue has shut down and is empty */
            break;
        }

        /* Execute the registered connection task */
        if (pool->task_fn != NULL) {
            pool->task_fn(client_fd, worker_id, pool->task_arg);
        }

        /* Close descriptor exactly once per accepted connection */
        close(client_fd);
    }

    return NULL;
}

int threadpool_init(threadpool_t *pool, size_t num_threads, size_t queue_capacity,
                    worker_task_fn task_fn, void *task_arg) {
    if (pool == NULL || num_threads == 0 || queue_capacity == 0 || task_fn == NULL) {
        return -1;
    }

    memset(pool, 0, sizeof(*pool));
    pool->num_threads = num_threads;
    pool->task_fn = task_fn;
    pool->task_arg = task_arg;
    pool->threads_created = 0;
    pool->is_started = false;

    if (conn_queue_init(&pool->queue, queue_capacity) != 0) {
        return -1;
    }

    pool->threads = calloc(num_threads, sizeof(pthread_t));
    if (pool->threads == NULL) {
        LOG_FATAL("Failed to allocate thread array for %zu workers", num_threads);
        conn_queue_destroy(&pool->queue);
        return -1;
    }

    pool->contexts = calloc(num_threads, sizeof(worker_context_t));
    if (pool->contexts == NULL) {
        LOG_FATAL("Failed to allocate worker context array for %zu workers", num_threads);
        free(pool->threads);
        pool->threads = NULL;
        conn_queue_destroy(&pool->queue);
        return -1;
    }

    /* Spawn worker threads; handle partial startup failures safely */
    for (size_t i = 0; i < num_threads; i++) {
        pool->contexts[i].pool_ptr = pool;
        pool->contexts[i].worker_id = (int)i;

        int rc = pthread_create(&pool->threads[i], NULL, threadpool_worker_thread, &pool->contexts[i]);
        if (rc != 0) {
            LOG_FATAL("pthread_create failed for worker %zu: %s", i, strerror(rc));

            /* Shut down queue and join already created threads */
            conn_queue_shutdown(&pool->queue);
            for (size_t j = 0; j < pool->threads_created; j++) {
                pthread_join(pool->threads[j], NULL);
            }

            conn_queue_destroy(&pool->queue);
            free(pool->contexts);
            free(pool->threads);
            pool->contexts = NULL;
            pool->threads = NULL;
            return -1;
        }
        pool->threads_created++;
    }

    pool->is_started = true;
    LOG_INFO("Thread pool initialized with %zu workers and queue capacity %zu", num_threads, queue_capacity);
    return 0;
}

int threadpool_submit(threadpool_t *pool, int client_fd) {
    if (pool == NULL || !pool->is_started || client_fd < 0) {
        return -1;
    }

    return conn_queue_push(&pool->queue, client_fd);
}

void threadpool_shutdown(threadpool_t *pool) {
    if (pool == NULL || !pool->is_started) {
        return;
    }

    /* Signal queue shutdown and unblock all workers */
    conn_queue_shutdown(&pool->queue);

    /* Join all successfully created worker threads */
    for (size_t i = 0; i < pool->threads_created; i++) {
        int rc = pthread_join(pool->threads[i], NULL);
        if (rc != 0) {
            LOG_ERROR("pthread_join failed for worker thread %zu: %s", i, strerror(rc));
        }
    }

    conn_queue_destroy(&pool->queue);

    if (pool->contexts != NULL) {
        free(pool->contexts);
        pool->contexts = NULL;
    }
    if (pool->threads != NULL) {
        free(pool->threads);
        pool->threads = NULL;
    }

    pool->is_started = false;
    pool->threads_created = 0;
    LOG_INFO("Thread pool successfully shut down and all workers joined.");
}

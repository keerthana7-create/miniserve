#ifndef MINISERVE_SERVER_H
#define MINISERVE_SERVER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "hashtable.h"
#include "threadpool.h"

/**
 * Concurrency execution models supported across MiniServe phases.
 */
typedef enum {
    SERVER_MODE_SINGLE = 0,
    SERVER_MODE_THREADPOOL,
    SERVER_MODE_EPOLL
} server_mode_t;

/**
 * Runtime server configuration parameters parsed from CLI flags.
 */
typedef struct {
    uint16_t port;
    int num_threads;
    server_mode_t mode;
    char log_file[256];
    int max_connections;
    int idle_timeout_sec;
} server_config_t;

/**
 * MiniServe server runtime context.
 */
typedef struct {
    server_config_t config;
    hashtable_t *ht;
    int listen_fd;
    int signal_pipe[2];
    int epoll_fd;
    int *worker_active_fds;
    pthread_mutex_t state_lock;
    bool running;
    bool stop_requested;
    bool pool_shutting_down;
    threadpool_t pool;
} server_t;

/**
 * Populate a server_config_t struct with standard default options.
 *
 * @param config Pointer to the configuration struct to initialize.
 */
void server_config_init_defaults(server_config_t *config);

/**
 * Initialize the server instance: creates socket, sets SO_REUSEADDR, binds, and listens.
 *
 * @param server Pointer to server structure.
 * @param config Pointer to configuration struct.
 * @param ht Storage engine hash table.
 * @return 0 on success, -1 on failure.
 */
int server_init(server_t *server, const server_config_t *config, hashtable_t *ht);

/**
 * Start the blocking accept and request-processing loop.
 * Runs sequentially until server_stop is called or SIGINT/SIGTERM is received.
 * If server_stop was called prior to invocation, returns immediately without starting.
 *
 * @param server Pointer to initialized server structure.
 * @return 0 on clean shutdown, -1 on fatal error.
 */
int server_start(server_t *server);

/**
 * Signal the server to stop accepting connections and unblock active accept/recv calls.
 * Thread-safe and safe to invoke before, during, or after server_start().
 *
 * @param server Pointer to server structure.
 * @return 0 on success, -1 on failure.
 */
int server_stop(server_t *server);

/**
 * Clean up server resources, destroy mutex, and close listening socket.
 *
 * @param server Pointer to server structure.
 */
void server_cleanup(server_t *server);

/**
 * Convenience helper: initialize, run, and cleanup the server.
 *
 * @param config Pointer to configuration struct.
 * @param ht Pointer to hash table storage engine.
 * @return 0 on clean shutdown, -1 on failure.
 */
int server_run(const server_config_t *config, hashtable_t *ht);

/**
 * Verify that signal handling atomics are lock-free on the current platform.
 * Checked in ordinary execution context prior to installing signal handlers.
 *
 * @return true if both g_signal_shutdown and g_signal_pipe_write_fd are lock-free, false otherwise.
 */
bool server_signal_atomics_lock_free(void);

/**
 * Test hook: override lock-free check for unit testing initialization failure paths.
 *
 * @param simulate If true, forces server_signal_atomics_lock_free() to report false.
 */
void server_set_simulate_atomics_not_lock_free(bool simulate);

/**
 * Test hook: override output buffer limit for testing backpressure and overflow handling.
 *
 * @param limit Maximum capacity in bytes. If 0 or greater than CLIENT_OUT_BUF_SIZE,
 *              resets to standard CLIENT_OUT_BUF_SIZE (65536 bytes).
 */
void server_set_test_out_buf_limit(size_t limit);

/**
 * Test hook: override client socket SO_SNDBUF on accepted connections for testing backpressure.
 *
 * @param size Buffer size in bytes. If 0 or negative, leaves SO_SNDBUF as system default.
 */
void server_set_test_client_sndbuf(int size);

#endif /* MINISERVE_SERVER_H */

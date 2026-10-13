#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "logger.h"
#include "protocol.h"
#include "threadpool.h"
#include "util.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define CLIENT_BUFFER_SIZE 4096
#define CLIENT_DEFAULT_IDLE_TIMEOUT_SEC 30
#define CLIENT_POLL_TIMEOUT_MS 200

#define EPOLL_MAX_EVENTS 64
#define CLIENT_IN_BUF_SIZE 4096
#define CLIENT_OUT_BUF_SIZE 65536

/**
 * Lock-free C11 atomic shutdown flag and self-pipe write descriptor modified strictly
 * by async-signal-safe handlers.
 *
 * Synchronization design:
 *   - Both g_signal_shutdown and g_signal_pipe_write_fd are verified to be lock-free at runtime
 *     using atomic_is_lock_free() in ordinary execution context (server_init and server_setup_signals)
 *     before installing signal handlers.
 *   - If either atomic is not lock-free on the target platform, server initialization fails cleanly
 *     before any handlers are installed or resources allocated.
 *   - Inside the signal handler, only async-signal-safe operations are executed: memory_order_relaxed
 *     atomic store/load and POSIX write() on the non-blocking self-pipe.
 *   - Server worker and accept threads observe the atomic shutdown flag using memory_order_acquire.
 */
static atomic_int g_signal_shutdown = 0;
static atomic_int g_signal_pipe_write_fd = -1;

/* Test simulation flag for validating clean initialization failure on platforms without lock-free atomics */
static bool g_simulate_atomics_not_lock_free = false;

void server_set_simulate_atomics_not_lock_free(bool simulate) {
    g_simulate_atomics_not_lock_free = simulate;
}

bool server_signal_atomics_lock_free(void) {
    if (g_simulate_atomics_not_lock_free) {
        return false;
    }
    return atomic_is_lock_free(&g_signal_shutdown) &&
           atomic_is_lock_free(&g_signal_pipe_write_fd);
}

static void server_sigint_handler(int signum) {
    (void)signum;
    atomic_store_explicit(&g_signal_shutdown, 1, memory_order_relaxed);
    int wfd = atomic_load_explicit(&g_signal_pipe_write_fd, memory_order_relaxed);
    if (wfd >= 0) {
        char byte = 'S';
        (void)write(wfd, &byte, 1);
    }
}

static int server_setup_signals(void) {
    /* Verify in ordinary execution context that both atomics are lock-free before installing handlers */
    if (!server_signal_atomics_lock_free()) {
        LOG_FATAL("Signal atomics are not lock-free on this target; cannot safely install signal handlers");
        return -1;
    }

    /* Ignore SIGPIPE so writing to disconnected peers generates EPIPE instead of terminating */
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa_pipe, NULL) < 0) {
        LOG_ERROR("sigaction(SIGPIPE) failed: %s", strerror(errno));
        return -1;
    }

    /* Handle SIGINT, SIGTERM, and SIGQUIT without SA_RESTART so blocking system calls unblock with EINTR */
    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = server_sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    if (sigaction(SIGINT, &sa_int, NULL) < 0) {
        LOG_ERROR("sigaction(SIGINT) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGTERM, &sa_int, NULL) < 0) {
        LOG_ERROR("sigaction(SIGTERM) failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGQUIT, &sa_int, NULL) < 0) {
        LOG_ERROR("sigaction(SIGQUIT) failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

void server_config_init_defaults(server_config_t *config) {
    if (config == NULL) {
        return;
    }

    config->port = 8080;
    config->num_threads = 4;
    config->mode = SERVER_MODE_THREADPOOL;
    config->log_file[0] = '\0';
    config->max_connections = 1024;
    config->idle_timeout_sec = CLIENT_DEFAULT_IDLE_TIMEOUT_SEC;
    config->max_keys = 0;
}

/**
 * Thread-safe check of server run state combining signal and cross-thread shutdown flags.
 */
static bool server_is_running(server_t *server) {
    if (server == NULL) {
        return false;
    }
    if (atomic_load_explicit(&g_signal_shutdown, memory_order_acquire) != 0) {
        return false;
    }
    pthread_mutex_lock(&server->state_lock);
    bool is_running = server->running && !server->stop_requested;
    pthread_mutex_unlock(&server->state_lock);
    return is_running;
}

/**
 * Robustly send all bytes over a stream socket, handling partial writes and EINTR.
 */
static ssize_t send_all(int fd, const char *buf, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = send(fd, buf + total_sent, len - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        total_sent += (size_t)sent;
    }
    return (ssize_t)total_sent;
}

/**
 * Client request handler for a connection executed by a worker thread (or main thread in single mode).
 * Manages stream buffering, line extraction, protocol execution, response delivery,
 * and responsive shutdown during idle client periods using sliced polling and Slowloris protection.
 */
static void server_handle_client(server_t *server, int client_fd, int worker_id) {
    pthread_mutex_lock(&server->state_lock);
    if (!server->running || server->stop_requested) {
        pthread_mutex_unlock(&server->state_lock);
        if (server->config.mode == SERVER_MODE_SINGLE) {
            close(client_fd);
        }
        return;
    }
    if (server->worker_active_fds != NULL) {
        server->worker_active_fds[worker_id] = client_fd;
    }
    int idle_limit_sec = (server->config.idle_timeout_sec > 0) ?
                         server->config.idle_timeout_sec : CLIENT_DEFAULT_IDLE_TIMEOUT_SEC;
    pthread_mutex_unlock(&server->state_lock);

    char buffer[CLIENT_BUFFER_SIZE];
    size_t buf_len = 0;
    bool discarding_oversized = false;
    int idle_accum_ms = 0;
    int line_accum_ms = 0;

    while (server_is_running(server)) {
        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        int poll_ret = poll(&pfd, 1, CLIENT_POLL_TIMEOUT_MS);
        if (poll_ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_WARN("poll() failed on client fd %d: %s", client_fd, strerror(errno));
            break;
        }

        if (poll_ret == 0) {
            /* Sliced timeout interval elapsed; check server run state and idle bounds */
            if (!server_is_running(server)) {
                break;
            }
            idle_accum_ms += CLIENT_POLL_TIMEOUT_MS;
            if (buf_len > 0) {
                line_accum_ms += CLIENT_POLL_TIMEOUT_MS;
            }
            if (idle_accum_ms >= idle_limit_sec * 1000) {
                LOG_WARN("Client fd %d timed out after %d seconds of inactivity", client_fd, idle_limit_sec);
                break;
            }
            if (line_accum_ms >= idle_limit_sec * 1000) {
                LOG_WARN("Client fd %d timed out waiting for complete request line (slowloris protection)", client_fd);
                break;
            }
            continue;
        }

        idle_accum_ms = 0;

        ssize_t bytes_read = recv(client_fd, buffer + buf_len, sizeof(buffer) - 1 - buf_len, 0);
        if (bytes_read > 0) {
            buf_len += (size_t)bytes_read;
            buffer[buf_len] = '\0';

            bool close_connection = false;

            /* Parse and execute all complete newline-delimited commands in the buffer */
            while (buf_len > 0) {
                if (discarding_oversized) {
                    char *newline_pos = memchr(buffer, '\n', buf_len);
                    if (newline_pos == NULL) {
                        /* Discard all buffered bytes for this oversized command and continue */
                        buf_len = 0;
                        buffer[0] = '\0';
                        line_accum_ms = 0;
                        break;
                    }

                    /* End of oversized command encountered; discard up to and including \n */
                    size_t discarded_len = (size_t)(newline_pos - buffer) + 1;
                    size_t remaining = buf_len - discarded_len;
                    memmove(buffer, newline_pos + 1, remaining);
                    buf_len = remaining;
                    buffer[buf_len] = '\0';
                    discarding_oversized = false;
                    line_accum_ms = 0;
                    continue;
                }

                char *newline_pos = memchr(buffer, '\n', buf_len);
                if (newline_pos == NULL) {
                    /* Promptly reject unfinished lines exceeding 1024-byte protocol limit */
                    if (buf_len > PROTO_MAX_LINE_LEN) {
                        const char *err = "ERR OVERSIZED Command line exceeds 1024 bytes\r\n";
                        if (send_all(client_fd, err, strlen(err)) < 0) {
                            close_connection = true;
                            break;
                        }
                        discarding_oversized = true;
                        buf_len = 0;
                        buffer[0] = '\0';
                        line_accum_ms = 0;
                    }
                    break; /* Await remainder of line */
                }

                /* A full line has arrived; reset Slowloris cumulative line timer */
                line_accum_ms = 0;

                size_t line_len = (size_t)(newline_pos - buffer) + 1;
                if (line_len > PROTO_MAX_LINE_LEN) {
                    const char *err = "ERR OVERSIZED Command line exceeds 1024 bytes\r\n";
                    if (send_all(client_fd, err, strlen(err)) < 0) {
                        close_connection = true;
                        break;
                    }
                } else if (memchr(buffer, '\0', line_len) != NULL) {
                    /* Reject request lines containing embedded NUL bytes */
                    const char *err = "ERR MALFORMED Control characters not allowed\r\n";
                    if (send_all(client_fd, err, strlen(err)) < 0) {
                        close_connection = true;
                        break;
                    }
                } else {
                    char line[PROTO_MAX_LINE_LEN + 1];
                    memcpy(line, buffer, line_len);
                    line[line_len] = '\0';

                    command_response_t res;
                    protocol_process_line(line, server->ht, &res);

                    if (res.response[0] != '\0') {
                        if (send_all(client_fd, res.response, strlen(res.response)) < 0) {
                            close_connection = true;
                            break;
                        }
                    }

                    if (res.close_connection) {
                        close_connection = true;
                        break;
                    }
                }

                /* Shift remaining bytes to front of buffer */
                size_t remaining = buf_len - line_len;
                memmove(buffer, newline_pos + 1, remaining);
                buf_len = remaining;
                buffer[buf_len] = '\0';
                if (buf_len == 0) {
                    line_accum_ms = 0;
                }
            }

            if (close_connection) {
                break;
            }
        } else if (bytes_read == 0) {
            /* Clean client disconnect (EOF) */
            LOG_DEBUG("Client fd %d disconnected cleanly (EOF)", client_fd);
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            LOG_WARN("Client fd %d read error: %s", client_fd, strerror(errno));
            break;
        }
    }

    pthread_mutex_lock(&server->state_lock);
    if (server->worker_active_fds != NULL) {
        server->worker_active_fds[worker_id] = -1;
    }
    pthread_mutex_unlock(&server->state_lock);

    /* In single-threaded mode, close client_fd here.
     * In threadpool mode, the worker loop closes client_fd immediately after task returns. */
    if (server->config.mode == SERVER_MODE_SINGLE) {
        close(client_fd);
        LOG_INFO("Connection closed for client fd %d", client_fd);
    }
}

/**
 * Worker thread callback routine for threadpool mode.
 */
static void server_worker_task(int client_fd, int worker_id, void *arg) {
    server_t *server = (server_t *)arg;
    server_handle_client(server, client_fd, worker_id);
}

/* ============================================================================
 * Phase 6: Linux Epoll Event Loop Implementation
 * ============================================================================ */

typedef enum {
    EVENT_SOURCE_LISTEN = 1,
    EVENT_SOURCE_SIGNAL = 2,
    EVENT_SOURCE_CLIENT = 3
} event_source_type_t;

typedef struct event_source {
    event_source_type_t type;
    int fd;
} event_source_t;

typedef struct client_conn {
    event_source_type_t type; /* Must be first field matching event_source_t */
    int fd;
    char in_buf[CLIENT_IN_BUF_SIZE];
    size_t in_len;
    char out_buf[CLIENT_OUT_BUF_SIZE];
    size_t out_len;
    size_t out_sent;
    bool close_after_write;
    bool discarding_oversized;
    bool epollout_active;
    time_t last_activity;
    time_t line_start_time;
    struct client_conn *prev;
    struct client_conn *next;
} client_conn_t;

/**
 * Close an active epoll client connection and free its state safely.
 * The descriptor is removed from epoll and closed exactly once.
 */
static void epoll_close_client(int epoll_fd, client_conn_t *conn, client_conn_t **head, int *active_count) {
    if (conn == NULL) {
        return;
    }

    /* 1. Remove from epoll instance */
    if (conn->fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
        close(conn->fd);
        conn->fd = -1;
    }

    /* 2. Unlink from active connections list */
    if (conn->prev != NULL) {
        conn->prev->next = conn->next;
    }
    if (conn->next != NULL) {
        conn->next->prev = conn->prev;
    }
    if (*head == conn) {
        *head = conn->next;
    }

    if (active_count != NULL && *active_count > 0) {
        (*active_count)--;
    }

    /* 3. Free heap-allocated connection state */
    free(conn);
}

/**
 * Attempt to flush buffered output data to a non-blocking client socket.
 * If socket send buffer is full, registers EPOLLOUT to await writeability.
 * When buffer is completely flushed, removes EPOLLOUT to prevent busy-spinning.
 * Returns true if connection remains alive, false if connection was closed and freed.
 */
static bool epoll_client_try_flush(int epoll_fd, client_conn_t *conn, client_conn_t **head, int *active_count) {
    if (conn == NULL) {
        return false;
    }

    size_t unsent = conn->out_len - conn->out_sent;
    while (unsent > 0) {
        ssize_t sent = send(conn->fd, conn->out_buf + conn->out_sent, unsent, MSG_NOSIGNAL);
        if (sent > 0) {
            conn->out_sent += (size_t)sent;
            unsent = conn->out_len - conn->out_sent;
        } else if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Kernel buffer full; register EPOLLOUT to wait for writeability */
                if (!conn->epollout_active) {
                    struct epoll_event ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.events = (conn->close_after_write ? 0 : EPOLLIN) | EPOLLOUT | EPOLLRDHUP;
                    ev.data.ptr = conn;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
                        LOG_WARN("epoll_ctl MOD EPOLLOUT failed for fd %d: %s", conn->fd, strerror(errno));
                        epoll_close_client(epoll_fd, conn, head, active_count);
                        return false;
                    }
                    conn->epollout_active = true;
                }
                return true;
            }
            /* Socket write error (EPIPE, ECONNRESET, etc.) */
            LOG_DEBUG("Write error on epoll client fd %d: %s", conn->fd, strerror(errno));
            epoll_close_client(epoll_fd, conn, head, active_count);
            return false;
        } else {
            epoll_close_client(epoll_fd, conn, head, active_count);
            return false;
        }
    }

    /* All pending bytes sent; reset offsets */
    conn->out_len = 0;
    conn->out_sent = 0;

    /* If command requested QUIT or connection termination, close promptly */
    if (conn->close_after_write) {
        epoll_close_client(epoll_fd, conn, head, active_count);
        return false;
    }

    /* Remove EPOLLOUT interest so level-triggered epoll does not busy-spin */
    if (conn->epollout_active) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.ptr = conn;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev) < 0) {
            LOG_WARN("epoll_ctl MOD remove EPOLLOUT failed for fd %d: %s", conn->fd, strerror(errno));
            epoll_close_client(epoll_fd, conn, head, active_count);
            return false;
        }
        conn->epollout_active = false;
    }

    return true;
}

static volatile atomic_size_t g_test_out_buf_limit = ATOMIC_VAR_INIT(CLIENT_OUT_BUF_SIZE);

void server_set_test_out_buf_limit(size_t limit) {
    if (limit == 0 || limit > CLIENT_OUT_BUF_SIZE) {
        atomic_store_explicit(&g_test_out_buf_limit, CLIENT_OUT_BUF_SIZE, memory_order_release);
    } else {
        atomic_store_explicit(&g_test_out_buf_limit, limit, memory_order_release);
    }
}

static size_t server_get_out_buf_limit(void) {
    size_t limit = atomic_load_explicit(&g_test_out_buf_limit, memory_order_acquire);
    if (limit == 0 || limit > CLIENT_OUT_BUF_SIZE) {
        return CLIENT_OUT_BUF_SIZE;
    }
    return limit;
}

static volatile atomic_int g_test_client_sndbuf = ATOMIC_VAR_INIT(0);

void server_set_test_client_sndbuf(int size) {
    atomic_store_explicit(&g_test_client_sndbuf, size, memory_order_release);
}

/**
 * Append serialized protocol response to client output buffer.
 */
static bool epoll_client_append_response(client_conn_t *conn, const char *data, size_t len) {
    if (len == 0) {
        return true;
    }

    /* Compact buffer if head space has accumulated */
    if (conn->out_sent > 0) {
        size_t remaining = conn->out_len - conn->out_sent;
        if (remaining > 0) {
            memmove(conn->out_buf, conn->out_buf + conn->out_sent, remaining);
        }
        conn->out_len = remaining;
        conn->out_sent = 0;
    }

    size_t max_buf = server_get_out_buf_limit();
    if (conn->out_len + len > max_buf) {
        LOG_WARN("Epoll client fd %d output buffer overflow (len %zu exceeds capacity %zu)",
                 conn->fd, conn->out_len + len, max_buf);
        return false;
    }

    memcpy(conn->out_buf + conn->out_len, data, len);
    conn->out_len += len;
    return true;
}

/**
 * Append serialized protocol response to client output buffer.
 * If the buffer capacity is exceeded, attempts an immediate flush to relieve backpressure.
 * If capacity remains exceeded after flush, terminates the overflowing connection safely
 * to prevent unbounded memory consumption and stream desynchronization.
 * Returns true if connection remains alive, false if connection was closed and freed.
 */
static bool epoll_client_send_or_buffer(int epoll_fd, client_conn_t *conn,
                                        const char *data, size_t len,
                                        client_conn_t **head, int *active_count) {
    if (epoll_client_append_response(conn, data, len)) {
        return true;
    }

    /* Output buffer capacity exceeded; attempt immediate flush of existing buffered data */
    if (!epoll_client_try_flush(epoll_fd, conn, head, active_count)) {
        return false; /* Connection closed during flush (e.g. write error or QUIT) */
    }

    /* Retry appending after buffer compaction */
    if (epoll_client_append_response(conn, data, len)) {
        return true;
    }

    /* Backpressure policy: unrecoverable output buffer overflow terminates client connection safely */
    size_t max_buf = server_get_out_buf_limit();
    LOG_WARN("Output buffer capacity (%zu bytes) exceeded for client fd %d; terminating connection",
             max_buf, conn->fd);
    epoll_close_client(epoll_fd, conn, head, active_count);
    return false;
}

/**
 * Process readable incoming data for an active epoll client socket.
 * Returns true if connection remains alive, false if connection was closed and freed.
 */
static bool epoll_handle_client_read(int epoll_fd, server_t *server, client_conn_t *conn,
                                     client_conn_t **head, int *active_count) {
    if (conn == NULL) {
        return false;
    }

    while (true) {
        size_t avail = sizeof(conn->in_buf) - 1 - conn->in_len;
        if (avail == 0) {
            /* Input buffer full without a newline */
            break;
        }

        ssize_t bytes_read = recv(conn->fd, conn->in_buf + conn->in_len, avail, 0);
        if (bytes_read > 0) {
            conn->in_len += (size_t)bytes_read;
            conn->in_buf[conn->in_len] = '\0';
            conn->last_activity = time(NULL);

            /* Parse and execute all complete lines in buffer */
            while (conn->in_len > 0) {
                if (conn->discarding_oversized) {
                    char *newline_pos = memchr(conn->in_buf, '\n', conn->in_len);
                    if (newline_pos == NULL) {
                        conn->in_len = 0;
                        conn->in_buf[0] = '\0';
                        conn->line_start_time = 0;
                        break;
                    }
                    size_t discarded_len = (size_t)(newline_pos - conn->in_buf) + 1;
                    size_t remaining = conn->in_len - discarded_len;
                    memmove(conn->in_buf, newline_pos + 1, remaining);
                    conn->in_len = remaining;
                    conn->in_buf[conn->in_len] = '\0';
                    conn->discarding_oversized = false;
                    conn->line_start_time = 0;
                    continue;
                }

                char *newline_pos = memchr(conn->in_buf, '\n', conn->in_len);
                if (newline_pos == NULL) {
                    /* Check prompt rejection of oversized unfinished line */
                    if (conn->in_len > PROTO_MAX_LINE_LEN) {
                        const char *err = "ERR OVERSIZED Command line exceeds 1024 bytes\r\n";
                        if (!epoll_client_send_or_buffer(epoll_fd, conn, err, strlen(err), head, active_count)) {
                            return false;
                        }
                        conn->discarding_oversized = true;
                        conn->in_len = 0;
                        conn->in_buf[0] = '\0';
                        conn->line_start_time = 0;
                    } else if (conn->line_start_time == 0) {
                        conn->line_start_time = time(NULL);
                    }
                    break; /* Await remainder of command line */
                }

                /* Complete line received; reset line timer */
                conn->line_start_time = 0;
                size_t line_len = (size_t)(newline_pos - conn->in_buf) + 1;

                if (line_len > PROTO_MAX_LINE_LEN) {
                    const char *err = "ERR OVERSIZED Command line exceeds 1024 bytes\r\n";
                    if (!epoll_client_send_or_buffer(epoll_fd, conn, err, strlen(err), head, active_count)) {
                        return false;
                    }
                } else if (memchr(conn->in_buf, '\0', line_len) != NULL) {
                    /* Reject request lines containing embedded NUL bytes */
                    const char *err = "ERR MALFORMED Control characters not allowed\r\n";
                    if (!epoll_client_send_or_buffer(epoll_fd, conn, err, strlen(err), head, active_count)) {
                        return false;
                    }
                } else {
                    char line[PROTO_MAX_LINE_LEN + 1];
                    memcpy(line, conn->in_buf, line_len);
                    line[line_len] = '\0';

                    command_response_t res;
                    protocol_process_line(line, server->ht, &res);

                    if (res.response[0] != '\0') {
                        if (!epoll_client_send_or_buffer(epoll_fd, conn, res.response, strlen(res.response), head, active_count)) {
                            return false;
                        }
                    }
                    if (res.close_connection) {
                        conn->close_after_write = true;
                    }
                }

                /* Shift remaining bytes in input buffer */
                size_t remaining = conn->in_len - line_len;
                memmove(conn->in_buf, newline_pos + 1, remaining);
                conn->in_len = remaining;
                conn->in_buf[conn->in_len] = '\0';

                /* If connection was marked to close after write (e.g. QUIT), stop parsing */
                if (conn->close_after_write) {
                    break;
                }
            }

            /* Attempt immediate flush of responses */
            if (!epoll_client_try_flush(epoll_fd, conn, head, active_count)) {
                return false; /* Connection was closed and freed */
            }

            /* If connection is closing after write, stop reading further data */
            if (conn->close_after_write) {
                break;
            }

            /* If less bytes read than available, socket buffer has been drained */
            if ((size_t)bytes_read < avail) {
                break;
            }
        } else if (bytes_read == 0) {
            /* Client disconnected cleanly (EOF) */
            LOG_DEBUG("Epoll client fd %d disconnected cleanly (EOF)", conn->fd);
            epoll_close_client(epoll_fd, conn, head, active_count);
            return false;
        } else {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Socket buffer drained */
                break;
            }
            LOG_WARN("Recv error on epoll client fd %d: %s", conn->fd, strerror(errno));
            epoll_close_client(epoll_fd, conn, head, active_count);
            return false;
        }
    }

    return true;
}

/**
 * Handle new incoming connections on listening socket in non-blocking epoll mode.
 */
static void epoll_handle_accept(int epoll_fd, server_t *server, client_conn_t **head, int *active_count) {
    int max_conn = (server->config.max_connections > 0) ? server->config.max_connections : 1024;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* All pending incoming connections drained */
                break;
            }
            if (errno == EMFILE || errno == ENFILE) {
                LOG_WARN("File descriptor table full (EMFILE/ENFILE) in epoll; backing off 20ms...");
                usleep(20000);
                break;
            }
            if (errno == ECONNABORTED) {
                continue;
            }
            LOG_ERROR("epoll accept() failed: %s", strerror(errno));
            break;
        }

        /* Set non-blocking mode on client descriptor */
        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            LOG_WARN("Failed to set client fd %d to non-blocking mode: %s; closing descriptor",
                     client_fd, strerror(errno));
            close(client_fd);
            continue;
        }

        int fdflags = fcntl(client_fd, F_GETFD, 0);
        if (fdflags >= 0) {
            fcntl(client_fd, F_SETFD, fdflags | FD_CLOEXEC);
        }

        int test_sndbuf = atomic_load_explicit(&g_test_client_sndbuf, memory_order_acquire);
        if (test_sndbuf > 0) {
            setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &test_sndbuf, sizeof(test_sndbuf));
        }

        /* Enforce configured maximum connection limit */
        if (*active_count >= max_conn) {
            LOG_WARN("Epoll connection limit reached (%d); rejecting client fd %d", max_conn, client_fd);
            const char *err = "ERR OVERLOAD Maximum connection limit reached\r\n";
            (void)send(client_fd, err, strlen(err), MSG_NOSIGNAL);
            close(client_fd);
            continue;
        }

        client_conn_t *conn = calloc(1, sizeof(client_conn_t));
        if (conn == NULL) {
            LOG_ERROR("Failed to allocate client_conn_t for fd %d", client_fd);
            close(client_fd);
            continue;
        }

        conn->type = EVENT_SOURCE_CLIENT;
        conn->fd = client_fd;
        conn->last_activity = time(NULL);
        conn->line_start_time = 0;
        conn->epollout_active = false;

        /* Add to active connections list */
        conn->next = *head;
        if (*head != NULL) {
            (*head)->prev = conn;
        }
        *head = conn;
        (*active_count)++;

        /* Register with epoll instance */
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.ptr = conn;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            LOG_ERROR("epoll_ctl ADD failed for client fd %d: %s", client_fd, strerror(errno));
            epoll_close_client(epoll_fd, conn, head, active_count);
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
            strncpy(client_ip, "unknown", sizeof(client_ip));
            client_ip[sizeof(client_ip) - 1] = '\0';
        }
        uint16_t client_port = ntohs(client_addr.sin_port);
        LOG_INFO("Epoll accepted connection from %s:%u (fd: %d, active: %d/%d)",
                 client_ip, client_port, client_fd, *active_count, max_conn);
    }
}

/**
 * Main event loop for Phase 6 epoll execution mode.
 */
static int server_start_epoll(server_t *server) {
    /* 1. Set listen_fd to non-blocking */
    int lflags = fcntl(server->listen_fd, F_GETFL, 0);
    if (lflags < 0 || fcntl(server->listen_fd, F_SETFL, lflags | O_NONBLOCK) < 0) {
        LOG_FATAL("Failed to set listening socket to non-blocking mode: %s", strerror(errno));
        pthread_mutex_lock(&server->state_lock);
        server->running = false;
        pthread_mutex_unlock(&server->state_lock);
        return -1;
    }

    /* 2. Create epoll instance with close-on-exec */
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        LOG_FATAL("epoll_create1() failed: %s", strerror(errno));
        fcntl(server->listen_fd, F_SETFL, lflags);
        pthread_mutex_lock(&server->state_lock);
        server->running = false;
        pthread_mutex_unlock(&server->state_lock);
        return -1;
    }

    pthread_mutex_lock(&server->state_lock);
    server->epoll_fd = epoll_fd;
    pthread_mutex_unlock(&server->state_lock);

    /* 3. Register listen_fd and signal_pipe[0] */
    event_source_t listen_source = { .type = EVENT_SOURCE_LISTEN, .fd = server->listen_fd };
    event_source_t signal_source = { .type = EVENT_SOURCE_SIGNAL, .fd = server->signal_pipe[0] };

    struct epoll_event ev_listen;
    memset(&ev_listen, 0, sizeof(ev_listen));
    ev_listen.events = EPOLLIN;
    ev_listen.data.ptr = &listen_source;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server->listen_fd, &ev_listen) < 0) {
        LOG_FATAL("epoll_ctl ADD listen_fd failed: %s", strerror(errno));
        close(epoll_fd);
        pthread_mutex_lock(&server->state_lock);
        server->epoll_fd = -1;
        server->running = false;
        pthread_mutex_unlock(&server->state_lock);
        fcntl(server->listen_fd, F_SETFL, lflags);
        return -1;
    }

    struct epoll_event ev_signal;
    memset(&ev_signal, 0, sizeof(ev_signal));
    ev_signal.events = EPOLLIN;
    ev_signal.data.ptr = &signal_source;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server->signal_pipe[0], &ev_signal) < 0) {
        LOG_FATAL("epoll_ctl ADD signal_pipe[0] failed: %s", strerror(errno));
        close(epoll_fd);
        pthread_mutex_lock(&server->state_lock);
        server->epoll_fd = -1;
        server->running = false;
        pthread_mutex_unlock(&server->state_lock);
        fcntl(server->listen_fd, F_SETFL, lflags);
        return -1;
    }

    client_conn_t *active_conns = NULL;
    int active_count = 0;
    int idle_timeout = (server->config.idle_timeout_sec > 0) ?
                       server->config.idle_timeout_sec : CLIENT_DEFAULT_IDLE_TIMEOUT_SEC;

    struct epoll_event events[EPOLL_MAX_EVENTS];

    LOG_INFO("Epoll event loop started on port %u (max_conn: %d, idle_timeout: %ds)",
             server->config.port, server->config.max_connections, idle_timeout);

    while (server_is_running(server)) {
        int nready = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, 500); /* 500ms sliced timeout */
        if (nready < 0) {
            if (errno == EINTR) {
                if (!server_is_running(server)) {
                    break;
                }
                continue;
            }
            LOG_ERROR("epoll_wait() failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nready; i++) {
            event_source_t *source = (event_source_t *)events[i].data.ptr;
            if (source == NULL) {
                continue;
            }

            if (source->type == EVENT_SOURCE_SIGNAL) {
                /* Signal or shutdown received; drain self-pipe */
                char drain_buf[64];
                while (read(server->signal_pipe[0], drain_buf, sizeof(drain_buf)) > 0) {}
                if (!server_is_running(server)) {
                    break;
                }
            } else if (source->type == EVENT_SOURCE_LISTEN) {
                epoll_handle_accept(epoll_fd, server, &active_conns, &active_count);
            } else if (source->type == EVENT_SOURCE_CLIENT) {
                client_conn_t *conn = (client_conn_t *)source;
                uint32_t revents = events[i].events;
                bool conn_alive = true;

                /* Handle incoming data */
                if (revents & EPOLLIN) {
                    conn_alive = epoll_handle_client_read(epoll_fd, server, conn, &active_conns, &active_count);
                }

                /* If connection still alive, handle writeability */
                if (conn_alive && (revents & EPOLLOUT)) {
                    conn_alive = epoll_client_try_flush(epoll_fd, conn, &active_conns, &active_count);
                }

                /* If connection still alive, handle disconnect or error */
                if (conn_alive && (revents & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))) {
                    if ((revents & (EPOLLHUP | EPOLLERR)) != 0 || (conn->out_len - conn->out_sent == 0)) {
                        epoll_close_client(epoll_fd, conn, &active_conns, &active_count);
                        conn_alive = false;
                    } else {
                        /* Peer closed write half (EPOLLRDHUP) but pending output remains */
                        conn->close_after_write = true;
                    }
                }
            }
        }

        if (!server_is_running(server)) {
            break;
        }

        /* Periodic idle timeout & slowloris sweep */
        time_t now = time(NULL);
        client_conn_t *curr = active_conns;
        while (curr != NULL) {
            client_conn_t *next = curr->next;
            if (idle_timeout > 0) {
                if (now - curr->last_activity >= idle_timeout) {
                    LOG_WARN("Epoll client fd %d timed out after %d sec of inactivity", curr->fd, idle_timeout);
                    epoll_close_client(epoll_fd, curr, &active_conns, &active_count);
                    curr = next;
                    continue;
                }
                if (curr->line_start_time > 0 && (now - curr->line_start_time >= idle_timeout)) {
                    LOG_WARN("Epoll client fd %d timed out waiting for full command line (slowloris)", curr->fd);
                    epoll_close_client(epoll_fd, curr, &active_conns, &active_count);
                    curr = next;
                    continue;
                }
            }
            curr = next;
        }
    }

    /* Teardown: close all remaining active client connections cleanly */
    while (active_conns != NULL) {
        epoll_close_client(epoll_fd, active_conns, &active_conns, &active_count);
    }

    /* Close epoll instance */
    close(epoll_fd);

    pthread_mutex_lock(&server->state_lock);
    server->epoll_fd = -1;
    server->running = false;
    pthread_mutex_unlock(&server->state_lock);

    /* Restore blocking mode on listen_fd */
    if (lflags >= 0) {
        fcntl(server->listen_fd, F_SETFL, lflags);
    }

    LOG_INFO("Epoll event loop finished cleanly.");
    return 0;
}

int server_init(server_t *server, const server_config_t *config, hashtable_t *ht) {
    if (server == NULL || config == NULL || ht == NULL) {
        return -1;
    }

    /* Verify in ordinary execution context that both signal atomics are lock-free
     * before allocating resources or installing signal handlers. If either is not
     * lock-free, fail initialization cleanly with -1 without assuming atomic_int is
     * lock-free on every target platform. */
    if (!server_signal_atomics_lock_free()) {
        LOG_FATAL("Signal atomics are not lock-free on this target architecture; cannot safely initialize server");
        return -1;
    }

    server->config = *config;
    server->ht = ht;
    server->listen_fd = -1;
    server->signal_pipe[0] = -1;
    server->signal_pipe[1] = -1;
    server->epoll_fd = -1;
    server->worker_active_fds = NULL;
    server->running = false;
    server->stop_requested = false;
    server->pool_shutting_down = false;
    memset(&server->pool, 0, sizeof(server->pool));

    if (pthread_mutex_init(&server->state_lock, NULL) != 0) {
        LOG_FATAL("pthread_mutex_init() failed: %s", strerror(errno));
        return -1;
    }

    int num_workers = (server->config.mode == SERVER_MODE_SINGLE || server->config.mode == SERVER_MODE_EPOLL) ?
                      1 : server->config.num_threads;
    if (num_workers < 1) {
        num_workers = 1;
    }
    server->worker_active_fds = calloc((size_t)num_workers, sizeof(int));
    if (server->worker_active_fds == NULL) {
        LOG_FATAL("Failed to allocate active worker descriptors array");
        pthread_mutex_destroy(&server->state_lock);
        return -1;
    }
    for (int i = 0; i < num_workers; i++) {
        server->worker_active_fds[i] = -1;
    }

    /* Initialize self-pipe for async-signal-safe shutdown notification */
    if (pipe(server->signal_pipe) < 0) {
        LOG_FATAL("pipe() failed for signal self-pipe: %s", strerror(errno));
        goto err_mutex;
    }

    for (int i = 0; i < 2; i++) {
        int flags = fcntl(server->signal_pipe[i], F_GETFL, 0);
        if (flags >= 0) {
            fcntl(server->signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
        int fdflags = fcntl(server->signal_pipe[i], F_GETFD, 0);
        if (fdflags >= 0) {
            fcntl(server->signal_pipe[i], F_SETFD, fdflags | FD_CLOEXEC);
        }
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_FATAL("socket() failed: %s", strerror(errno));
        goto err_pipe;
    }

    /* Set close-on-exec on listening socket to prevent child descriptor leaks */
    int lflags = fcntl(listen_fd, F_GETFD, 0);
    if (lflags >= 0) {
        fcntl(listen_fd, F_SETFD, lflags | FD_CLOEXEC);
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_FATAL("setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
        goto err_socket;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(server->config.port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_FATAL("bind() on port %u failed: %s", server->config.port, strerror(errno));
        goto err_socket;
    }

    /* If port was configured as 0 (ephemeral), retrieve kernel-assigned port.
     * Fail initialization cleanly if getsockname() fails or returns an invalid port,
     * ensuring shutdown is never left with an unknown port. */
    if (server->config.port == 0) {
        struct sockaddr_in bound_addr;
        socklen_t addr_len = sizeof(bound_addr);
        if (getsockname(listen_fd, (struct sockaddr *)&bound_addr, &addr_len) < 0) {
            LOG_FATAL("getsockname() failed to retrieve ephemeral port: %s", strerror(errno));
            goto err_socket;
        }
        uint16_t assigned_port = ntohs(bound_addr.sin_port);
        if (assigned_port == 0) {
            LOG_FATAL("getsockname() returned invalid ephemeral port 0");
            goto err_socket;
        }
        server->config.port = assigned_port;
    }

    int backlog = 128;
    if (listen(listen_fd, backlog) < 0) {
        LOG_FATAL("listen() failed: %s", strerror(errno));
        goto err_socket;
    }

    server->listen_fd = listen_fd;
    LOG_INFO("Server listening on port %u (backlog: %d, self-pipe: [%d,%d])",
             server->config.port, backlog, server->signal_pipe[0], server->signal_pipe[1]);
    return 0;

err_socket:
    close(listen_fd);
err_pipe:
    close(server->signal_pipe[0]);
    close(server->signal_pipe[1]);
    server->signal_pipe[0] = -1;
    server->signal_pipe[1] = -1;
err_mutex:
    free(server->worker_active_fds);
    server->worker_active_fds = NULL;
    pthread_mutex_destroy(&server->state_lock);
    return -1;
}

/**
 * Safely shuts down the worker thread pool, joins all worker threads,
 * and destroys queue synchronization primitives with clear single ownership.
 */
static void server_teardown_pool(server_t *server) {
    if (server == NULL) {
        return;
    }

    pthread_mutex_lock(&server->state_lock);
    if (server->config.mode != SERVER_MODE_THREADPOOL ||
        !server->pool.is_started ||
        server->pool_shutting_down) {
        pthread_mutex_unlock(&server->state_lock);
        return;
    }

    server->pool_shutting_down = true;

    /* Signal queue shutdown so workers and producers unblock immediately */
    conn_queue_shutdown(&server->pool.queue);
    size_t num_threads = server->pool.threads_created;
    pthread_t *threads = server->pool.threads;
    worker_context_t *contexts = server->pool.contexts;
    pthread_mutex_unlock(&server->state_lock);

    /* Join all worker threads OUTSIDE state_lock to prevent lock-inversion deadlock */
    for (size_t i = 0; i < num_threads; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            LOG_ERROR("pthread_join failed for worker thread %zu: %s", i, strerror(rc));
        }
    }

    /* All workers have terminated and joined. No worker thread or producer
     * will ever access the queue again. Destroy queue primitives and release
     * memory under state_lock to prevent any overlap with server_stop(). */
    pthread_mutex_lock(&server->state_lock);
    conn_queue_destroy(&server->pool.queue);

    if (contexts != NULL) {
        free(contexts);
    }
    if (threads != NULL) {
        free(threads);
    }

    server->pool.threads = NULL;
    server->pool.contexts = NULL;
    server->pool.threads_created = 0;
    server->pool.is_started = false;
    server->pool_shutting_down = false;
    pthread_mutex_unlock(&server->state_lock);

    LOG_INFO("Thread pool successfully shut down, all workers joined, and queue destroyed.");
}

int server_start(server_t *server) {
    if (server == NULL || server->listen_fd < 0) {
        return -1;
    }

    /* 1. Prompt check: if stop was requested prior to startup, exit immediately without service */
    pthread_mutex_lock(&server->state_lock);
    if (server->stop_requested) {
        pthread_mutex_unlock(&server->state_lock);
        LOG_INFO("server_start() called after stop requested; exiting promptly.");
        return 0;
    }
    uint16_t port = server->config.port;
    server_mode_t mode = server->config.mode;
    int num_threads = server->config.num_threads;
    int max_conn = server->config.max_connections;
    pthread_mutex_unlock(&server->state_lock);

    /* 2. Reset the atomic signal flag BEFORE installing signal handlers to prevent stale signals
     * from a previous run while ensuring any signal received during or after setup is retained. */
    atomic_store_explicit(&g_signal_shutdown, 0, memory_order_release);
    atomic_store_explicit(&g_signal_pipe_write_fd, server->signal_pipe[1], memory_order_release);

    /* 3. Install signal handlers */
    if (server_setup_signals() != 0) {
        LOG_FATAL("Failed to configure signal handlers.");
        atomic_store_explicit(&g_signal_pipe_write_fd, -1, memory_order_release);
        return -1;
    }

    /* 4. Ensure startup cannot overwrite a shutdown request received during initialization.
     * Check both cross-thread stop_requested and signal flag under lock before declaring running. */
    pthread_mutex_lock(&server->state_lock);
    if (server->stop_requested || atomic_load_explicit(&g_signal_shutdown, memory_order_acquire) != 0) {
        server->running = false;
        pthread_mutex_unlock(&server->state_lock);
        atomic_store_explicit(&g_signal_pipe_write_fd, -1, memory_order_release);
        LOG_INFO("Shutdown requested during initialization; exiting promptly.");
        return 0;
    }

    /* 5. Start thread pool if configured in threadpool mode */
    if (mode == SERVER_MODE_THREADPOOL) {
        size_t queue_cap = (max_conn > 0) ? (size_t)max_conn : 1024;
        if (threadpool_init(&server->pool, (size_t)num_threads, queue_cap,
                            server_worker_task, server) != 0) {
            server->running = false;
            pthread_mutex_unlock(&server->state_lock);
            atomic_store_explicit(&g_signal_pipe_write_fd, -1, memory_order_release);
            LOG_FATAL("Failed to initialize worker thread pool");
            return -1;
        }
    }

    server->running = true;
    pthread_mutex_unlock(&server->state_lock);

    /* If configured in Phase 6 epoll mode, delegate to epoll event loop */
    if (mode == SERVER_MODE_EPOLL) {
        int epoll_rc = server_start_epoll(server);
        atomic_store_explicit(&g_signal_pipe_write_fd, -1, memory_order_release);
        return epoll_rc;
    }

    LOG_INFO("Server accept loop started on port %u (mode: %s, threads: %d)",
             port, (mode == SERVER_MODE_THREADPOOL) ? "threadpool" : "single", num_threads);

    while (server_is_running(server)) {
        struct pollfd pfds[2];
        pfds[0].fd = server->listen_fd;
        pfds[0].events = POLLIN;
        pfds[1].fd = server->signal_pipe[0];
        pfds[1].events = POLLIN;

        int poll_ret = poll(pfds, 2, 500); /* 500ms sliced timeout for responsive liveness */
        if (poll_ret < 0) {
            if (errno == EINTR) {
                if (!server_is_running(server)) {
                    break;
                }
                continue;
            }
            LOG_ERROR("poll() on listen/signal descriptors failed: %s", strerror(errno));
            break;
        }

        if (poll_ret == 0) {
            /* Timeout tick; re-evaluate server_is_running */
            continue;
        }

        /* Check if signal self-pipe triggered unblocking */
        if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
            char drain_buf[64];
            while (read(server->signal_pipe[0], drain_buf, sizeof(drain_buf)) > 0) {
                /* Drain self-pipe until empty */
            }
            if (!server_is_running(server)) {
                break;
            }
        }

        /* Check if new incoming client connection is ready on listening socket */
        if (pfds[0].revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    if (!server_is_running(server)) {
                        break;
                    }
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                    /* Connection was aborted or reset by client before accept */
                    continue;
                }
                if (errno == EMFILE || errno == ENFILE) {
                    /* Process descriptor table limit (EMFILE) or system limit (ENFILE) reached.
                     * Back off briefly to prevent 100% CPU busy-spinning while active workers
                     * complete and close client descriptors. */
                    LOG_WARN("File descriptor table full (EMFILE/ENFILE); backing off 20ms...");
                    usleep(20000); /* 20ms backoff */
                    continue;
                }
                if (errno == EBADF || errno == EINVAL) {
                    /* Listening socket closed by server_cleanup or server_stop */
                    break;
                }
                LOG_ERROR("accept() failed: %s", strerror(errno));
                continue;
            }

            if (!server_is_running(server)) {
                close(client_fd);
                break;
            }

            /* Set close-on-exec on accepted client descriptor to prevent child leakage */
            int cflags = fcntl(client_fd, F_GETFD, 0);
            if (cflags >= 0) {
                fcntl(client_fd, F_SETFD, cflags | FD_CLOEXEC);
            }

            char client_ip[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip)) == NULL) {
                strncpy(client_ip, "unknown", sizeof(client_ip));
                client_ip[sizeof(client_ip) - 1] = '\0';
            }
            uint16_t client_port = ntohs(client_addr.sin_port);
            LOG_INFO("Accepted connection from %s:%u (fd: %d)", client_ip, client_port, client_fd);

            if (mode == SERVER_MODE_THREADPOOL) {
                /* Enqueue into bounded connection queue; blocks if full, applying backpressure */
                int ret = threadpool_submit(&server->pool, client_fd);
                if (ret != 0) {
                    /* Submission rejected due to shutdown */
                    close(client_fd);
                    break;
                }
            } else {
                /* Single-threaded sequential execution */
                server_handle_client(server, client_fd, 0);
            }
        }
    }

    pthread_mutex_lock(&server->state_lock);
    server->running = false;
    pthread_mutex_unlock(&server->state_lock);

    atomic_store_explicit(&g_signal_pipe_write_fd, -1, memory_order_release);

    /* If thread pool is active, gracefully shutdown and join all workers */
    if (mode == SERVER_MODE_THREADPOOL) {
        server_teardown_pool(server);
    }

    LOG_INFO("Server accept loop finished.");
    return 0;
}

int server_stop(server_t *server) {
    if (server == NULL) {
        return -1;
    }

    pthread_mutex_lock(&server->state_lock);
    bool was_running = server->running;
    server->stop_requested = true;
    server->running = false;

    /* Shut down all active client sockets across all workers to immediately unblock idle poll/recv */
    int num_workers = (server->config.mode == SERVER_MODE_SINGLE || server->config.mode == SERVER_MODE_EPOLL) ?
                      1 : server->config.num_threads;
    if (server->worker_active_fds != NULL) {
        for (int i = 0; i < num_workers; i++) {
            int active_fd = server->worker_active_fds[i];
            if (active_fd >= 0) {
                shutdown(active_fd, SHUT_RDWR);
            }
        }
    }

    /* Wake blocked queue producers and consumers if thread pool queue is active and not already shutting down */
    if (server->config.mode == SERVER_MODE_THREADPOOL &&
        server->pool.is_started &&
        !server->pool_shutting_down) {
        conn_queue_shutdown(&server->pool.queue);
    }

    /* Signal the self-pipe to immediately unblock the accept loop poll() or epoll_wait() call */
    if (server->signal_pipe[1] >= 0) {
        char byte = 'Q';
        (void)write(server->signal_pipe[1], &byte, 1);
    }

    int listen_fd = server->listen_fd;
    uint16_t port = server->config.port;
    pthread_mutex_unlock(&server->state_lock);

    /* Secondary wake-up mechanism: connect via loopback in case accept() was blocked */
    if (was_running && listen_fd >= 0 && port > 0) {
        int wake_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (wake_fd >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");

            int ret = connect(wake_fd, (struct sockaddr *)&addr, sizeof(addr));
            if (ret < 0) {
                int err = errno;
                close(wake_fd);
                if (err == ECONNREFUSED) {
                    LOG_DEBUG("server_stop: wake-up connect refused; server already stopped");
                    return 0;
                }
                LOG_DEBUG("server_stop: loopback wake-up connect returned: %s", strerror(err));
                return 0;
            }
            close(wake_fd);
        }
    }

    return 0;
}

void server_cleanup(server_t *server) {
    if (server == NULL) {
        return;
    }

    pthread_mutex_lock(&server->state_lock);
    server->running = false;
    server->stop_requested = true;
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    if (server->signal_pipe[0] >= 0) {
        close(server->signal_pipe[0]);
        server->signal_pipe[0] = -1;
    }
    if (server->signal_pipe[1] >= 0) {
        close(server->signal_pipe[1]);
        server->signal_pipe[1] = -1;
    }
    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }
    pthread_mutex_unlock(&server->state_lock);

    if (server->config.mode == SERVER_MODE_THREADPOOL) {
        server_teardown_pool(server);
    }

    pthread_mutex_lock(&server->state_lock);
    if (server->worker_active_fds != NULL) {
        free(server->worker_active_fds);
        server->worker_active_fds = NULL;
    }
    pthread_mutex_unlock(&server->state_lock);

    pthread_mutex_destroy(&server->state_lock);
}

int server_run(const server_config_t *config, hashtable_t *ht) {
    server_t server;
    if (server_init(&server, config, ht) != 0) {
        return -1;
    }

    int rc = server_start(&server);
    server_cleanup(&server);
    return rc;
}

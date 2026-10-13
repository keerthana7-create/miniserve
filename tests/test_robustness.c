#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "hashtable.h"
#include "logger.h"
#include "server.h"

static int tcp_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int ret = -1;
    for (int retry = 0; retry < 50; retry++) {
        ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (ret == 0) {
            break;
        }
        usleep(10000); /* 10ms */
    }
    assert(ret == 0);
    return fd;
}

static void tcp_send(int fd, const char *data, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(fd, data + total, len - total, 0);
        assert(n > 0);
        total += (size_t)n;
    }
}

static void tcp_send_str(int fd, const char *str) {
    tcp_send(fd, str, strlen(str));
}

static bool tcp_recv_line(int fd, char *buf, size_t max_len) {
    size_t idx = 0;
    while (idx < max_len - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) {
            buf[idx] = '\0';
            return false;
        }
        buf[idx++] = c;
        if (c == '\n') {
            break;
        }
    }
    buf[idx] = '\0';
    return true;
}

static void *server_thread_worker(void *arg) {
    server_t *server = (server_t *)arg;
    int rc = server_start(server);
    return (void *)(intptr_t)rc;
}

/**
 * 1. Test unblocking accept loop via SIGINT and self-pipe.
 */
static void test_robustness_self_pipe_signal_unblock(void) {
    hashtable_t *ht = hashtable_create(64);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0; /* Ephemeral port */
    config.num_threads = 2;
    config.mode = SERVER_MODE_THREADPOOL;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000); /* Wait for server accept loop to enter poll() */

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* Send SIGINT to the process: signal handler writes to self-pipe and unblocks poll() */
    assert(kill(getpid(), SIGINT) == 0);

    /* Server accept loop must wake up and exit promptly without hanging */
    void *ret_val = NULL;
    assert(pthread_join(tid, &ret_val) == 0);
    assert((intptr_t)ret_val == 0);

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double elapsed = (double)(end_ts.tv_sec - start_ts.tv_sec) +
                     (double)(end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
    assert(elapsed < 1.0);

    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_robustness_self_pipe_signal_unblock (unblocked via self-pipe in %.3f sec)\n", elapsed);
}

/**
 * 2. Test idle client disconnection after configured idle timeout.
 */
static void test_robustness_idle_client_timeout(void) {
    hashtable_t *ht = hashtable_create(64);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.idle_timeout_sec = 1; /* 1 second inactivity timeout */

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000);

    uint16_t port = server.config.port;
    int client_fd = tcp_connect(port);

    /* Client connects but sends nothing; wait for timeout to expire */
    usleep(1500000); /* 1.5 seconds */

    /* Server must have closed connection due to idle timeout */
    char c = 0;
    ssize_t n = recv(client_fd, &c, 1, 0);
    assert(n == 0); /* EOF */

    close(client_fd);

    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_robustness_idle_client_timeout\n");
}

/**
 * 3. Test Slowloris protection: partial command send times out if line is not completed within timeout.
 */
static void test_robustness_slowloris_partial_send_timeout(void) {
    hashtable_t *ht = hashtable_create(64);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.idle_timeout_sec = 1; /* 1 second timeout */

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000);

    uint16_t port = server.config.port;
    int client_fd = tcp_connect(port);

    /* Send partial request without newline delimiter (Slowloris pattern) */
    tcp_send_str(client_fd, "SET slow_key incomplete_value");

    /* Wait 1.5 seconds without sending the newline */
    usleep(1500000);

    /* Server must drop the connection due to slowloris timeout */
    char c = 0;
    ssize_t n = recv(client_fd, &c, 1, 0);
    assert(n == 0); /* EOF */

    close(client_fd);

    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_robustness_slowloris_partial_send_timeout\n");
}

typedef struct {
    uint16_t port;
    int mode; /* 0: immediate close, 1: junk and close */
} storm_client_arg_t;

static void *storm_client_worker(void *arg) {
    storm_client_arg_t *carg = (storm_client_arg_t *)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(carg->port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    if (carg->mode == 1) {
        const char *noise = "GARBAGE NOISE 1234\r\n";
        (void)send(fd, noise, strlen(noise), 0);
    }

    /* Abrupt socket closure (RST or immediate close) */
    struct linger sl = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(fd);
    return NULL;
}

/**
 * 4. Test burst disconnect storm: rapid concurrent connects and immediate resets.
 */
static void test_robustness_burst_disconnect_storm(void) {
    hashtable_t *ht = hashtable_create(64);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.num_threads = 4;
    config.mode = SERVER_MODE_THREADPOOL;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000);

    uint16_t port = server.config.port;

    #define STORM_CLIENT_COUNT 60
    pthread_t storm_threads[STORM_CLIENT_COUNT];
    storm_client_arg_t args[STORM_CLIENT_COUNT];

    for (int i = 0; i < STORM_CLIENT_COUNT; i++) {
        args[i].port = port;
        args[i].mode = (i % 2 == 0) ? 0 : 1;
        int rc = pthread_create(&storm_threads[i], NULL, storm_client_worker, &args[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < STORM_CLIENT_COUNT; i++) {
        int rc = pthread_join(storm_threads[i], NULL);
        assert(rc == 0);
    }

    /* Verify server remains completely functional after disconnect storm */
    int client_fd = tcp_connect(port);
    char buf[512];

    tcp_send_str(client_fd, "SET storm:status Alive\r\n");
    assert(tcp_recv_line(client_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(client_fd, "GET storm:status\r\n");
    assert(tcp_recv_line(client_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE Alive\r\n") == 0);

    tcp_send_str(client_fd, "QUIT\r\n");
    assert(tcp_recv_line(client_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(client_fd);

    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_robustness_burst_disconnect_storm (%d clients withstood)\n", STORM_CLIENT_COUNT);
}

/**
 * 5. Test repeated signal shutdown cycles (SIGTERM).
 */
static void test_robustness_repeated_signal_cycles(void) {
    for (int iter = 0; iter < 3; iter++) {
        hashtable_t *ht = hashtable_create(32);
        assert(ht != NULL);

        server_config_t config;
        server_config_init_defaults(&config);
        config.port = 0;
        config.num_threads = 2;

        server_t server;
        assert(server_init(&server, &config, ht) == 0);

        pthread_t tid;
        assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
        usleep(20000);

        /* Trigger shutdown using SIGTERM */
        assert(kill(getpid(), SIGTERM) == 0);
        assert(pthread_join(tid, NULL) == 0);

        server_cleanup(&server);
        hashtable_destroy(ht);
    }
    printf("  [PASS] test_robustness_repeated_signal_cycles (3 cycles)\n");
}

/**
 * 6. Test lock-free atomics check and clean initialization failure if unsupported.
 */
static void test_robustness_lock_free_atomics_and_init_failure(void) {
    /* Verify lock-free status of both signal atomics on the host execution target */
    assert(server_signal_atomics_lock_free() == true);

    /* Simulate unsupported target where atomics are not lock-free */
    server_set_simulate_atomics_not_lock_free(true);
    assert(server_signal_atomics_lock_free() == false);

    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;

    server_t server;
    /* server_init must fail cleanly without installing signal handlers or creating sockets */
    int ret = server_init(&server, &config, ht);
    assert(ret == -1);

    /* Reset simulation flag to normal execution context */
    server_set_simulate_atomics_not_lock_free(false);
    assert(server_signal_atomics_lock_free() == true);

    /* Verify server_init succeeds when lock-free atomics are present */
    assert(server_init(&server, &config, ht) == 0);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_robustness_lock_free_atomics_and_init_failure\n");
}

int main(void) {
    printf("Running Phase 5 Robustness and Signal Test Suite:\n");

    /* Initialize logger in warn mode to keep test output clean */
    logger_init(NULL, LOG_LEVEL_WARN, false);

    test_robustness_lock_free_atomics_and_init_failure();
    test_robustness_self_pipe_signal_unblock();
    test_robustness_idle_client_timeout();
    test_robustness_slowloris_partial_send_timeout();
    test_robustness_burst_disconnect_storm();
    test_robustness_repeated_signal_cycles();

    logger_close();
    printf("All Phase 5 robustness and signal tests passed successfully!\n");
    return EXIT_SUCCESS;
}

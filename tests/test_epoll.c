#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
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

    /* Enforce bounded timeouts on all socket test operations so regressions fail fast rather than hanging */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0);

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
 * 1. Basic CRUD and connection teardown in epoll mode.
 */
static void test_epoll_basic_crud(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* SET */
    tcp_send_str(fd, "SET epoll:key epoll_val\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* GET */
    tcp_send_str(fd, "GET epoll:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE epoll_val\r\n") == 0);

    /* EXISTS */
    tcp_send_str(fd, "EXISTS epoll:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "1\r\n") == 0);

    /* COUNT */
    tcp_send_str(fd, "COUNT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "COUNT 1\r\n") == 0);

    /* DEL */
    tcp_send_str(fd, "DEL epoll:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "DELETED\r\n") == 0);

    /* GET after DEL */
    tcp_send_str(fd, "GET epoll:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "NOT_FOUND\r\n") == 0);

    /* QUIT */
    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);

    /* Verify clean EOF disconnect */
    char c = 0;
    ssize_t n = recv(fd, &c, 1, 0);
    assert(n == 0);

    close(fd);
    printf("  [PASS] test_epoll_basic_crud\n");
}

typedef struct {
    uint16_t port;
    int client_id;
} epoll_client_arg_t;

static void *epoll_conc_client_worker(void *arg) {
    epoll_client_arg_t *carg = (epoll_client_arg_t *)arg;
    int fd = tcp_connect(carg->port);
    char buf[512];
    char cmd[256];

    for (int i = 0; i < 25; i++) {
        snprintf(cmd, sizeof(cmd), "SET epoll_c%d:k%d val%d\r\n", carg->client_id, i, i);
        tcp_send_str(fd, cmd);
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "OK\r\n") == 0);

        snprintf(cmd, sizeof(cmd), "GET epoll_c%d:k%d\r\n", carg->client_id, i);
        tcp_send_str(fd, cmd);
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        char expected[256];
        snprintf(expected, sizeof(expected), "VALUE val%d\r\n", i);
        assert(strcmp(buf, expected) == 0);
    }

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);
    return NULL;
}

/**
 * 2. Simultaneous concurrent clients in epoll mode.
 */
static void test_epoll_concurrent_clients(uint16_t port) {
    #define EPOLL_CONC_CLIENTS 8
    pthread_t threads[EPOLL_CONC_CLIENTS];
    epoll_client_arg_t args[EPOLL_CONC_CLIENTS];

    for (int i = 0; i < EPOLL_CONC_CLIENTS; i++) {
        args[i].port = port;
        args[i].client_id = i;
        int rc = pthread_create(&threads[i], NULL, epoll_conc_client_worker, &args[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < EPOLL_CONC_CLIENTS; i++) {
        int rc = pthread_join(threads[i], NULL);
        assert(rc == 0);
    }

    printf("  [PASS] test_epoll_concurrent_clients (%d simultaneous clients)\n", EPOLL_CONC_CLIENTS);
}

/**
 * 3. Fragmented input and pipelined commands in epoll mode.
 */
static void test_epoll_fragmented_and_pipelined(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Fragmented transmission */
    tcp_send_str(fd, "S");
    usleep(5000);
    tcp_send_str(fd, "ET epoll_frag_k ");
    usleep(5000);
    tcp_send_str(fd, "epoll_frag_v\r");
    usleep(5000);
    tcp_send_str(fd, "\n");

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* Verify stored */
    tcp_send_str(fd, "GET epoll_frag_k\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE epoll_frag_v\r\n") == 0);

    /* Pipelined commands in a single write */
    tcp_send_str(fd, "SET ep1 v1\r\nSET ep2 v2\r\nGET ep1\r\nGET ep2\r\n");

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE v1\r\n") == 0);

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE v2\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_epoll_fragmented_and_pipelined\n");
}

/**
 * 4. Oversized lines and control character rejection in epoll mode.
 */
static void test_epoll_oversized_and_control_chars(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Oversized completed line */
    char oversized[1100];
    memset(oversized, 'X', 1050);
    oversized[1050] = '\r';
    oversized[1051] = '\n';
    oversized[1052] = '\0';
    tcp_send(fd, oversized, 1052);
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strstr(buf, "ERR OVERSIZED") != NULL);

    /* Prompt oversized rejection without newline */
    char oversized_no_nl[1050];
    memset(oversized_no_nl, 'Y', 1025);
    tcp_send(fd, oversized_no_nl, 1025);
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strstr(buf, "ERR OVERSIZED") != NULL);

    /* Send newline to end oversized command, then valid command */
    tcp_send_str(fd, "\nSET epoll_rec ok\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* Embedded NUL byte rejection */
    char nul_req[] = "SET epoll_nul key\0val\r\n";
    tcp_send(fd, nul_req, sizeof(nul_req) - 1);
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "ERR MALFORMED Control characters not allowed\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_epoll_oversized_and_control_chars\n");
}

typedef struct {
    uint16_t port;
    int mode;
} epoll_storm_arg_t;

static void *epoll_storm_client_worker(void *arg) {
    epoll_storm_arg_t *carg = (epoll_storm_arg_t *)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

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
        (void)send(fd, "MALFORMED NOISE 1234\r\n", 22, 0);
    }

    /* Force abrupt RST disconnect */
    struct linger sl = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    close(fd);
    return NULL;
}

/**
 * 5. Abrupt disconnect storm resilience in epoll mode.
 */
static void test_epoll_abrupt_disconnect_storm(uint16_t port) {
    #define EPOLL_STORM_COUNT 50
    pthread_t threads[EPOLL_STORM_COUNT];
    epoll_storm_arg_t args[EPOLL_STORM_COUNT];

    for (int i = 0; i < EPOLL_STORM_COUNT; i++) {
        args[i].port = port;
        args[i].mode = (i % 2 == 0) ? 0 : 1;
        int rc = pthread_create(&threads[i], NULL, epoll_storm_client_worker, &args[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < EPOLL_STORM_COUNT; i++) {
        int rc = pthread_join(threads[i], NULL);
        assert(rc == 0);
    }

    /* Verify server remains responsive */
    int fd = tcp_connect(port);
    char buf[512];
    tcp_send_str(fd, "SET epoll_storm:status Healthy\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_epoll_abrupt_disconnect_storm (%d clients)\n", EPOLL_STORM_COUNT);
}

/**
 * 6. Connection limit enforcement in epoll mode.
 */
static void test_epoll_connection_limits(void) {
    hashtable_t *ht = hashtable_create(32);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.mode = SERVER_MODE_EPOLL;
    config.max_connections = 3; /* Limit to 3 concurrent connections */

    server_t server;
    assert(server_init(&server, &config, ht) == 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000);

    uint16_t port = server.config.port;

    /* Open 3 active clients */
    int c1 = tcp_connect(port);
    int c2 = tcp_connect(port);
    int c3 = tcp_connect(port);

    char buf[512];
    tcp_send_str(c1, "SET k1 v1\r\n");
    assert(tcp_recv_line(c1, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* Open 4th client: exceeds limit of 3 */
    int c4 = socket(AF_INET, SOCK_STREAM, 0);
    assert(c4 >= 0);
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(c4, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c4, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    assert(connect(c4, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    /* 4th client should receive overload rejection or immediate EOF */
    bool rejected = false;
    if (tcp_recv_line(c4, buf, sizeof(buf))) {
        if (strstr(buf, "ERR OVERLOAD") != NULL) {
            rejected = true;
        }
    } else {
        /* Immediate connection close */
        rejected = true;
    }
    assert(rejected);
    close(c4);

    /* Close c1 to free up capacity */
    tcp_send_str(c1, "QUIT\r\n");
    assert(tcp_recv_line(c1, buf, sizeof(buf)));
    close(c1);
    usleep(10000);

    /* Now a new client can connect successfully */
    int c5 = tcp_connect(port);
    tcp_send_str(c5, "GET k1\r\n");
    assert(tcp_recv_line(c5, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE v1\r\n") == 0);

    tcp_send_str(c5, "QUIT\r\n");
    assert(tcp_recv_line(c5, buf, sizeof(buf)));
    close(c5);

    tcp_send_str(c2, "QUIT\r\n");
    assert(tcp_recv_line(c2, buf, sizeof(buf)));
    close(c2);

    tcp_send_str(c3, "QUIT\r\n");
    assert(tcp_recv_line(c3, buf, sizeof(buf)));
    close(c3);

    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_epoll_connection_limits\n");
}

/**
 * 7. Inactivity timeout and Slowloris protection in epoll mode.
 */
static void test_epoll_idle_timeout_and_slowloris(void) {
    hashtable_t *ht = hashtable_create(32);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.mode = SERVER_MODE_EPOLL;
    config.idle_timeout_sec = 1; /* 1 second timeout */

    server_t server;
    assert(server_init(&server, &config, ht) == 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000);

    uint16_t port = server.config.port;

    /* 1. Inactivity timeout: connect and send nothing */
    int fd_idle = tcp_connect(port);
    usleep(1600000); /* Wait > 1s for epoll periodic sweep */
    char c = 0;
    ssize_t n = recv(fd_idle, &c, 1, 0);
    assert(n == 0); /* EOF */
    close(fd_idle);

    /* 2. Slowloris protection: send partial command and stall */
    int fd_slow = tcp_connect(port);
    tcp_send_str(fd_slow, "SET incomplete_cmd ");
    usleep(1600000);
    n = recv(fd_slow, &c, 1, 0);
    assert(n == 0); /* EOF */
    close(fd_slow);

    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_epoll_idle_timeout_and_slowloris\n");
}

static atomic_bool g_epoll_load_active = true;

static void *epoll_load_client_worker(void *arg) {
    uint16_t port = *(uint16_t *)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }

    char buf[512];
    while (atomic_load_explicit(&g_epoll_load_active, memory_order_acquire)) {
        if (send(fd, "COUNT\r\n", 7, 0) <= 0) break;
        if (!tcp_recv_line(fd, buf, sizeof(buf))) break;
        usleep(1000);
    }

    close(fd);
    return NULL;
}

/**
 * 8. Graceful shutdown under concurrent load in epoll mode.
 */
static void test_epoll_shutdown_under_load(void) {
    hashtable_t *ht = hashtable_create(32);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.mode = SERVER_MODE_EPOLL;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000);

    uint16_t port = server.config.port;
    atomic_store_explicit(&g_epoll_load_active, true, memory_order_release);

    #define EPOLL_LOAD_CLIENTS 6
    pthread_t clients[EPOLL_LOAD_CLIENTS];
    for (int i = 0; i < EPOLL_LOAD_CLIENTS; i++) {
        int rc = pthread_create(&clients[i], NULL, epoll_load_client_worker, &port);
        assert(rc == 0);
    }

    usleep(25000); /* Allow clients to generate concurrent load */

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* Issue stop while clients are active */
    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double elapsed = (double)(end_ts.tv_sec - start_ts.tv_sec) +
                     (double)(end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
    assert(elapsed < 1.5);

    atomic_store_explicit(&g_epoll_load_active, false, memory_order_release);
    for (int i = 0; i < EPOLL_LOAD_CLIENTS; i++) {
        int rc = pthread_join(clients[i], NULL);
        assert(rc == 0);
    }

    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_epoll_shutdown_under_load (unblocked in %.3f sec)\n", elapsed);
}

/**
 * 9. Repeated epoll server startup and shutdown cycles.
 */
static void test_epoll_repeated_startup_shutdown(void) {
    for (int iter = 0; iter < 5; iter++) {
        hashtable_t *ht = hashtable_create(32);
        assert(ht != NULL);

        server_config_t config;
        server_config_init_defaults(&config);
        config.port = 0;
        config.mode = SERVER_MODE_EPOLL;

        server_t server;
        assert(server_init(&server, &config, ht) == 0);

        pthread_t tid;
        assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
        usleep(20000);

        int fd = tcp_connect(server.config.port);
        char buf[512];
        tcp_send_str(fd, "SET cycle:k cycle:v\r\n");
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "OK\r\n") == 0);

        tcp_send_str(fd, "QUIT\r\n");
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "BYE\r\n") == 0);
        close(fd);

        assert(server_stop(&server) == 0);
        assert(pthread_join(tid, NULL) == 0);

        server_cleanup(&server);
        hashtable_destroy(ht);
    }

    printf("  [PASS] test_epoll_repeated_startup_shutdown (5 cycles)\n");
}

/**
 * 10. Regression: Client sends request and immediately disconnects.
 * Exercises server receiving EPOLLIN and client disconnect without reading response.
 */
static void test_epoll_request_immediate_disconnect(uint16_t port) {
    for (int i = 0; i < 40; i++) {
        int fd = tcp_connect(port);
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "SET imm_key_%d imm_val_%d\r\n", i, i);
        tcp_send_str(fd, cmd);
        /* Immediately close socket without reading response */
        close(fd);
    }

    /* Verify server is completely healthy and all writes succeeded */
    int fd = tcp_connect(port);
    char buf[512];
    tcp_send_str(fd, "GET imm_key_39\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE imm_val_39\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_epoll_request_immediate_disconnect (40 cycles)\n");
}

/**
 * 11. Regression: QUIT with pending output and pipelined commands.
 * Exercises epoll_client_try_flush handling close_after_write with multiple buffered responses.
 */
static void test_epoll_quit_with_pending_output(uint16_t port) {
    for (int iter = 0; iter < 25; iter++) {
        int fd = tcp_connect(port);

        /* Set smaller receive buffer to encourage output buffering */
        int rcvbuf = 2048;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        /* Pipeline 10 SET commands and a QUIT command in a single write */
        char batch[1024];
        int offset = 0;
        for (int k = 0; k < 10; k++) {
            offset += snprintf(batch + offset, sizeof(batch) - (size_t)offset,
                               "SET qk_%d_%d qv\r\n", iter, k);
        }
        offset += snprintf(batch + offset, sizeof(batch) - (size_t)offset, "QUIT\r\n");

        tcp_send(fd, batch, (size_t)offset);

        /* Receive all 10 OK responses */
        char buf[512];
        for (int k = 0; k < 10; k++) {
            assert(tcp_recv_line(fd, buf, sizeof(buf)));
            assert(strcmp(buf, "OK\r\n") == 0);
        }

        /* Receive BYE response */
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "BYE\r\n") == 0);

        /* Verify clean EOF */
        char c = 0;
        ssize_t n = recv(fd, &c, 1, 0);
        assert(n == 0);

        close(fd);
    }

    printf("  [PASS] test_epoll_quit_with_pending_output (25 cycles)\n");
}

/**
 * 12. Regression: Simultaneous read and disconnect flags (EPOLLIN + EPOLLRDHUP).
 * Client sends a request and immediately calls shutdown(SHUT_WR).
 */
static void test_epoll_simultaneous_read_disconnect_flags(uint16_t port) {
    for (int i = 0; i < 30; i++) {
        int fd = tcp_connect(port);
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "SET rdhup_key_%d rdhup_val\r\n", i);
        tcp_send_str(fd, cmd);

        /* Half-close writing end immediately; triggers EPOLLRDHUP alongside or right after EPOLLIN */
        assert(shutdown(fd, SHUT_WR) == 0);

        /* Server must still flush response despite peer half-close */
        char buf[512];
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "OK\r\n") == 0);

        /* Server should close connection when finished */
        char c = 0;
        ssize_t n = recv(fd, &c, 1, 0);
        assert(n == 0);

        close(fd);
    }

    printf("  [PASS] test_epoll_simultaneous_read_disconnect_flags (30 cycles)\n");
}

/**
 * 13. Regression: Abrupt RST disconnect during write/read (SO_LINGER RST).
 * Exercises write errors (EPIPE / ECONNRESET) and error flags in epoll event loop.
 */
static void test_epoll_rst_write_errors(uint16_t port) {
    for (int i = 0; i < 30; i++) {
        int fd = tcp_connect(port);

        /* Send partial command or command and immediately force RST */
        if (i % 2 == 0) {
            (void)send(fd, "SET rst_err partial", 19, 0);
        } else {
            (void)send(fd, "GET rst_err\r\n", 13, 0);
        }

        /* Force TCP RST via SO_LINGER */
        struct linger sl = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
        close(fd);
    }

    /* Verify server remains responsive after RST storm */
    int fd = tcp_connect(port);
    char buf[512];
    tcp_send_str(fd, "SET rst_health ok\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_epoll_rst_write_errors (30 cycles)\n");
}

/**
 * 14. Regression: Output buffer capacity exceeded (safe backpressure termination).
 * Deterministically exercises the production 64KB output-buffer overflow policy using:
 *  - Test-only configuration hook (server_set_test_client_sndbuf) to constrain server TCP send buffer,
 *  - Controlled small client receive buffer (SO_RCVBUF = 1024) to generate rapid TCP window backpressure,
 *  - Pipelined requests (350 x 508-byte responses = ~177 KB) without reading responses,
 *  - Explicit bounded timeouts (SO_RCVTIMEO / SO_SNDTIMEO = 2s) so any failure terminates promptly.
 * Verifies safe connection termination on 64KB overflow, descriptor closure, and continued server health.
 */
static void test_epoll_output_buffer_overflow(uint16_t port) {
    /* 1. Setup a test key with 500-byte value so each response produces ~508 bytes */
    int setup_fd = tcp_connect(port);
    char large_val[501];
    memset(large_val, 'X', 500);
    large_val[500] = '\0';

    char setup_cmd[600];
    snprintf(setup_cmd, sizeof(setup_cmd), "SET of_key %s\r\n", large_val);
    tcp_send_str(setup_fd, setup_cmd);

    char buf[512];
    assert(tcp_recv_line(setup_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(setup_fd, "QUIT\r\n");
    assert(tcp_recv_line(setup_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(setup_fd);

    /* 2. Configure test hook: small SO_SNDBUF on accepted client socket to induce backpressure */
    server_set_test_client_sndbuf(2048);
    /* Ensure testing the production output buffer capacity (CLIENT_OUT_BUF_SIZE = 65536) */
    server_set_test_out_buf_limit(0);

    /* 3. Connect client with small receive buffer and bounded timeouts */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    /* Explicit bounded timeouts (2 seconds) so regression fails rather than hanging */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    assert(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0);

    /* Request minimal TCP receive buffer to generate rapid TCP window backpressure */
    int rcvbuf = 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    assert(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    /* 4. Pipeline requests without reading responses.
     * Each response is "VALUE <500 bytes>\r\n" (508 bytes).
     * 350 requests * 508 bytes = ~177,800 bytes >> 65,536 byte production buffer.
     * Restricted socket buffers (~4KB total kernel buffer) cause server send() to return EAGAIN.
     * Subsequent responses accumulate in conn->out_buf until exceeding 65,536 bytes. */
    const char *get_cmd = "GET of_key\r\n";
    size_t get_len = strlen(get_cmd);
    bool write_error = false;

    for (int i = 0; i < 350; i++) {
        ssize_t n = send(fd, get_cmd, get_len, MSG_NOSIGNAL);
        if (n <= 0) {
            write_error = true;
            break;
        }
    }

    /* Wait briefly for server event loop to process requests, encounter backpressure, and terminate */
    usleep(50000); /* 50ms */

    /* 5. Client drains any in-flight data. Server must safely terminate connection.
     * With SO_RCVTIMEO = 2s, recv() will return 0 (EOF) or -1 (ECONNRESET) once closed.
     * If the server never closed the connection, recv() would time out with EAGAIN/EWOULDBLOCK
     * and terminated remains false, asserting and failing the test instead of hanging. */
    bool terminated = false;
    char drain_buf[4096];
    while (true) {
        ssize_t r = recv(fd, drain_buf, sizeof(drain_buf), 0);
        if (r == 0) {
            terminated = true; /* Safe EOF termination by server */
            break;
        }
        if (r < 0) {
            if (errno == ECONNRESET || errno == EPIPE) {
                terminated = true; /* Connection reset by server */
                break;
            }
            /* Any other error (including EAGAIN / EWOULDBLOCK timeout) stops draining */
            break;
        }
    }

    /* If write failed due to connection reset or read got EOF/reset, connection was terminated */
    assert(write_error || terminated);
    close(fd);

    /* 6. Restore default test hooks */
    server_set_test_client_sndbuf(0);
    server_set_test_out_buf_limit(0);

    /* 7. Verify server remains healthy and accepts fresh clients */
    int health_fd = tcp_connect(port);
    tcp_send_str(health_fd, "SET overflow_recovery ok\r\n");
    assert(tcp_recv_line(health_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(health_fd, "GET overflow_recovery\r\n");
    assert(tcp_recv_line(health_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE ok\r\n") == 0);

    tcp_send_str(health_fd, "QUIT\r\n");
    assert(tcp_recv_line(health_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(health_fd);

    printf("  [PASS] test_epoll_output_buffer_overflow (deterministic backpressure termination on 64KB overflow)\n");
}

/**
 * 15. Regression: Clean startup failure if listen_fd cannot be configured.
 */
static void test_epoll_invalid_listen_fd_startup_failure(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.mode = SERVER_MODE_EPOLL;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);

    /* Corrupt listen_fd to an invalid descriptor so fcntl fails */
    int valid_listen = server.listen_fd;
    server.listen_fd = -1;

    int ret = server_start(&server);
    /* server_start must fail cleanly with -1 */
    assert(ret == -1);

    /* Restore and clean up resources */
    server.listen_fd = valid_listen;
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_epoll_invalid_listen_fd_startup_failure (clean init failure)\n");
}

int main(void) {
    printf("Running Phase 6 Linux Epoll Integration Tests:\n");

    /* Initialize logger to warn level during test suite */
    logger_init(NULL, LOG_LEVEL_WARN, false);

    hashtable_t *ht = hashtable_create(64);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.mode = SERVER_MODE_EPOLL;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(25000);

    uint16_t port = server.config.port;

    test_epoll_basic_crud(port);
    test_epoll_concurrent_clients(port);
    test_epoll_fragmented_and_pipelined(port);
    test_epoll_oversized_and_control_chars(port);
    test_epoll_abrupt_disconnect_storm(port);
    test_epoll_request_immediate_disconnect(port);
    test_epoll_quit_with_pending_output(port);
    test_epoll_simultaneous_read_disconnect_flags(port);
    test_epoll_rst_write_errors(port);
    test_epoll_output_buffer_overflow(port);

    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);
    server_cleanup(&server);
    hashtable_destroy(ht);
    logger_close();

    /* Standalone configuration and lifecycle tests */
    test_epoll_connection_limits();
    test_epoll_idle_timeout_and_slowloris();
    test_epoll_shutdown_under_load();
    test_epoll_repeated_startup_shutdown();
    test_epoll_invalid_listen_fd_startup_failure();

    printf("All Phase 6 Linux Epoll integration tests passed successfully!\n");
    return EXIT_SUCCESS;
}

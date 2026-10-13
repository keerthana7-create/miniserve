#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static void test_server_basic_crud(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* SET */
    tcp_send_str(fd, "SET test:key Hello_World\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* GET */
    tcp_send_str(fd, "GET test:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE Hello_World\r\n") == 0);

    /* EXISTS */
    tcp_send_str(fd, "EXISTS test:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "1\r\n") == 0);

    /* COUNT */
    tcp_send_str(fd, "COUNT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "COUNT 1\r\n") == 0);

    /* DEL */
    tcp_send_str(fd, "DEL test:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "DELETED\r\n") == 0);

    /* GET after DEL */
    tcp_send_str(fd, "GET test:key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "NOT_FOUND\r\n") == 0);

    /* QUIT */
    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);

    /* Verify server closes connection cleanly (EOF on next recv) */
    char c;
    ssize_t n = recv(fd, &c, 1, 0);
    assert(n == 0);

    close(fd);
    printf("  [PASS] test_server_basic_crud\n");
}

static void test_server_sequential_clients(uint16_t port) {
    /* Client 1 writes a key and exits */
    int fd1 = tcp_connect(port);
    char buf[512];

    tcp_send_str(fd1, "SET user:99 Alice\r\n");
    assert(tcp_recv_line(fd1, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(fd1, "QUIT\r\n");
    assert(tcp_recv_line(fd1, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd1);

    /* Client 2 connects sequentially and reads the key */
    int fd2 = tcp_connect(port);
    tcp_send_str(fd2, "GET user:99\r\n");
    assert(tcp_recv_line(fd2, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE Alice\r\n") == 0);

    tcp_send_str(fd2, "QUIT\r\n");
    assert(tcp_recv_line(fd2, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd2);

    printf("  [PASS] test_server_sequential_clients\n");
}

static void test_server_partial_reads(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Fragmented transmission of "SET frag_key Fragmented_Value\r\n" */
    tcp_send_str(fd, "S");
    usleep(5000);
    tcp_send_str(fd, "ET fra");
    usleep(5000);
    tcp_send_str(fd, "g_key Frag");
    usleep(5000);
    tcp_send_str(fd, "mented_Value\r");
    usleep(5000);
    tcp_send_str(fd, "\n");

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* Verify value stored correctly */
    tcp_send_str(fd, "GET frag_key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE Fragmented_Value\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_server_partial_reads\n");
}

static void test_server_pipelined_commands(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Send 4 commands pipelined together in a single TCP write */
    tcp_send_str(fd, "SET pipe:1 Val1\r\nSET pipe:2 Val2\r\nGET pipe:1\r\nGET pipe:2\r\n");

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE Val1\r\n") == 0);

    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE Val2\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_server_pipelined_commands\n");
}

static void test_server_malformed_and_control_chars(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Unknown command verb */
    tcp_send_str(fd, "UNKNOWN_VERB arg1\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strstr(buf, "ERR UNKNOWN") != NULL);

    /* Malformed arguments */
    tcp_send_str(fd, "SET\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strstr(buf, "ERR MALFORMED") != NULL);

    /* Control character in content */
    tcp_send_str(fd, "SET bad\x01key bad_val\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "ERR MALFORMED Control characters not allowed\r\n") == 0);

    /* Server remains alive and functional after errors */
    tcp_send_str(fd, "SET recover_key ok\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(fd, "GET recover_key\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE ok\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_server_malformed_and_control_chars\n");
}

static void test_server_oversized_line(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Construct line with 1050 characters exceeding 1024-byte protocol limit */
    char oversized[1100];
    memset(oversized, 'A', 1050);
    oversized[1050] = '\r';
    oversized[1051] = '\n';
    oversized[1052] = '\0';

    tcp_send(fd, oversized, 1052);
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strstr(buf, "ERR OVERSIZED") != NULL);

    /* Subsequent valid command still succeeds */
    tcp_send_str(fd, "SET post_oversized recovered\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_server_oversized_line\n");
}

static void test_server_oversized_without_newline(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Send 1025 'B' characters WITHOUT a newline */
    char oversized_no_nl[1050];
    memset(oversized_no_nl, 'B', 1025);
    oversized_no_nl[1025] = '\0';

    tcp_send(fd, oversized_no_nl, 1025);

    /* Server must promptly reject without waiting for 4095-byte buffer to fill */
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "ERR OVERSIZED Command line exceeds 1024 bytes\r\n") == 0);

    /* Now send the terminating newline followed by a valid command */
    tcp_send_str(fd, "\nSET recover_after_prompt ok\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(fd, "GET recover_after_prompt\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE ok\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_server_oversized_without_newline\n");
}

static void test_server_abrupt_disconnect(uint16_t port) {
    /* Client 1 sets a value then abruptly closes without QUIT */
    int fd1 = tcp_connect(port);
    char buf[512];

    tcp_send_str(fd1, "SET abrupt_k abrupt_v\r\n");
    assert(tcp_recv_line(fd1, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* Abruptly close socket */
    close(fd1);

    /* Client 2 immediately connects and verifies server handled disconnect cleanly */
    int fd2 = tcp_connect(port);
    tcp_send_str(fd2, "GET abrupt_k\r\n");
    assert(tcp_recv_line(fd2, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE abrupt_v\r\n") == 0);

    tcp_send_str(fd2, "QUIT\r\n");
    assert(tcp_recv_line(fd2, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd2);

    printf("  [PASS] test_server_abrupt_disconnect\n");
}

static void test_server_embedded_nul(uint16_t port) {
    int fd = tcp_connect(port);
    char buf[512];

    /* Byte sequence with embedded NUL: "SET k v\0junk\r\n" (14 bytes total) */
    const char payload[] = "SET k v\0junk\r\n";
    size_t payload_len = sizeof(payload) - 1;

    tcp_send(fd, payload, payload_len);

    /* Must reject with exact error without truncating test payload */
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "ERR MALFORMED Control characters not allowed\r\n") == 0);

    /* Server remains fully operational after rejecting embedded NUL */
    tcp_send_str(fd, "SET after_nul recovered\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(fd, "GET after_nul\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "VALUE recovered\r\n") == 0);

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);

    printf("  [PASS] test_server_embedded_nul\n");
}

static void *server_thread_worker(void *arg) {
    server_t *server = (server_t *)arg;
    server_start(server);
    return NULL;
}

static void test_server_stop_before_start(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);

    /* Request stop before starting server */
    assert(server_stop(&server) == 0);

    /* server_start must return promptly without starting service */
    assert(server_start(&server) == 0);

    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_server_stop_before_start\n");
}

static void test_server_repeated_shutdown_in_accept(void) {
    for (int iter = 0; iter < 10; iter++) {
        hashtable_t *ht = hashtable_create(16);
        assert(ht != NULL);

        server_config_t config;
        server_config_init_defaults(&config);
        config.port = 0;

        server_t server;
        assert(server_init(&server, &config, ht) == 0);
        assert(server.config.port > 0);

        pthread_t tid;
        assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);

        /* Wait 10ms so server thread is blocked inside accept() */
        usleep(10000);

        /* Stop server while blocked in accept() */
        assert(server_stop(&server) == 0);

        /* Verify clean thread exit without race or hang */
        assert(pthread_join(tid, NULL) == 0);

        server_cleanup(&server);
        hashtable_destroy(ht);
    }
    printf("  [PASS] test_server_repeated_shutdown_in_accept (10 iterations)\n");
}

static void test_server_stop_with_idle_client(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(20000);

    /* Connect client and leave it completely idle */
    int client_fd = tcp_connect(server.config.port);
    assert(client_fd >= 0);

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* Issue stop while idle client is connected */
    assert(server_stop(&server) == 0);

    /* Server thread must exit promptly without waiting for the 30s receive timeout */
    assert(pthread_join(tid, NULL) == 0);

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double elapsed_sec = (double)(end_ts.tv_sec - start_ts.tv_sec) +
                         (double)(end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;

    /* Ensure shutdown completed promptly in under 2 seconds */
    assert(elapsed_sec < 2.0);

    close(client_fd);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_server_stop_with_idle_client (unblocked in %.3f sec)\n", elapsed_sec);
}

static void test_server_stop_already_stopped(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);

    /* Idempotent stop calls prior to startup must succeed */
    assert(server_stop(&server) == 0);
    assert(server_stop(&server) == 0);

    /* server_start must return promptly without starting service */
    assert(server_start(&server) == 0);

    /* Calling server_stop after server has already stopped must succeed promptly */
    assert(server_stop(&server) == 0);

    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_server_stop_already_stopped\n");
}

static void test_server_stop_while_client_active(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(20000);

    /* Connect client and execute an initial command */
    int client_fd = tcp_connect(server.config.port);
    char buf[512];
    tcp_send_str(client_fd, "SET race_key race_val\r\n");
    assert(tcp_recv_line(client_fd, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* Stop server while client descriptor is active.
     * With shutdown() called under state_lock, descriptor cannot be closed and reused concurrently. */
    assert(server_stop(&server) == 0);

    /* Server thread must join cleanly and promptly */
    assert(pthread_join(tid, NULL) == 0);

    close(client_fd);
    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_server_stop_while_client_active\n");
}

static void test_server_null_and_invalid_params(void) {
    assert(server_signal_atomics_lock_free() == true);
    assert(server_stop(NULL) == -1);
    assert(server_start(NULL) == -1);
    assert(server_init(NULL, NULL, NULL) == -1);
    server_cleanup(NULL);
    server_config_init_defaults(NULL);

    printf("  [PASS] test_server_null_and_invalid_params\n");
}

#define NUM_CONC_CLIENTS 8
#define OPS_PER_CLIENT 50

typedef struct {
    uint16_t port;
    int client_id;
} conc_client_arg_t;

static void *conc_client_worker(void *arg) {
    conc_client_arg_t *carg = (conc_client_arg_t *)arg;
    int fd = tcp_connect(carg->port);
    char buf[512];
    char cmd[256];

    for (int i = 0; i < OPS_PER_CLIENT; i++) {
        /* SET cX:kY valY */
        snprintf(cmd, sizeof(cmd), "SET c%d:k%d val_%d\r\n", carg->client_id, i, i);
        tcp_send_str(fd, cmd);
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "OK\r\n") == 0);

        /* GET cX:kY */
        snprintf(cmd, sizeof(cmd), "GET c%d:k%d\r\n", carg->client_id, i);
        tcp_send_str(fd, cmd);
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        char expected_val[256];
        snprintf(expected_val, sizeof(expected_val), "VALUE val_%d\r\n", i);
        assert(strcmp(buf, expected_val) == 0);

        /* EXISTS cX:kY */
        snprintf(cmd, sizeof(cmd), "EXISTS c%d:k%d\r\n", carg->client_id, i);
        tcp_send_str(fd, cmd);
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "1\r\n") == 0);

        /* DEL cX:kY */
        snprintf(cmd, sizeof(cmd), "DEL c%d:k%d\r\n", carg->client_id, i);
        tcp_send_str(fd, cmd);
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "DELETED\r\n") == 0);
    }

    /* COUNT */
    tcp_send_str(fd, "COUNT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strncmp(buf, "COUNT ", 6) == 0);

    /* QUIT */
    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);

    close(fd);
    return NULL;
}

static void test_server_concurrent_multi_client_load(uint16_t port) {
    pthread_t threads[NUM_CONC_CLIENTS];
    conc_client_arg_t args[NUM_CONC_CLIENTS];

    for (int i = 0; i < NUM_CONC_CLIENTS; i++) {
        args[i].port = port;
        args[i].client_id = i;
        assert(pthread_create(&threads[i], NULL, conc_client_worker, &args[i]) == 0);
    }

    for (int i = 0; i < NUM_CONC_CLIENTS; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    printf("  [PASS] test_server_concurrent_multi_client_load (%d clients, %d ops each)\n",
           NUM_CONC_CLIENTS, OPS_PER_CLIENT);
}

static void *contention_client_worker(void *arg) {
    uint16_t port = *(uint16_t *)arg;
    int fd = tcp_connect(port);
    char buf[512];

    for (int i = 0; i < 40; i++) {
        tcp_send_str(fd, "SET shared:counter tick\r\n");
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "OK\r\n") == 0);

        tcp_send_str(fd, "GET shared:counter\r\n");
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "VALUE tick\r\n") == 0 || strcmp(buf, "NOT_FOUND\r\n") == 0);

        tcp_send_str(fd, "EXISTS shared:counter\r\n");
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "1\r\n") == 0 || strcmp(buf, "0\r\n") == 0);

        tcp_send_str(fd, "DEL shared:counter\r\n");
        assert(tcp_recv_line(fd, buf, sizeof(buf)));
        assert(strcmp(buf, "DELETED\r\n") == 0 || strcmp(buf, "NOT_FOUND\r\n") == 0);
    }

    tcp_send_str(fd, "QUIT\r\n");
    assert(tcp_recv_line(fd, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(fd);
    return NULL;
}

static void test_server_concurrent_shared_key_contention(uint16_t port) {
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        assert(pthread_create(&threads[i], NULL, contention_client_worker, &port) == 0);
    }
    for (int i = 0; i < 4; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    printf("  [PASS] test_server_concurrent_shared_key_contention\n");
}

static void test_server_queue_full_and_backpressure(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.num_threads = 2;       /* 2 worker threads */
    config.max_connections = 2;   /* Queue capacity 2 */
    config.mode = SERVER_MODE_THREADPOOL;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);
    usleep(20000);

    /* Connect 2 active clients that occupy the 2 worker threads */
    int active1 = tcp_connect(server.config.port);
    int active2 = tcp_connect(server.config.port);

    char buf[512];
    tcp_send_str(active1, "SET k_act1 v1\r\n");
    assert(tcp_recv_line(active1, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(active2, "SET k_act2 v2\r\n");
    assert(tcp_recv_line(active2, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    /* Connect 2 more clients: these fill the connection queue (capacity = 2) */
    int queued1 = tcp_connect(server.config.port);
    int queued2 = tcp_connect(server.config.port);

    /* Active 1 sends QUIT, freeing a worker thread */
    tcp_send_str(active1, "QUIT\r\n");
    assert(tcp_recv_line(active1, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(active1);

    /* The freed worker dequeues queued1, which can now execute commands */
    tcp_send_str(queued1, "SET k_q1 v_q1\r\n");
    assert(tcp_recv_line(queued1, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(queued1, "QUIT\r\n");
    assert(tcp_recv_line(queued1, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(queued1);

    /* Active 2 sends QUIT */
    tcp_send_str(active2, "QUIT\r\n");
    assert(tcp_recv_line(active2, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(active2);

    /* Queued 2 executes commands */
    tcp_send_str(queued2, "SET k_q2 v_q2\r\n");
    assert(tcp_recv_line(queued2, buf, sizeof(buf)));
    assert(strcmp(buf, "OK\r\n") == 0);

    tcp_send_str(queued2, "QUIT\r\n");
    assert(tcp_recv_line(queued2, buf, sizeof(buf)));
    assert(strcmp(buf, "BYE\r\n") == 0);
    close(queued2);

    /* Clean server shutdown */
    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);

    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_server_queue_full_and_backpressure\n");
}

static atomic_bool g_load_active = true;

static void *continuous_client_worker(void *arg) {
    uint16_t port = *(uint16_t *)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

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
    while (atomic_load_explicit(&g_load_active, memory_order_acquire)) {
        if (send(fd, "COUNT\r\n", 7, 0) <= 0) break;
        if (!tcp_recv_line(fd, buf, sizeof(buf))) break;
        usleep(2000);
    }

    close(fd);
    return NULL;
}

static void test_server_shutdown_under_concurrent_load(void) {
    hashtable_t *ht = hashtable_create(32);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0;
    config.num_threads = 4;
    config.mode = SERVER_MODE_THREADPOOL;

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t srv_tid;
    assert(pthread_create(&srv_tid, NULL, server_thread_worker, &server) == 0);
    usleep(20000);

    atomic_store_explicit(&g_load_active, true, memory_order_release);
    uint16_t port = server.config.port;
    pthread_t clients[6];
    for (int i = 0; i < 6; i++) {
        int rc = pthread_create(&clients[i], NULL, continuous_client_worker, &port);
        assert(rc == 0);
    }

    usleep(25000); /* Let clients generate concurrent load */

    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    /* Issue shutdown while multiple clients are actively sending commands */
    assert(server_stop(&server) == 0);

    /* Server thread and workers must join promptly */
    assert(pthread_join(srv_tid, NULL) == 0);

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    double elapsed = (double)(end_ts.tv_sec - start_ts.tv_sec) +
                     (double)(end_ts.tv_nsec - start_ts.tv_nsec) / 1e9;
    assert(elapsed < 2.0);

    atomic_store_explicit(&g_load_active, false, memory_order_release);
    for (int i = 0; i < 6; i++) {
        int rc = pthread_join(clients[i], NULL);
        assert(rc == 0);
    }

    server_cleanup(&server);
    hashtable_destroy(ht);

    printf("  [PASS] test_server_shutdown_under_concurrent_load (unblocked in %.3f sec)\n", elapsed);
}

static void test_server_repeated_concurrency_suite(void) {
    for (int iter = 0; iter < 5; iter++) {
        test_server_queue_full_and_backpressure();
        test_server_shutdown_under_concurrent_load();
    }
    printf("  [PASS] test_server_repeated_concurrency_suite (stress tested 5 cycles)\n");
}

int main(void) {
    printf("Running TCP Server Integration Tests:\n");

    /* Initialize logger to discard debug noise during tests */
    logger_init(NULL, LOG_LEVEL_WARN, false);

    hashtable_t *ht = hashtable_create(64);
    assert(ht != NULL);

    server_config_t config;
    server_config_init_defaults(&config);
    config.port = 0; /* Bind to ephemeral port to guarantee zero port collisions */

    server_t server;
    assert(server_init(&server, &config, ht) == 0);
    assert(server.config.port > 0);

    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_worker, &server) == 0);

    /* Give background server thread a moment to enter accept loop */
    usleep(25000);

    uint16_t port = server.config.port;

    test_server_basic_crud(port);
    test_server_sequential_clients(port);
    test_server_partial_reads(port);
    test_server_pipelined_commands(port);
    test_server_malformed_and_control_chars(port);
    test_server_oversized_line(port);
    test_server_oversized_without_newline(port);
    test_server_embedded_nul(port);
    test_server_abrupt_disconnect(port);

    /* Phase 4 concurrent load tests on running server */
    test_server_concurrent_multi_client_load(port);
    test_server_concurrent_shared_key_contention(port);

    /* Stop server cleanly and join thread */
    assert(server_stop(&server) == 0);
    assert(pthread_join(tid, NULL) == 0);

    server_cleanup(&server);
    hashtable_destroy(ht);
    logger_close();

    /* Robust shutdown regression tests */
    test_server_null_and_invalid_params();
    test_server_stop_before_start();
    test_server_stop_already_stopped();
    test_server_repeated_shutdown_in_accept();
    test_server_stop_with_idle_client();
    test_server_stop_while_client_active();

    /* Phase 4 queue-full and concurrent shutdown tests */
    test_server_queue_full_and_backpressure();
    test_server_shutdown_under_concurrent_load();
    test_server_repeated_concurrency_suite();

    printf("All TCP Server integration tests passed successfully!\n");
    return EXIT_SUCCESS;
}

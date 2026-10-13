#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define DEFAULT_CLIENTS 10
#define DEFAULT_REQUESTS_PER_CLIENT 1000
#define DEFAULT_KEY_SPACE 1000
#define DEFAULT_GET_RATIO 80
#define DEFAULT_VAL_LEN 64

/**
 * Benchmark configuration parsed from CLI flags.
 */
typedef struct {
    char host[128];
    uint16_t port;
    int num_clients;
    int requests_per_client;
    int key_space;
    int get_ratio; /* 0 to 100 % GET requests */
    int val_len;
    int timeout_sec;
} bench_config_t;

/**
 * Thread-local worker execution state and recorded latencies.
 */
typedef struct {
    int thread_id;
    const bench_config_t *config;
    int successful_requests;
    int failed_requests;
    double *latencies_us; /* Latency of each completed request in microseconds */
} worker_context_t;

static void print_usage(const char *prog_name) {
    printf("MiniServe Phase 7 High-Performance Benchmark Client\n");
    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");
    printf("  --host <ip>        Server IPv4 address (default: %s)\n", DEFAULT_HOST);
    printf("  --port <port>      Server TCP port (default: %d)\n", DEFAULT_PORT);
    printf("  -c, --clients <N>  Number of concurrent client threads (default: %d)\n", DEFAULT_CLIENTS);
    printf("  -n, --requests <M> Requests per client thread (default: %d)\n", DEFAULT_REQUESTS_PER_CLIENT);
    printf("  --keys <K>         Key space size (default: %d)\n", DEFAULT_KEY_SPACE);
    printf("  --ratio <pct>      Percentage of GET requests (0-100, default: %d)\n", DEFAULT_GET_RATIO);
    printf("  --val-len <bytes>  Payload value length in bytes (default: %d)\n", DEFAULT_VAL_LEN);
    printf("  -h, --help         Display this help message and exit\n");
}

static int parse_args(int argc, char **argv, bench_config_t *config) {
    /* Initialize default configuration */
    strncpy(config->host, DEFAULT_HOST, sizeof(config->host) - 1);
    config->host[sizeof(config->host) - 1] = '\0';
    config->port = DEFAULT_PORT;
    config->num_clients = DEFAULT_CLIENTS;
    config->requests_per_client = DEFAULT_REQUESTS_PER_CLIENT;
    config->key_space = DEFAULT_KEY_SPACE;
    config->get_ratio = DEFAULT_GET_RATIO;
    config->val_len = DEFAULT_VAL_LEN;
    config->timeout_sec = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        } else if (strcmp(argv[i], "--host") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --host requires an IP address\n");
                return -1;
            }
            strncpy(config->host, argv[i], sizeof(config->host) - 1);
            config->host[sizeof(config->host) - 1] = '\0';
        } else if (strcmp(argv[i], "--port") == 0) {
            if (++i >= argc || !parse_port(argv[i], &config->port)) {
                fprintf(stderr, "Error: Invalid port number: %s\n", (i < argc) ? argv[i] : "");
                return -1;
            }
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clients") == 0) {
            if (++i >= argc || !parse_int(argv[i], &config->num_clients, 1, 2048)) {
                fprintf(stderr, "Error: Invalid clients count (1-2048): %s\n", (i < argc) ? argv[i] : "");
                return -1;
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--requests") == 0) {
            if (++i >= argc || !parse_int(argv[i], &config->requests_per_client, 1, 10000000)) {
                fprintf(stderr, "Error: Invalid requests per client: %s\n", (i < argc) ? argv[i] : "");
                return -1;
            }
        } else if (strcmp(argv[i], "--keys") == 0) {
            if (++i >= argc || !parse_int(argv[i], &config->key_space, 1, 1000000)) {
                fprintf(stderr, "Error: Invalid key space count: %s\n", (i < argc) ? argv[i] : "");
                return -1;
            }
        } else if (strcmp(argv[i], "--ratio") == 0) {
            if (++i >= argc || !parse_int(argv[i], &config->get_ratio, 0, 100)) {
                fprintf(stderr, "Error: Invalid GET ratio percentage (0-100): %s\n", (i < argc) ? argv[i] : "");
                return -1;
            }
        } else if (strcmp(argv[i], "--val-len") == 0) {
            if (++i >= argc || !parse_int(argv[i], &config->val_len, 1, 400)) {
                fprintf(stderr, "Error: Invalid value length (1-400 bytes): %s\n", (i < argc) ? argv[i] : "");
                return -1;
            }
        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

static int connect_to_server(const bench_config_t *config) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    /* Set socket receive and send timeouts */
    struct timeval tv;
    tv.tv_sec = config->timeout_sec;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const void *)&tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const void *)&tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config->port);
    if (inet_pton(AF_INET, config->host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static bool read_response_line(int fd, char *buf, size_t max_len) {
    size_t idx = 0;
    while (idx < max_len - 1) {
        ssize_t n = recv(fd, buf + idx, max_len - 1 - idx, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
            return false;
        }
        idx += (size_t)n;
        buf[idx] = '\0';
        if (memchr(buf, '\n', idx) != NULL) {
            return true;
        }
    }
    return false;
}

static void *benchmark_worker(void *arg) {
    worker_context_t *ctx = (worker_context_t *)arg;
    const bench_config_t *cfg = ctx->config;

    int fd = connect_to_server(cfg);
    if (fd < 0) {
        ctx->failed_requests = cfg->requests_per_client;
        return NULL;
    }

    /* Thread-safe PRNG state */
    unsigned int seed = (unsigned int)(time(NULL) ^ (uintptr_t)pthread_self() ^ (unsigned int)ctx->thread_id);

    char val_payload[512];
    memset(val_payload, 'A' + (ctx->thread_id % 26), (size_t)cfg->val_len);
    val_payload[cfg->val_len] = '\0';

    char req_buf[1024];
    char resp_buf[1024];

    for (int req = 0; req < cfg->requests_per_client; req++) {
        int key_idx = (int)(rand_r(&seed) % (unsigned int)cfg->key_space);
        bool is_get = ((int)(rand_r(&seed) % 100) < cfg->get_ratio);

        int req_len;
        if (is_get) {
            req_len = snprintf(req_buf, sizeof(req_buf), "GET bench:k%d\r\n", key_idx);
        } else {
            req_len = snprintf(req_buf, sizeof(req_buf), "SET bench:k%d %s\r\n", key_idx, val_payload);
        }

        if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
            ctx->failed_requests++;
            continue;
        }

        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        /* Send request */
        size_t total_sent = 0;
        bool send_ok = true;
        while (total_sent < (size_t)req_len) {
            ssize_t s = send(fd, req_buf + total_sent, (size_t)req_len - total_sent, MSG_NOSIGNAL);
            if (s <= 0) {
                if (s < 0 && (errno == EINTR)) {
                    continue;
                }
                send_ok = false;
                break;
            }
            total_sent += (size_t)s;
        }

        if (!send_ok) {
            ctx->failed_requests += (cfg->requests_per_client - req);
            break;
        }

        /* Read response line */
        if (!read_response_line(fd, resp_buf, sizeof(resp_buf))) {
            ctx->failed_requests += (cfg->requests_per_client - req);
            break;
        }

        /* Validate response format according to command type */
        bool resp_ok = false;
        if (is_get) {
            if (strncmp(resp_buf, "VALUE ", 6) == 0 || strncmp(resp_buf, "NOT_FOUND", 9) == 0) {
                resp_ok = true;
            }
        } else {
            if (strncmp(resp_buf, "OK", 2) == 0) {
                resp_ok = true;
            }
        }

        if (!resp_ok) {
            ctx->failed_requests++;
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts_end);

        double elapsed_us = (double)(ts_end.tv_sec - ts_start.tv_sec) * 1000000.0 +
                            (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1000.0;

        ctx->latencies_us[ctx->successful_requests] = elapsed_us;
        ctx->successful_requests++;
    }

    /* Gracefully notify server and flush before closing */
    (void)send(fd, "QUIT\r\n", 6, MSG_NOSIGNAL);
    (void)read_response_line(fd, resp_buf, sizeof(resp_buf));
    close(fd);
    return NULL;
}

static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static inline double get_percentile(const double *sorted, int n, double pct) {
    if (n <= 0) return 0.0;
    size_t idx = (size_t)((double)n * pct);
    if (idx >= (size_t)n) {
        idx = (size_t)(n - 1);
    }
    return sorted[idx];
}

int main(int argc, char **argv) {
    /* Ignore SIGPIPE so writing to terminated peer generates EPIPE */
    signal(SIGPIPE, SIG_IGN);

    bench_config_t config;
    int parse_res = parse_args(argc, argv, &config);
    if (parse_res != 0) {
        return (parse_res > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    printf("================================================================================\n");
    printf(" MiniServe Phase 7 Benchmark Client\n");
    printf(" Target Server   : %s:%u\n", config.host, config.port);
    printf(" Concurrency     : %d client threads\n", config.num_clients);
    printf(" Requests/Client : %d requests (Total: %d)\n",
           config.requests_per_client, config.num_clients * config.requests_per_client);
    printf(" Key Space       : %d keys | GET/SET Ratio: %d%% GET / %d%% SET\n",
           config.key_space, config.get_ratio, 100 - config.get_ratio);
    printf(" Value Length    : %d bytes\n", config.val_len);
    printf("================================================================================\n\n");

    /* Quick pre-flight connectivity check */
    int test_fd = connect_to_server(&config);
    if (test_fd < 0) {
        fprintf(stderr, "Error: Unable to connect to MiniServe at %s:%u (%s)\n",
                config.host, config.port, strerror(errno));
        return EXIT_FAILURE;
    }
    char bye_buf[64];
    (void)send(test_fd, "QUIT\r\n", 6, MSG_NOSIGNAL);
    (void)read_response_line(test_fd, bye_buf, sizeof(bye_buf));
    close(test_fd);

    /* Allocate worker contexts and latency buffers */
    pthread_t *threads = calloc((size_t)config.num_clients, sizeof(pthread_t));
    bool *thread_created = calloc((size_t)config.num_clients, sizeof(bool));
    worker_context_t *contexts = calloc((size_t)config.num_clients, sizeof(worker_context_t));
    if (threads == NULL || thread_created == NULL || contexts == NULL) {
        fprintf(stderr, "Error: Memory allocation failed for benchmark workers\n");
        free(threads);
        free(thread_created);
        free(contexts);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < config.num_clients; i++) {
        contexts[i].thread_id = i;
        contexts[i].config = &config;
        contexts[i].successful_requests = 0;
        contexts[i].failed_requests = 0;
        contexts[i].latencies_us = calloc((size_t)config.requests_per_client, sizeof(double));
        if (contexts[i].latencies_us == NULL) {
            fprintf(stderr, "Error: Memory allocation failed for latency array %d\n", i);
            for (int j = 0; j < i; j++) {
                free(contexts[j].latencies_us);
            }
            free(threads);
            free(thread_created);
            free(contexts);
            return EXIT_FAILURE;
        }
    }

    printf("Executing benchmark...\n");
    struct timespec bench_start, bench_end;
    clock_gettime(CLOCK_MONOTONIC, &bench_start);

    for (int i = 0; i < config.num_clients; i++) {
        int rc = pthread_create(&threads[i], NULL, benchmark_worker, &contexts[i]);
        if (rc == 0) {
            thread_created[i] = true;
        } else {
            fprintf(stderr, "Error: Failed to create thread %d: %s\n", i, strerror(rc));
            thread_created[i] = false;
        }
    }

    for (int i = 0; i < config.num_clients; i++) {
        if (thread_created[i]) {
            (void)pthread_join(threads[i], NULL);
        } else {
            contexts[i].failed_requests = config.requests_per_client;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &bench_end);

    double total_wall_sec = (double)(bench_end.tv_sec - bench_start.tv_sec) +
                           (double)(bench_end.tv_nsec - bench_start.tv_nsec) / 1e9;

    /* Aggregate results across all threads */
    int total_expected = config.num_clients * config.requests_per_client;
    int total_success = 0;
    int total_failed = 0;
    for (int i = 0; i < config.num_clients; i++) {
        total_success += contexts[i].successful_requests;
        total_failed += contexts[i].failed_requests;
    }
    if (total_success + total_failed < total_expected) {
        total_failed = total_expected - total_success;
    }

    if (total_success == 0) {
        fprintf(stderr, "\nBenchmark failed: Zero requests succeeded out of %d total.\n",
                total_expected);
        for (int i = 0; i < config.num_clients; i++) {
            free(contexts[i].latencies_us);
        }
        free(threads);
        free(thread_created);
        free(contexts);
        return EXIT_FAILURE;
    }

    double *all_latencies = malloc((size_t)total_success * sizeof(double));
    if (all_latencies == NULL) {
        fprintf(stderr, "Error: Failed to allocate aggregated latency buffer\n");
        for (int i = 0; i < config.num_clients; i++) {
            free(contexts[i].latencies_us);
        }
        free(threads);
        free(thread_created);
        free(contexts);
        return EXIT_FAILURE;
    }

    size_t copy_offset = 0;
    double sum_us = 0.0;
    for (int i = 0; i < config.num_clients; i++) {
        for (int j = 0; j < contexts[i].successful_requests; j++) {
            double lat = contexts[i].latencies_us[j];
            all_latencies[copy_offset++] = lat;
            sum_us += lat;
        }
    }

    qsort(all_latencies, (size_t)total_success, sizeof(double), compare_doubles);

    double min_lat = all_latencies[0];
    double max_lat = all_latencies[total_success - 1];
    double mean_lat = sum_us / (double)total_success;

    double p50 = get_percentile(all_latencies, total_success, 0.50);
    double p75 = get_percentile(all_latencies, total_success, 0.75);
    double p90 = get_percentile(all_latencies, total_success, 0.90);
    double p95 = get_percentile(all_latencies, total_success, 0.95);
    double p99 = get_percentile(all_latencies, total_success, 0.99);
    double p999 = get_percentile(all_latencies, total_success, 0.999);

    double rps = (double)total_success / total_wall_sec;

    printf("\n================================================================================\n");
    printf(" BENCHMARK RESULTS SUMMARY\n");
    printf("================================================================================\n");
    printf(" Total Completed Requests : %d\n", total_success);
    printf(" Failed / Timed Out       : %d\n", total_failed);
    printf(" Elapsed Wall-Clock Time  : %.3f seconds\n", total_wall_sec);
    printf(" Throughput               : %.2f requests/sec\n", rps);
    printf("--------------------------------------------------------------------------------\n");
    printf(" LATENCY DISTRIBUTION (ms)\n");
    printf("--------------------------------------------------------------------------------\n");
    printf("   Min     : %8.3f ms\n", min_lat / 1000.0);
    printf("   Mean    : %8.3f ms\n", mean_lat / 1000.0);
    printf("   p50     : %8.3f ms (Median)\n", p50 / 1000.0);
    printf("   p75     : %8.3f ms\n", p75 / 1000.0);
    printf("   p90     : %8.3f ms\n", p90 / 1000.0);
    printf("   p95     : %8.3f ms\n", p95 / 1000.0);
    printf("   p99     : %8.3f ms\n", p99 / 1000.0);
    printf("   p99.9   : %8.3f ms\n", p999 / 1000.0);
    printf("   Max     : %8.3f ms\n", max_lat / 1000.0);
    printf("================================================================================\n\n");

    /* Cleanup memory */
    free(all_latencies);
    for (int i = 0; i < config.num_clients; i++) {
        free(contexts[i].latencies_us);
    }
    free(threads);
    free(thread_created);
    free(contexts);

    /* Return failure if any requests failed or timed out */
    if (total_failed > 0) {
        fprintf(stderr, "Error: Benchmark encountered %d failed or timed-out request(s) out of %d.\n",
                total_failed, total_expected);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

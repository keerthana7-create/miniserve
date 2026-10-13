#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "server.h"
#include "util.h"

static void print_usage(const char *prog_name) {
    printf("MiniServe - High-Performance In-Memory Key-Value Server\n\n");
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -p, --port <port>             TCP port to listen on (default: 8080)\n");
    printf("  -t, --threads <num>           Number of worker threads (default: 4, range: 1-128)\n");
    printf("  -m, --mode <single|threadpool|epoll>\n");
    printf("                                Concurrency architecture mode (default: threadpool)\n");
    printf("  -l, --log-file <path>         File path to write persistent logs (default: stderr only)\n");
    printf("  -c, --max-connections <num>   Maximum connection queue capacity (default: 1024)\n");
    printf("  -T, --idle-timeout <sec>      Client inactivity timeout in seconds (default: 30, range: 1-3600)\n");
    printf("  -K, --max-keys <num>          Maximum key capacity before LRU eviction (default: 0 = unbounded)\n");
    printf("  -h, --help                    Display this help message and exit\n");
}

static void print_banner(const server_config_t *config) {
    const char *mode_str = "threadpool";
    if (config->mode == SERVER_MODE_SINGLE) {
        mode_str = "single";
    } else if (config->mode == SERVER_MODE_EPOLL) {
        mode_str = "epoll";
    }

    printf("\n");
    printf(" ================================================================\n");
    printf("   __  __ _       _ ____                                         \n");
    printf("  |  \\/  (_)_ __ (_) ___|  ___ _ ____   _____                    \n");
    printf("  | |\\/| | | '_ \\| \\___ \\ / _ \\ '__\\ \\ / / _ \\             \n");
    printf("  | |  | | | | | | |___) |  __/ |   \\ V /  __/                   \n");
    printf("  |_|  |_|_|_| |_|_|____/ \\___|_|    \\_/ \\___|                   \n");
    printf(" ================================================================\n");
    printf("  Listening Port:     %u\n", config->port);
    printf("  Worker Threads:     %d%s\n", config->num_threads,
           (config->mode == SERVER_MODE_EPOLL) ? " (unused in epoll mode)" : "");
    printf("  Concurrency Mode:   %s\n", mode_str);
    printf("  Queue Capacity:     %d (bounded connection queue)\n", config->max_connections);
    printf("  Idle Timeout:       %d seconds\n", config->idle_timeout_sec);
    printf("  Max Keys (LRU):     %zu%s\n", config->max_keys, (config->max_keys == 0) ? " (unbounded)" : "");
    printf("  Log Destination:    %s\n", config->log_file[0] != '\0' ? config->log_file : "stderr");
    printf(" ================================================================\n\n");
}

int main(int argc, char *argv[]) {
    server_config_t config;
    server_config_init_defaults(&config);

    static struct option long_options[] = {
        {"port",            required_argument, 0, 'p'},
        {"threads",         required_argument, 0, 't'},
        {"mode",            required_argument, 0, 'm'},
        {"log-file",        required_argument, 0, 'l'},
        {"max-connections", required_argument, 0, 'c'},
        {"idle-timeout",    required_argument, 0, 'T'},
        {"max-keys",        required_argument, 0, 'K'},
        {"help",            no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt = 0;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "p:t:m:l:c:T:K:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'p': {
                if (!parse_port(optarg, &config.port)) {
                    fprintf(stderr, "Error: Invalid port number '%s'. Must be between 1 and 65535.\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            }
            case 't': {
                if (!parse_int(optarg, &config.num_threads, 1, 128)) {
                    fprintf(stderr, "Error: Invalid thread count '%s'. Must be between 1 and 128.\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'm': {
                if (strcmp(optarg, "single") == 0) {
                    config.mode = SERVER_MODE_SINGLE;
                } else if (strcmp(optarg, "threadpool") == 0) {
                    config.mode = SERVER_MODE_THREADPOOL;
                } else if (strcmp(optarg, "epoll") == 0) {
                    config.mode = SERVER_MODE_EPOLL;
                } else {
                    fprintf(stderr, "Error: Invalid concurrency mode '%s'. Supported: 'single', 'threadpool', 'epoll'.\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'l': {
                size_t len = strlen(optarg);
                if (len >= sizeof(config.log_file)) {
                    fprintf(stderr, "Error: Log file path exceeds maximum length of %zu characters.\n", sizeof(config.log_file) - 1);
                    return EXIT_FAILURE;
                }
                memcpy(config.log_file, optarg, len);
                config.log_file[len] = '\0';
                break;
            }
            case 'c': {
                if (!parse_int(optarg, &config.max_connections, 1, 65536)) {
                    fprintf(stderr, "Error: Invalid max connections '%s'. Must be between 1 and 65536.\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'T': {
                if (!parse_int(optarg, &config.idle_timeout_sec, 1, 3600)) {
                    fprintf(stderr, "Error: Invalid idle timeout '%s'. Must be between 1 and 3600 seconds.\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'K': {
                int max_k = 0;
                if (!parse_int(optarg, &max_k, 0, 10000000)) {
                    fprintf(stderr, "Error: Invalid max keys '%s'. Must be >= 0.\n", optarg);
                    return EXIT_FAILURE;
                }
                config.max_keys = (size_t)max_k;
                break;
            }
            case 'h': {
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            }
            default: {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
        }
    }

    if (optind < argc) {
        fprintf(stderr, "Error: Unexpected positional argument '%s'.\n", argv[optind]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (config.mode == SERVER_MODE_SINGLE && config.num_threads > 1) {
        fprintf(stderr, "Error: Concurrency mode 'single' supports only 1 worker thread (got %d).\n", config.num_threads);
        return EXIT_FAILURE;
    }

    /* Initialize thread-safe logging subsystem */
    const char *log_path = config.log_file[0] != '\0' ? config.log_file : NULL;
    if (logger_init(log_path, LOG_LEVEL_INFO, true) != 0) {
        perror("Failed to initialize logger");
        return EXIT_FAILURE;
    }

    print_banner(&config);

    const char *mode_str = "threadpool";
    if (config.mode == SERVER_MODE_SINGLE) {
        mode_str = "single";
    } else if (config.mode == SERVER_MODE_EPOLL) {
        mode_str = "epoll";
    }

    LOG_INFO("MiniServe starting up...");
    LOG_INFO("Configuration loaded: port=%u, threads=%d, mode=%s, max_conn=%d, idle_timeout=%ds",
             config.port, config.num_threads, mode_str,
             config.max_connections, config.idle_timeout_sec);

    /* Allocate hash table storage engine */
    hashtable_t *ht = hashtable_create(1024);
    if (ht == NULL) {
        LOG_FATAL("Failed to allocate hash table storage engine.");
        logger_close();
        return EXIT_FAILURE;
    }

    if (config.max_keys > 0) {
        hashtable_set_max_capacity(ht, config.max_keys);
        LOG_INFO("LRU cache eviction enabled (max_keys=%zu).", config.max_keys);
    }

    LOG_INFO("Storage engine initialized (1024 initial buckets).");

    /* Run server engine */
    int rc = server_run(&config, ht);

    /* Clean up storage engine */
    hashtable_destroy(ht);
    LOG_INFO("Storage engine released.");

    /* Graceful teardown of initialized subsystems */
    LOG_INFO("Shutting down MiniServe.");
    if (logger_close() != 0) {
        fprintf(stderr, "Warning: Failed to flush and close logger cleanly.\n");
        return EXIT_FAILURE;
    }

    return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

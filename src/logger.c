#include "logger.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Internal logger state protected by log_mutex */
static FILE *g_log_file = NULL;
static log_level_t g_min_level = LOG_LEVEL_INFO;
static bool g_log_to_stderr = true;
static bool g_is_tty = false;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;

/* String representation of log levels */
static const char *const LEVEL_STRINGS[] = {
    "DEBUG",
    "INFO ",
    "WARN ",
    "ERROR",
    "FATAL"
};

/* ANSI color codes for interactive terminal output */
static const char *const LEVEL_COLORS[] = {
    "\x1b[36m", /* DEBUG: Cyan */
    "\x1b[32m", /* INFO:  Green */
    "\x1b[33m", /* WARN:  Yellow */
    "\x1b[31m", /* ERROR: Red */
    "\x1b[35m"  /* FATAL: Magenta */
};
static const char *const COLOR_RESET = "\x1b[0m";

int logger_init(const char *filepath, log_level_t min_level, bool log_to_stderr) {
    if (pthread_mutex_lock(&g_log_mutex) != 0) {
        return -1;
    }

    int ret = 0;

    /* If already opened, flush and close previous file handle */
    if (g_log_file != NULL) {
        if (fflush(g_log_file) != 0) {
            ret = -1;
        }
        if (fclose(g_log_file) != 0) {
            ret = -1;
        }
        g_log_file = NULL;
    }

    g_min_level = min_level;
    g_log_to_stderr = log_to_stderr;
    g_is_tty = isatty(STDERR_FILENO) == 1;

    if (filepath != NULL && filepath[0] != '\0') {
        g_log_file = fopen(filepath, "a");
        if (g_log_file == NULL) {
            ret = -1;
        }
    }

    if (ret == 0) {
        g_initialized = true;
    }

    if (pthread_mutex_unlock(&g_log_mutex) != 0) {
        return -1;
    }
    return ret;
}

int logger_close(void) {
    if (pthread_mutex_lock(&g_log_mutex) != 0) {
        return -1;
    }

    int ret = 0;
    if (g_log_file != NULL) {
        if (fflush(g_log_file) != 0) {
            ret = -1;
        }
        if (fclose(g_log_file) != 0) {
            ret = -1;
        }
        g_log_file = NULL;
    }

    g_initialized = false;
    if (pthread_mutex_unlock(&g_log_mutex) != 0) {
        return -1;
    }
    return ret;
}

int logger_log(log_level_t level, const char *file, int line, const char *fmt, ...) {
    /* Validate log level bounds before indexing LEVEL_STRINGS or LEVEL_COLORS */
    if (level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_FATAL) {
        return -1;
    }

    if (pthread_mutex_lock(&g_log_mutex) != 0) {
        return -1;
    }

    /*
     * Correctness first: The unlocked fast-path check was removed because reading
     * g_min_level without holding g_log_mutex while logger_init() writes it under
     * the mutex is a data race (C11 §5.1.2.4) that ThreadSanitizer flags.
     * The cost of acquiring an uncontended mutex to evaluate the filter level
     * is negligible at this stage.
     */
    if (g_initialized && level < g_min_level) {
        (void)pthread_mutex_unlock(&g_log_mutex);
        return 0;
    }

    /* Extract high-resolution system timestamp */
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
    }

    /* Convert to broken-down calendar time using reentrant localtime_r */
    struct tm tm_buf;
    if (localtime_r(&ts.tv_sec, &tm_buf) == NULL) {
        (void)pthread_mutex_unlock(&g_log_mutex);
        return -1;
    }

    char time_str[32];
    (void)strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
    long millis = ts.tv_nsec / 1000000L;

    /* Strip leading directory path from __FILE__ for concise log output */
    const char *short_file = strrchr(file, '/');
    if (short_file == NULL) {
        short_file = strrchr(file, '\\');
    }
    short_file = (short_file != NULL) ? short_file + 1 : file;

    /*
     * Safe fallback: If logger_log is called before logger_init or after
     * logger_close (!g_initialized), do not crash and do not touch g_log_file.
     * Output the message safely to stderr with identical formatting and return 0.
     */
    if (!g_initialized) {
        va_list args;
        va_start(args, fmt);
        if (isatty(STDERR_FILENO) == 1) {
            fprintf(stderr, "[%s.%03ld] [%s%s%s] [%s:%d] ",
                    time_str, millis,
                    LEVEL_COLORS[level], LEVEL_STRINGS[level], COLOR_RESET,
                    short_file, line);
        } else {
            fprintf(stderr, "[%s.%03ld] [%s] [%s:%d] ",
                    time_str, millis,
                    LEVEL_STRINGS[level],
                    short_file, line);
        }
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
        (void)fflush(stderr);

        (void)pthread_mutex_unlock(&g_log_mutex);
        return 0;
    }

    int status = 0;

    /* 1. Write to stderr if enabled */
    if (g_log_to_stderr) {
        va_list args;
        va_start(args, fmt);

        int n1 = 0;
        if (g_is_tty) {
            n1 = fprintf(stderr, "[%s.%03ld] [%s%s%s] [%s:%d] ",
                         time_str, millis,
                         LEVEL_COLORS[level], LEVEL_STRINGS[level], COLOR_RESET,
                         short_file, line);
        } else {
            n1 = fprintf(stderr, "[%s.%03ld] [%s] [%s:%d] ",
                         time_str, millis,
                         LEVEL_STRINGS[level],
                         short_file, line);
        }

        int n2 = vfprintf(stderr, fmt, args);
        int n3 = fprintf(stderr, "\n");
        va_end(args);

        if (n1 < 0 || n2 < 0 || n3 < 0 || fflush(stderr) != 0) {
            status = -1;
        }
    }

    /* 2. Write to log file if configured */
    if (g_log_file != NULL) {
        va_list args;
        va_start(args, fmt);

        int n1 = fprintf(g_log_file, "[%s.%03ld] [%s] [%s:%d] ",
                         time_str, millis,
                         LEVEL_STRINGS[level],
                         short_file, line);

        int n2 = vfprintf(g_log_file, fmt, args);
        int n3 = fprintf(g_log_file, "\n");
        va_end(args);

        if (n1 < 0 || n2 < 0 || n3 < 0 || fflush(g_log_file) != 0) {
            status = -1;
        }
    }

    if (pthread_mutex_unlock(&g_log_mutex) != 0) {
        return -1;
    }
    return status;
}

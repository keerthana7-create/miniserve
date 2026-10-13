#ifndef MINISERVE_LOGGER_H
#define MINISERVE_LOGGER_H

#include <stdbool.h>

/**
 * Log levels in ascending order of severity.
 */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} log_level_t;

/**
 * Initialize the global logging subsystem.
 *
 * @param filepath Path to log file, or NULL to log only to stderr.
 * @param min_level Minimum severity level to record.
 * @param log_to_stderr If true, output will also be mirrored to stderr.
 * @return 0 on success, -1 on failure (e.g., file cannot be opened).
 */
int logger_init(const char *filepath, log_level_t min_level, bool log_to_stderr);

/**
 * Flush pending output and release logger resources (closes file handle).
 *
 * Thread ownership contract:
 * The caller is responsible for ensuring all worker threads performing logging
 * have ceased or joined before calling logger_close(). Calling logger_log()
 * concurrently during or after logger_close() results in undefined behavior.
 *
 * @return 0 on success, -1 if flushing or closing the stream encounters an error.
 */
int logger_close(void);

/**
 * Core logging routine. Protected by internal mutex for thread-safety.
 * Attribute format ensures printf-style format string and argument type checking at compile time.
 *
 * @return 0 on success, -1 on write/flush or synchronization failure.
 */
int logger_log(log_level_t level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/*
 * Strictly conforming C11 variadic logging macros.
 * Captures calling file and line while forwarding any format string and arguments via __VA_ARGS__.
 */
#define LOG_DEBUG(...) logger_log(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  logger_log(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  logger_log(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) logger_log(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) logger_log(LOG_LEVEL_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#endif /* MINISERVE_LOGGER_H */

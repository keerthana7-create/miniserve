#ifndef MINISERVE_UTIL_H
#define MINISERVE_UTIL_H

#include <stdbool.h>
#include <stdint.h>

/**
 * Safely parse a numeric string into an integer with strict range validation.
 * Rejects leading whitespace anomalies, trailing garbage characters, and overflow/underflow.
 *
 * @param str Input string to parse.
 * @param out Output integer pointer.
 * @param min_val Inclusive minimum acceptable value.
 * @param max_val Inclusive maximum acceptable value.
 * @return true on success, false if invalid or out of range.
 */
bool parse_int(const char *str, int *out, int min_val, int max_val);

/**
 * Safely parse a port number (valid range 1 to 65535).
 *
 * @param str Input string to parse.
 * @param out Pointer to output uint16_t.
 * @return true on success, false otherwise.
 */
bool parse_port(const char *str, uint16_t *out);

#endif /* MINISERVE_UTIL_H */

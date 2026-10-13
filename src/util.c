#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

bool parse_int(const char *str, int *out, int min_val, int max_val) {
    if (str == NULL || out == NULL || *str == '\0' || min_val > max_val) {
        return false;
    }

    /* Reject leading whitespace - callers must provide cleanly formatted tokens */
    if (isspace((unsigned char)*str)) {
        return false;
    }

    char *endptr = NULL;
    errno = 0;
    long val = strtol(str, &endptr, 10);

    /* Check for conversion errors, no conversion made, or unparsed trailing bytes */
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return false;
    }

    /* Check integer bounds */
    if (val < min_val || val > max_val) {
        return false;
    }

    *out = (int)val;
    return true;
}

bool parse_port(const char *str, uint16_t *out) {
    if (out == NULL) {
        return false;
    }

    int port_val = 0;
    /* Port numbers range from 1 to 65535 (port 0 is reserved in standard networking) */
    if (!parse_int(str, &port_val, 1, 65535)) {
        return false;
    }

    *out = (uint16_t)port_val;
    return true;
}

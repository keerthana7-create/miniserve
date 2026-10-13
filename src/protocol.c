#include "protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/**
 * Helper to skip leading whitespace characters.
 */
static const char *skip_whitespace(const char *p) {
    while (*p != '\0' && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

int protocol_parse(const char *line, command_request_t *req, command_response_t *res) {
    if (req == NULL || res == NULL) {
        return -1;
    }

    res->close_connection = false;
    res->response[0] = '\0';
    req->type = CMD_UNKNOWN;
    req->key[0] = '\0';
    req->value[0] = '\0';
    req->ttl_sec = 0;

    if (line == NULL) {
        snprintf(res->response, sizeof(res->response), "ERR MALFORMED Null command\r\n");
        return -1;
    }

    size_t raw_len = strlen(line);
    if (raw_len > PROTO_MAX_LINE_LEN) {
        snprintf(res->response, sizeof(res->response), "ERR OVERSIZED Command line exceeds 1024 bytes\r\n");
        return -1;
    }

    /* Copy into local buffer and strip trailing CRLF / LF */
    char buf[PROTO_MAX_LINE_LEN + 1];
    memcpy(buf, line, raw_len);
    buf[raw_len] = '\0';

    while (raw_len > 0 && (buf[raw_len - 1] == '\r' || buf[raw_len - 1] == '\n')) {
        buf[--raw_len] = '\0';
    }

    /* Reject control bytes within command content: < 0x20 except '\t', and 0x7F */
    for (size_t i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if ((c < 0x20 && c != '\t') || c == 0x7F) {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Control characters not allowed\r\n");
            return -1;
        }
    }

    const char *p = skip_whitespace(buf);
    if (*p == '\0') {
        snprintf(res->response, sizeof(res->response), "ERR MALFORMED Empty command\r\n");
        return -1;
    }

    /* Extract command verb */
    char verb[32];
    size_t verb_len = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && verb_len < sizeof(verb) - 1) {
        verb[verb_len++] = *p++;
    }
    verb[verb_len] = '\0';

    /* Match and process verbs */
    if (strcmp(verb, "SET") == 0) {
        p = skip_whitespace(p);
        if (*p == '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Missing key and value for SET\r\n");
            return -1;
        }

        /* Extract key */
        size_t key_len = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            if (key_len < PROTO_MAX_KEY_LEN) {
                req->key[key_len++] = *p;
            } else {
                key_len++;
            }
            p++;
        }

        if (key_len > PROTO_MAX_KEY_LEN) {
            snprintf(res->response, sizeof(res->response), "ERR OVERSIZED Key exceeds 64 bytes\r\n");
            return -1;
        }
        req->key[key_len] = '\0';

        p = skip_whitespace(p);
        if (*p == '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Missing value for SET\r\n");
            return -1;
        }

        /* Value is the remainder of the line */
        size_t val_len = strlen(p);
        if (val_len > PROTO_MAX_VAL_LEN) {
            snprintf(res->response, sizeof(res->response), "ERR OVERSIZED Value exceeds 512 bytes\r\n");
            return -1;
        }

        memcpy(req->value, p, val_len);
        req->value[val_len] = '\0';
        req->type = CMD_SET;
        return 0;
    }

    if (strcmp(verb, "SETEX") == 0) {
        p = skip_whitespace(p);
        if (*p == '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Missing arguments for SETEX\r\n");
            return -1;
        }

        /* Extract key */
        size_t key_len = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            if (key_len < PROTO_MAX_KEY_LEN) {
                req->key[key_len++] = *p;
            } else {
                key_len++;
            }
            p++;
        }

        if (key_len > PROTO_MAX_KEY_LEN) {
            snprintf(res->response, sizeof(res->response), "ERR OVERSIZED Key exceeds 64 bytes\r\n");
            return -1;
        }
        req->key[key_len] = '\0';

        p = skip_whitespace(p);
        if (*p == '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Missing TTL for SETEX\r\n");
            return -1;
        }

        /* Extract TTL string */
        char ttl_str[32];
        size_t ttl_len = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t' && ttl_len < sizeof(ttl_str) - 1) {
            ttl_str[ttl_len++] = *p++;
        }
        ttl_str[ttl_len] = '\0';

        int ttl_val = 0;
        if (!parse_int(ttl_str, &ttl_val, 1, 2147483647)) {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Invalid TTL for SETEX\r\n");
            return -1;
        }
        req->ttl_sec = ttl_val;

        p = skip_whitespace(p);
        if (*p == '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Missing value for SETEX\r\n");
            return -1;
        }

        /* Value is the remainder of the line */
        size_t val_len = strlen(p);
        if (val_len > PROTO_MAX_VAL_LEN) {
            snprintf(res->response, sizeof(res->response), "ERR OVERSIZED Value exceeds 512 bytes\r\n");
            return -1;
        }

        memcpy(req->value, p, val_len);
        req->value[val_len] = '\0';
        req->type = CMD_SETEX;
        return 0;
    }

    if (strcmp(verb, "GET") == 0 || strcmp(verb, "DEL") == 0 || strcmp(verb, "EXISTS") == 0 || strcmp(verb, "TTL") == 0) {
        p = skip_whitespace(p);
        if (*p == '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Missing key for %s\r\n", verb);
            return -1;
        }

        /* Extract key */
        size_t key_len = 0;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            if (key_len < PROTO_MAX_KEY_LEN) {
                req->key[key_len++] = *p;
            } else {
                key_len++;
            }
            p++;
        }

        if (key_len > PROTO_MAX_KEY_LEN) {
            snprintf(res->response, sizeof(res->response), "ERR OVERSIZED Key exceeds 64 bytes\r\n");
            return -1;
        }
        req->key[key_len] = '\0';

        /* Ensure no extraneous trailing arguments */
        p = skip_whitespace(p);
        if (*p != '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED Unexpected trailing arguments for %s\r\n", verb);
            return -1;
        }

        if (strcmp(verb, "GET") == 0) {
            req->type = CMD_GET;
        } else if (strcmp(verb, "DEL") == 0) {
            req->type = CMD_DEL;
        } else if (strcmp(verb, "TTL") == 0) {
            req->type = CMD_TTL;
        } else {
            req->type = CMD_EXISTS;
        }
        return 0;
    }

    if (strcmp(verb, "COUNT") == 0) {
        p = skip_whitespace(p);
        if (*p != '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED COUNT takes no arguments\r\n");
            return -1;
        }
        req->type = CMD_COUNT;
        return 0;
    }

    if (strcmp(verb, "QUIT") == 0) {
        p = skip_whitespace(p);
        if (*p != '\0') {
            snprintf(res->response, sizeof(res->response), "ERR MALFORMED QUIT takes no arguments\r\n");
            return -1;
        }
        req->type = CMD_QUIT;
        return 0;
    }

    snprintf(res->response, sizeof(res->response), "ERR UNKNOWN Unknown command '%s'\r\n", verb);
    return -1;
}

void protocol_execute(const command_request_t *req, hashtable_t *ht, command_response_t *res) {
    if (req == NULL || res == NULL) {
        return;
    }

    res->close_connection = false;

    switch (req->type) {
        case CMD_SET: {
            if (hashtable_set(ht, req->key, req->value) == 0) {
                snprintf(res->response, sizeof(res->response), "OK\r\n");
            } else {
                snprintf(res->response, sizeof(res->response), "ERR STORAGE Storage failure on SET\r\n");
            }
            break;
        }
        case CMD_SETEX: {
            if (hashtable_set_ttl(ht, req->key, req->value, req->ttl_sec) == 0) {
                snprintf(res->response, sizeof(res->response), "OK\r\n");
            } else {
                snprintf(res->response, sizeof(res->response), "ERR STORAGE Storage failure on SETEX\r\n");
            }
            break;
        }
        case CMD_GET: {
            char val_buf[PROTO_MAX_VAL_LEN + 1];
            if (hashtable_get(ht, req->key, val_buf, sizeof(val_buf))) {
                snprintf(res->response, sizeof(res->response), "VALUE %s\r\n", val_buf);
            } else {
                snprintf(res->response, sizeof(res->response), "NOT_FOUND\r\n");
            }
            break;
        }
        case CMD_DEL: {
            if (hashtable_del(ht, req->key)) {
                snprintf(res->response, sizeof(res->response), "DELETED\r\n");
            } else {
                snprintf(res->response, sizeof(res->response), "NOT_FOUND\r\n");
            }
            break;
        }
        case CMD_EXISTS: {
            if (hashtable_exists(ht, req->key)) {
                snprintf(res->response, sizeof(res->response), "1\r\n");
            } else {
                snprintf(res->response, sizeof(res->response), "0\r\n");
            }
            break;
        }
        case CMD_COUNT: {
            size_t n = hashtable_count(ht);
            snprintf(res->response, sizeof(res->response), "COUNT %zu\r\n", n);
            break;
        }
        case CMD_TTL: {
            int64_t rem = hashtable_get_ttl(ht, req->key);
            if (rem >= 0) {
                snprintf(res->response, sizeof(res->response), "TTL %lld\r\n", (long long)rem);
            } else if (rem == -1) {
                snprintf(res->response, sizeof(res->response), "-1\r\n");
            } else {
                snprintf(res->response, sizeof(res->response), "NOT_FOUND\r\n");
            }
            break;
        }
        case CMD_QUIT: {
            res->close_connection = true;
            snprintf(res->response, sizeof(res->response), "BYE\r\n");
            break;
        }
        default: {
            snprintf(res->response, sizeof(res->response), "ERR UNKNOWN Command cannot be executed\r\n");
            break;
        }
    }
}

void protocol_process_line(const char *line, hashtable_t *ht, command_response_t *res) {
    command_request_t req;
    if (protocol_parse(line, &req, res) == 0) {
        protocol_execute(&req, ht, res);
    }
}

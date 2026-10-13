#ifndef MINISERVE_PROTOCOL_H
#define MINISERVE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include "hashtable.h"

#define PROTO_MAX_LINE_LEN   1024
#define PROTO_MAX_KEY_LEN    64
#define PROTO_MAX_VAL_LEN    512
#define PROTO_MAX_RESP_LEN   1024

/**
 * Protocol command verb types.
 */
typedef enum {
    CMD_UNKNOWN = 0,
    CMD_SET,
    CMD_SETEX,
    CMD_GET,
    CMD_DEL,
    CMD_EXISTS,
    CMD_COUNT,
    CMD_TTL,
    CMD_QUIT
} command_type_t;

/**
 * Parsed request representation.
 */
typedef struct {
    command_type_t type;
    char key[PROTO_MAX_KEY_LEN + 1];
    char value[PROTO_MAX_VAL_LEN + 1];
    int ttl_sec;
} command_request_t;

/**
 * Formatted protocol response representation.
 */
typedef struct {
    bool close_connection;
    char response[PROTO_MAX_RESP_LEN];
} command_response_t;

/**
 * Parse an incoming ASCII request line.
 *
 * @param line Null-terminated ASCII input line (supports \r\n, \n, or raw string).
 * @param req Output parsed command struct.
 * @param res Output response struct if an immediate error occurs.
 * @return 0 on successful parse, -1 on parsing error (res will be populated with ERR response).
 */
int protocol_parse(const char *line, command_request_t *req, command_response_t *res);

/**
 * Execute a parsed command request against the hash table and format the response.
 *
 * @param req Parsed command request.
 * @param ht Hash table storage engine.
 * @param res Output response struct populated with formatted response string.
 */
void protocol_execute(const command_request_t *req, hashtable_t *ht, command_response_t *res);

/**
 * Full line-processing entry point: parses the raw line and executes it against the hash table.
 *
 * @param line Null-terminated ASCII line.
 * @param ht Hash table storage engine.
 * @param res Output response struct.
 */
void protocol_process_line(const char *line, hashtable_t *ht, command_response_t *res);

#endif /* MINISERVE_PROTOCOL_H */

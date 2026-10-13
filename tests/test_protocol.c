#ifdef NDEBUG
#error "Unit tests must NOT be compiled with NDEBUG; assertions must remain active!"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashtable.h"
#include "protocol.h"

static void test_protocol_valid(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);
    command_response_t res;

    /* SET key value with \r\n */
    protocol_process_line("SET user:1 Alice\r\n", ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);
    assert(!res.close_connection);

    /* SET key with space in value and \n */
    protocol_process_line("SET greeting Hello World!\n", ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);

    /* GET user:1 */
    protocol_process_line("GET user:1\r\n", ht, &res);
    assert(strcmp(res.response, "VALUE Alice\r\n") == 0);

    /* GET greeting */
    protocol_process_line("GET greeting\n", ht, &res);
    assert(strcmp(res.response, "VALUE Hello World!\r\n") == 0);

    /* GET non-existent key */
    protocol_process_line("GET missing\r\n", ht, &res);
    assert(strcmp(res.response, "NOT_FOUND\r\n") == 0);

    /* EXISTS */
    protocol_process_line("EXISTS user:1\r\n", ht, &res);
    assert(strcmp(res.response, "1\r\n") == 0);

    protocol_process_line("EXISTS missing\r\n", ht, &res);
    assert(strcmp(res.response, "0\r\n") == 0);

    /* COUNT */
    protocol_process_line("COUNT\r\n", ht, &res);
    assert(strcmp(res.response, "COUNT 2\r\n") == 0);

    /* DEL user:1 */
    protocol_process_line("DEL user:1\r\n", ht, &res);
    assert(strcmp(res.response, "DELETED\r\n") == 0);

    /* Second DEL user:1 -> NOT_FOUND */
    protocol_process_line("DEL user:1\r\n", ht, &res);
    assert(strcmp(res.response, "NOT_FOUND\r\n") == 0);

    /* COUNT after DEL */
    protocol_process_line("COUNT\n", ht, &res);
    assert(strcmp(res.response, "COUNT 1\r\n") == 0);

    /* QUIT */
    protocol_process_line("QUIT\r\n", ht, &res);
    assert(strcmp(res.response, "BYE\r\n") == 0);
    assert(res.close_connection);

    hashtable_destroy(ht);
    printf("  [PASS] test_protocol_valid\n");
}

static void test_protocol_malformed(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);
    command_response_t res;

    /* Missing arguments for SET */
    protocol_process_line("SET\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("SET key\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    /* Missing key for GET/DEL/EXISTS */
    protocol_process_line("GET\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("DEL\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("EXISTS\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    /* Unexpected trailing arguments */
    protocol_process_line("GET key extra\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("COUNT extra\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("QUIT extra\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    /* Empty line */
    protocol_process_line("\r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    protocol_process_line("   \r\n", ht, &res);
    assert(strstr(res.response, "ERR MALFORMED") != NULL);

    /* Unknown command verb */
    protocol_process_line("PING\r\n", ht, &res);
    assert(strstr(res.response, "ERR UNKNOWN") != NULL);

    protocol_process_line("INVALID key val\r\n", ht, &res);
    assert(strstr(res.response, "ERR UNKNOWN") != NULL);

    hashtable_destroy(ht);
    printf("  [PASS] test_protocol_malformed\n");
}

static void test_protocol_boundary(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);
    command_response_t res;

    /* Exactly 64-byte key */
    char line_64_key[256];
    char key_64[65];
    memset(key_64, 'k', 64);
    key_64[64] = '\0';
    snprintf(line_64_key, sizeof(line_64_key), "SET %s val\r\n", key_64);
    protocol_process_line(line_64_key, ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);

    /* Oversized key (65 bytes) */
    char line_65_key[256];
    char key_65[66];
    memset(key_65, 'k', 65);
    key_65[65] = '\0';
    snprintf(line_65_key, sizeof(line_65_key), "SET %s val\r\n", key_65);
    protocol_process_line(line_65_key, ht, &res);
    assert(strstr(res.response, "ERR OVERSIZED") != NULL);

    /* Exactly 512-byte value */
    char val_512[513];
    memset(val_512, 'v', 512);
    val_512[512] = '\0';

    char line_512_val[600];
    snprintf(line_512_val, sizeof(line_512_val), "SET test_key %s\r\n", val_512);
    protocol_process_line(line_512_val, ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);

    /* Oversized value (513 bytes) */
    char val_513[514];
    memset(val_513, 'v', 513);
    val_513[513] = '\0';

    char line_513_val[600];
    snprintf(line_513_val, sizeof(line_513_val), "SET test_key %s\r\n", val_513);
    protocol_process_line(line_513_val, ht, &res);
    assert(strstr(res.response, "ERR OVERSIZED") != NULL);

    /* Line exceeding 1024 bytes */
    char huge_line[1100];
    memset(huge_line, 'a', 1050);
    huge_line[1050] = '\0';
    protocol_process_line(huge_line, ht, &res);
    assert(strstr(res.response, "ERR OVERSIZED") != NULL);

    hashtable_destroy(ht);
    printf("  [PASS] test_protocol_boundary\n");
}

static void test_protocol_control_characters(void) {
    hashtable_t *ht = hashtable_create(16);
    assert(ht != NULL);
    command_response_t res;

    /* 1. Embedded newline in command content */
    protocol_process_line("SET key\npart val\r\n", ht, &res);
    assert(strcmp(res.response, "ERR MALFORMED Control characters not allowed\r\n") == 0);

    /* 2. Embedded carriage return in command content */
    protocol_process_line("SET key\rpart val\r\n", ht, &res);
    assert(strcmp(res.response, "ERR MALFORMED Control characters not allowed\r\n") == 0);

    /* 3. Byte 0x01 (SOH) in command content */
    protocol_process_line("SET key\x01test val\r\n", ht, &res);
    assert(strcmp(res.response, "ERR MALFORMED Control characters not allowed\r\n") == 0);

    /* 4. Byte 0x7F (DEL) in command content */
    protocol_process_line("SET key\x7Ftest val\r\n", ht, &res);
    assert(strcmp(res.response, "ERR MALFORMED Control characters not allowed\r\n") == 0);

    /* 5. Confirm a valid trailing \r\n is still accepted */
    protocol_process_line("SET valid_key valid_val\r\n", ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);

    /* 6. Confirm a valid trailing \n is still accepted */
    protocol_process_line("GET valid_key\n", ht, &res);
    assert(strcmp(res.response, "VALUE valid_val\r\n") == 0);

    /* 7. Confirm tab (0x09) within content is permitted */
    protocol_process_line("SET tab_key val\twith\ttabs\r\n", ht, &res);
    assert(strcmp(res.response, "OK\r\n") == 0);

    hashtable_destroy(ht);
    printf("  [PASS] test_protocol_control_characters\n");
}

int main(void) {
    printf("Running Protocol Parser Unit Tests:\n");
    test_protocol_valid();
    test_protocol_malformed();
    test_protocol_boundary();
    test_protocol_control_characters();
    printf("All Protocol Parser tests passed successfully!\n");
    return EXIT_SUCCESS;
}

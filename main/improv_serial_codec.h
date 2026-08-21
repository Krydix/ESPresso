#pragma once

#include <stddef.h>
#include <stdint.h>

#define IMPROV_SERIAL_VERSION 1
#define IMPROV_SERIAL_MAX_DATA 255
#define IMPROV_SERIAL_PACKET_OVERHEAD 10
#define IMPROV_SERIAL_MAX_PACKET_SIZE \
    (IMPROV_SERIAL_MAX_DATA + IMPROV_SERIAL_PACKET_OVERHEAD)

typedef enum {
    IMPROV_SERIAL_PARSE_NONE = 0,
    IMPROV_SERIAL_PARSE_PACKET,
    IMPROV_SERIAL_PARSE_INVALID,
} improv_serial_parse_result_t;

typedef struct {
    uint8_t bytes[IMPROV_SERIAL_MAX_PACKET_SIZE];
    size_t length;
    size_t expected_length;
} improv_serial_parser_t;

typedef struct {
    uint8_t type;
    uint8_t length;
    uint8_t data[IMPROV_SERIAL_MAX_DATA];
} improv_serial_packet_t;

void improv_serial_parser_reset(improv_serial_parser_t *parser);
improv_serial_parse_result_t improv_serial_parser_feed(
    improv_serial_parser_t *parser, uint8_t byte, improv_serial_packet_t *packet);

size_t improv_serial_encode_packet(uint8_t type, const uint8_t *data, size_t data_length,
                                   uint8_t *output, size_t capacity);

#include "improv_serial_codec.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_encode_and_parse(void)
{
    const uint8_t rpc[] = {0x02, 0x00};
    uint8_t encoded[IMPROV_SERIAL_MAX_PACKET_SIZE];
    size_t length = improv_serial_encode_packet(0x03, rpc, sizeof(rpc),
                                                encoded, sizeof(encoded));
    assert(length == 12);
    assert(memcmp(encoded, "IMPROV", 6) == 0);
    assert(encoded[6] == 1);
    assert(encoded[7] == 3);
    assert(encoded[8] == 2);

    uint8_t checksum = 0;
    for (size_t i = 0; i + 1 < length; ++i) {
        checksum = (uint8_t)(checksum + encoded[i]);
    }
    assert(encoded[length - 1] == checksum);

    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet = {0};
    const uint8_t noise[] = {'l', 'o', 'g', '\n'};
    for (size_t i = 0; i < sizeof(noise); ++i) {
        assert(improv_serial_parser_feed(&parser, noise[i], &packet) ==
               IMPROV_SERIAL_PARSE_NONE);
    }
    for (size_t i = 0; i < length; ++i) {
        improv_serial_parse_result_t expected =
            i + 1 == length ? IMPROV_SERIAL_PARSE_PACKET : IMPROV_SERIAL_PARSE_NONE;
        assert(improv_serial_parser_feed(&parser, encoded[i], &packet) == expected);
    }
    assert(packet.type == 0x03);
    assert(packet.length == sizeof(rpc));
    assert(memcmp(packet.data, rpc, sizeof(rpc)) == 0);
}

static void test_invalid_checksum_recovers(void)
{
    const uint8_t state[] = {0x02};
    uint8_t encoded[IMPROV_SERIAL_MAX_PACKET_SIZE];
    size_t length = improv_serial_encode_packet(0x01, state, sizeof(state),
                                                encoded, sizeof(encoded));
    encoded[length - 1]++;

    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet = {0};
    for (size_t i = 0; i < length; ++i) {
        improv_serial_parse_result_t expected =
            i + 1 == length ? IMPROV_SERIAL_PARSE_INVALID : IMPROV_SERIAL_PARSE_NONE;
        assert(improv_serial_parser_feed(&parser, encoded[i], &packet) == expected);
    }
    assert(parser.length == 0);
}

static void test_partial_header_resynchronizes(void)
{
    const uint8_t state[] = {0x04};
    uint8_t encoded[IMPROV_SERIAL_MAX_PACKET_SIZE];
    size_t length = improv_serial_encode_packet(0x01, state, sizeof(state),
                                                encoded, sizeof(encoded));
    const uint8_t prefix[] = {'I', 'M', 'X', 'I'};

    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet = {0};
    for (size_t i = 0; i < sizeof(prefix); ++i) {
        assert(improv_serial_parser_feed(&parser, prefix[i], &packet) ==
               IMPROV_SERIAL_PARSE_NONE);
    }
    for (size_t i = 1; i < length; ++i) {
        improv_serial_parse_result_t expected =
            i + 1 == length ? IMPROV_SERIAL_PARSE_PACKET : IMPROV_SERIAL_PARSE_NONE;
        assert(improv_serial_parser_feed(&parser, encoded[i], &packet) == expected);
    }
    assert(packet.data[0] == 0x04);
}

static void test_encode_rejects_invalid_arguments(void)
{
    uint8_t output[IMPROV_SERIAL_MAX_PACKET_SIZE];
    uint8_t data[IMPROV_SERIAL_MAX_DATA] = {0};
    assert(improv_serial_encode_packet(1, data, sizeof(data), output,
                                       sizeof(output)) == sizeof(output));
    assert(improv_serial_encode_packet(1, NULL, 1, output, sizeof(output)) == 0);
    assert(improv_serial_encode_packet(1, data, sizeof(data), output,
                                       sizeof(output) - 1) == 0);
}

int main(void)
{
    test_encode_and_parse();
    test_invalid_checksum_recovers();
    test_partial_header_resynchronizes();
    test_encode_rejects_invalid_arguments();
    puts("Improv Serial codec tests passed");
    return 0;
}

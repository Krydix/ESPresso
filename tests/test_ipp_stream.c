#include "ipp_stream.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *input;
    size_t input_length;
    size_t input_offset;
    size_t max_read;
    uint8_t *output;
    size_t output_capacity;
    size_t output_length;
    size_t max_write;
    size_t fail_read_at;
    size_t fail_write_at;
} stream_fixture_t;

static int fixture_read(void *context, uint8_t *buffer, size_t length)
{
    stream_fixture_t *fixture = context;
    if (fixture->input_offset >= fixture->fail_read_at) {
        return -1;
    }
    size_t available = fixture->input_length - fixture->input_offset;
    if (length > available) {
        length = available;
    }
    if (length > fixture->max_read) {
        length = fixture->max_read;
    }
    memcpy(buffer, fixture->input + fixture->input_offset, length);
    fixture->input_offset += length;
    return (int)length;
}

static int fixture_write(void *context, const uint8_t *buffer, size_t length)
{
    stream_fixture_t *fixture = context;
    if (fixture->output_length >= fixture->fail_write_at) {
        return -1;
    }
    if (length > fixture->max_write) {
        length = fixture->max_write;
    }
    assert(fixture->output_length + length <= fixture->output_capacity);
    memcpy(fixture->output + fixture->output_length, buffer, length);
    fixture->output_length += length;
    return (int)length;
}

static stream_fixture_t fixture_for(const uint8_t *input, size_t length,
                                    uint8_t *output)
{
    stream_fixture_t fixture = {
        .input = input,
        .input_length = length,
        .max_read = length ? length : 1,
        .output = output,
        .output_capacity = length,
        .max_write = length ? length : 1,
        .fail_read_at = SIZE_MAX,
        .fail_write_at = SIZE_MAX,
    };
    return fixture;
}

static void test_short_io_preserves_large_document(void)
{
    const size_t length = 1024 * 1024 + 37;
    uint8_t *input = malloc(length);
    uint8_t *output = malloc(length);
    assert(input && output);
    for (size_t i = 0; i < length; ++i) {
        input[i] = (uint8_t)(i * 31u + i / 251u);
    }
    stream_fixture_t fixture = fixture_for(input, length, output);
    fixture.max_read = 73;
    fixture.max_write = 11;
    assert(ipp_stream_copy(fixture_read, &fixture, fixture_write, &fixture,
                           length, 4096) == IPP_STREAM_OK);
    assert(fixture.input_offset == length);
    assert(fixture.output_length == length);
    assert(memcmp(input, output, length) == 0);
    free(output);
    free(input);
}

static void test_reports_ambiguous_transport_failures(void)
{
    uint8_t input[128];
    uint8_t output[128];
    memset(input, 0xa5, sizeof(input));
    stream_fixture_t fixture = fixture_for(input, sizeof(input), output);
    fixture.max_read = 17;
    fixture.fail_read_at = 34;
    assert(ipp_stream_copy(fixture_read, &fixture, fixture_write, &fixture,
                           sizeof(input), 32) == IPP_STREAM_READ_ERROR);

    fixture = fixture_for(input, sizeof(input), output);
    fixture.max_write = 9;
    fixture.fail_write_at = 18;
    assert(ipp_stream_copy(fixture_read, &fixture, fixture_write, &fixture,
                           sizeof(input), 32) == IPP_STREAM_WRITE_ERROR);
}

static void test_empty_and_invalid_streams(void)
{
    uint8_t byte = 0;
    stream_fixture_t fixture = fixture_for(&byte, 0, &byte);
    assert(ipp_stream_copy(fixture_read, &fixture, fixture_write, &fixture,
                           0, 4096) == IPP_STREAM_OK);
    assert(ipp_stream_copy(NULL, &fixture, fixture_write, &fixture,
                           1, 4096) == IPP_STREAM_READ_ERROR);
    assert(ipp_stream_copy(fixture_read, &fixture, fixture_write, &fixture,
                           1, 0) == IPP_STREAM_READ_ERROR);
    assert(ipp_stream_write_all(NULL, &fixture, &byte, 1) ==
           IPP_STREAM_WRITE_ERROR);
}

static void test_writes_http_chunks_with_short_writes(void)
{
    static const uint8_t input[] = "abcdefgh";
    uint8_t output[64] = {0};
    stream_fixture_t fixture = fixture_for(input, sizeof(input) - 1, output);
    fixture.output_capacity = sizeof(output);
    fixture.max_write = 2;
    assert(ipp_stream_write_http_chunk(fixture_write, &fixture, input,
                                       sizeof(input) - 1) == IPP_STREAM_OK);
    assert(ipp_stream_write_http_chunk(fixture_write, &fixture, NULL, 0) ==
           IPP_STREAM_OK);
    static const uint8_t expected[] = "8\r\nabcdefgh\r\n0\r\n\r\n";
    assert(fixture.output_length == sizeof(expected) - 1);
    assert(memcmp(output, expected, sizeof(expected) - 1) == 0);
}

int main(void)
{
    test_short_io_preserves_large_document();
    test_reports_ambiguous_transport_failures();
    test_empty_and_invalid_streams();
    test_writes_http_chunks_with_short_writes();
    puts("IPP streaming tests passed");
    return 0;
}

#include "ipp_http_request.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
    size_t maximum_read;
} input_t;

static int input_read(void *context, uint8_t *buffer, size_t length)
{
    input_t *input = context;
    if (input->offset == input->length) {
        return 0;
    }
    if (length > input->maximum_read) {
        length = input->maximum_read;
    }
    if (length > input->length - input->offset) {
        length = input->length - input->offset;
    }
    memcpy(buffer, input->data + input->offset, length);
    input->offset += length;
    return (int)length;
}

static ipp_http_request_t *open_request(input_t *input)
{
    ipp_http_request_t *request = NULL;
    assert(ipp_http_request_open(input_read, input, &request) == IPP_HTTP_OK);
    assert(request);
    return request;
}

static size_t read_body(ipp_http_request_t *request, uint8_t *output,
                        size_t capacity, size_t piece_size)
{
    size_t length = 0;
    while (true) {
        size_t received = 0;
        uint8_t overflow = 0;
        uint8_t *destination = length < capacity ? output + length : &overflow;
        size_t wanted = length < capacity ? piece_size : 1;
        if (length < capacity && wanted > capacity - length) {
            wanted = capacity - length;
        }
        ipp_http_result_t result = ipp_http_request_read(
            request, destination, wanted, &received);
        if (result == IPP_HTTP_DONE) {
            assert(received == 0);
            return length;
        }
        assert(result == IPP_HTTP_OK);
        assert(received > 0 && received <= wanted);
        assert(length + received <= capacity);
        length += received;
    }
}

static void test_content_length_request(void)
{
    static const uint8_t wire[] =
        "POST /ipp/print HTTP/1.1\r\n"
        "Host: espresso.local\r\n"
        "Content-Type: application/ipp; charset=binary\r\n"
        "Content-Length: 8\r\n"
        "Expect: 100-continue\r\n\r\n"
        "12345678";
    input_t input = {wire, sizeof(wire) - 1, 0, 3};
    ipp_http_request_t *request = open_request(&input);
    assert(strcmp(ipp_http_request_method(request), "POST") == 0);
    assert(strcmp(ipp_http_request_path(request), "/ipp/print") == 0);
    assert(strcmp(ipp_http_request_content_type(request),
                  "application/ipp; charset=binary") == 0);
    assert(ipp_http_request_expects_continue(request));
    assert(!ipp_http_request_is_chunked(request));
    assert(ipp_http_request_content_length(request) == 8);

    uint8_t body[8];
    assert(read_body(request, body, sizeof(body), 2) == sizeof(body));
    assert(memcmp(body, "12345678", sizeof(body)) == 0);
    assert(ipp_http_request_decoded_length(request) == sizeof(body));
    ipp_http_request_destroy(request);
}

static void test_chunked_request_with_extensions_and_trailers(void)
{
    static const uint8_t wire[] =
        "POST /ipp/print HTTP/1.1\r\n"
        "transfer-encoding: Chunked\r\n"
        "content-type: application/ipp\r\n\r\n"
        "3;source=airprint\r\nabc\r\n"
        "1\r\nd\r\n"
        "4\r\nefgh\r\n"
        "0\r\nX-Trace: ignored\r\n\r\n";
    input_t input = {wire, sizeof(wire) - 1, 0, 1};
    ipp_http_request_t *request = open_request(&input);
    assert(ipp_http_request_is_chunked(request));
    assert(ipp_http_request_content_length(request) == 0);

    uint8_t body[8];
    assert(read_body(request, body, sizeof(body), 3) == sizeof(body));
    assert(memcmp(body, "abcdefgh", sizeof(body)) == 0);
    assert(ipp_http_request_decoded_length(request) == sizeof(body));
    ipp_http_request_destroy(request);
}

static void test_large_chunked_body_stays_streaming(void)
{
    const size_t body_length = 1024 * 1024;
    const size_t chunk_size = 1024;
    const size_t chunks = body_length / chunk_size;
    const size_t header_length = strlen(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");
    const size_t per_chunk = 5 + chunk_size + 2;
    size_t wire_length = header_length + chunks * per_chunk + 5;
    uint8_t *wire = malloc(wire_length);
    assert(wire);
    int header_written = snprintf(
        (char *)wire, wire_length,
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");
    assert(header_written > 0 && (size_t)header_written == header_length);
    size_t offset = (size_t)header_written;
    for (size_t chunk = 0; chunk < chunks; ++chunk) {
        memcpy(wire + offset, "400\r\n", 5);
        offset += 5;
        for (size_t index = 0; index < chunk_size; ++index) {
            wire[offset++] = (uint8_t)((chunk * chunk_size + index) & 0xff);
        }
        memcpy(wire + offset, "\r\n", 2);
        offset += 2;
    }
    memcpy(wire + offset, "0\r\n\r\n", 5);
    offset += 5;
    assert(offset == wire_length);

    input_t input = {wire, wire_length, 0, 17};
    ipp_http_request_t *request = open_request(&input);
    uint8_t buffer[257];
    size_t decoded = 0;
    while (true) {
        size_t received = 0;
        ipp_http_result_t result = ipp_http_request_read(
            request, buffer, sizeof(buffer), &received);
        if (result == IPP_HTTP_DONE) {
            break;
        }
        assert(result == IPP_HTTP_OK && received > 0);
        for (size_t index = 0; index < received; ++index) {
            assert(buffer[index] == (uint8_t)((decoded + index) & 0xff));
        }
        decoded += received;
    }
    assert(decoded == body_length);
    assert(ipp_http_request_decoded_length(request) == body_length);
    assert(ipp_http_request_buffer_capacity(request) <= 4096);
    ipp_http_request_destroy(request);
    free(wire);
}

static void expect_open_failure(const char *wire)
{
    input_t input = {(const uint8_t *)wire, strlen(wire), 0, 7};
    ipp_http_request_t *request = NULL;
    ipp_http_result_t result = ipp_http_request_open(
        input_read, &input, &request);
    assert(result == IPP_HTTP_MALFORMED || result == IPP_HTTP_TOO_LARGE);
    assert(!request);
}

static void expect_body_failure(const char *wire)
{
    input_t input = {(const uint8_t *)wire, strlen(wire), 0, 5};
    ipp_http_request_t *request = open_request(&input);
    uint8_t buffer[32];
    size_t received = 0;
    ipp_http_result_t result;
    do {
        result = ipp_http_request_read(request, buffer, sizeof(buffer),
                                       &received);
    } while (result == IPP_HTTP_OK);
    assert(result == IPP_HTTP_MALFORMED || result == IPP_HTTP_IO_ERROR);
    ipp_http_request_destroy(request);
}

static void test_rejects_ambiguous_and_malformed_framing(void)
{
    expect_open_failure(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Content-Length: 8\r\nTransfer-Encoding: chunked\r\n\r\n");
    expect_open_failure(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Transfer-Encoding: gzip\r\n\r\n");
    expect_open_failure(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Content-Length: -1\r\n\r\n");
    expect_open_failure(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Content-Length: 8\r\nContent-Length: 8\r\n\r\n12345678");
    expect_body_failure(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n\r\nXYZ\r\nabc\r\n0\r\n\r\n");
    expect_body_failure(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n\r\n3\r\nabcX\n0\r\n\r\n");
    expect_body_failure(
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Transfer-Encoding: chunked\r\n\r\n8\r\nshort");

    static const uint8_t nul_header[] =
        "POST /ipp/print HTTP/1.1\r\nContent-Type: application/ipp\r\n"
        "Content-Length: 8\0X-Hidden: value\r\n\r\n12345678";
    input_t input = {nul_header, sizeof(nul_header) - 1, 0, 11};
    ipp_http_request_t *request = NULL;
    assert(ipp_http_request_open(input_read, &input, &request) ==
           IPP_HTTP_MALFORMED);
    assert(!request);
}

int main(void)
{
    test_content_length_request();
    test_chunked_request_with_extensions_and_trailers();
    test_large_chunked_body_stays_streaming();
    test_rejects_ambiguous_and_malformed_framing();
    puts("IPP HTTP request framing tests passed");
    return 0;
}

#include "ipp_http_request.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#define HTTP_BUFFER_SIZE 4096
#define HTTP_METHOD_SIZE 16
#define HTTP_PATH_SIZE 256
#define HTTP_CONTENT_TYPE_SIZE 128
#define HTTP_LINE_SIZE 256
#define HTTP_TRAILERS_MAX 2048

struct ipp_http_request {
    ipp_http_read_fn reader;
    void *reader_context;
    uint8_t buffer[HTTP_BUFFER_SIZE];
    size_t buffer_offset;
    size_t buffer_length;
    char method[HTTP_METHOD_SIZE];
    char path[HTTP_PATH_SIZE];
    char content_type[HTTP_CONTENT_TYPE_SIZE];
    size_t content_length;
    size_t fixed_remaining;
    size_t chunk_remaining;
    size_t decoded_length;
    bool chunked;
    bool expects_continue;
    bool chunk_crlf_pending;
    bool done;
};

static char *trim(char *value)
{
    while (*value && isspace((unsigned char)*value)) {
        ++value;
    }
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return value;
}

static bool copy_string(char *output, size_t output_size, const char *value)
{
    size_t length = strlen(value);
    if (length >= output_size) {
        return false;
    }
    memcpy(output, value, length + 1);
    return true;
}

static bool parse_decimal_size(const char *value, size_t *result)
{
    if (!value || !*value) {
        return false;
    }
    size_t parsed = 0;
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; ++cursor) {
        if (!isdigit(*cursor)) {
            return false;
        }
        unsigned digit = (unsigned)(*cursor - '0');
        if (parsed > (SIZE_MAX - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *result = parsed;
    return true;
}

static ipp_http_result_t raw_read(ipp_http_request_t *request, uint8_t *buffer,
                                  size_t capacity, size_t *received)
{
    *received = 0;
    if (!capacity) {
        return IPP_HTTP_OK;
    }
    if (request->buffer_offset < request->buffer_length) {
        size_t available = request->buffer_length - request->buffer_offset;
        if (available > capacity) {
            available = capacity;
        }
        memcpy(buffer, request->buffer + request->buffer_offset, available);
        request->buffer_offset += available;
        *received = available;
        return IPP_HTTP_OK;
    }
    int result = request->reader(request->reader_context, buffer, capacity);
    if (result <= 0 || (size_t)result > capacity) {
        return IPP_HTTP_IO_ERROR;
    }
    *received = (size_t)result;
    return IPP_HTTP_OK;
}

static ipp_http_result_t raw_byte(ipp_http_request_t *request, uint8_t *value)
{
    size_t received = 0;
    return raw_read(request, value, 1, &received) == IPP_HTTP_OK && received == 1
               ? IPP_HTTP_OK
               : IPP_HTTP_IO_ERROR;
}

static ipp_http_result_t raw_exact(ipp_http_request_t *request, uint8_t *buffer,
                                   size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        size_t received = 0;
        ipp_http_result_t result = raw_read(
            request, buffer + offset, length - offset, &received);
        if (result != IPP_HTTP_OK || !received) {
            return result == IPP_HTTP_OK ? IPP_HTTP_IO_ERROR : result;
        }
        offset += received;
    }
    return IPP_HTTP_OK;
}

static ipp_http_result_t read_line(ipp_http_request_t *request, char *line,
                                   size_t line_size, size_t *line_length)
{
    size_t length = 0;
    while (true) {
        uint8_t value = 0;
        ipp_http_result_t result = raw_byte(request, &value);
        if (result != IPP_HTTP_OK) {
            return result;
        }
        if (value == '\r') {
            result = raw_byte(request, &value);
            if (result != IPP_HTTP_OK) {
                return result;
            }
            if (value != '\n') {
                return IPP_HTTP_MALFORMED;
            }
            line[length] = '\0';
            *line_length = length;
            return IPP_HTTP_OK;
        }
        if (value == '\n' || length + 1 >= line_size) {
            return length + 1 >= line_size ? IPP_HTTP_TOO_LARGE
                                           : IPP_HTTP_MALFORMED;
        }
        line[length++] = (char)value;
    }
}

static ipp_http_result_t parse_chunk_size(const char *line, size_t *size)
{
    const unsigned char *cursor = (const unsigned char *)line;
    if (!isxdigit(*cursor)) {
        return IPP_HTTP_MALFORMED;
    }
    size_t parsed = 0;
    while (isxdigit(*cursor)) {
        unsigned digit;
        if (*cursor >= '0' && *cursor <= '9') {
            digit = (unsigned)(*cursor - '0');
        } else {
            digit = (unsigned)(tolower(*cursor) - 'a' + 10);
        }
        if (parsed > (SIZE_MAX - digit) / 16) {
            return IPP_HTTP_TOO_LARGE;
        }
        parsed = parsed * 16 + digit;
        ++cursor;
    }
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }
    if (*cursor && *cursor != ';') {
        return IPP_HTTP_MALFORMED;
    }
    *size = parsed;
    return IPP_HTTP_OK;
}

static ipp_http_result_t finish_chunked_body(ipp_http_request_t *request)
{
    size_t trailer_bytes = 0;
    while (true) {
        char line[HTTP_LINE_SIZE];
        size_t line_length = 0;
        ipp_http_result_t result = read_line(
            request, line, sizeof(line), &line_length);
        if (result != IPP_HTTP_OK) {
            return result;
        }
        if (line_length == 0) {
            request->done = true;
            return IPP_HTTP_DONE;
        }
        if (line_length > HTTP_TRAILERS_MAX - trailer_bytes) {
            return IPP_HTTP_TOO_LARGE;
        }
        trailer_bytes += line_length;
        if (!strchr(line, ':')) {
            return IPP_HTTP_MALFORMED;
        }
    }
}

static ipp_http_result_t parse_headers(ipp_http_request_t *request,
                                       size_t header_end)
{
    char *headers = (char *)request->buffer;
    headers[header_end - 2] = '\0';
    char *line_end = strstr(headers, "\r\n");
    if (!line_end) {
        return IPP_HTTP_MALFORMED;
    }
    *line_end = '\0';
    char *first_space = strchr(headers, ' ');
    char *second_space = first_space ? strchr(first_space + 1, ' ') : NULL;
    if (!first_space || !second_space || strchr(second_space + 1, ' ') ||
        (strcmp(second_space + 1, "HTTP/1.1") != 0 &&
         strcmp(second_space + 1, "HTTP/1.0") != 0)) {
        return IPP_HTTP_MALFORMED;
    }
    *first_space = '\0';
    *second_space = '\0';
    if (!copy_string(request->method, sizeof(request->method), headers) ||
        !copy_string(request->path, sizeof(request->path), first_space + 1)) {
        return IPP_HTTP_TOO_LARGE;
    }

    bool has_content_length = false;
    bool has_transfer_encoding = false;
    bool has_content_type = false;
    char *line = line_end + 2;
    while (*line) {
        line_end = strstr(line, "\r\n");
        if (line_end) {
            *line_end = '\0';
        }
        char *colon = strchr(line, ':');
        if (!colon) {
            return IPP_HTTP_MALFORMED;
        }
        *colon = '\0';
        char *name = trim(line);
        char *value = trim(colon + 1);
        if (!*name) {
            return IPP_HTTP_MALFORMED;
        }
        if (strcasecmp(name, "Content-Length") == 0) {
            if (has_content_length ||
                !parse_decimal_size(value, &request->content_length)) {
                return IPP_HTTP_MALFORMED;
            }
            has_content_length = true;
        } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
            if (has_transfer_encoding || strcasecmp(value, "chunked") != 0) {
                return IPP_HTTP_MALFORMED;
            }
            has_transfer_encoding = true;
            request->chunked = true;
        } else if (strcasecmp(name, "Content-Type") == 0) {
            if (has_content_type ||
                !copy_string(request->content_type,
                             sizeof(request->content_type), value)) {
                return IPP_HTTP_MALFORMED;
            }
            has_content_type = true;
        } else if (strcasecmp(name, "Expect") == 0 &&
                   strcasecmp(value, "100-continue") == 0) {
            request->expects_continue = true;
        }
        if (!line_end) {
            break;
        }
        line = line_end + 2;
    }
    if (has_content_length == has_transfer_encoding) {
        return IPP_HTTP_MALFORMED;
    }
    request->fixed_remaining = request->content_length;
    return IPP_HTTP_OK;
}

ipp_http_result_t ipp_http_request_open(ipp_http_read_fn reader,
                                        void *reader_context,
                                        ipp_http_request_t **output)
{
    if (!reader || !output) {
        return IPP_HTTP_MALFORMED;
    }
    *output = NULL;
    ipp_http_request_t *request = calloc(1, sizeof(*request));
    if (!request) {
        return IPP_HTTP_NO_MEMORY;
    }
    request->reader = reader;
    request->reader_context = reader_context;

    size_t search_start = 0;
    size_t header_end = 0;
    while (!header_end) {
        if (request->buffer_length == sizeof(request->buffer)) {
            free(request);
            return IPP_HTTP_TOO_LARGE;
        }
        int received = reader(reader_context,
                              request->buffer + request->buffer_length,
                              sizeof(request->buffer) - request->buffer_length);
        if (received <= 0 ||
            (size_t)received > sizeof(request->buffer) - request->buffer_length) {
            free(request);
            return IPP_HTTP_IO_ERROR;
        }
        size_t previous_length = request->buffer_length;
        request->buffer_length += (size_t)received;
        if (previous_length >= 3) {
            search_start = previous_length - 3;
        }
        for (size_t index = search_start;
             index + 3 < request->buffer_length; ++index) {
            if (memcmp(request->buffer + index, "\r\n\r\n", 4) == 0) {
                header_end = index + 4;
                break;
            }
        }
        search_start = request->buffer_length >= 3
                           ? request->buffer_length - 3
                           : 0;
    }
    request->buffer_offset = header_end;
    if (memchr(request->buffer, '\0', header_end)) {
        free(request);
        return IPP_HTTP_MALFORMED;
    }
    ipp_http_result_t result = parse_headers(request, header_end);
    if (result != IPP_HTTP_OK) {
        free(request);
        return result;
    }
    *output = request;
    return IPP_HTTP_OK;
}

void ipp_http_request_destroy(ipp_http_request_t *request)
{
    free(request);
}

const char *ipp_http_request_method(const ipp_http_request_t *request)
{
    return request ? request->method : "";
}

const char *ipp_http_request_path(const ipp_http_request_t *request)
{
    return request ? request->path : "";
}

const char *ipp_http_request_content_type(const ipp_http_request_t *request)
{
    return request ? request->content_type : "";
}

bool ipp_http_request_expects_continue(const ipp_http_request_t *request)
{
    return request && request->expects_continue;
}

bool ipp_http_request_is_chunked(const ipp_http_request_t *request)
{
    return request && request->chunked;
}

size_t ipp_http_request_content_length(const ipp_http_request_t *request)
{
    return request && !request->chunked ? request->content_length : 0;
}

size_t ipp_http_request_decoded_length(const ipp_http_request_t *request)
{
    return request ? request->decoded_length : 0;
}

size_t ipp_http_request_buffer_capacity(const ipp_http_request_t *request)
{
    return request ? sizeof(request->buffer) : 0;
}

ipp_http_result_t ipp_http_request_read(ipp_http_request_t *request,
                                        uint8_t *buffer, size_t capacity,
                                        size_t *received)
{
    if (!request || !received || (!buffer && capacity)) {
        return IPP_HTTP_MALFORMED;
    }
    *received = 0;
    if (request->done) {
        return IPP_HTTP_DONE;
    }
    if (!capacity) {
        return IPP_HTTP_OK;
    }
    if (!request->chunked) {
        if (!request->fixed_remaining) {
            request->done = true;
            return IPP_HTTP_DONE;
        }
        size_t wanted = request->fixed_remaining < capacity
                            ? request->fixed_remaining
                            : capacity;
        ipp_http_result_t result = raw_read(request, buffer, wanted, received);
        if (result != IPP_HTTP_OK) {
            return result;
        }
        request->fixed_remaining -= *received;
    } else {
        while (!request->chunk_remaining) {
            if (request->chunk_crlf_pending) {
                uint8_t crlf[2];
                ipp_http_result_t result = raw_exact(
                    request, crlf, sizeof(crlf));
                if (result != IPP_HTTP_OK) {
                    return result;
                }
                if (crlf[0] != '\r' || crlf[1] != '\n') {
                    return IPP_HTTP_MALFORMED;
                }
                request->chunk_crlf_pending = false;
            }
            char line[HTTP_LINE_SIZE];
            size_t line_length = 0;
            ipp_http_result_t result = read_line(
                request, line, sizeof(line), &line_length);
            if (result != IPP_HTTP_OK) {
                return result;
            }
            result = parse_chunk_size(line, &request->chunk_remaining);
            if (result != IPP_HTTP_OK) {
                return result;
            }
            if (!request->chunk_remaining) {
                return finish_chunked_body(request);
            }
        }
        size_t wanted = request->chunk_remaining < capacity
                            ? request->chunk_remaining
                            : capacity;
        ipp_http_result_t result = raw_read(request, buffer, wanted, received);
        if (result != IPP_HTTP_OK) {
            return result;
        }
        request->chunk_remaining -= *received;
        if (!request->chunk_remaining) {
            request->chunk_crlf_pending = true;
        }
    }
    if (*received > SIZE_MAX - request->decoded_length) {
        return IPP_HTTP_TOO_LARGE;
    }
    request->decoded_length += *received;
    return IPP_HTTP_OK;
}

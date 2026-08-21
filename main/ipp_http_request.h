#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*ipp_http_read_fn)(void *context, uint8_t *buffer, size_t length);

typedef enum {
    IPP_HTTP_OK = 0,
    IPP_HTTP_DONE,
    IPP_HTTP_MALFORMED,
    IPP_HTTP_TOO_LARGE,
    IPP_HTTP_NO_MEMORY,
    IPP_HTTP_IO_ERROR,
} ipp_http_result_t;

typedef struct ipp_http_request ipp_http_request_t;

/* Parse one HTTP/1.x request head and retain any already-read body bytes. */
ipp_http_result_t ipp_http_request_open(ipp_http_read_fn reader,
                                        void *reader_context,
                                        ipp_http_request_t **request);

void ipp_http_request_destroy(ipp_http_request_t *request);

const char *ipp_http_request_method(const ipp_http_request_t *request);
const char *ipp_http_request_path(const ipp_http_request_t *request);
const char *ipp_http_request_content_type(const ipp_http_request_t *request);
bool ipp_http_request_expects_continue(const ipp_http_request_t *request);
bool ipp_http_request_is_chunked(const ipp_http_request_t *request);

/* Zero for a chunked request, whose decoded size is not known in advance. */
size_t ipp_http_request_content_length(const ipp_http_request_t *request);
size_t ipp_http_request_decoded_length(const ipp_http_request_t *request);

/* The fixed internal receive buffer used independently of document size. */
size_t ipp_http_request_buffer_capacity(const ipp_http_request_t *request);

/* Return decoded body bytes, IPP_HTTP_DONE at the framing terminator, or error. */
ipp_http_result_t ipp_http_request_read(ipp_http_request_t *request,
                                        uint8_t *buffer, size_t capacity,
                                        size_t *received);

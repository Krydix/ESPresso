#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int (*ipp_stream_read_fn)(void *context, uint8_t *buffer, size_t length);
typedef int (*ipp_stream_write_fn)(void *context, const uint8_t *buffer,
                                  size_t length);

typedef enum {
    IPP_STREAM_OK = 0,
    IPP_STREAM_NO_MEMORY,
    IPP_STREAM_READ_ERROR,
    IPP_STREAM_WRITE_ERROR,
} ipp_stream_result_t;

ipp_stream_result_t ipp_stream_write_all(ipp_stream_write_fn writer,
                                         void *writer_context,
                                         const uint8_t *data, size_t length);

/* Write one RFC HTTP chunk; a zero-length chunk terminates the body. */
ipp_stream_result_t ipp_stream_write_http_chunk(ipp_stream_write_fn writer,
                                                void *writer_context,
                                                const uint8_t *data,
                                                size_t length);

/* Copy exactly `length` bytes with bounded memory and short-I/O handling. */
ipp_stream_result_t ipp_stream_copy(ipp_stream_read_fn reader,
                                    void *reader_context,
                                    ipp_stream_write_fn writer,
                                    void *writer_context, size_t length,
                                    size_t chunk_size);

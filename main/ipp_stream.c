#include "ipp_stream.h"

#include <stdlib.h>

ipp_stream_result_t ipp_stream_write_all(ipp_stream_write_fn writer,
                                         void *writer_context,
                                         const uint8_t *data, size_t length)
{
    if (!writer || (!data && length)) {
        return IPP_STREAM_WRITE_ERROR;
    }
    size_t sent = 0;
    while (sent < length) {
        int result = writer(writer_context, data + sent, length - sent);
        if (result <= 0 || (size_t)result > length - sent) {
            return IPP_STREAM_WRITE_ERROR;
        }
        sent += (size_t)result;
    }
    return IPP_STREAM_OK;
}

ipp_stream_result_t ipp_stream_copy(ipp_stream_read_fn reader,
                                    void *reader_context,
                                    ipp_stream_write_fn writer,
                                    void *writer_context, size_t length,
                                    size_t chunk_size)
{
    if (!reader || !writer || !chunk_size) {
        return IPP_STREAM_READ_ERROR;
    }
    if (!length) {
        return IPP_STREAM_OK;
    }
    if (chunk_size > length) {
        chunk_size = length;
    }
    uint8_t *buffer = malloc(chunk_size);
    if (!buffer) {
        return IPP_STREAM_NO_MEMORY;
    }
    ipp_stream_result_t result = IPP_STREAM_OK;
    size_t remaining = length;
    while (remaining) {
        size_t wanted = remaining < chunk_size ? remaining : chunk_size;
        int received = reader(reader_context, buffer, wanted);
        if (received <= 0 || (size_t)received > wanted) {
            result = IPP_STREAM_READ_ERROR;
            break;
        }
        result = ipp_stream_write_all(writer, writer_context, buffer,
                                      (size_t)received);
        if (result != IPP_STREAM_OK) {
            break;
        }
        remaining -= (size_t)received;
    }
    free(buffer);
    return result;
}

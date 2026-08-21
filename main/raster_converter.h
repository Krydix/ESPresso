#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int (*espresso_raster_read_fn)(void *context, uint8_t *data,
                                      size_t length);
typedef int (*espresso_raster_write_fn)(void *context, const uint8_t *data,
                                       size_t length);

typedef enum {
    ESPRESSO_RASTER_OK = 0,
    ESPRESSO_RASTER_INVALID,
    ESPRESSO_RASTER_UNSUPPORTED,
    ESPRESSO_RASTER_READ_ERROR,
    ESPRESSO_RASTER_WRITE_ERROR,
} espresso_raster_result_t;

/* Convert compressed, network-byte-order PWG Raster to Apple Raster while
 * retaining compressed rows. No scan-line or page buffer is allocated. */
espresso_raster_result_t espresso_pwg_to_urf(
    espresso_raster_read_fn reader, void *reader_context,
    espresso_raster_write_fn writer, void *writer_context);

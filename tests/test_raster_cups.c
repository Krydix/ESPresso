#ifndef __APPLE__
#define _POSIX_C_SOURCE 200809L
#endif

#include "raster_converter.h"

#include <cups/raster.h>

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int file_read(void *context, uint8_t *data, size_t length)
{
    ssize_t result = read(*(int *)context, data, length);
    return result < 0 ? -1 : (int)result;
}

static int file_write(void *context, const uint8_t *data, size_t length)
{
    ssize_t result = write(*(int *)context, data, length);
    return result < 0 ? -1 : (int)result;
}

int main(void)
{
    char pwg_path[] = "/tmp/espresso-pwg-XXXXXX";
    char urf_path[] = "/tmp/espresso-urf-XXXXXX";
    int temporary = mkstemp(pwg_path);
    assert(temporary >= 0);
    close(temporary);
    temporary = mkstemp(urf_path);
    assert(temporary >= 0);
    close(temporary);

    int pwg_fd = open(pwg_path, O_WRONLY | O_TRUNC);
    assert(pwg_fd >= 0);
    cups_raster_t *writer = cupsRasterOpen(pwg_fd, CUPS_RASTER_WRITE_PWG);
    assert(writer);
    cups_page_header2_t header;
    memset(&header, 0, sizeof(header));
    strcpy(header.MediaType, "stationery");
    header.Duplex = 1;
    header.HWResolution[0] = 300;
    header.HWResolution[1] = 300;
    header.cupsWidth = 2;
    header.cupsHeight = 2;
    header.cupsBitsPerColor = 8;
    header.cupsBitsPerPixel = 24;
    header.cupsBytesPerLine = 6;
    header.cupsColorOrder = CUPS_ORDER_CHUNKED;
    header.cupsColorSpace = CUPS_CSPACE_SRGB;
    header.cupsNumColors = 3;
    header.cupsInteger[CUPS_RASTER_PWG_TotalPageCount] = 1;
    header.cupsInteger[CUPS_RASTER_PWG_PrintQuality] = 4;
    assert(cupsRasterWriteHeader2(writer, &header));
    unsigned char pixels[] = {
        1, 2, 3, 4, 5, 6,
        7, 8, 9, 10, 11, 12,
    };
    assert(cupsRasterWritePixels(writer, pixels, sizeof(pixels)) ==
           sizeof(pixels));
    cupsRasterClose(writer);

    pwg_fd = open(pwg_path, O_RDONLY);
    int urf_fd = open(urf_path, O_WRONLY | O_TRUNC);
    assert(pwg_fd >= 0 && urf_fd >= 0);
    assert(espresso_pwg_to_urf(file_read, &pwg_fd, file_write, &urf_fd) ==
           ESPRESSO_RASTER_OK);
    close(pwg_fd);
    close(urf_fd);

    urf_fd = open(urf_path, O_RDONLY);
    assert(urf_fd >= 0);
    cups_raster_t *reader = cupsRasterOpen(urf_fd, CUPS_RASTER_READ);
    assert(reader);
    cups_page_header2_t converted;
    assert(cupsRasterReadHeader2(reader, &converted));
    assert(converted.cupsWidth == header.cupsWidth);
    assert(converted.cupsHeight == header.cupsHeight);
    assert(converted.HWResolution[0] == 300);
    assert(converted.HWResolution[1] == 300);
    assert(converted.Duplex);
    unsigned char decoded[sizeof(pixels)] = {0};
    assert(cupsRasterReadPixels(reader, decoded, sizeof(decoded)) ==
           sizeof(decoded));
    assert(memcmp(decoded, pixels, sizeof(pixels)) == 0);
    cupsRasterClose(reader);
    unlink(pwg_path);
    unlink(urf_path);
    puts("CUPS-generated PWG Raster conversion test passed");
    return 0;
}

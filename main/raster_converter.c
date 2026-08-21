#include "raster_converter.h"

#include <stdbool.h>
#include <string.h>

#define PWG_HEADER_SIZE 1796U
#define APPLE_PAGE_HEADER_SIZE 32U
#define MAX_RASTER_WIDTH 20000U
#define MAX_RASTER_HEIGHT 200000U
#define MAX_BYTES_PER_LINE (256U * 1024U)

#define OFFSET_DUPLEX 272U
#define OFFSET_X_RESOLUTION 276U
#define OFFSET_Y_RESOLUTION 280U
#define OFFSET_MEDIA_POSITION 324U
#define OFFSET_TUMBLE 368U
#define OFFSET_WIDTH 372U
#define OFFSET_HEIGHT 376U
#define OFFSET_BITS_PER_COLOR 384U
#define OFFSET_BITS_PER_PIXEL 388U
#define OFFSET_BYTES_PER_LINE 392U
#define OFFSET_COLOR_ORDER 396U
#define OFFSET_COLOR_SPACE 400U
#define OFFSET_NUM_COLORS 420U
#define OFFSET_TOTAL_PAGE_COUNT 452U
#define OFFSET_PRINT_QUALITY 484U

typedef struct {
    espresso_raster_read_fn reader;
    void *context;
} input_t;

typedef struct {
    espresso_raster_write_fn writer;
    void *context;
} output_t;

typedef enum {
    READ_OK = 0,
    READ_EOF,
    READ_FAILED,
} read_result_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_line;
    uint32_t bits_per_pixel;
    uint32_t bytes_per_pixel;
    uint8_t apple_color_space;
} page_info_t;

static uint32_t read_u32(const uint8_t *data, size_t offset)
{
    return ((uint32_t)data[offset] << 24) |
           ((uint32_t)data[offset + 1] << 16) |
           ((uint32_t)data[offset + 2] << 8) |
           (uint32_t)data[offset + 3];
}

static void write_u32(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset] = (uint8_t)(value >> 24);
    data[offset + 1] = (uint8_t)(value >> 16);
    data[offset + 2] = (uint8_t)(value >> 8);
    data[offset + 3] = (uint8_t)value;
}

static read_result_t read_exact(input_t *input, uint8_t *data, size_t length,
                                bool eof_allowed)
{
    size_t received = 0;
    while (received < length) {
        int result = input->reader(input->context, data + received,
                                   length - received);
        if (result < 0 || (size_t)result > length - received) {
            return READ_FAILED;
        }
        if (result == 0) {
            return received == 0 && eof_allowed ? READ_EOF : READ_FAILED;
        }
        received += (size_t)result;
    }
    return READ_OK;
}

static bool write_all(output_t *output, const uint8_t *data, size_t length)
{
    size_t written = 0;
    while (written < length) {
        int result = output->writer(output->context, data + written,
                                    length - written);
        if (result <= 0 || (size_t)result > length - written) {
            return false;
        }
        written += (size_t)result;
    }
    return true;
}

static bool color_space(uint32_t pwg, uint32_t colors, uint8_t *apple)
{
    switch (pwg) {
        case 18: /* sgray */
            *apple = 0;
            return colors == 1;
        case 19: /* sRGB */
            *apple = 1;
            return colors == 3;
        case 16: /* CIE Lab */
            *apple = 2;
            return colors == 3;
        case 20: /* Adobe RGB */
            *apple = 3;
            return colors == 3;
        case 0: /* device gray */
            *apple = 4;
            return colors == 1;
        case 1: /* device RGB */
            *apple = 5;
            return colors == 3;
        case 6: /* device CMYK */
            *apple = 6;
            return colors == 4;
        default:
            return false;
    }
}

static uint8_t media_type(const uint8_t *header)
{
    static const char *const values[] = {
        "auto", "stationery", "transparency", "envelope", "cardstock",
        "labels", "stationery-letterhead", "disc", "photographic-matte",
        "photographic-satin", "photographic-semi-gloss",
        "photographic-glossy", "photographic-high-gloss", "other",
    };
    const char *input = (const char *)header + 128;
    size_t input_length = strnlen(input, 64);
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        size_t length = strlen(values[i]);
        if (input_length == length && memcmp(input, values[i], length) == 0) {
            return (uint8_t)i;
        }
    }
    return 13;
}

static espresso_raster_result_t build_page_header(const uint8_t *pwg,
                                                   uint8_t *apple,
                                                   page_info_t *page)
{
    uint32_t xdpi = read_u32(pwg, OFFSET_X_RESOLUTION);
    uint32_t ydpi = read_u32(pwg, OFFSET_Y_RESOLUTION);
    uint32_t bits_per_color = read_u32(pwg, OFFSET_BITS_PER_COLOR);
    uint32_t color_order = read_u32(pwg, OFFSET_COLOR_ORDER);
    uint32_t color_space_value = read_u32(pwg, OFFSET_COLOR_SPACE);
    uint32_t colors = read_u32(pwg, OFFSET_NUM_COLORS);
    page->width = read_u32(pwg, OFFSET_WIDTH);
    page->height = read_u32(pwg, OFFSET_HEIGHT);
    page->bits_per_pixel = read_u32(pwg, OFFSET_BITS_PER_PIXEL);
    page->bytes_per_line = read_u32(pwg, OFFSET_BYTES_PER_LINE);

    if (memcmp(pwg, "PwgRaster", 9) != 0 || !xdpi || xdpi != ydpi ||
        !page->width || page->width > MAX_RASTER_WIDTH || !page->height ||
        page->height > MAX_RASTER_HEIGHT || color_order != 0 ||
        page->bits_per_pixel == 0 || page->bits_per_pixel > 255 ||
        (bits_per_color != 1 && bits_per_color != 2 && bits_per_color != 4 &&
         bits_per_color != 8 && bits_per_color != 16) ||
        page->bits_per_pixel != bits_per_color * colors ||
        !color_space(color_space_value, colors, &page->apple_color_space)) {
        return ESPRESSO_RASTER_UNSUPPORTED;
    }
    uint64_t expected_line =
        ((uint64_t)page->width * page->bits_per_pixel + 7U) / 8U;
    if (!page->bytes_per_line || page->bytes_per_line > MAX_BYTES_PER_LINE ||
        expected_line != page->bytes_per_line) {
        return ESPRESSO_RASTER_INVALID;
    }
    page->bytes_per_pixel = (page->bits_per_pixel + 7U) / 8U;
    if (!page->bytes_per_pixel || page->bytes_per_pixel > 8) {
        return ESPRESSO_RASTER_UNSUPPORTED;
    }

    memset(apple, 0, APPLE_PAGE_HEADER_SIZE);
    apple[0] = (uint8_t)page->bits_per_pixel;
    apple[1] = page->apple_color_space;
    apple[2] = read_u32(pwg, OFFSET_DUPLEX) ?
                   (read_u32(pwg, OFFSET_TUMBLE) ? 2 : 3) : 1;
    apple[3] = (uint8_t)read_u32(pwg, OFFSET_PRINT_QUALITY);
    apple[4] = media_type(pwg);
    apple[5] = (uint8_t)read_u32(pwg, OFFSET_MEDIA_POSITION);
    write_u32(apple, 12, page->width);
    write_u32(apple, 16, page->height);
    write_u32(apple, 20, xdpi);
    return ESPRESSO_RASTER_OK;
}

static espresso_raster_result_t copy_byte(input_t *input, output_t *output,
                                          uint8_t *value)
{
    if (read_exact(input, value, 1, false) != READ_OK) {
        return ESPRESSO_RASTER_INVALID;
    }
    return write_all(output, value, 1) ? ESPRESSO_RASTER_OK :
                                        ESPRESSO_RASTER_WRITE_ERROR;
}

static espresso_raster_result_t copy_payload(input_t *input, output_t *output,
                                             size_t length)
{
    uint8_t buffer[64];
    while (length) {
        size_t piece = length < sizeof(buffer) ? length : sizeof(buffer);
        if (read_exact(input, buffer, piece, false) != READ_OK) {
            return ESPRESSO_RASTER_INVALID;
        }
        if (!write_all(output, buffer, piece)) {
            return ESPRESSO_RASTER_WRITE_ERROR;
        }
        length -= piece;
    }
    return ESPRESSO_RASTER_OK;
}

static espresso_raster_result_t copy_page(input_t *input, output_t *output,
                                          const page_info_t *page)
{
    uint32_t rows = 0;
    while (rows < page->height) {
        uint8_t repeat;
        espresso_raster_result_t result = copy_byte(input, output, &repeat);
        if (result != ESPRESSO_RASTER_OK) {
            return result;
        }
        uint32_t row_count = (uint32_t)repeat + 1U;
        if (row_count > page->height - rows) {
            return ESPRESSO_RASTER_INVALID;
        }
        size_t decoded = 0;
        while (decoded < page->bytes_per_line) {
            uint8_t control;
            result = copy_byte(input, output, &control);
            if (result != ESPRESSO_RASTER_OK) {
                return result;
            }
            if (control == 128) {
                return ESPRESSO_RASTER_INVALID;
            }
            size_t units = control & 0x80U ? 257U - control :
                                                   (size_t)control + 1U;
            size_t expanded = units * page->bytes_per_pixel;
            if (expanded > page->bytes_per_line - decoded) {
                return ESPRESSO_RASTER_INVALID;
            }
            size_t encoded = control & 0x80U ? expanded :
                                                   page->bytes_per_pixel;
            result = copy_payload(input, output, encoded);
            if (result != ESPRESSO_RASTER_OK) {
                return result;
            }
            decoded += expanded;
        }
        rows += row_count;
    }
    return ESPRESSO_RASTER_OK;
}

espresso_raster_result_t espresso_pwg_to_urf(
    espresso_raster_read_fn reader, void *reader_context,
    espresso_raster_write_fn writer, void *writer_context)
{
    if (!reader || !writer) {
        return ESPRESSO_RASTER_INVALID;
    }
    input_t input = {reader, reader_context};
    output_t output = {writer, writer_context};
    uint8_t sync[4];
    read_result_t read_result = read_exact(&input, sync, sizeof(sync), false);
    if (read_result != READ_OK) {
        return read_result == READ_FAILED ? ESPRESSO_RASTER_INVALID :
                                           ESPRESSO_RASTER_READ_ERROR;
    }
    if (memcmp(sync, "RaS2", sizeof(sync)) != 0) {
        return ESPRESSO_RASTER_UNSUPPORTED;
    }

    uint8_t pwg[PWG_HEADER_SIZE];
    if (read_exact(&input, pwg, sizeof(pwg), false) != READ_OK) {
        return ESPRESSO_RASTER_INVALID;
    }
    uint32_t expected_pages = read_u32(pwg, OFFSET_TOTAL_PAGE_COUNT);
    uint8_t file_header[12] = {'U', 'N', 'I', 'R', 'A', 'S', 'T', 0};
    write_u32(file_header, 8, expected_pages);
    if (!write_all(&output, file_header, sizeof(file_header))) {
        return ESPRESSO_RASTER_WRITE_ERROR;
    }

    uint32_t pages = 0;
    while (true) {
        uint8_t apple[APPLE_PAGE_HEADER_SIZE];
        page_info_t page;
        espresso_raster_result_t result =
            build_page_header(pwg, apple, &page);
        if (result != ESPRESSO_RASTER_OK) {
            return result;
        }
        if (!write_all(&output, apple, sizeof(apple))) {
            return ESPRESSO_RASTER_WRITE_ERROR;
        }
        result = copy_page(&input, &output, &page);
        if (result != ESPRESSO_RASTER_OK) {
            return result;
        }
        ++pages;
        read_result = read_exact(&input, pwg, sizeof(pwg), true);
        if (read_result == READ_EOF) {
            break;
        }
        if (read_result != READ_OK) {
            return ESPRESSO_RASTER_INVALID;
        }
    }
    if (expected_pages != 0 && expected_pages != 0xffffffffU &&
        pages != expected_pages) {
        return ESPRESSO_RASTER_INVALID;
    }
    return ESPRESSO_RASTER_OK;
}

#include "raster_converter.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PWG_HEADER_SIZE 1796

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t offset;
    size_t piece;
} input_t;

typedef struct {
    uint8_t data[4096];
    size_t length;
    size_t piece;
} output_t;

static int read_input(void *context, uint8_t *data, size_t length)
{
    input_t *input = context;
    if (input->offset == input->length) {
        return 0;
    }
    if (length > input->piece) {
        length = input->piece;
    }
    if (length > input->length - input->offset) {
        length = input->length - input->offset;
    }
    memcpy(data, input->data + input->offset, length);
    input->offset += length;
    return (int)length;
}

static int write_output(void *context, const uint8_t *data, size_t length)
{
    output_t *output = context;
    if (length > output->piece) {
        length = output->piece;
    }
    if (length > sizeof(output->data) - output->length) {
        return -1;
    }
    memcpy(output->data + output->length, data, length);
    output->length += length;
    return (int)length;
}

static void put_u32(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset] = (uint8_t)(value >> 24);
    data[offset + 1] = (uint8_t)(value >> 16);
    data[offset + 2] = (uint8_t)(value >> 8);
    data[offset + 3] = (uint8_t)value;
}

static size_t make_pwg(uint8_t *data, uint32_t xdpi, uint32_t ydpi,
                       uint32_t colorspace)
{
    memcpy(data, "RaS2", 4);
    uint8_t *header = data + 4;
    memset(header, 0, PWG_HEADER_SIZE);
    memcpy(header, "PwgRaster", 9);
    memcpy(header + 128, "stationery", 10);
    put_u32(header, 272, 1);
    put_u32(header, 276, xdpi);
    put_u32(header, 280, ydpi);
    put_u32(header, 324, 1);
    put_u32(header, 368, 0);
    put_u32(header, 372, 2);
    put_u32(header, 376, 2);
    put_u32(header, 384, 8);
    put_u32(header, 388, 24);
    put_u32(header, 392, 6);
    put_u32(header, 396, 0);
    put_u32(header, 400, colorspace);
    put_u32(header, 420, 3);
    put_u32(header, 452, 1);
    put_u32(header, 484, 4);
    uint8_t row[] = {1, 255, 1, 2, 3, 4, 5, 6};
    memcpy(header + PWG_HEADER_SIZE, row, sizeof(row));
    return 4 + PWG_HEADER_SIZE + sizeof(row);
}

static void test_converts_fragmented_pwg_to_urf(void)
{
    uint8_t pwg[2048];
    size_t pwg_length = make_pwg(pwg, 300, 300, 19);
    input_t input = {pwg, pwg_length, 0, 3};
    output_t output = {.piece = 5};
    assert(espresso_pwg_to_urf(read_input, &input, write_output, &output) ==
           ESPRESSO_RASTER_OK);
    assert(output.length == 12 + 32 + 8);
    assert(memcmp(output.data, "UNIRAST\0", 8) == 0);
    assert(output.data[11] == 1);
    assert(output.data[12] == 24);
    assert(output.data[13] == 1);
    assert(output.data[14] == 3);
    assert(output.data[15] == 4);
    assert(output.data[17] == 1);
    assert(output.data[27] == 2);
    assert(output.data[31] == 2);
    assert(output.data[35] == 44);
    assert(memcmp(output.data + 44, pwg + 4 + PWG_HEADER_SIZE, 8) == 0);
}

static void test_rejects_unsupported_or_malformed_streams(void)
{
    uint8_t pwg[2048];
    size_t length = make_pwg(pwg, 600, 300, 19);
    input_t input = {pwg, length, 0, 4096};
    output_t output = {.piece = 4096};
    assert(espresso_pwg_to_urf(read_input, &input, write_output, &output) ==
           ESPRESSO_RASTER_UNSUPPORTED);

    length = make_pwg(pwg, 300, 300, 19);
    input = (input_t){pwg, length - 1, 0, 4096};
    output = (output_t){.piece = 4096};
    assert(espresso_pwg_to_urf(read_input, &input, write_output, &output) ==
           ESPRESSO_RASTER_INVALID);

    length = make_pwg(pwg, 300, 300, 99);
    input = (input_t){pwg, length, 0, 4096};
    output = (output_t){.piece = 4096};
    assert(espresso_pwg_to_urf(read_input, &input, write_output, &output) ==
           ESPRESSO_RASTER_UNSUPPORTED);

    length = make_pwg(pwg, 300, 300, 19);
    pwg[4 + PWG_HEADER_SIZE + 1] = 128;
    input = (input_t){pwg, length, 0, 4096};
    output = (output_t){.piece = 4096};
    assert(espresso_pwg_to_urf(read_input, &input, write_output, &output) ==
           ESPRESSO_RASTER_INVALID);
}

int main(void)
{
    test_converts_fragmented_pwg_to_urf();
    test_rejects_unsupported_or_malformed_streams();
    puts("PWG Raster to URF converter tests passed");
    return 0;
}

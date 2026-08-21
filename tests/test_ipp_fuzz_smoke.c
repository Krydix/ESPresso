#include "ipp_codec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t random_state = 0x45535052u;

static uint8_t next_random_byte(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return (uint8_t)random_state;
}

static void exercise_message(const uint8_t *data, size_t length)
{
    ipp_request_info_t info;
    (void)ipp_codec_inspect_request(data, length, &info);

    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    (void)ipp_codec_rewrite(data, length, "ipp://printer/ipp/print",
                            "ipp://printer", &output, &output_length,
                            &attributes_length);
    free(output);

    printer_target_t target = {0};
    snprintf(target.instance, sizeof(target.instance), "Fuzz fixture");
    snprintf(target.pdl, sizeof(target.pdl), "image/urf");
    snprintf(target.urf, sizeof(target.urf), "W8,RS300");
    (void)ipp_codec_apply_printer_attributes(data, length, &target);

    output = NULL;
    output_length = 0;
    attributes_length = 0;
    (void)ipp_codec_normalize_printer_response(
        data, length, "ipp://espresso.local/ipp/print",
        "ipp://espresso.local", "fuzz-uuid", &target, &output,
        &output_length, &attributes_length);
    free(output);

    output = NULL;
    output_length = 0;
    attributes_length = 0;
    (void)ipp_codec_filter_printer_response(
        data, length, "printer-uri-supported,media-supported", &output,
        &output_length, &attributes_length);
    free(output);
}

int main(void)
{
    uint8_t *seed = NULL;
    size_t seed_length = 0;
    assert(ipp_codec_build_get_printer_attributes_for_format(
               2, 0, 77, "ipp://printer.local:631/ipp/print", true,
               "image/urf", &seed, &seed_length) == IPP_CODEC_OK);

    for (size_t length = 0; length <= seed_length; ++length) {
        exercise_message(seed, length);
    }
    uint8_t *mutation = malloc(seed_length);
    assert(mutation);
    for (size_t offset = 0; offset < seed_length; ++offset) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            memcpy(mutation, seed, seed_length);
            mutation[offset] ^= (uint8_t)(1u << bit);
            exercise_message(mutation, seed_length);
        }
    }
    free(mutation);
    free(seed);

    uint8_t random_message[1024];
    for (size_t iteration = 0; iteration < 2000; ++iteration) {
        size_t length = ((size_t)next_random_byte() << 2) |
                        (next_random_byte() & 3u);
        for (size_t i = 0; i < length; ++i) {
            random_message[i] = next_random_byte();
        }
        exercise_message(random_message, length);
    }
    puts("IPP deterministic fuzz smoke tests passed");
    return 0;
}

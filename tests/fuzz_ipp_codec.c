#include "ipp_codec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESPRESSO_STANDALONE_FUZZ
#define FUZZ_MAX_INPUT 4096
#define FUZZ_MAX_CORPUS 256
#define FUZZ_COVERAGE_BITS 65536

typedef struct {
    uint8_t *data;
    size_t size;
} fuzz_corpus_item_t;

static uint8_t fuzz_coverage[FUZZ_COVERAGE_BITS];
static uint32_t fuzz_guard_count;
static size_t fuzz_coverage_count;
static bool fuzz_found_coverage;
static uint32_t fuzz_random_state = 0x45535052u;

__attribute__((no_sanitize("coverage")))
void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    if (start == stop || *start) {
        return;
    }
    for (uint32_t *guard = start; guard < stop; ++guard) {
        *guard = ++fuzz_guard_count;
    }
}

__attribute__((no_sanitize("coverage")))
void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    uint32_t index = *guard;
    if (!index) {
        return;
    }
    index %= FUZZ_COVERAGE_BITS;
    if (!fuzz_coverage[index]) {
        fuzz_coverage[index] = 1;
        ++fuzz_coverage_count;
        fuzz_found_coverage = true;
    }
}

static uint32_t fuzz_random(void)
{
    fuzz_random_state ^= fuzz_random_state << 13;
    fuzz_random_state ^= fuzz_random_state >> 17;
    fuzz_random_state ^= fuzz_random_state << 5;
    return fuzz_random_state;
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ipp_request_info_t info;
    (void)ipp_codec_inspect_request(data, size, &info);

    printer_target_t target = {0};
    target.profile_schema = ESPRESSO_PROFILE_SCHEMA;
    memcpy(target.instance, "Fuzz printer", sizeof("Fuzz printer"));
    memcpy(target.pdl, "image/urf", sizeof("image/urf"));
    memcpy(target.urf, "W8,RS300", sizeof("W8,RS300"));
    ipp_codec_finalize_profile(&target);

    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    (void)ipp_codec_rewrite_request(
        data, size, "ipp://printer.local/ipp/print", "ipp://printer.local",
        &target, &output, &output_length, &attributes_length);
    free(output);

    (void)ipp_codec_apply_printer_attributes(data, size, &target);

    output = NULL;
    output_length = 0;
    attributes_length = 0;
    (void)ipp_codec_normalize_printer_response(
        data, size, "ipp://espresso.local/ipp/print", "ipp://espresso.local",
        "00000000-0000-4000-8000-000000000001", &target, &output,
        &output_length, &attributes_length);
    free(output);

    output = NULL;
    output_length = 0;
    attributes_length = 0;
    (void)ipp_codec_filter_response(
        data, size, IPP_RESPONSE_KIND_PRINTER,
        "all,job-template,media-col-database", &output, &output_length,
        &attributes_length);
    free(output);

    output = NULL;
    output_length = 0;
    attributes_length = 0;
    (void)ipp_codec_filter_response(
        data, size, IPP_RESPONSE_KIND_JOB, "job-description,job-template",
        &output, &output_length, &attributes_length);
    free(output);
    return 0;
}

#ifdef ESPRESSO_STANDALONE_FUZZ
static void mutate(uint8_t *data, size_t *size)
{
    unsigned changes = 1 + fuzz_random() % 8;
    while (changes--) {
        unsigned operation = fuzz_random() % 4;
        if (operation == 0 && *size) {
            data[fuzz_random() % *size] ^= (uint8_t)(1U << (fuzz_random() % 8));
        } else if (operation == 1 && *size) {
            data[fuzz_random() % *size] = (uint8_t)fuzz_random();
        } else if (operation == 2 && *size < FUZZ_MAX_INPUT) {
            size_t position = *size ? fuzz_random() % (*size + 1) : 0;
            memmove(data + position + 1, data + position, *size - position);
            data[position] = (uint8_t)fuzz_random();
            ++*size;
        } else if (*size) {
            size_t position = fuzz_random() % *size;
            memmove(data + position, data + position + 1,
                    *size - position - 1);
            --*size;
        }
    }
}

int main(void)
{
    fuzz_corpus_item_t corpus[FUZZ_MAX_CORPUS] = {0};
    uint8_t *seed = NULL;
    size_t seed_size = 0;
    if (ipp_codec_build_get_printer_attributes_for_format(
            2, 0, 1, "ipp://printer.local:631/ipp/print", true, "image/urf",
            &seed, &seed_size) != IPP_CODEC_OK) {
        return 1;
    }
    corpus[0].data = seed;
    corpus[0].size = seed_size;
    size_t corpus_count = 1;
    LLVMFuzzerTestOneInput(seed, seed_size);

    uint8_t *candidate = malloc(FUZZ_MAX_INPUT);
    if (!candidate) {
        free(seed);
        return 1;
    }
    for (size_t iteration = 0; iteration < 5000; ++iteration) {
        fuzz_corpus_item_t *source = corpus + fuzz_random() % corpus_count;
        size_t size = source->size;
        memcpy(candidate, source->data, size);
        mutate(candidate, &size);
        fuzz_found_coverage = false;
        LLVMFuzzerTestOneInput(candidate, size);
        if (fuzz_found_coverage && corpus_count < FUZZ_MAX_CORPUS) {
            uint8_t *copy = malloc(size ? size : 1);
            if (!copy) {
                break;
            }
            memcpy(copy, candidate, size);
            corpus[corpus_count].data = copy;
            corpus[corpus_count].size = size;
            ++corpus_count;
        }
    }
    free(candidate);
    for (size_t index = 0; index < corpus_count; ++index) {
        free(corpus[index].data);
    }
    printf("Coverage-guided IPP fuzz smoke passed: %zu edges, %zu inputs\n",
           fuzz_coverage_count, corpus_count);
    return 0;
}
#endif

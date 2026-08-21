#include "ipp_codec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int contains(const uint8_t *haystack, size_t haystack_length,
                    const char *needle)
{
    size_t needle_length = strlen(needle);
    if (needle_length > haystack_length) {
        return 0;
    }
    for (size_t i = 0; i + needle_length <= haystack_length; ++i) {
        if (memcmp(haystack + i, needle, needle_length) == 0) {
            return 1;
        }
    }
    return 0;
}

static size_t add_attribute(uint8_t *message, size_t cursor, uint8_t tag,
                            const char *name, const char *value)
{
    size_t name_length = strlen(name);
    size_t value_length = strlen(value);
    message[cursor++] = tag;
    message[cursor++] = (uint8_t)(name_length >> 8);
    message[cursor++] = (uint8_t)name_length;
    memcpy(message + cursor, name, name_length);
    cursor += name_length;
    message[cursor++] = (uint8_t)(value_length >> 8);
    message[cursor++] = (uint8_t)value_length;
    memcpy(message + cursor, value, value_length);
    return cursor + value_length;
}

static size_t add_raw_attribute(uint8_t *message, size_t cursor, uint8_t tag,
                                const char *name, const uint8_t *value,
                                size_t value_length)
{
    size_t name_length = strlen(name);
    message[cursor++] = tag;
    message[cursor++] = (uint8_t)(name_length >> 8);
    message[cursor++] = (uint8_t)name_length;
    memcpy(message + cursor, name, name_length);
    cursor += name_length;
    message[cursor++] = (uint8_t)(value_length >> 8);
    message[cursor++] = (uint8_t)value_length;
    memcpy(message + cursor, value, value_length);
    return cursor + value_length;
}

static void test_rewrites_printer_uri_and_preserves_document(void)
{
    uint8_t message[512] = {2, 0, 0, 2, 0, 0, 0, 7, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47, "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x45, "printer-uri",
                           "ipp://espresso.local:631/ipp/print");
    message[length++] = 3;
    const uint8_t document[] = {0x25, 0x50, 0x44, 0x46, 0x03, 0xff};
    memcpy(message + length, document, sizeof(document));
    length += sizeof(document);

    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    assert(ipp_codec_rewrite(message, length,
                             "ipp://192.168.1.20:631/ipp/printer",
                             "ipp://192.168.1.20:631", &output,
                             &output_length, &attributes_length) == IPP_CODEC_OK);
    assert(output_length > sizeof(document));
    assert(contains(output, output_length, "ipp://192.168.1.20:631/ipp/printer"));
    assert(memcmp(output + output_length - sizeof(document), document, sizeof(document)) == 0);
    assert(attributes_length < output_length);
    free(output);
}

static void test_rewrites_job_uri_authority(void)
{
    uint8_t message[256] = {2, 0, 0, 9, 0, 0, 0, 8, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x45, "job-uri",
                           "ipp://old-printer.local:631/jobs/42");
    message[length++] = 3;

    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    assert(ipp_codec_rewrite(message, length,
                             "ipp://espresso.local:631/ipp/print",
                             "ipp://espresso.local:631", &output,
                             &output_length, &attributes_length) == IPP_CODEC_OK);
    const char expected[] = "ipp://espresso.local:631/jobs/42";
    assert(contains(output, output_length, expected));
    free(output);
}

static void test_reports_incomplete_message(void)
{
    const uint8_t message[] = {2, 0, 0, 2, 0, 0, 0, 1, 1, 0};
    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    assert(ipp_codec_rewrite(message, sizeof(message), "ipp://printer/ipp/print",
                             "ipp://printer", &output, &output_length,
                             &attributes_length) == IPP_CODEC_INCOMPLETE);
    assert(output == NULL);
}

static void test_does_not_rewrite_unrelated_uri(void)
{
    uint8_t message[256] = {2, 0, 0, 2, 0, 0, 0, 9, 1};
    size_t length = 9;
    const char original[] = "http://old-printer.local/help";
    length = add_attribute(message, length, 0x45, "printer-more-info", original);
    message[length++] = 3;
    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    assert(ipp_codec_rewrite(message, length, "ipp://espresso.local/ipp/print",
                             "ipp://espresso.local", &output, &output_length,
                             &attributes_length) == IPP_CODEC_OK);
    assert(contains(output, output_length, original));
    free(output);
}

static void test_normalizes_frontend_ipp_version(void)
{
    uint8_t message[128] = {1, 1, 0, 0, 0, 0, 0, 10, 4};
    size_t length = 9;
    length = add_attribute(message, length, 0x44, "ipp-versions-supported", "1.1");
    length = add_attribute(message, length, 0x45, "printer-uuid",
                           "urn:uuid:upstream-printer");
    length = add_attribute(message, length, 0x44, "uri-security-supported", "tls");
    message[length++] = 3;
    printer_target_t target = {0};
    strcpy(target.instance, "Legacy printer");
    strcpy(target.label, "Legacy Printer 2000");
    strcpy(target.pdl, "image/urf");
    strcpy(target.urf, "W8,SRGB24,RS600");
    strcpy(target.media, "iso_a4_210x297mm,na_letter_8.5x11in");
    strcpy(target.media_default, "iso_a4_210x297mm");
    ipp_codec_finalize_profile(&target);
    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    assert(ipp_codec_normalize_printer_response(
               message, length, "ipp://espresso.local/ipp/print",
               "ipp://espresso.local", "bridge-uuid", &target, &output,
               &output_length, &attributes_length) == IPP_CODEC_OK);
    assert(contains(output, output_length, "2.0"));
    assert(contains(output, output_length, "1.1"));
    assert(contains(output, output_length, "urn:uuid:bridge-uuid"));
    assert(!contains(output, output_length, "upstream-printer"));
    assert(!contains(output, output_length, "tls"));
    assert(contains(output, output_length, "uri-authentication-supported"));
    assert(contains(output, output_length, "document-format-default"));
    assert(contains(output, output_length, "media-col-database"));
    assert(contains(output, output_length, "media-col-default"));
    assert(contains(output, output_length, "print-color-mode-supported"));
    assert(contains(output, output_length, "printer-resolution-supported"));
    free(output);
}

static void test_builds_cups_style_capability_query(void)
{
    uint8_t *request = NULL;
    size_t request_length = 0;
    assert(ipp_codec_build_get_printer_attributes(
               2, 0, 42, "ipp://192.168.1.20:631/ipp/print", true,
               &request, &request_length) == IPP_CODEC_OK);
    assert(request[0] == 2 && request[1] == 0);
    assert(ipp_codec_message_code(request, request_length) == 0x000b);
    assert(contains(request, request_length, "requested-attributes"));
    assert(contains(request, request_length, "media-col-database"));
    assert(request[request_length - 1] == 3);
    free(request);
}

static void test_extracts_relayable_capability_profile(void)
{
    uint8_t message[2048] = {1, 1, 0, 0, 0, 0, 0, 43, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47, "attributes-charset", "utf-8");
    message[length++] = 4;
    length = add_attribute(message, length, 0x41, "printer-make-and-model",
                           "TestCo Legacy 500");
    length = add_attribute(message, length, 0x45, "printer-uuid",
                           "urn:uuid:physical-printer");
    length = add_attribute(message, length, 0x44, "ipp-versions-supported", "1.1");
    length = add_attribute(message, length, 0x49, "document-format-supported",
                           "image/urf");
    length = add_attribute(message, length, 0x49, "", "application/pdf");
    length = add_attribute(message, length, 0x44, "urf-supported", "W8");
    length = add_attribute(message, length, 0x44, "", "SRGB24");
    length = add_attribute(message, length, 0x44, "", "RS600");
    length = add_attribute(message, length, 0x44, "media-supported",
                           "iso_a4_210x297mm");
    length = add_attribute(message, length, 0x44, "media-default",
                           "iso_a4_210x297mm");
    length = add_attribute(message, length, 0x44, "output-mode-default", "color");
    length = add_attribute(message, length, 0x44, "sides-supported",
                           "one-sided");
    length = add_attribute(message, length, 0x44, "",
                           "two-sided-long-edge");
    const uint8_t color[] = {1};
    length = add_raw_attribute(message, length, 0x22, "color-supported",
                               color, sizeof(color));
    const uint8_t copies[] = {0, 0, 0, 1, 0, 0, 0, 99};
    length = add_raw_attribute(message, length, 0x33, "copies-supported",
                               copies, sizeof(copies));
    const uint8_t print_job[] = {0, 0, 0, 2};
    length = add_raw_attribute(message, length, 0x23, "operations-supported",
                               print_job, sizeof(print_job));
    const uint8_t get_printer[] = {0, 0, 0, 11};
    length = add_raw_attribute(message, length, 0x23, "", get_printer,
                               sizeof(get_printer));
    message[length++] = 3;

    printer_target_t target = {0};
    strcpy(target.pdl, "application/octet-stream");
    assert(ipp_codec_apply_printer_attributes(message, length, &target) ==
           IPP_CODEC_OK);
    assert(strcmp(target.label, "TestCo Legacy 500") == 0);
    assert(strcmp(target.uuid, "physical-printer") == 0);
    assert(contains((const uint8_t *)target.pdl, strlen(target.pdl), "image/urf"));
    assert(contains((const uint8_t *)target.pdl, strlen(target.pdl), "application/pdf"));
    assert(!contains((const uint8_t *)target.pdl, strlen(target.pdl),
                     "application/octet-stream"));
    assert(contains((const uint8_t *)target.urf, strlen(target.urf), "SRGB24"));
    assert(target.color);
    assert(target.duplex);
    assert(target.copies && target.copies_upper == 99);
    assert(strcmp(target.media_default, "iso_a4_210x297mm") == 0);
    assert(strcmp(target.color_mode_default, "color") == 0);
    assert(target.resolution_low_dpi == 600 && target.resolution_high_dpi == 600);
    assert((target.operations_supported & (1ULL << 2)) != 0);
    assert((target.operations_supported & (1ULL << 11)) != 0);
    assert(target.upstream_ipp_major == 1 && target.upstream_ipp_minor == 1);
}

int main(void)
{
    test_rewrites_printer_uri_and_preserves_document();
    test_rewrites_job_uri_authority();
    test_reports_incomplete_message();
    test_does_not_rewrite_unrelated_uri();
    test_normalizes_frontend_ipp_version();
    test_builds_cups_style_capability_query();
    test_extracts_relayable_capability_profile();
    puts("IPP codec tests passed");
    return 0;
}

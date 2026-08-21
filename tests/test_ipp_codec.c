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
    if (value_length) {
        memcpy(message + cursor, value, value_length);
    }
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

static uint8_t *filter_response(const uint8_t *message, size_t length,
                                ipp_response_kind_t response_kind,
                                const char *requested_attributes,
                                size_t *filtered_length)
{
    uint8_t *filtered = NULL;
    size_t attributes_length = 0;
    assert(ipp_codec_filter_response(
               message, length, response_kind, requested_attributes, &filtered,
               filtered_length, &attributes_length) == IPP_CODEC_OK);
    assert(attributes_length <= *filtered_length);
    return filtered;
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

static void test_consumes_facade_defaults_and_translates_legacy_color(void)
{
    uint8_t message[1024] = {2, 0, 0, 2, 0, 0, 0, 31, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47,
                           "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x48,
                           "attributes-natural-language", "en");
    length = add_attribute(message, length, 0x45, "printer-uri",
                           "ipp://espresso.local/ipp/print");
    message[length++] = 2;
    const uint8_t one[] = {0, 0, 0, 1};
    const uint8_t none[] = {0, 0, 0, 3};
    const uint8_t normal[] = {0, 0, 0, 4};
    length = add_raw_attribute(message, length, 0x21, "copies", one,
                               sizeof(one));
    length = add_raw_attribute(message, length, 0x23, "finishings", none,
                               sizeof(none));
    length = add_raw_attribute(message, length, 0x23,
                               "orientation-requested", none, sizeof(none));
    length = add_attribute(message, length, 0x44, "output-bin", "face-down");
    length = add_raw_attribute(message, length, 0x23, "print-quality", normal,
                               sizeof(normal));
    length = add_attribute(message, length, 0x44, "sides", "one-sided");
    length = add_attribute(message, length, 0x44, "print-color-mode", "color");
    message[length++] = 3;
    const uint8_t document[] = {1, 2, 3, 4};
    memcpy(message + length, document, sizeof(document));
    length += sizeof(document);

    printer_target_t target = {0};
    target.legacy_output_mode = true;
    uint8_t *output = NULL;
    size_t output_length = 0;
    size_t attributes_length = 0;
    assert(ipp_codec_rewrite_request(
               message, length, "ipp://legacy.local/ipp/print",
               "ipp://legacy.local", &target, &output, &output_length,
               &attributes_length) == IPP_CODEC_OK);
    assert(!contains(output, output_length, "copies"));
    assert(!contains(output, output_length, "finishings"));
    assert(!contains(output, output_length, "orientation-requested"));
    assert(!contains(output, output_length, "output-bin"));
    assert(!contains(output, output_length, "print-quality"));
    assert(!contains(output, output_length, "sides"));
    assert(!contains(output, output_length, "print-color-mode"));
    assert(contains(output, output_length, "output-mode"));
    assert(memcmp(output + output_length - sizeof(document), document,
                  sizeof(document)) == 0);
    free(output);

    target.job_template_capabilities = ESPRESSO_JOB_CAP_FINISHINGS;
    assert(ipp_codec_rewrite_request(
               message, length, "ipp://legacy.local/ipp/print",
               "ipp://legacy.local", &target, &output, &output_length,
               &attributes_length) == IPP_CODEC_OK);
    assert(contains(output, output_length, "finishings"));
    free(output);

    const uint8_t staple[] = {0, 0, 0, 4};
    uint8_t unsupported[256] = {2, 0, 0, 2, 0, 0, 0, 32, 1};
    size_t unsupported_length = 9;
    unsupported_length = add_attribute(unsupported, unsupported_length, 0x47,
                                       "attributes-charset", "utf-8");
    unsupported[unsupported_length++] = 2;
    unsupported_length = add_raw_attribute(
        unsupported, unsupported_length, 0x23, "finishings", staple,
        sizeof(staple));
    unsupported[unsupported_length++] = 3;
    target.job_template_capabilities = 0;
    assert(ipp_codec_rewrite_request(
               unsupported, unsupported_length,
               "ipp://legacy.local/ipp/print", "ipp://legacy.local", &target,
               &output, &output_length, &attributes_length) ==
           IPP_CODEC_UNSUPPORTED);
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
    assert(contains(output, output_length, "finishings-default"));
    assert(contains(output, output_length, "finishings-supported"));
    assert(contains(output, output_length, "orientation-requested-default"));
    assert(contains(output, output_length, "orientation-requested-supported"));
    assert(contains(output, output_length, "output-bin-default"));
    assert(contains(output, output_length, "output-bin-supported"));
    assert(contains(output, output_length, "print-quality-default"));
    assert(contains(output, output_length, "print-quality-supported"));
    assert(contains(output, output_length, "sides-default"));
    assert(contains(output, output_length, "pages-per-minute"));
    assert(contains(output, output_length, "pages-per-minute-color"));
    assert(contains(output, output_length, "copies-default"));
    assert(contains(output, output_length, "media-default"));
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

static void test_builds_format_specific_capability_query(void)
{
    uint8_t *request = NULL;
    size_t request_length = 0;
    assert(ipp_codec_build_get_printer_attributes_for_format(
               1, 1, 77, "ipp://printer.local/ipp/print", false,
               "image/urf", &request, &request_length) == IPP_CODEC_OK);
    assert(contains(request, request_length, "document-format"));
    assert(contains(request, request_length, "image/urf"));
    ipp_request_info_t info;
    assert(ipp_codec_inspect_request(request, request_length, &info) ==
           IPP_CODEC_OK);
    assert(info.operation_id == IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    assert(info.request_id == 77);
    assert(strcmp(info.document_format, "image/urf") == 0);
    assert(!info.has_document);
    assert(info.operation_attributes_valid);
    free(request);
}

static void test_detects_wrong_operation_attribute_order(void)
{
    uint8_t message[256] = {2, 0, 0, 11, 0, 0, 0, 99, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x48,
                           "attributes-natural-language", "en");
    length = add_attribute(message, length, 0x47,
                           "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x45, "printer-uri",
                           "ipp://espresso.local/ipp/print");
    message[length++] = 3;
    ipp_request_info_t info;
    assert(ipp_codec_inspect_request(message, length, &info) == IPP_CODEC_OK);
    assert(!info.operation_attributes_valid);
}

static void test_filters_unrequested_printer_attributes(void)
{
    uint8_t message[512] = {2, 0, 0, 0, 0, 0, 0, 17, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47,
                           "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x48,
                           "attributes-natural-language", "en");
    message[length++] = 4;
    length = add_attribute(message, length, 0x45,
                           "printer-uri-supported",
                           "ipp://espresso.local/ipp/print");
    length = add_attribute(message, length, 0x42, "printer-name", "ESPresso");
    message[length++] = 3;
    uint8_t *filtered = NULL;
    size_t filtered_length = 0;
    size_t attributes_length = 0;
    assert(ipp_codec_filter_printer_response(
               message, length, "printer-uri-supported", &filtered,
               &filtered_length, &attributes_length) == IPP_CODEC_OK);
    assert(contains(filtered, filtered_length, "printer-uri-supported"));
    assert(!contains(filtered, filtered_length, "printer-name"));
    assert(contains(filtered, filtered_length, "attributes-charset"));
    free(filtered);
}

static void test_expands_requested_printer_attribute_groups(void)
{
    uint8_t message[2048] = {2, 0, 0, 0, 0, 0, 0, 18, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47,
                           "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x48,
                           "attributes-natural-language", "en");
    message[length++] = 4;
    length = add_attribute(message, length, 0x45,
                           "printer-uri-supported",
                           "ipp://espresso.local/ipp/print");
    length = add_attribute(message, length, 0x42, "printer-name", "ESPresso");
    const uint8_t printer_state[] = {0, 0, 0, 3};
    length = add_raw_attribute(message, length, 0x23, "printer-state",
                               printer_state, sizeof(printer_state));
    const uint8_t copies[] = {0, 0, 0, 1, 0, 0, 0, 99};
    length = add_raw_attribute(message, length, 0x33, "copies-supported",
                               copies, sizeof(copies));
    length = add_attribute(message, length, 0x44, "sides-supported",
                           "one-sided");
    length = add_attribute(message, length, 0x44, "",
                           "two-sided-long-edge");
    length = add_raw_attribute(message, length, 0x34, "media-col-default",
                               NULL, 0);
    length = add_attribute(message, length, 0x4a, "", "media-size");
    length = add_raw_attribute(message, length, 0x34, "", NULL, 0);
    length = add_attribute(message, length, 0x4a, "", "x-dimension");
    const uint8_t x_dimension[] = {0, 0, 0x52, 0x08};
    length = add_raw_attribute(message, length, 0x21, "", x_dimension,
                               sizeof(x_dimension));
    length = add_raw_attribute(message, length, 0x37, "", NULL, 0);
    length = add_raw_attribute(message, length, 0x37, "", NULL, 0);
    length = add_raw_attribute(message, length, 0x34, "media-col-database",
                               NULL, 0);
    length = add_attribute(message, length, 0x4a, "", "media-size-name");
    length = add_attribute(message, length, 0x44, "", "iso_a4_210x297mm");
    length = add_raw_attribute(message, length, 0x37, "", NULL, 0);
    length = add_attribute(message, length, 0x44, "print-color-mode-default",
                           "color");
    const uint8_t page_ranges[] = {1};
    length = add_raw_attribute(message, length, 0x22, "page-ranges-supported",
                               page_ranges, sizeof(page_ranges));
    length = add_raw_attribute(message, length, 0x22, "pages-ranges-supported",
                               page_ranges, sizeof(page_ranges));
    message[length++] = 3;

    size_t filtered_length = 0;
    uint8_t *filtered = filter_response(
        message, length, IPP_RESPONSE_KIND_PRINTER, "printer-description",
        &filtered_length);
    assert(contains(filtered, filtered_length, "attributes-charset"));
    assert(contains(filtered, filtered_length, "attributes-natural-language"));
    assert(contains(filtered, filtered_length, "printer-uri-supported"));
    assert(contains(filtered, filtered_length, "printer-name"));
    assert(contains(filtered, filtered_length, "printer-state"));
    assert(!contains(filtered, filtered_length, "copies-supported"));
    assert(!contains(filtered, filtered_length, "sides-supported"));
    assert(!contains(filtered, filtered_length, "media-col-default"));
    assert(!contains(filtered, filtered_length, "media-col-database"));
    free(filtered);

    filtered = filter_response(message, length, IPP_RESPONSE_KIND_PRINTER,
                               "job-template", &filtered_length);
    assert(!contains(filtered, filtered_length, "printer-name"));
    assert(!contains(filtered, filtered_length, "printer-state"));
    assert(contains(filtered, filtered_length, "copies-supported"));
    assert(contains(filtered, filtered_length, "sides-supported"));
    assert(contains(filtered, filtered_length, "two-sided-long-edge"));
    assert(contains(filtered, filtered_length, "media-col-default"));
    assert(!contains(filtered, filtered_length, "media-col-database"));
    assert(contains(filtered, filtered_length, "media-size"));
    assert(contains(filtered, filtered_length, "x-dimension"));
    assert(contains(filtered, filtered_length, "print-color-mode-default"));
    assert(contains(filtered, filtered_length, "page-ranges-supported"));
    assert(contains(filtered, filtered_length, "pages-ranges-supported"));
    free(filtered);

    filtered = filter_response(
        message, length, IPP_RESPONSE_KIND_PRINTER,
        "printer-description,job-template", &filtered_length);
    assert(contains(filtered, filtered_length, "printer-name"));
    assert(contains(filtered, filtered_length, "copies-supported"));
    assert(contains(filtered, filtered_length, "media-col-default"));
    assert(!contains(filtered, filtered_length, "media-col-database"));
    free(filtered);

    filtered = filter_response(
        message, length, IPP_RESPONSE_KIND_PRINTER,
        "printer-description,copies-supported", &filtered_length);
    assert(contains(filtered, filtered_length, "printer-name"));
    assert(contains(filtered, filtered_length, "copies-supported"));
    assert(!contains(filtered, filtered_length, "sides-supported"));
    free(filtered);

    filtered = filter_response(
        message, length, IPP_RESPONSE_KIND_PRINTER, "media-col-database",
        &filtered_length);
    assert(contains(filtered, filtered_length, "media-col-database"));
    assert(contains(filtered, filtered_length, "iso_a4_210x297mm"));
    assert(!contains(filtered, filtered_length, "media-col-default"));
    assert(!contains(filtered, filtered_length, "printer-name"));
    free(filtered);

    filtered = filter_response(message, length, IPP_RESPONSE_KIND_PRINTER,
                               "unknown-selector", &filtered_length);
    assert(contains(filtered, filtered_length, "attributes-charset"));
    assert(!contains(filtered, filtered_length, "printer-name"));
    assert(!contains(filtered, filtered_length, "copies-supported"));
    free(filtered);

    filtered = filter_response(
        message, length, IPP_RESPONSE_KIND_PRINTER,
        "job-template,job-template,sides-supported", &filtered_length);
    assert(contains(filtered, filtered_length, "copies-supported"));
    assert(contains(filtered, filtered_length, "two-sided-long-edge"));
    assert(!contains(filtered, filtered_length, "printer-name"));
    size_t deduplicated_length = 0;
    uint8_t *deduplicated = filter_response(
        message, length, IPP_RESPONSE_KIND_PRINTER, "job-template",
        &deduplicated_length);
    assert(filtered_length == deduplicated_length);
    assert(memcmp(filtered, deduplicated, filtered_length) == 0);
    free(deduplicated);
    free(filtered);

    filtered = filter_response(message, length, IPP_RESPONSE_KIND_PRINTER,
                               "all", &filtered_length);
    assert(contains(filtered, filtered_length, "printer-name"));
    assert(contains(filtered, filtered_length, "media-col-default"));
    assert(!contains(filtered, filtered_length, "media-col-database"));
    free(filtered);

    filtered = filter_response(message, length, IPP_RESPONSE_KIND_PRINTER,
                               "", &filtered_length);
    assert(contains(filtered, filtered_length, "printer-name"));
    assert(!contains(filtered, filtered_length, "media-col-database"));
    free(filtered);

    filtered = filter_response(
        message, length, IPP_RESPONSE_KIND_PRINTER,
        "all,media-col-database", &filtered_length);
    assert(filtered_length == length);
    assert(memcmp(filtered, message, length) == 0);
    free(filtered);
}

static void test_keeps_job_description_and_printer_description_distinct(void)
{
    uint8_t message[1024] = {2, 0, 0, 0, 0, 0, 0, 19, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47,
                           "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x48,
                           "attributes-natural-language", "en");
    message[length++] = 2;
    length = add_attribute(message, length, 0x45, "job-uri",
                           "ipp://espresso.local/jobs/7");
    const uint8_t job_id[] = {0, 0, 0, 7};
    length = add_raw_attribute(message, length, 0x21, "job-id", job_id,
                               sizeof(job_id));
    const uint8_t copies[] = {0, 0, 0, 2};
    length = add_raw_attribute(message, length, 0x21, "copies", copies,
                               sizeof(copies));
    length = add_attribute(message, length, 0x44, "sides",
                           "two-sided-long-edge");
    message[length++] = 3;

    size_t filtered_length = 0;
    uint8_t *filtered = filter_response(
        message, length, IPP_RESPONSE_KIND_JOB, "job-description",
        &filtered_length);
    assert(contains(filtered, filtered_length, "job-uri"));
    assert(contains(filtered, filtered_length, "job-id"));
    assert(!contains(filtered, filtered_length, "copies"));
    assert(!contains(filtered, filtered_length, "two-sided-long-edge"));
    free(filtered);

    filtered = filter_response(message, length, IPP_RESPONSE_KIND_JOB,
                               "job-template", &filtered_length);
    assert(!contains(filtered, filtered_length, "job-uri"));
    assert(!contains(filtered, filtered_length, "job-id"));
    assert(contains(filtered, filtered_length, "copies"));
    assert(contains(filtered, filtered_length, "two-sided-long-edge"));
    free(filtered);

    filtered = filter_response(message, length, IPP_RESPONSE_KIND_JOB,
                               "printer-description", &filtered_length);
    assert(!contains(filtered, filtered_length, "job-uri"));
    assert(!contains(filtered, filtered_length, "copies"));
    free(filtered);
}

static void test_inspects_setof_requested_attribute_selectors(void)
{
    uint8_t message[512] = {2, 0, 0, 11, 0, 0, 0, 20, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47,
                           "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x48,
                           "attributes-natural-language", "en");
    length = add_attribute(message, length, 0x45, "printer-uri",
                           "ipp://espresso.local/ipp/print");
    length = add_attribute(message, length, 0x44, "requested-attributes",
                           "printer-description");
    length = add_attribute(message, length, 0x44, "", "job-template");
    length = add_attribute(message, length, 0x44, "", "job-template");
    length = add_attribute(message, length, 0x44, "", "printer-state");
    message[length++] = 3;

    ipp_request_info_t info;
    assert(ipp_codec_inspect_request(message, length, &info) == IPP_CODEC_OK);
    assert(strcmp(info.requested_attributes,
                  "printer-description,job-template,printer-state") == 0);
}

static void test_inspects_print_job_document(void)
{
    uint8_t message[256] = {2, 0, 0, 2, 0, 0, 0, 91, 1};
    size_t length = 9;
    length = add_attribute(message, length, 0x47, "attributes-charset", "utf-8");
    length = add_attribute(message, length, 0x49, "document-format", "image/urf");
    message[length++] = 3;
    message[length++] = 'U';
    message[length++] = 'N';
    message[length++] = 'I';
    message[length++] = 'R';
    ipp_request_info_t info;
    assert(ipp_codec_inspect_request(message, length, &info) == IPP_CODEC_OK);
    assert(info.operation_id == IPP_OPERATION_PRINT_JOB);
    assert(info.request_id == 91);
    assert(info.has_document);
    assert(strcmp(info.document_format, "image/urf") == 0);
    assert(info.attributes_length == length - 4);
}

static void test_builds_parseable_ipp_error(void)
{
    uint8_t *response = NULL;
    size_t response_length = 0;
    assert(ipp_codec_build_status_response(
               2, 0, IPP_STATUS_SERVER_ERROR_OPERATION_NOT_SUPPORTED, 123,
               "No relay", &response, &response_length) == IPP_CODEC_OK);
    assert(response[0] == 2 && response[1] == 0);
    assert(ipp_codec_message_code(response, response_length) ==
           IPP_STATUS_SERVER_ERROR_OPERATION_NOT_SUPPORTED);
    assert(contains(response, response_length, "status-message"));
    assert(contains(response, response_length, "No relay"));
    free(response);
}

static void test_filters_operations_and_formats(void)
{
    uint64_t upstream = (1ULL << IPP_OPERATION_PRINT_JOB) |
                        (1ULL << 3) |
                        (1ULL << IPP_OPERATION_CREATE_JOB) |
                        (1ULL << IPP_OPERATION_CANCEL_JOB);
    uint64_t relayed = ipp_codec_relay_operations(upstream);
    assert((relayed & (1ULL << IPP_OPERATION_PRINT_JOB)) != 0);
    assert((relayed & (1ULL << IPP_OPERATION_CANCEL_JOB)) != 0);
    assert((relayed & (1ULL << IPP_OPERATION_GET_PRINTER_ATTRIBUTES)) != 0);
    assert((relayed & (1ULL << 3)) == 0);
    assert((relayed & (1ULL << IPP_OPERATION_CREATE_JOB)) == 0);

    printer_target_t target = {0};
    strcpy(target.pdl, "image/urf,application/pdf");
    assert(ipp_codec_format_supported(&target, "image/urf"));
    assert(ipp_codec_format_supported(&target, "APPLICATION/PDF"));
    assert(!ipp_codec_format_supported(&target, "image/pwg-raster"));
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
    length = add_attribute(message, length, 0x44, "output-mode-supported",
                           "monochrome");
    length = add_attribute(message, length, 0x44, "", "color");
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
    const uint8_t printer_state[] = {0, 0, 0, 3};
    length = add_raw_attribute(message, length, 0x23, "printer-state",
                               printer_state, sizeof(printer_state));
    const uint8_t accepting[] = {1};
    length = add_raw_attribute(message, length, 0x22,
                               "printer-is-accepting-jobs", accepting,
                               sizeof(accepting));
    length = add_attribute(message, length, 0x44, "printer-state-reasons", "none");
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
    assert(target.legacy_output_mode);
    assert(target.resolution_low_dpi == 600 && target.resolution_high_dpi == 600);
    assert((target.operations_supported & (1ULL << 2)) != 0);
    assert((target.operations_supported & (1ULL << 11)) != 0);
    assert(target.upstream_ipp_major == 1 && target.upstream_ipp_minor == 1);
    assert(target.printer_state == 3);
    assert(target.accepting_jobs && target.accepting_jobs_known);
    assert(strcmp(target.state_reasons, "none") == 0);
}

int main(void)
{
    test_rewrites_printer_uri_and_preserves_document();
    test_rewrites_job_uri_authority();
    test_reports_incomplete_message();
    test_does_not_rewrite_unrelated_uri();
    test_consumes_facade_defaults_and_translates_legacy_color();
    test_normalizes_frontend_ipp_version();
    test_builds_cups_style_capability_query();
    test_builds_format_specific_capability_query();
    test_inspects_print_job_document();
    test_detects_wrong_operation_attribute_order();
    test_filters_unrequested_printer_attributes();
    test_expands_requested_printer_attribute_groups();
    test_keeps_job_description_and_printer_description_distinct();
    test_inspects_setof_requested_attribute_selectors();
    test_builds_parseable_ipp_error();
    test_filters_operations_and_formats();
    test_extracts_relayable_capability_profile();
    puts("IPP codec tests passed");
    return 0;
}

#include "ipp_codec.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPP_HEADER_LENGTH 8
#define IPP_NAME_MAX 255

#define IPP_TAG_OPERATION_ATTRIBUTES 0x01
#define IPP_TAG_JOB_ATTRIBUTES 0x02
#define IPP_TAG_END_ATTRIBUTES 0x03
#define IPP_TAG_PRINTER_ATTRIBUTES 0x04
#define IPP_TAG_INTEGER 0x21
#define IPP_TAG_BOOLEAN 0x22
#define IPP_TAG_ENUM 0x23
#define IPP_TAG_OCTETSTRING 0x30
#define IPP_TAG_RESOLUTION 0x32
#define IPP_TAG_RANGE 0x33
#define IPP_TAG_BEGIN_COLLECTION 0x34
#define IPP_TAG_END_COLLECTION 0x37
#define IPP_TAG_TEXT 0x41
#define IPP_TAG_NAME 0x42
#define IPP_TAG_KEYWORD 0x44
#define IPP_TAG_URI 0x45
#define IPP_TAG_URI_SCHEME 0x46
#define IPP_TAG_CHARSET 0x47
#define IPP_TAG_LANGUAGE 0x48
#define IPP_TAG_MIMETYPE 0x49
#define IPP_TAG_MEMBER_NAME 0x4a

#define IPP_STATUS_ERROR_BAD_REQUEST 0x0400

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} byte_buffer_t;

typedef uint64_t present_attribute_t;
#define PRESENT_VERSIONS (1ULL << 0)
#define PRESENT_OPERATIONS (1ULL << 1)
#define PRESENT_PRINTER_URI (1ULL << 2)
#define PRESENT_URI_AUTH (1ULL << 3)
#define PRESENT_URI_SECURITY (1ULL << 4)
#define PRESENT_UUID (1ULL << 5)
#define PRESENT_NAME (1ULL << 6)
#define PRESENT_INFO (1ULL << 7)
#define PRESENT_MAKE_MODEL (1ULL << 8)
#define PRESENT_LOCATION (1ULL << 9)
#define PRESENT_FORMATS (1ULL << 10)
#define PRESENT_FORMAT_DEFAULT (1ULL << 11)
#define PRESENT_URF (1ULL << 12)
#define PRESENT_COLOR (1ULL << 13)
#define PRESENT_SIDES (1ULL << 14)
#define PRESENT_COPIES (1ULL << 15)
#define PRESENT_MEDIA (1ULL << 16)
#define PRESENT_COMPRESSION (1ULL << 17)
#define PRESENT_MULTIPLE_DOCUMENTS (1ULL << 18)
#define PRESENT_MEDIA_COL_DATABASE (1ULL << 19)
#define PRESENT_MEDIA_COL_DEFAULT (1ULL << 20)
#define PRESENT_PRINT_COLOR_MODE (1ULL << 21)
#define PRESENT_PRINT_COLOR_DEFAULT (1ULL << 22)
#define PRESENT_RESOLUTION (1ULL << 23)
#define PRESENT_RESOLUTION_DEFAULT (1ULL << 24)
#define PRESENT_JOB_CREATION (1ULL << 25)
#define PRESENT_PRINTER_STATE (1ULL << 26)
#define PRESENT_STATE_REASONS (1ULL << 27)
#define PRESENT_ACCEPTING_JOBS (1ULL << 28)
#define PRESENT_CHARSET_CONFIGURED (1ULL << 29)
#define PRESENT_CHARSET_SUPPORTED (1ULL << 30)
#define PRESENT_LANGUAGE_CONFIGURED (1ULL << 31)
#define PRESENT_LANGUAGE_SUPPORTED (1ULL << 32)
#define PRESENT_MORE_INFO (1ULL << 33)
#define PRESENT_UP_TIME (1ULL << 34)
#define PRESENT_PDL_OVERRIDE (1ULL << 35)
#define PRESENT_QUEUED_JOB_COUNT (1ULL << 36)
#define PRESENT_COPIES_DEFAULT (1ULL << 37)
#define PRESENT_MEDIA_DEFAULT (1ULL << 38)
#define PRESENT_FINISHINGS_DEFAULT (1ULL << 39)
#define PRESENT_FINISHINGS_SUPPORTED (1ULL << 40)
#define PRESENT_ORIENTATION_DEFAULT (1ULL << 41)
#define PRESENT_ORIENTATION_SUPPORTED (1ULL << 42)
#define PRESENT_OUTPUT_BIN_DEFAULT (1ULL << 43)
#define PRESENT_OUTPUT_BIN_SUPPORTED (1ULL << 44)
#define PRESENT_PRINT_QUALITY_DEFAULT (1ULL << 45)
#define PRESENT_PRINT_QUALITY_SUPPORTED (1ULL << 46)
#define PRESENT_SIDES_DEFAULT (1ULL << 47)
#define PRESENT_PAGES_PER_MINUTE (1ULL << 48)
#define PRESENT_PAGES_PER_MINUTE_COLOR (1ULL << 49)
#define PRESENT_MEDIA_COL_SUPPORTED (1ULL << 50)
#define PRESENT_MEDIA_COL_READY (1ULL << 51)
#define PRESENT_MEDIA_SOURCE_SUPPORTED (1ULL << 52)
#define PRESENT_MEDIA_TYPE_SUPPORTED (1ULL << 53)
#define PRESENT_PRINTER_INPUT_TRAY (1ULL << 54)
#define PRESENT_MANDATORY_JOB_ATTRIBUTES (1ULL << 55)
#define PRESENT_PRINT_SCALING_SUPPORTED (1ULL << 56)
#define PRESENT_PRINT_SCALING_DEFAULT (1ULL << 57)
#define PRESENT_LANDSCAPE_PREFERRED (1ULL << 58)
#define PRESENT_DOCUMENT_PASSWORD_SUPPORTED (1ULL << 59)
#define PRESENT_REFERENCE_URI_SCHEMES (1ULL << 60)
#define PRESENT_MEDIA_KEY_SUPPORTED (1ULL << 61)
#define PRESENT_MEDIA_SIZE_SUPPORTED (1ULL << 62)

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static bool append(byte_buffer_t *buffer, const void *data, size_t length)
{
    if (length == 0) {
        return true;
    }
    if (!data) {
        return false;
    }
    if (length > SIZE_MAX - buffer->length) {
        return false;
    }
    size_t required = buffer->length + length;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        uint8_t *resized = realloc(buffer->data, capacity);
        if (!resized) {
            return false;
        }
        buffer->data = resized;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return true;
}

static bool append_u16(byte_buffer_t *buffer, size_t value)
{
    if (value > UINT16_MAX) {
        return false;
    }
    uint8_t bytes[] = {(uint8_t)(value >> 8), (uint8_t)value};
    return append(buffer, bytes, sizeof(bytes));
}

static bool append_attribute(byte_buffer_t *buffer, uint8_t tag, const char *name,
                             const void *value, size_t value_length)
{
    size_t name_length = name ? strlen(name) : 0;
    return append(buffer, &tag, 1) && append_u16(buffer, name_length) &&
           append(buffer, name, name_length) && append_u16(buffer, value_length) &&
           append(buffer, value, value_length);
}

static bool append_string(byte_buffer_t *buffer, uint8_t tag, const char *name,
                          const char *value)
{
    return value && append_attribute(buffer, tag, name, value, strlen(value));
}

static bool equal_span(const char *left, size_t left_length,
                       const char *right, size_t right_length)
{
    if (left_length != right_length) {
        return false;
    }
    for (size_t i = 0; i < left_length; ++i) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) {
            return false;
        }
    }
    return true;
}

static bool csv_contains(const char *csv, const char *value, size_t value_length)
{
    if (!csv || !value || value_length == 0) {
        return false;
    }
    const char *cursor = csv;
    while (*cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        const char *end = cursor;
        while (end > start && isspace((unsigned char)end[-1])) {
            --end;
        }
        if (equal_span(start, (size_t)(end - start), value, value_length)) {
            return true;
        }
    }
    return false;
}

static void csv_add(char *csv, size_t csv_size, const uint8_t *value,
                    size_t value_length)
{
    while (value_length && isspace((unsigned char)*value)) {
        ++value;
        --value_length;
    }
    while (value_length && isspace((unsigned char)value[value_length - 1])) {
        --value_length;
    }
    if (csv_size == 0 || value_length == 0 ||
        csv_contains(csv, (const char *)value, value_length)) {
        return;
    }
    size_t used = strlen(csv);
    size_t separator = used ? 1 : 0;
    if (used + separator + value_length >= csv_size) {
        return;
    }
    if (separator) {
        csv[used++] = ',';
    }
    memcpy(csv + used, value, value_length);
    csv[used + value_length] = '\0';
}

static void csv_add_split(char *csv, size_t csv_size, const uint8_t *value,
                          size_t value_length)
{
    size_t start = 0;
    for (size_t i = 0; i <= value_length; ++i) {
        if (i == value_length || value[i] == ',') {
            csv_add(csv, csv_size, value + start, i - start);
            start = i + 1;
        }
    }
}

typedef uint8_t requested_group_mask_t;
#define REQUESTED_GROUP_JOB_TEMPLATE (1U << 0)
#define REQUESTED_GROUP_JOB_DESCRIPTION (1U << 1)
#define REQUESTED_GROUP_PRINTER_DESCRIPTION (1U << 2)

typedef struct {
    const char *selector;
    requested_group_mask_t group;
} requested_group_selector_t;

static const requested_group_selector_t REQUESTED_GROUP_SELECTORS[] = {
    {"job-template", REQUESTED_GROUP_JOB_TEMPLATE},
    {"job-description", REQUESTED_GROUP_JOB_DESCRIPTION},
    {"printer-description", REQUESTED_GROUP_PRINTER_DESCRIPTION},
};

/* RFC 8011 section 5.2 plus the driverless-printing Job Template extensions. */
static const char *const JOB_TEMPLATE_ATTRIBUTES[] = {
    "copies",
    "finishings",
    "finishings-col",
    "finishing-template",
    "job-account-id",
    "job-accounting-user-id",
    "job-hold-until",
    "job-priority",
    "job-sheets",
    "job-sheets-col",
    "media",
    "media-col",
    "multiple-document-handling",
    "number-up",
    "orientation-requested",
    "output-bin",
    "page-ranges",
    "pages-ranges",
    "print-color-mode",
    "print-content-optimize",
    "print-quality",
    "print-rendering-intent",
    "printer-resolution",
    "sheet-collate",
    "sides",
};

static const char *const PRINTER_JOB_TEMPLATE_SUFFIXES[] = {
    "-default",
    "-supported",
    "-ready",
};

/* Expensive capability databases are returned only when explicitly named. */
static const char *const EXPLICIT_ONLY_PRINTER_ATTRIBUTES[] = {
    "media-col-database",
};

static bool attribute_name_equals(const char *name, const char *expected)
{
    return equal_span(name, strlen(name), expected, strlen(expected));
}

static bool is_job_template_attribute(uint8_t delimiter_tag, const char *name)
{
    for (size_t i = 0;
         i < sizeof(JOB_TEMPLATE_ATTRIBUTES) / sizeof(JOB_TEMPLATE_ATTRIBUTES[0]);
         ++i) {
        const char *base = JOB_TEMPLATE_ATTRIBUTES[i];
        size_t base_length = strlen(base);
        if (delimiter_tag == IPP_TAG_JOB_ATTRIBUTES &&
            attribute_name_equals(name, base)) {
            return true;
        }
        if (delimiter_tag != IPP_TAG_PRINTER_ATTRIBUTES ||
            strlen(name) <= base_length ||
            !equal_span(name, base_length, base, base_length)) {
            continue;
        }
        const char *suffix = name + base_length;
        for (size_t suffix_index = 0;
             suffix_index < sizeof(PRINTER_JOB_TEMPLATE_SUFFIXES) /
                                sizeof(PRINTER_JOB_TEMPLATE_SUFFIXES[0]);
             ++suffix_index) {
            if (attribute_name_equals(
                    suffix, PRINTER_JOB_TEMPLATE_SUFFIXES[suffix_index])) {
                return true;
            }
        }
    }
    return false;
}

static bool is_explicit_only_printer_attribute(const char *name)
{
    for (size_t i = 0;
         i < sizeof(EXPLICIT_ONLY_PRINTER_ATTRIBUTES) /
                 sizeof(EXPLICIT_ONLY_PRINTER_ATTRIBUTES[0]);
         ++i) {
        if (attribute_name_equals(name, EXPLICIT_ONLY_PRINTER_ATTRIBUTES[i])) {
            return true;
        }
    }
    return false;
}

static requested_group_mask_t classify_response_attribute(uint8_t delimiter_tag,
                                                           const char *name)
{
    if (delimiter_tag == IPP_TAG_PRINTER_ATTRIBUTES) {
        if (is_explicit_only_printer_attribute(name)) {
            return 0;
        }
        return is_job_template_attribute(delimiter_tag, name) ?
                   REQUESTED_GROUP_JOB_TEMPLATE :
                   REQUESTED_GROUP_PRINTER_DESCRIPTION;
    }
    if (delimiter_tag == IPP_TAG_JOB_ATTRIBUTES) {
        return is_job_template_attribute(delimiter_tag, name) ?
                   REQUESTED_GROUP_JOB_TEMPLATE :
                   REQUESTED_GROUP_JOB_DESCRIPTION;
    }
    return 0;
}

static requested_group_mask_t requested_group_mask(
    const char *requested_attributes)
{
    requested_group_mask_t groups = 0;
    for (size_t i = 0;
         i < sizeof(REQUESTED_GROUP_SELECTORS) /
                 sizeof(REQUESTED_GROUP_SELECTORS[0]);
         ++i) {
        const requested_group_selector_t *selector =
            &REQUESTED_GROUP_SELECTORS[i];
        if (csv_contains(requested_attributes, selector->selector,
                         strlen(selector->selector))) {
            groups |= selector->group;
        }
    }
    return groups;
}

static void copy_value(char *destination, size_t destination_size,
                       const uint8_t *value, size_t value_length)
{
    if (!destination_size) {
        return;
    }
    if (value_length >= destination_size) {
        value_length = destination_size - 1;
    }
    memcpy(destination, value, value_length);
    destination[value_length] = '\0';
}

static bool value_contains(const uint8_t *value, size_t value_length,
                           const char *needle)
{
    size_t needle_length = strlen(needle);
    if (needle_length > value_length) {
        return false;
    }
    for (size_t i = 0; i + needle_length <= value_length; ++i) {
        if (equal_span((const char *)value + i, needle_length, needle, needle_length)) {
            return true;
        }
    }
    return false;
}

static bool web_url_value(const uint8_t *value, size_t value_length)
{
    bool scheme = (value_length > 7 && memcmp(value, "http://", 7) == 0) ||
                  (value_length > 8 && memcmp(value, "https://", 8) == 0);
    if (!scheme) {
        return false;
    }
    for (size_t i = 0; i < value_length; ++i) {
        if (value[i] < 0x20 || value[i] == 0x7f) {
            return false;
        }
    }
    return true;
}

static char *rewrite_uri(const char *name, const uint8_t *value, size_t value_length,
                         const char *printer_uri, const char *authority)
{
    if (strcmp(name, "printer-more-info") == 0) {
        static const char landing_page[] = "http://espresso.local/";
        char *rewritten = malloc(sizeof(landing_page));
        if (rewritten) {
            memcpy(rewritten, landing_page, sizeof(landing_page));
        }
        return rewritten;
    }
    if (strcmp(name, "printer-uri") == 0 ||
        strcmp(name, "printer-uri-supported") == 0 ||
        strcmp(name, "job-printer-uri") == 0) {
        size_t length = strlen(printer_uri);
        char *rewritten = malloc(length + 1);
        if (rewritten) {
            memcpy(rewritten, printer_uri, length + 1);
        }
        return rewritten;
    }

    if (strcmp(name, "job-uri") != 0) {
        return NULL;
    }
    const uint8_t *path = NULL;
    for (size_t i = 0; i + 2 < value_length; ++i) {
        if (value[i] == ':' && value[i + 1] == '/' && value[i + 2] == '/') {
            for (size_t j = i + 3; j < value_length; ++j) {
                if (value[j] == '/') {
                    path = value + j;
                    break;
                }
            }
            break;
        }
    }
    size_t authority_length = strlen(authority);
    size_t path_length = path ? (size_t)(value + value_length - path) : 0;
    char *rewritten = malloc(authority_length + path_length + 1);
    if (!rewritten) {
        return NULL;
    }
    memcpy(rewritten, authority, authority_length);
    if (path_length) {
        memcpy(rewritten + authority_length, path, path_length);
    }
    rewritten[authority_length + path_length] = '\0';
    return rewritten;
}

static uint64_t attribute_presence(const char *name)
{
    static const struct {
        const char *name;
        uint64_t flag;
    } attributes[] = {
        {"ipp-versions-supported", PRESENT_VERSIONS},
        {"operations-supported", PRESENT_OPERATIONS},
        {"printer-uri-supported", PRESENT_PRINTER_URI},
        {"uri-authentication-supported", PRESENT_URI_AUTH},
        {"uri-security-supported", PRESENT_URI_SECURITY},
        {"printer-uuid", PRESENT_UUID},
        {"printer-name", PRESENT_NAME},
        {"printer-info", PRESENT_INFO},
        {"printer-make-and-model", PRESENT_MAKE_MODEL},
        {"printer-location", PRESENT_LOCATION},
        {"document-format-supported", PRESENT_FORMATS},
        {"document-format-default", PRESENT_FORMAT_DEFAULT},
        {"urf-supported", PRESENT_URF},
        {"color-supported", PRESENT_COLOR},
        {"sides-supported", PRESENT_SIDES},
        {"copies-supported", PRESENT_COPIES},
        {"media-supported", PRESENT_MEDIA},
        {"compression-supported", PRESENT_COMPRESSION},
        {"multiple-document-jobs-supported", PRESENT_MULTIPLE_DOCUMENTS},
        {"media-col-database", PRESENT_MEDIA_COL_DATABASE},
        {"media-col-default", PRESENT_MEDIA_COL_DEFAULT},
        {"print-color-mode-supported", PRESENT_PRINT_COLOR_MODE},
        {"print-color-mode-default", PRESENT_PRINT_COLOR_DEFAULT},
        {"printer-resolution-supported", PRESENT_RESOLUTION},
        {"printer-resolution-default", PRESENT_RESOLUTION_DEFAULT},
        {"job-creation-attributes-supported", PRESENT_JOB_CREATION},
        {"printer-state", PRESENT_PRINTER_STATE},
        {"printer-state-reasons", PRESENT_STATE_REASONS},
        {"printer-is-accepting-jobs", PRESENT_ACCEPTING_JOBS},
        {"charset-configured", PRESENT_CHARSET_CONFIGURED},
        {"charset-supported", PRESENT_CHARSET_SUPPORTED},
        {"natural-language-configured", PRESENT_LANGUAGE_CONFIGURED},
        {"generated-natural-language-supported", PRESENT_LANGUAGE_SUPPORTED},
        {"printer-more-info", PRESENT_MORE_INFO},
        {"printer-up-time", PRESENT_UP_TIME},
        {"pdl-override-supported", PRESENT_PDL_OVERRIDE},
        {"queued-job-count", PRESENT_QUEUED_JOB_COUNT},
        {"copies-default", PRESENT_COPIES_DEFAULT},
        {"media-default", PRESENT_MEDIA_DEFAULT},
        {"finishings-default", PRESENT_FINISHINGS_DEFAULT},
        {"finishings-supported", PRESENT_FINISHINGS_SUPPORTED},
        {"orientation-requested-default", PRESENT_ORIENTATION_DEFAULT},
        {"orientation-requested-supported", PRESENT_ORIENTATION_SUPPORTED},
        {"output-bin-default", PRESENT_OUTPUT_BIN_DEFAULT},
        {"output-bin-supported", PRESENT_OUTPUT_BIN_SUPPORTED},
        {"print-quality-default", PRESENT_PRINT_QUALITY_DEFAULT},
        {"print-quality-supported", PRESENT_PRINT_QUALITY_SUPPORTED},
        {"sides-default", PRESENT_SIDES_DEFAULT},
        {"pages-per-minute", PRESENT_PAGES_PER_MINUTE},
        {"pages-per-minute-color", PRESENT_PAGES_PER_MINUTE_COLOR},
        {"media-col-supported", PRESENT_MEDIA_COL_SUPPORTED},
        {"media-col-ready", PRESENT_MEDIA_COL_READY},
        {"media-source-supported", PRESENT_MEDIA_SOURCE_SUPPORTED},
        {"media-type-supported", PRESENT_MEDIA_TYPE_SUPPORTED},
        {"printer-input-tray", PRESENT_PRINTER_INPUT_TRAY},
        {"printer-mandatory-job-attributes", PRESENT_MANDATORY_JOB_ATTRIBUTES},
        {"print-scaling-supported", PRESENT_PRINT_SCALING_SUPPORTED},
        {"print-scaling-default", PRESENT_PRINT_SCALING_DEFAULT},
        {"landscape-orientation-requested-preferred", PRESENT_LANDSCAPE_PREFERRED},
        {"document-password-supported", PRESENT_DOCUMENT_PASSWORD_SUPPORTED},
        {"reference-uri-schemes-supported", PRESENT_REFERENCE_URI_SCHEMES},
        {"media-key-supported", PRESENT_MEDIA_KEY_SUPPORTED},
        {"media-size-supported", PRESENT_MEDIA_SIZE_SUPPORTED},
    };
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); ++i) {
        if (strcmp(name, attributes[i].name) == 0) {
            return attributes[i].flag;
        }
    }
    return 0;
}

static bool append_csv_attributes(byte_buffer_t *buffer, uint8_t tag,
                                  const char *name, const char *csv)
{
    bool first = true;
    const char *cursor = csv;
    while (cursor && *cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        const char *end = cursor;
        while (end > start && isspace((unsigned char)end[-1])) {
            --end;
        }
        if (end > start && !append_attribute(buffer, tag, first ? name : NULL,
                                             start, (size_t)(end - start))) {
            return false;
        }
        if (end > start) {
            first = false;
        }
    }
    return true;
}

static bool append_enum(byte_buffer_t *buffer, const char *name, uint32_t value,
                        bool first)
{
    uint8_t encoded[] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16),
                         (uint8_t)(value >> 8), (uint8_t)value};
    return append_attribute(buffer, IPP_TAG_ENUM, first ? name : NULL,
                            encoded, sizeof(encoded));
}

static bool append_i32(byte_buffer_t *buffer, uint8_t tag, const char *name,
                       uint32_t value)
{
    uint8_t encoded[] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16),
                         (uint8_t)(value >> 8), (uint8_t)value};
    return append_attribute(buffer, tag, name, encoded, sizeof(encoded));
}

static bool media_dimensions(const char *name, size_t name_length,
                             uint32_t *width, uint32_t *height)
{
    static const struct {
        const char *name;
        uint32_t width;
        uint32_t height;
    } aliases[] = {
        {"A3", 29700, 42000}, {"iso-a3", 29700, 42000},
        {"A4", 21000, 29700}, {"iso-a4", 21000, 29700},
        {"A5", 14800, 21000}, {"iso-a5", 14800, 21000},
        {"Letter", 21590, 27940}, {"na-letter", 21590, 27940},
        {"Legal", 21590, 35560}, {"na-legal", 21590, 35560},
        {"Executive", 18415, 26670}, {"Tabloid", 27940, 43180},
        {"4x6", 10160, 15240}, {"5x7", 12700, 17780},
        {"Env10", 10478, 24130}, {"EnvDL", 11000, 22000},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (equal_span(name, name_length, aliases[i].name,
                       strlen(aliases[i].name))) {
            *width = aliases[i].width;
            *height = aliases[i].height;
            return true;
        }
    }

    char media[ESPRESSO_MEDIA_NAME_MAX];
    if (name_length == 0 || name_length >= sizeof(media)) {
        return false;
    }
    memcpy(media, name, name_length);
    media[name_length] = '\0';
    char *dimensions = strrchr(media, '_');
    dimensions = dimensions ? dimensions + 1 : media;
    size_t dimensions_length = strlen(dimensions);
    double multiplier;
    if (dimensions_length > 2 &&
        equal_span(dimensions + dimensions_length - 2, 2, "mm", 2)) {
        multiplier = 100.0;
    } else if (dimensions_length > 2 &&
               equal_span(dimensions + dimensions_length - 2, 2, "in", 2)) {
        multiplier = 2540.0;
    } else {
        return false;
    }
    dimensions[dimensions_length - 2] = '\0';
    char *separator = strchr(dimensions, 'x');
    if (!separator) {
        return false;
    }
    *separator++ = '\0';
    char *width_end = NULL;
    char *height_end = NULL;
    double parsed_width = strtod(dimensions, &width_end);
    double parsed_height = strtod(separator, &height_end);
    if (!width_end || *width_end || !height_end || *height_end ||
        parsed_width <= 0.0 || parsed_height <= 0.0) {
        return false;
    }
    double width_hundredths = parsed_width * multiplier;
    double height_hundredths = parsed_height * multiplier;
    if (width_hundredths > UINT32_MAX || height_hundredths > UINT32_MAX) {
        return false;
    }
    *width = (uint32_t)(width_hundredths + 0.5);
    *height = (uint32_t)(height_hundredths + 0.5);
    return true;
}

static bool append_media_collection(byte_buffer_t *buffer, const char *attribute_name,
                                    const char *media_name, size_t media_name_length,
                                    uint32_t width, uint32_t height)
{
    if (!append_attribute(buffer, IPP_TAG_BEGIN_COLLECTION, attribute_name, NULL, 0) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-key") ||
        !append_attribute(buffer, IPP_TAG_KEYWORD, NULL, media_name,
                          media_name_length) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-size") ||
        !append_attribute(buffer, IPP_TAG_BEGIN_COLLECTION, NULL, NULL, 0) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "x-dimension") ||
        !append_i32(buffer, IPP_TAG_INTEGER, NULL, width) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "y-dimension") ||
        !append_i32(buffer, IPP_TAG_INTEGER, NULL, height) ||
        !append_attribute(buffer, IPP_TAG_END_COLLECTION, NULL, NULL, 0) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-size-name") ||
        !append_attribute(buffer, IPP_TAG_KEYWORD, NULL, media_name,
                          media_name_length) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-bottom-margin") ||
        !append_i32(buffer, IPP_TAG_INTEGER, NULL, 0) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-left-margin") ||
        !append_i32(buffer, IPP_TAG_INTEGER, NULL, 0) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-right-margin") ||
        !append_i32(buffer, IPP_TAG_INTEGER, NULL, 0) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-top-margin") ||
        !append_i32(buffer, IPP_TAG_INTEGER, NULL, 0) ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-source") ||
        !append_string(buffer, IPP_TAG_KEYWORD, NULL, "main") ||
        !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL, "media-type") ||
        !append_string(buffer, IPP_TAG_KEYWORD, NULL, "stationery") ||
        !append_attribute(buffer, IPP_TAG_END_COLLECTION, NULL, NULL, 0)) {
        return false;
    }
    return true;
}

static bool append_media_database(byte_buffer_t *buffer, const char *media_csv)
{
    bool first = true;
    const char *cursor = media_csv;
    while (cursor && *cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        const char *end = cursor;
        while (end > start && isspace((unsigned char)end[-1])) {
            --end;
        }
        uint32_t width = 0;
        uint32_t height = 0;
        if (end > start && media_dimensions(start, (size_t)(end - start),
                                            &width, &height)) {
            if (!append_media_collection(buffer,
                                         first ? "media-col-database" : NULL,
                                         start, (size_t)(end - start), width, height)) {
                return false;
            }
            first = false;
        }
    }
    return true;
}

static bool append_media_sizes(byte_buffer_t *buffer, const char *media_csv)
{
    bool first = true;
    const char *cursor = media_csv;
    while (cursor && *cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        const char *end = cursor;
        while (end > start && isspace((unsigned char)end[-1])) {
            --end;
        }
        uint32_t width = 0;
        uint32_t height = 0;
        if (end > start && media_dimensions(start, (size_t)(end - start),
                                            &width, &height)) {
            if (!append_attribute(buffer, IPP_TAG_BEGIN_COLLECTION,
                                  first ? "media-size-supported" : NULL,
                                  NULL, 0) ||
                !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL,
                               "x-dimension") ||
                !append_i32(buffer, IPP_TAG_INTEGER, NULL, width) ||
                !append_string(buffer, IPP_TAG_MEMBER_NAME, NULL,
                               "y-dimension") ||
                !append_i32(buffer, IPP_TAG_INTEGER, NULL, height) ||
                !append_attribute(buffer, IPP_TAG_END_COLLECTION, NULL,
                                  NULL, 0)) {
                return false;
            }
            first = false;
        }
    }
    return true;
}

static bool append_resolution(byte_buffer_t *buffer, const char *name,
                              uint16_t dpi)
{
    uint8_t value[] = {0, 0, (uint8_t)(dpi >> 8), (uint8_t)dpi,
                       0, 0, (uint8_t)(dpi >> 8), (uint8_t)dpi, 3};
    return append_attribute(buffer, IPP_TAG_RESOLUTION, name, value, sizeof(value));
}

static bool append_synthesized_attributes(byte_buffer_t *buffer, uint64_t present,
                                          uint8_t current_group,
                                          const char *printer_uri,
                                          const char *local_uuid,
                                          const printer_target_t *target)
{
    if (current_group != IPP_TAG_PRINTER_ATTRIBUTES) {
        uint8_t group = IPP_TAG_PRINTER_ATTRIBUTES;
        if (!append(buffer, &group, 1)) {
            return false;
        }
    }

    if (!(present & PRESENT_VERSIONS) &&
        (!append_string(buffer, IPP_TAG_KEYWORD, "ipp-versions-supported", "1.1") ||
         !append_string(buffer, IPP_TAG_KEYWORD, NULL, "2.0"))) {
        return false;
    }
    uint64_t relay_operations = ipp_codec_relay_operations(
        target->operations_supported);
    if (!(present & PRESENT_OPERATIONS) && relay_operations) {
        bool first = true;
        for (uint32_t operation = 0; operation < 64; ++operation) {
            if ((relay_operations & (1ULL << operation)) != 0) {
                if (!append_enum(buffer, "operations-supported", operation, first)) {
                    return false;
                }
                first = false;
            }
        }
    }
    if (!(present & PRESENT_PRINTER_URI) &&
        !append_string(buffer, IPP_TAG_URI, "printer-uri-supported", printer_uri)) {
        return false;
    }
    if (!(present & PRESENT_URI_AUTH) &&
        !append_string(buffer, IPP_TAG_KEYWORD, "uri-authentication-supported", "none")) {
        return false;
    }
    if (!(present & PRESENT_URI_SECURITY) &&
        !append_string(buffer, IPP_TAG_KEYWORD, "uri-security-supported", "none")) {
        return false;
    }
    if (!(present & PRESENT_REFERENCE_URI_SCHEMES) &&
        !append_string(buffer, IPP_TAG_URI_SCHEME,
                       "reference-uri-schemes-supported", "ftp")) {
        return false;
    }
    if (!(present & PRESENT_UUID) && local_uuid && *local_uuid) {
        char urn[ESPRESSO_UUID_MAX + 10];
        size_t uuid_length = strlen(local_uuid);
        if (uuid_length > 9 && memcmp(local_uuid, "urn:uuid:", 9) == 0) {
            if (!append_string(buffer, IPP_TAG_URI, "printer-uuid", local_uuid)) {
                return false;
            }
        } else {
            memcpy(urn, "urn:uuid:", 9);
            if (uuid_length >= sizeof(urn) - 9) {
                uuid_length = sizeof(urn) - 10;
            }
            memcpy(urn + 9, local_uuid, uuid_length);
            urn[9 + uuid_length] = '\0';
            if (!append_string(buffer, IPP_TAG_URI, "printer-uuid", urn)) {
                return false;
            }
        }
    }
    const char *label = target->label[0] ? target->label : target->instance;
    if (!(present & PRESENT_NAME) &&
        !append_string(buffer, IPP_TAG_NAME, "printer-name", label)) {
        return false;
    }
    if (!(present & PRESENT_INFO) &&
        !append_string(buffer, IPP_TAG_TEXT, "printer-info", label)) {
        return false;
    }
    if (!(present & PRESENT_MAKE_MODEL) &&
        !append_string(buffer, IPP_TAG_TEXT, "printer-make-and-model", label)) {
        return false;
    }
    if (!(present & PRESENT_LOCATION) && target->location[0] &&
        !append_string(buffer, IPP_TAG_TEXT, "printer-location", target->location)) {
        return false;
    }
    if (!(present & PRESENT_LOCATION) && !target->location[0] &&
        !append_string(buffer, IPP_TAG_TEXT, "printer-location", "")) {
        return false;
    }
    if (!(present & PRESENT_CHARSET_CONFIGURED) &&
        !append_string(buffer, IPP_TAG_CHARSET, "charset-configured", "utf-8")) {
        return false;
    }
    if (!(present & PRESENT_CHARSET_SUPPORTED) &&
        !append_string(buffer, IPP_TAG_CHARSET, "charset-supported", "utf-8")) {
        return false;
    }
    if (!(present & PRESENT_LANGUAGE_CONFIGURED) &&
        !append_string(buffer, IPP_TAG_LANGUAGE,
                       "natural-language-configured", "en")) {
        return false;
    }
    if (!(present & PRESENT_LANGUAGE_SUPPORTED) &&
        !append_string(buffer, IPP_TAG_LANGUAGE,
                       "generated-natural-language-supported", "en")) {
        return false;
    }
    if (!(present & PRESENT_MORE_INFO)) {
        if (!append_string(buffer, IPP_TAG_URI, "printer-more-info",
                           "http://espresso.local/")) {
            return false;
        }
    }
    if (!(present & PRESENT_UP_TIME) &&
        !append_i32(buffer, IPP_TAG_INTEGER, "printer-up-time", 1)) {
        return false;
    }
    if (!(present & PRESENT_PDL_OVERRIDE) &&
        !append_string(buffer, IPP_TAG_KEYWORD,
                       "pdl-override-supported", "not-attempted")) {
        return false;
    }
    if (!(present & PRESENT_QUEUED_JOB_COUNT) &&
        !append_i32(buffer, IPP_TAG_INTEGER, "queued-job-count", 0)) {
        return false;
    }
    if (!(present & PRESENT_FORMATS) && target->pdl[0] &&
        !append_csv_attributes(buffer, IPP_TAG_MIMETYPE,
                               "document-format-supported", target->pdl)) {
        return false;
    }
    if (!(present & PRESENT_FORMAT_DEFAULT)) {
        const char *format_default =
            csv_contains(target->pdl, "application/pdf",
                         strlen("application/pdf")) ?
                "application/pdf" :
            csv_contains(target->pdl, "image/urf", strlen("image/urf")) ?
                "image/urf" : NULL;
        if (format_default &&
            !append_string(buffer, IPP_TAG_MIMETYPE,
                           "document-format-default", format_default)) {
            return false;
        }
    }
    /* IPP Everywhere 1.1 section 9.3 requires this Printer Description
     * attribute whenever application/pdf is advertised. Current iOS asks for
     * it in the full AirPrint capability probe and does not submit a job when
     * the otherwise-PDF-capable facade omits it. The relay passes the optional
     * document-password operation attribute through to the target. */
    if (!(present & PRESENT_DOCUMENT_PASSWORD_SUPPORTED) &&
        csv_contains(target->pdl, "application/pdf",
                     strlen("application/pdf")) &&
        !append_i32(buffer, IPP_TAG_INTEGER,
                    "document-password-supported", 1023)) {
        return false;
    }
    if (!(present & PRESENT_URF) && target->urf[0] &&
        !append_csv_attributes(buffer, IPP_TAG_KEYWORD, "urf-supported", target->urf)) {
        return false;
    }
    if (!(present & PRESENT_COLOR)) {
        uint8_t color = target->color ? 1 : 0;
        if (!append_attribute(buffer, IPP_TAG_BOOLEAN, "color-supported", &color, 1)) {
            return false;
        }
    }
    if (!(present & PRESENT_SIDES)) {
        if (!append_string(buffer, IPP_TAG_KEYWORD, "sides-supported", "one-sided") ||
            (target->duplex &&
             (!append_string(buffer, IPP_TAG_KEYWORD, NULL, "two-sided-long-edge") ||
              !append_string(buffer, IPP_TAG_KEYWORD, NULL, "two-sided-short-edge")))) {
            return false;
        }
    }
    if (!(present & PRESENT_COPIES) && target->copies_upper) {
        uint32_t upper = target->copies_upper;
        uint8_t range[] = {0, 0, 0, 1, (uint8_t)(upper >> 24),
                           (uint8_t)(upper >> 16), (uint8_t)(upper >> 8),
                           (uint8_t)upper};
        if (!append_attribute(buffer, IPP_TAG_RANGE, "copies-supported", range,
                              sizeof(range))) {
            return false;
        }
    }
    if (!(present & PRESENT_COPIES_DEFAULT) && target->copies_upper &&
        !append_i32(buffer, IPP_TAG_INTEGER, "copies-default", 1)) {
        return false;
    }
    if (!(present & PRESENT_MEDIA) && target->media[0] &&
        !append_csv_attributes(buffer, IPP_TAG_KEYWORD, "media-supported", target->media)) {
        return false;
    }
    if (!(present & PRESENT_MEDIA_KEY_SUPPORTED) && target->media[0] &&
        !append_csv_attributes(buffer, IPP_TAG_KEYWORD,
                               "media-key-supported", target->media)) {
        return false;
    }
    if (!(present & PRESENT_MEDIA_SIZE_SUPPORTED) && target->media[0] &&
        !append_media_sizes(buffer, target->media)) {
        return false;
    }
    if (!(present & PRESENT_MEDIA_DEFAULT) && target->media_default[0] &&
        !append_string(buffer, IPP_TAG_KEYWORD, "media-default",
                       target->media_default)) {
        return false;
    }
    if (!(present & PRESENT_MEDIA_COL_DATABASE) && target->media[0] &&
        !append_media_database(buffer, target->media)) {
        return false;
    }
    if (!(present & PRESENT_MEDIA_COL_DEFAULT) && target->media_default[0]) {
        uint32_t width = 0;
        uint32_t height = 0;
        size_t media_length = strlen(target->media_default);
        if (media_dimensions(target->media_default, media_length, &width, &height) &&
            !append_media_collection(buffer, "media-col-default",
                                     target->media_default, media_length,
                                     width, height)) {
            return false;
        }
    }
    if (!(present & PRESENT_MEDIA_COL_SUPPORTED) && target->media[0]) {
        static const char *const members[] = {
            "media-bottom-margin", "media-key", "media-left-margin",
            "media-right-margin", "media-size", "media-size-name",
            "media-source", "media-top-margin", "media-type",
        };
        for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); ++i) {
            if (!append_string(buffer, IPP_TAG_KEYWORD,
                               i == 0 ? "media-col-supported" : NULL,
                               members[i])) {
                return false;
            }
        }
    }
    if (!(present & PRESENT_MEDIA_COL_READY) && target->media_default[0]) {
        uint32_t width = 0;
        uint32_t height = 0;
        size_t media_length = strlen(target->media_default);
        if (media_dimensions(target->media_default, media_length, &width, &height) &&
            !append_media_collection(buffer, "media-col-ready",
                                     target->media_default, media_length,
                                     width, height)) {
            return false;
        }
    }
    /* A single automatic source is the truthful facade for a legacy target
     * whose IPP profile does not expose individual trays. Apple clients use
     * these attributes for readiness checks before submitting Print-Job. */
    if (!(present & PRESENT_MEDIA_SOURCE_SUPPORTED) &&
        !append_string(buffer, IPP_TAG_KEYWORD,
                       "media-source-supported", "auto")) {
        return false;
    }
    if (!(present & PRESENT_MEDIA_SOURCE_SUPPORTED) &&
        !append_string(buffer, IPP_TAG_KEYWORD, NULL, "main")) {
        return false;
    }
    if (!(present & PRESENT_MEDIA_TYPE_SUPPORTED) &&
        !append_string(buffer, IPP_TAG_KEYWORD,
                       "media-type-supported", "stationery")) {
        return false;
    }
    if (!(present & PRESENT_PRINTER_INPUT_TRAY) &&
        !append_string(buffer, IPP_TAG_OCTETSTRING, "printer-input-tray",
                       "type=sheetFeedAutoRemovableTray;mediafeed=0;"
                       "mediaxfeed=0;maxcapacity=-2;level=-2;status=0;"
                       "name=auto")) {
        return false;
    }
    if (!(present & PRESENT_PRINTER_INPUT_TRAY) &&
        !append_string(buffer, IPP_TAG_OCTETSTRING, NULL,
                       "type=sheetFeedAutoRemovableTray;mediafeed=0;"
                       "mediaxfeed=0;maxcapacity=250;level=100;status=0;"
                       "name=main")) {
        return false;
    }
    /* Do not invent printer-mandatory-job-attributes.  Absence means the
     * facade has no additional mandatory Job Template attributes, matching
     * the Apple/CUPS IPP Everywhere reference printer. */
    if (!(present & PRESENT_PRINT_COLOR_MODE)) {
        if (!append_string(buffer, IPP_TAG_KEYWORD, "print-color-mode-supported",
                           "monochrome") ||
            (target->color &&
             !append_string(buffer, IPP_TAG_KEYWORD, NULL, "color"))) {
            return false;
        }
    }
    if (!(present & PRESENT_PRINT_COLOR_DEFAULT) && target->color_mode_default[0] &&
        !append_string(buffer, IPP_TAG_KEYWORD, "print-color-mode-default",
                       target->color_mode_default)) {
        return false;
    }
    if (!(present & PRESENT_RESOLUTION) && target->resolution_low_dpi) {
        if (!append_resolution(buffer, "printer-resolution-supported",
                               target->resolution_low_dpi) ||
            (target->resolution_high_dpi > target->resolution_low_dpi &&
             !append_resolution(buffer, NULL, target->resolution_high_dpi))) {
            return false;
        }
    }
    if (!(present & PRESENT_RESOLUTION_DEFAULT) && target->resolution_low_dpi &&
        !append_resolution(buffer, "printer-resolution-default",
                           target->resolution_low_dpi)) {
        return false;
    }
    if (!(present & PRESENT_FINISHINGS_DEFAULT) &&
        !append_i32(buffer, IPP_TAG_ENUM, "finishings-default", 3)) {
        return false;
    }
    if (!(present & PRESENT_FINISHINGS_SUPPORTED) &&
        !append_i32(buffer, IPP_TAG_ENUM, "finishings-supported", 3)) {
        return false;
    }
    if (!(present & PRESENT_ORIENTATION_DEFAULT) &&
        !append_i32(buffer, IPP_TAG_ENUM, "orientation-requested-default", 3)) {
        return false;
    }
    if (!(present & PRESENT_ORIENTATION_SUPPORTED) &&
        !append_i32(buffer, IPP_TAG_ENUM, "orientation-requested-supported", 3)) {
        return false;
    }
    if (!(present & PRESENT_OUTPUT_BIN_DEFAULT) &&
        !append_string(buffer, IPP_TAG_KEYWORD, "output-bin-default",
                       "face-down")) {
        return false;
    }
    if (!(present & PRESENT_OUTPUT_BIN_SUPPORTED) &&
        !append_string(buffer, IPP_TAG_KEYWORD, "output-bin-supported",
                       "face-down")) {
        return false;
    }
    if (!(present & PRESENT_PRINT_QUALITY_DEFAULT) &&
        !append_i32(buffer, IPP_TAG_ENUM, "print-quality-default", 4)) {
        return false;
    }
    if (!(present & PRESENT_PRINT_QUALITY_SUPPORTED) &&
        !append_i32(buffer, IPP_TAG_ENUM, "print-quality-supported", 4)) {
        return false;
    }
    if (!(present & PRESENT_SIDES_DEFAULT) &&
        !append_string(buffer, IPP_TAG_KEYWORD, "sides-default", "one-sided")) {
        return false;
    }
    if (!(present & PRESENT_PAGES_PER_MINUTE) &&
        !append_i32(buffer, IPP_TAG_INTEGER, "pages-per-minute", 0)) {
        return false;
    }
    if (!(present & PRESENT_PAGES_PER_MINUTE_COLOR) && target->color &&
        !append_i32(buffer, IPP_TAG_INTEGER, "pages-per-minute-color", 0)) {
        return false;
    }
    if (!(present & PRESENT_COMPRESSION) &&
        !append_string(buffer, IPP_TAG_KEYWORD, "compression-supported", "none")) {
        return false;
    }
    if (!(present & PRESENT_MULTIPLE_DOCUMENTS)) {
        uint8_t multiple = 0;
        if (!append_attribute(buffer, IPP_TAG_BOOLEAN,
                              "multiple-document-jobs-supported", &multiple, 1)) {
            return false;
        }
    }
    if (!(present & PRESENT_PRINTER_STATE) && target->printer_state &&
        !append_i32(buffer, IPP_TAG_ENUM, "printer-state",
                    target->printer_state)) {
        return false;
    }
    if (!(present & PRESENT_STATE_REASONS) && target->printer_state) {
        const char *reasons = target->state_reasons[0] ?
                              target->state_reasons : "none";
        if (!append_csv_attributes(buffer, IPP_TAG_KEYWORD,
                                   "printer-state-reasons", reasons)) {
            return false;
        }
    }
    if (!(present & PRESENT_ACCEPTING_JOBS) && target->accepting_jobs_known) {
        uint8_t accepting = target->accepting_jobs ? 1 : 0;
        if (!append_attribute(buffer, IPP_TAG_BOOLEAN,
                              "printer-is-accepting-jobs", &accepting, 1)) {
            return false;
        }
    }
    if (!(present & PRESENT_JOB_CREATION)) {
        if (!append_string(buffer, IPP_TAG_KEYWORD,
                           "job-creation-attributes-supported", "job-name") ||
            !append_string(buffer, IPP_TAG_KEYWORD, NULL, "document-format") ||
            (target->copies_upper > 1 &&
             !append_string(buffer, IPP_TAG_KEYWORD, NULL, "copies")) ||
            (target->media[0] &&
             (!append_string(buffer, IPP_TAG_KEYWORD, NULL, "media") ||
              !append_string(buffer, IPP_TAG_KEYWORD, NULL, "media-col"))) ||
            !append_string(buffer, IPP_TAG_KEYWORD, NULL, "sides") ||
            !append_string(buffer, IPP_TAG_KEYWORD, NULL, "print-color-mode") ||
            (target->resolution_low_dpi &&
             !append_string(buffer, IPP_TAG_KEYWORD, NULL,
                            "printer-resolution"))) {
            return false;
        }
    }
    return true;
}

static ipp_codec_result_t transform_message(
    const uint8_t *input, size_t input_length, const char *printer_uri,
    const char *uri_authority, bool normalize, const char *local_uuid,
    const printer_target_t *target, uint8_t **output, size_t *output_length,
    size_t *attributes_length)
{
    if (!input || !printer_uri || !uri_authority || !output || !output_length ||
        !attributes_length || (normalize && (!target || !local_uuid))) {
        return IPP_CODEC_MALFORMED;
    }
    *output = NULL;
    *output_length = 0;
    *attributes_length = 0;
    if (input_length < IPP_HEADER_LENGTH) {
        return IPP_CODEC_INCOMPLETE;
    }

    byte_buffer_t result = {0};
    if (!append(&result, input, IPP_HEADER_LENGTH)) {
        return IPP_CODEC_NO_MEMORY;
    }
    size_t cursor = IPP_HEADER_LENGTH;
    char current_name[IPP_NAME_MAX + 1] = {0};
    uint8_t current_group = 0;
    uint64_t present = 0;
    bool ended = false;

    while (cursor < input_length) {
        uint8_t tag = input[cursor++];
        if (tag == IPP_TAG_END_ATTRIBUTES) {
            if (normalize && !append_synthesized_attributes(
                                 &result, present, current_group, printer_uri,
                                 local_uuid, target)) {
                free(result.data);
                return IPP_CODEC_NO_MEMORY;
            }
            if (!append(&result, &tag, 1)) {
                free(result.data);
                return IPP_CODEC_NO_MEMORY;
            }
            ended = true;
            /* Preserve the input offset: callers use this to detect document data. */
            *attributes_length = cursor;
            break;
        }
        if (tag <= 0x0f) {
            current_group = tag;
            current_name[0] = '\0';
            if (!append(&result, &tag, 1)) {
                free(result.data);
                return IPP_CODEC_NO_MEMORY;
            }
            continue;
        }
        if (cursor + 2 > input_length) {
            free(result.data);
            return IPP_CODEC_INCOMPLETE;
        }
        uint16_t name_length = read_u16(input + cursor);
        cursor += 2;
        if (name_length > IPP_NAME_MAX || cursor + name_length + 2 > input_length) {
            free(result.data);
            return name_length > IPP_NAME_MAX ? IPP_CODEC_MALFORMED :
                                                IPP_CODEC_INCOMPLETE;
        }
        if (name_length > 0) {
            memcpy(current_name, input + cursor, name_length);
            current_name[name_length] = '\0';
        } else if (current_name[0] == '\0') {
            free(result.data);
            return IPP_CODEC_MALFORMED;
        }
        const uint8_t *name = input + cursor;
        cursor += name_length;
        uint16_t value_length = read_u16(input + cursor);
        cursor += 2;
        if (cursor + value_length > input_length) {
            free(result.data);
            return IPP_CODEC_INCOMPLETE;
        }
        const uint8_t *value = input + cursor;
        cursor += value_length;

        bool replace_profile_value = normalize &&
            (strcmp(current_name, "ipp-versions-supported") == 0 ||
             strcmp(current_name, "operations-supported") == 0 ||
             strcmp(current_name, "printer-uuid") == 0 ||
             strcmp(current_name, "uri-authentication-supported") == 0 ||
             strcmp(current_name, "uri-security-supported") == 0 ||
             strcmp(current_name, "document-format-supported") == 0 ||
             strcmp(current_name, "document-format-default") == 0);
        if (replace_profile_value) {
            continue;
        }
        present |= attribute_presence(current_name);

        const void *new_value = value;
        size_t new_value_length = value_length;
        char *allocated_value = NULL;
        if (tag == IPP_TAG_URI &&
            (strcmp(current_name, "printer-uri") == 0 ||
             strcmp(current_name, "printer-uri-supported") == 0 ||
             strcmp(current_name, "job-printer-uri") == 0 ||
             strcmp(current_name, "printer-more-info") == 0 ||
             strcmp(current_name, "job-uri") == 0)) {
            allocated_value = rewrite_uri(current_name, value, value_length,
                                          printer_uri, uri_authority);
            if (!allocated_value) {
                free(result.data);
                return IPP_CODEC_NO_MEMORY;
            }
            new_value = allocated_value;
            new_value_length = strlen(allocated_value);
        }

        bool appended = append(&result, &tag, 1) &&
                        append_u16(&result, name_length) &&
                        append(&result, name, name_length) &&
                        append_u16(&result, new_value_length) &&
                        append(&result, new_value, new_value_length);
        free(allocated_value);
        if (!appended) {
            free(result.data);
            return IPP_CODEC_NO_MEMORY;
        }
    }

    if (!ended) {
        free(result.data);
        return IPP_CODEC_INCOMPLETE;
    }
    if (cursor < input_length &&
        !append(&result, input + cursor, input_length - cursor)) {
        free(result.data);
        return IPP_CODEC_NO_MEMORY;
    }
    *output = result.data;
    *output_length = result.length;
    return IPP_CODEC_OK;
}

ipp_codec_result_t ipp_codec_rewrite(
    const uint8_t *input, size_t input_length, const char *printer_uri,
    const char *uri_authority, uint8_t **output, size_t *output_length,
    size_t *attributes_length)
{
    return transform_message(input, input_length, printer_uri, uri_authority,
                             false, NULL, NULL, output, output_length,
                             attributes_length);
}

typedef enum {
    JOB_ATTRIBUTE_KEEP,
    JOB_ATTRIBUTE_DROP,
    JOB_ATTRIBUTE_UNSUPPORTED,
} job_attribute_action_t;

static bool encoded_u32_equals(const uint8_t *value, size_t value_length,
                               uint32_t expected)
{
    return value_length == 4 && read_u32(value) == expected;
}

static job_attribute_action_t facade_job_attribute_action(
    const printer_target_t *target, const char *name, uint8_t tag,
    const uint8_t *value, size_t value_length, const char **replacement_name)
{
    *replacement_name = NULL;
    if (strcmp(name, "print-color-mode") == 0 && target->legacy_output_mode) {
        *replacement_name = "output-mode";
        return JOB_ATTRIBUTE_KEEP;
    }
    struct neutral_default {
        const char *name;
        uint32_t capability;
        uint8_t tag;
        uint32_t integer_value;
        const char *string_value;
    };
    static const struct neutral_default defaults[] = {
        {"copies", ESPRESSO_JOB_CAP_COPIES, IPP_TAG_INTEGER, 1, NULL},
        {"finishings", ESPRESSO_JOB_CAP_FINISHINGS, IPP_TAG_ENUM, 3, NULL},
        {"orientation-requested", ESPRESSO_JOB_CAP_ORIENTATION,
         IPP_TAG_ENUM, 3, NULL},
        {"output-bin", ESPRESSO_JOB_CAP_OUTPUT_BIN, IPP_TAG_KEYWORD, 0,
         "face-down"},
        {"print-quality", ESPRESSO_JOB_CAP_PRINT_QUALITY,
         IPP_TAG_ENUM, 4, NULL},
        {"sides", ESPRESSO_JOB_CAP_SIDES, IPP_TAG_KEYWORD, 0,
         "one-sided"},
    };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        const struct neutral_default *entry = defaults + i;
        if (strcmp(name, entry->name) != 0 ||
            (target->job_template_capabilities & entry->capability)) {
            continue;
        }
        bool matches;
        if (entry->string_value) {
            matches = (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME) &&
                      equal_span((const char *)value, value_length,
                                 entry->string_value,
                                 strlen(entry->string_value));
        } else {
            matches = tag == entry->tag &&
                      encoded_u32_equals(value, value_length,
                                         entry->integer_value);
        }
        return matches ? JOB_ATTRIBUTE_DROP : JOB_ATTRIBUTE_UNSUPPORTED;
    }
    return JOB_ATTRIBUTE_KEEP;
}

static ipp_codec_result_t normalize_facade_job_defaults(
    const uint8_t *input, size_t input_length, const printer_target_t *target,
    uint8_t **output, size_t *output_length, size_t *attributes_length,
    char *rejected_attribute, size_t rejected_attribute_size)
{
    if (!input || !target || !output || !output_length || !attributes_length) {
        return IPP_CODEC_MALFORMED;
    }
    *output = NULL;
    *output_length = 0;
    *attributes_length = 0;
    if (rejected_attribute_size) {
        rejected_attribute[0] = '\0';
    }
    byte_buffer_t result = {0};
    if (input_length < IPP_HEADER_LENGTH) {
        return IPP_CODEC_INCOMPLETE;
    }
    if (!append(&result, input, IPP_HEADER_LENGTH)) {
        return IPP_CODEC_NO_MEMORY;
    }
    size_t cursor = IPP_HEADER_LENGTH;
    uint8_t current_group = 0;
    bool current_group_emitted = true;
    char current_name[IPP_NAME_MAX + 1] = {0};
    bool ended = false;
    while (cursor < input_length) {
        uint8_t tag = input[cursor++];
        if (tag == IPP_TAG_END_ATTRIBUTES) {
            if (!append(&result, &tag, 1)) {
                free(result.data);
                return IPP_CODEC_NO_MEMORY;
            }
            *attributes_length = result.length;
            ended = true;
            break;
        }
        if (tag <= 0x0f) {
            current_group = tag;
            current_group_emitted = false;
            current_name[0] = '\0';
            continue;
        }
        if (cursor + 2 > input_length) {
            free(result.data);
            return IPP_CODEC_INCOMPLETE;
        }
        uint16_t name_length = read_u16(input + cursor);
        cursor += 2;
        if (name_length > IPP_NAME_MAX || cursor + name_length + 2 > input_length) {
            free(result.data);
            return name_length > IPP_NAME_MAX ? IPP_CODEC_MALFORMED :
                                                IPP_CODEC_INCOMPLETE;
        }
        const uint8_t *name_bytes = input + cursor;
        if (name_length) {
            memcpy(current_name, name_bytes, name_length);
            current_name[name_length] = '\0';
        } else if (!current_name[0]) {
            free(result.data);
            return IPP_CODEC_MALFORMED;
        }
        cursor += name_length;
        uint16_t value_length = read_u16(input + cursor);
        cursor += 2;
        if (cursor + value_length > input_length) {
            free(result.data);
            return IPP_CODEC_INCOMPLETE;
        }
        const uint8_t *value = input + cursor;
        cursor += value_length;

        const char *replacement_name = NULL;
        job_attribute_action_t action = JOB_ATTRIBUTE_KEEP;
        if (current_group == IPP_TAG_JOB_ATTRIBUTES) {
            action = facade_job_attribute_action(
                target, current_name, tag, value, value_length,
                &replacement_name);
        }
        if (action == JOB_ATTRIBUTE_UNSUPPORTED) {
            if (rejected_attribute_size) {
                snprintf(rejected_attribute, rejected_attribute_size, "%s",
                         current_name);
            }
            free(result.data);
            return IPP_CODEC_UNSUPPORTED;
        }
        if (action == JOB_ATTRIBUTE_DROP) {
            continue;
        }
        if (!current_group_emitted && !append(&result, &current_group, 1)) {
            free(result.data);
            return IPP_CODEC_NO_MEMORY;
        }
        current_group_emitted = true;
        const char *output_name = name_length && replacement_name ?
                                      replacement_name : NULL;
        size_t output_name_length = output_name ? strlen(output_name) :
                                                  name_length;
        const void *output_name_bytes = output_name ?
                                            (const void *)output_name :
                                            (const void *)name_bytes;
        if (!append(&result, &tag, 1) ||
            !append_u16(&result, output_name_length) ||
            !append(&result, output_name_bytes, output_name_length) ||
            !append_u16(&result, value_length) ||
            !append(&result, value, value_length)) {
            free(result.data);
            return IPP_CODEC_NO_MEMORY;
        }
    }
    if (!ended) {
        free(result.data);
        return IPP_CODEC_INCOMPLETE;
    }
    if (cursor < input_length &&
        !append(&result, input + cursor, input_length - cursor)) {
        free(result.data);
        return IPP_CODEC_NO_MEMORY;
    }
    *output = result.data;
    *output_length = result.length;
    return IPP_CODEC_OK;
}

ipp_codec_result_t ipp_codec_rewrite_request(
    const uint8_t *input, size_t input_length, const char *printer_uri,
    const char *uri_authority, const printer_target_t *target,
    uint8_t **output, size_t *output_length, size_t *attributes_length)
{
    return ipp_codec_rewrite_request_diagnostic(
        input, input_length, printer_uri, uri_authority, target, output,
        output_length, attributes_length, NULL, 0);
}

ipp_codec_result_t ipp_codec_rewrite_request_diagnostic(
    const uint8_t *input, size_t input_length, const char *printer_uri,
    const char *uri_authority, const printer_target_t *target,
    uint8_t **output, size_t *output_length, size_t *attributes_length,
    char *rejected_attribute, size_t rejected_attribute_size)
{
    if (rejected_attribute_size && !rejected_attribute) {
        return IPP_CODEC_MALFORMED;
    }
    if (rejected_attribute_size) {
        rejected_attribute[0] = '\0';
    }
    uint8_t *rewritten = NULL;
    size_t rewritten_length = 0;
    size_t rewritten_attributes = 0;
    ipp_codec_result_t result = ipp_codec_rewrite(
        input, input_length, printer_uri, uri_authority, &rewritten,
        &rewritten_length, &rewritten_attributes);
    if (result != IPP_CODEC_OK) {
        return result;
    }
    result = normalize_facade_job_defaults(
        rewritten, rewritten_length, target, output, output_length,
        attributes_length, rejected_attribute, rejected_attribute_size);
    free(rewritten);
    return result;
}

ipp_codec_result_t ipp_codec_normalize_printer_response(
    const uint8_t *input, size_t input_length, const char *printer_uri,
    const char *uri_authority, const char *local_uuid,
    const printer_target_t *target, uint8_t **output, size_t *output_length,
    size_t *attributes_length)
{
    if (ipp_codec_message_code(input, input_length) >= IPP_STATUS_ERROR_BAD_REQUEST) {
        return ipp_codec_rewrite(input, input_length, printer_uri, uri_authority,
                                 output, output_length, attributes_length);
    }
    return transform_message(input, input_length, printer_uri, uri_authority,
                             true, local_uuid, target, output, output_length,
                             attributes_length);
}

ipp_codec_result_t ipp_codec_build_get_printer_attributes(
    uint8_t major, uint8_t minor, uint32_t request_id, const char *printer_uri,
    bool include_media_col_database, uint8_t **output, size_t *output_length)
{
    return ipp_codec_build_get_printer_attributes_for_format(
        major, minor, request_id, printer_uri, include_media_col_database,
        NULL, output, output_length);
}

ipp_codec_result_t ipp_codec_build_get_printer_attributes_for_format(
    uint8_t major, uint8_t minor, uint32_t request_id, const char *printer_uri,
    bool include_media_col_database, const char *document_format,
    uint8_t **output, size_t *output_length)
{
    if (!major || !printer_uri || !output || !output_length) {
        return IPP_CODEC_MALFORMED;
    }
    *output = NULL;
    *output_length = 0;
    uint8_t header[] = {
        major, minor,
        (uint8_t)(IPP_OPERATION_GET_PRINTER_ATTRIBUTES >> 8),
        (uint8_t)IPP_OPERATION_GET_PRINTER_ATTRIBUTES,
        (uint8_t)(request_id >> 24), (uint8_t)(request_id >> 16),
        (uint8_t)(request_id >> 8), (uint8_t)request_id,
        IPP_TAG_OPERATION_ATTRIBUTES,
    };
    byte_buffer_t request = {0};
    bool valid = append(&request, header, sizeof(header)) &&
                 append_string(&request, IPP_TAG_CHARSET, "attributes-charset", "utf-8") &&
                 append_string(&request, IPP_TAG_LANGUAGE,
                               "attributes-natural-language", "en") &&
                 append_string(&request, IPP_TAG_URI, "printer-uri", printer_uri) &&
                 append_string(&request, IPP_TAG_KEYWORD, "requested-attributes", "all");
    if (valid && document_format && *document_format) {
        valid = append_string(&request, IPP_TAG_MIMETYPE, "document-format",
                              document_format);
    }
    if (valid && include_media_col_database) {
        valid = append_string(&request, IPP_TAG_KEYWORD, NULL, "media-col-database");
    }
    uint8_t end = IPP_TAG_END_ATTRIBUTES;
    valid = valid && append(&request, &end, 1);
    if (!valid) {
        free(request.data);
        return IPP_CODEC_NO_MEMORY;
    }
    *output = request.data;
    *output_length = request.length;
    return IPP_CODEC_OK;
}

ipp_codec_result_t ipp_codec_inspect_request(
    const uint8_t *input, size_t input_length, ipp_request_info_t *info)
{
    if (!input || !info) {
        return IPP_CODEC_MALFORMED;
    }
    memset(info, 0, sizeof(*info));
    if (input_length < IPP_HEADER_LENGTH) {
        return IPP_CODEC_INCOMPLETE;
    }
    info->major = input[0];
    info->minor = input[1];
    info->operation_id = read_u16(input + 2);
    info->request_id = read_u32(input + 4);
    info->operation_attributes_valid = true;

    size_t cursor = IPP_HEADER_LENGTH;
    char current_name[IPP_NAME_MAX + 1] = {0};
    uint8_t current_group = 0;
    size_t operation_attribute_index = 0;
    bool saw_group = false;
    bool saw_operation_group = false;
    while (cursor < input_length) {
        uint8_t tag = input[cursor++];
        if (tag == IPP_TAG_END_ATTRIBUTES) {
            if (!saw_operation_group || operation_attribute_index < 2) {
                info->operation_attributes_valid = false;
            }
            info->attributes_length = cursor;
            info->has_document = cursor < input_length;
            return IPP_CODEC_OK;
        }
        if (tag <= 0x0f) {
            if (!saw_group && tag != IPP_TAG_OPERATION_ATTRIBUTES) {
                info->operation_attributes_valid = false;
            }
            if (tag == IPP_TAG_OPERATION_ATTRIBUTES) {
                if (saw_operation_group) {
                    info->operation_attributes_valid = false;
                }
                saw_operation_group = true;
            }
            saw_group = true;
            current_group = tag;
            current_name[0] = '\0';
            continue;
        }
        if (cursor + 2 > input_length) {
            return IPP_CODEC_INCOMPLETE;
        }
        uint16_t name_length = read_u16(input + cursor);
        cursor += 2;
        if (name_length > IPP_NAME_MAX || cursor + name_length + 2 > input_length) {
            return name_length > IPP_NAME_MAX ? IPP_CODEC_MALFORMED :
                                                IPP_CODEC_INCOMPLETE;
        }
        if (name_length) {
            memcpy(current_name, input + cursor, name_length);
            current_name[name_length] = '\0';
        } else if (!current_name[0]) {
            return IPP_CODEC_MALFORMED;
        }
        cursor += name_length;
        uint16_t value_length = read_u16(input + cursor);
        cursor += 2;
        if (cursor + value_length > input_length) {
            return IPP_CODEC_INCOMPLETE;
        }
        if (current_group == IPP_TAG_OPERATION_ATTRIBUTES) {
            bool is_charset = strcmp(current_name, "attributes-charset") == 0 &&
                              tag == IPP_TAG_CHARSET;
            bool is_language =
                strcmp(current_name, "attributes-natural-language") == 0 &&
                tag == IPP_TAG_LANGUAGE;
            if ((operation_attribute_index == 0 && !is_charset) ||
                (operation_attribute_index == 1 && !is_language) ||
                (operation_attribute_index > 1 && (is_charset || is_language))) {
                info->operation_attributes_valid = false;
            }
            ++operation_attribute_index;
        }
        if (current_group == IPP_TAG_OPERATION_ATTRIBUTES &&
            strcmp(current_name, "document-format") == 0 &&
            tag == IPP_TAG_MIMETYPE) {
            copy_value(info->document_format, sizeof(info->document_format),
                       input + cursor, value_length);
        } else if (current_group == IPP_TAG_OPERATION_ATTRIBUTES &&
                   strcmp(current_name, "attributes-charset") == 0 &&
                   tag == IPP_TAG_CHARSET) {
            info->has_attributes_charset = true;
            copy_value(info->attributes_charset,
                       sizeof(info->attributes_charset), input + cursor,
                       value_length);
        } else if (current_group == IPP_TAG_OPERATION_ATTRIBUTES &&
                   strcmp(current_name, "attributes-natural-language") == 0 &&
                   tag == IPP_TAG_LANGUAGE) {
            info->has_natural_language = true;
        } else if (current_group == IPP_TAG_OPERATION_ATTRIBUTES &&
                   (strcmp(current_name, "printer-uri") == 0 ||
                    strcmp(current_name, "job-uri") == 0) &&
                   tag == IPP_TAG_URI) {
            info->has_target_uri = true;
        } else if (current_group == IPP_TAG_OPERATION_ATTRIBUTES &&
                   strcmp(current_name, "requested-attributes") == 0 &&
                   tag == IPP_TAG_KEYWORD) {
            csv_add_split(info->requested_attributes,
                          sizeof(info->requested_attributes), input + cursor,
                          value_length);
        } else if (strcmp(current_name, "job-name") == 0 &&
                   (tag == IPP_TAG_NAME || tag == IPP_TAG_TEXT)) {
            copy_value(info->job_name, sizeof(info->job_name), input + cursor,
                       value_length);
        } else if (current_group == IPP_TAG_OPERATION_ATTRIBUTES &&
                   strcmp(current_name, "job-id") == 0 &&
                   tag == IPP_TAG_INTEGER && value_length == 4) {
            info->has_job_id = true;
            info->job_id = read_u32(input + cursor);
        } else if (current_group == IPP_TAG_OPERATION_ATTRIBUTES &&
                   strcmp(current_name, "last-document") == 0 &&
                   tag == IPP_TAG_BOOLEAN && value_length == 1) {
            info->has_last_document = true;
            info->last_document = input[cursor] != 0;
        }
        cursor += value_length;
    }
    return IPP_CODEC_INCOMPLETE;
}

bool ipp_codec_get_u32_attribute(const uint8_t *input, size_t input_length,
                                 const char *attribute_name, uint32_t *value)
{
    if (!input || !attribute_name || !value || input_length < IPP_HEADER_LENGTH) {
        return false;
    }
    size_t cursor = IPP_HEADER_LENGTH;
    char current_name[IPP_NAME_MAX + 1] = {0};
    while (cursor < input_length) {
        uint8_t tag = input[cursor++];
        if (tag == IPP_TAG_END_ATTRIBUTES) {
            return false;
        }
        if (tag <= 0x0f) {
            current_name[0] = '\0';
            continue;
        }
        if (cursor + 2 > input_length) {
            return false;
        }
        uint16_t name_length = read_u16(input + cursor);
        cursor += 2;
        if (name_length > IPP_NAME_MAX || cursor + name_length + 2 > input_length) {
            return false;
        }
        if (name_length) {
            memcpy(current_name, input + cursor, name_length);
            current_name[name_length] = '\0';
        } else if (!current_name[0]) {
            return false;
        }
        cursor += name_length;
        uint16_t value_length = read_u16(input + cursor);
        cursor += 2;
        if (cursor + value_length > input_length) {
            return false;
        }
        if ((tag == IPP_TAG_INTEGER || tag == IPP_TAG_ENUM) &&
            value_length == 4 && strcmp(current_name, attribute_name) == 0) {
            *value = read_u32(input + cursor);
            return true;
        }
        cursor += value_length;
    }
    return false;
}

ipp_codec_result_t ipp_codec_filter_response(
    const uint8_t *input, size_t input_length,
    ipp_response_kind_t response_kind, const char *requested_attributes,
    uint8_t **output, size_t *output_length, size_t *attributes_length)
{
    if (!input || !output || !output_length || !attributes_length) {
        return IPP_CODEC_MALFORMED;
    }
    if (response_kind != IPP_RESPONSE_KIND_PRINTER &&
        response_kind != IPP_RESPONSE_KIND_JOB) {
        return IPP_CODEC_MALFORMED;
    }
    if (input_length < IPP_HEADER_LENGTH) {
        return IPP_CODEC_INCOMPLETE;
    }
    byte_buffer_t result = {0};
    if (!append(&result, input, IPP_HEADER_LENGTH)) {
        return IPP_CODEC_NO_MEMORY;
    }
    size_t cursor = IPP_HEADER_LENGTH;
    uint8_t current_group = 0;
    uint8_t filtered_group = response_kind == IPP_RESPONSE_KIND_PRINTER ?
                                 IPP_TAG_PRINTER_ATTRIBUTES :
                                 IPP_TAG_JOB_ATTRIBUTES;
    requested_group_mask_t requested_groups =
        requested_group_mask(requested_attributes);
    bool request_all = !requested_attributes || !*requested_attributes ||
                       csv_contains(requested_attributes, "all", 3);
    char current_name[IPP_NAME_MAX + 1] = {0};
    bool ended = false;
    while (cursor < input_length) {
        size_t record_start = cursor;
        uint8_t tag = input[cursor++];
        if (tag == IPP_TAG_END_ATTRIBUTES) {
            if (!append(&result, &tag, 1)) {
                free(result.data);
                return IPP_CODEC_NO_MEMORY;
            }
            *attributes_length = result.length;
            ended = true;
            break;
        }
        if (tag <= 0x0f) {
            current_group = tag;
            current_name[0] = '\0';
            if (!append(&result, &tag, 1)) {
                free(result.data);
                return IPP_CODEC_NO_MEMORY;
            }
            continue;
        }
        if (cursor + 2 > input_length) {
            free(result.data);
            return IPP_CODEC_INCOMPLETE;
        }
        uint16_t name_length = read_u16(input + cursor);
        cursor += 2;
        if (name_length > IPP_NAME_MAX || cursor + name_length + 2 > input_length) {
            free(result.data);
            return name_length > IPP_NAME_MAX ? IPP_CODEC_MALFORMED :
                                                IPP_CODEC_INCOMPLETE;
        }
        if (name_length) {
            memcpy(current_name, input + cursor, name_length);
            current_name[name_length] = '\0';
        } else if (!current_name[0]) {
            free(result.data);
            return IPP_CODEC_MALFORMED;
        }
        cursor += name_length;
        uint16_t value_length = read_u16(input + cursor);
        cursor += 2;
        if (cursor + value_length > input_length) {
            free(result.data);
            return IPP_CODEC_INCOMPLETE;
        }
        cursor += value_length;
        requested_group_mask_t attribute_group =
            classify_response_attribute(current_group, current_name);
        bool include = current_group != filtered_group ||
                       csv_contains(requested_attributes, current_name,
                                    strlen(current_name)) ||
                       (request_all && attribute_group != 0) ||
                       (attribute_group & requested_groups) != 0;
        if (include && !append(&result, input + record_start,
                               cursor - record_start)) {
            free(result.data);
            return IPP_CODEC_NO_MEMORY;
        }
    }
    if (!ended) {
        free(result.data);
        return IPP_CODEC_INCOMPLETE;
    }
    if (cursor < input_length &&
        !append(&result, input + cursor, input_length - cursor)) {
        free(result.data);
        return IPP_CODEC_NO_MEMORY;
    }
    *output = result.data;
    *output_length = result.length;
    return IPP_CODEC_OK;
}

ipp_codec_result_t ipp_codec_filter_printer_response(
    const uint8_t *input, size_t input_length, const char *requested_attributes,
    uint8_t **output, size_t *output_length, size_t *attributes_length)
{
    return ipp_codec_filter_response(
        input, input_length, IPP_RESPONSE_KIND_PRINTER, requested_attributes,
        output, output_length, attributes_length);
}

ipp_codec_result_t ipp_codec_build_status_response(
    uint8_t major, uint8_t minor, uint16_t status_code, uint32_t request_id,
    const char *status_message, uint8_t **output, size_t *output_length)
{
    if (!major || !output || !output_length) {
        return IPP_CODEC_MALFORMED;
    }
    *output = NULL;
    *output_length = 0;
    uint8_t header[] = {
        major, minor, (uint8_t)(status_code >> 8), (uint8_t)status_code,
        (uint8_t)(request_id >> 24), (uint8_t)(request_id >> 16),
        (uint8_t)(request_id >> 8), (uint8_t)request_id,
        IPP_TAG_OPERATION_ATTRIBUTES,
    };
    byte_buffer_t response = {0};
    bool valid = append(&response, header, sizeof(header)) &&
                 append_string(&response, IPP_TAG_CHARSET,
                               "attributes-charset", "utf-8") &&
                 append_string(&response, IPP_TAG_LANGUAGE,
                               "attributes-natural-language", "en");
    if (valid && status_message && *status_message) {
        valid = append_string(&response, IPP_TAG_TEXT, "status-message",
                              status_message);
    }
    uint8_t end = IPP_TAG_END_ATTRIBUTES;
    valid = valid && append(&response, &end, 1);
    if (!valid) {
        free(response.data);
        return IPP_CODEC_NO_MEMORY;
    }
    *output = response.data;
    *output_length = response.length;
    return IPP_CODEC_OK;
}

uint64_t ipp_codec_relay_operations(uint64_t upstream_operations)
{
    const uint64_t individually_safe =
        (1ULL << IPP_OPERATION_PRINT_JOB) |
        (1ULL << IPP_OPERATION_PRINT_URI) |
        (1ULL << IPP_OPERATION_VALIDATE_JOB) |
        (1ULL << IPP_OPERATION_SEND_URI) |
        (1ULL << IPP_OPERATION_CANCEL_JOB) |
        (1ULL << IPP_OPERATION_GET_JOB_ATTRIBUTES) |
        (1ULL << IPP_OPERATION_GET_JOBS) |
        (1ULL << IPP_OPERATION_GET_PRINTER_ATTRIBUTES) |
        (1ULL << IPP_OPERATION_CANCEL_MY_JOBS) |
        (1ULL << IPP_OPERATION_CLOSE_JOB) |
        (1ULL << IPP_OPERATION_IDENTIFY_PRINTER);
    uint64_t relay = upstream_operations & individually_safe;
    relay |= 1ULL << IPP_OPERATION_GET_PRINTER_ATTRIBUTES;
    if ((upstream_operations & (1ULL << IPP_OPERATION_CREATE_JOB)) &&
        (upstream_operations & (1ULL << IPP_OPERATION_SEND_DOCUMENT))) {
        relay |= (1ULL << IPP_OPERATION_CREATE_JOB) |
                 (1ULL << IPP_OPERATION_SEND_DOCUMENT);
    }
    return relay;
}

bool ipp_codec_format_supported(const printer_target_t *target,
                                const char *document_format)
{
    return target && document_format && *document_format &&
           csv_contains(target->pdl, document_format, strlen(document_format));
}

void ipp_codec_finalize_profile(printer_target_t *target)
{
    if (!target) {
        return;
    }
    const char *cursor = target->urf;
    while (*cursor) {
        while (*cursor == ',' || isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        const char *end = cursor;
        while (end > start && isspace((unsigned char)end[-1])) {
            --end;
        }
        size_t length = (size_t)(end - start);
        if (length >= 2 && tolower((unsigned char)start[0]) == 'r' &&
            tolower((unsigned char)start[1]) == 's') {
            char resolution[24];
            size_t resolution_length = length - 2;
            if (resolution_length >= sizeof(resolution)) {
                resolution_length = sizeof(resolution) - 1;
            }
            memcpy(resolution, start + 2, resolution_length);
            resolution[resolution_length] = '\0';
            char *next = NULL;
            unsigned long low = strtoul(resolution, &next, 10);
            unsigned long high = low;
            if (next && *next == '-') {
                high = strtoul(next + 1, &next, 10);
            }
            if (next && *next == '\0' && low > 0 && low <= UINT16_MAX &&
                high > 0 && high <= UINT16_MAX) {
                target->resolution_low_dpi = (uint16_t)low;
                target->resolution_high_dpi = (uint16_t)high;
            }
        }
        if (value_contains((const uint8_t *)start, length, "srgb") ||
            value_contains((const uint8_t *)start, length, "adobergb") ||
            value_contains((const uint8_t *)start, length, "devrgb")) {
            target->color = true;
        }
    }
    target->copies = target->copies || target->copies_upper > 1;
    if (target->urf[0]) {
        /* A discovered AirPrint/URF printer necessarily implements these two
         * operations even when old firmware omits operations-supported. */
        target->operations_supported |=
            (1ULL << IPP_OPERATION_PRINT_JOB) |
            (1ULL << IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
        if (!target->copies_upper) {
            target->copies_upper = 1;
        }
    }
    if (!target->media_default[0] && target->media[0]) {
        const char *separator = strchr(target->media, ',');
        size_t length = separator ? (size_t)(separator - target->media) :
                                    strlen(target->media);
        while (length && isspace((unsigned char)target->media[length - 1])) {
            --length;
        }
        copy_value(target->media_default, sizeof(target->media_default),
                   (const uint8_t *)target->media, length);
    }
    if (!target->ipp_versions[0] && target->upstream_ipp_major) {
        snprintf(target->ipp_versions, sizeof(target->ipp_versions), "%u.%u",
                 target->upstream_ipp_major, target->upstream_ipp_minor);
    }
}

ipp_codec_result_t ipp_codec_apply_printer_attributes(
    const uint8_t *input, size_t input_length, printer_target_t *target)
{
    if (!input || !target) {
        return IPP_CODEC_MALFORMED;
    }
    if (input_length < IPP_HEADER_LENGTH) {
        return IPP_CODEC_INCOMPLETE;
    }
    target->profile_schema = ESPRESSO_PROFILE_SCHEMA;
    target->upstream_ipp_major = input[0];
    target->upstream_ipp_minor = input[1];

    size_t cursor = IPP_HEADER_LENGTH;
    char current_name[IPP_NAME_MAX + 1] = {0};
    while (cursor < input_length) {
        uint8_t tag = input[cursor++];
        if (tag == IPP_TAG_END_ATTRIBUTES) {
            ipp_codec_finalize_profile(target);
            return IPP_CODEC_OK;
        }
        if (tag <= 0x0f) {
            current_name[0] = '\0';
            continue;
        }
        if (cursor + 2 > input_length) {
            return IPP_CODEC_INCOMPLETE;
        }
        uint16_t name_length = read_u16(input + cursor);
        cursor += 2;
        if (name_length > IPP_NAME_MAX || cursor + name_length + 2 > input_length) {
            return name_length > IPP_NAME_MAX ? IPP_CODEC_MALFORMED :
                                                IPP_CODEC_INCOMPLETE;
        }
        if (name_length) {
            memcpy(current_name, input + cursor, name_length);
            current_name[name_length] = '\0';
        } else if (!current_name[0]) {
            return IPP_CODEC_MALFORMED;
        }
        cursor += name_length;
        uint16_t value_length = read_u16(input + cursor);
        cursor += 2;
        if (cursor + value_length > input_length) {
            return IPP_CODEC_INCOMPLETE;
        }
        const uint8_t *value = input + cursor;
        cursor += value_length;

        if (strcmp(current_name, "document-format-supported") == 0 &&
            tag == IPP_TAG_MIMETYPE) {
            csv_add_split(target->pdl, sizeof(target->pdl), value, value_length);
        } else if (strcmp(current_name, "urf-supported") == 0 &&
                   tag == IPP_TAG_KEYWORD) {
            csv_add_split(target->urf, sizeof(target->urf), value, value_length);
            if (value_contains(value, value_length, "srgb") ||
                value_contains(value, value_length, "adobergb") ||
                value_contains(value, value_length, "devrgb")) {
                target->color = true;
            }
        } else if (strcmp(current_name, "media-supported") == 0) {
            csv_add_split(target->media, sizeof(target->media), value, value_length);
        } else if (strcmp(current_name, "media-default") == 0) {
            copy_value(target->media_default, sizeof(target->media_default),
                       value, value_length);
        } else if (strcmp(current_name, "ipp-versions-supported") == 0 &&
                   tag == IPP_TAG_KEYWORD) {
            csv_add_split(target->ipp_versions, sizeof(target->ipp_versions),
                          value, value_length);
        } else if (strcmp(current_name, "printer-make-and-model") == 0) {
            copy_value(target->label, sizeof(target->label), value, value_length);
        } else if (strcmp(current_name, "printer-info") == 0 && !target->label[0]) {
            copy_value(target->label, sizeof(target->label), value, value_length);
        } else if (strcmp(current_name, "printer-location") == 0) {
            copy_value(target->location, sizeof(target->location), value, value_length);
        } else if (strcmp(current_name, "printer-more-info") == 0 &&
                   tag == IPP_TAG_URI &&
                   value_length < sizeof(target->admin_url) &&
                   web_url_value(value, value_length)) {
            copy_value(target->admin_url, sizeof(target->admin_url),
                       value, value_length);
        } else if (strcmp(current_name, "printer-uuid") == 0) {
            if (value_length > 9 && memcmp(value, "urn:uuid:", 9) == 0) {
                value += 9;
                value_length -= 9;
            }
            copy_value(target->uuid, sizeof(target->uuid), value, value_length);
        } else if (strcmp(current_name, "color-supported") == 0 &&
                   tag == IPP_TAG_BOOLEAN && value_length == 1) {
            target->color = value[0] != 0;
        } else if (strcmp(current_name, "print-color-mode-default") == 0 ||
                   strcmp(current_name, "output-mode-default") == 0) {
            if (value_contains(value, value_length, "monochrome") ||
                value_contains(value, value_length, "gray") ||
                value_contains(value, value_length, "bi-level")) {
                snprintf(target->color_mode_default,
                         sizeof(target->color_mode_default), "monochrome");
            } else if (value_contains(value, value_length, "color") ||
                       value_contains(value, value_length, "rgb") ||
                       value_contains(value, value_length, "cmyk")) {
                snprintf(target->color_mode_default,
                         sizeof(target->color_mode_default), "color");
            } else if (value_contains(value, value_length, "auto")) {
                snprintf(target->color_mode_default,
                         sizeof(target->color_mode_default), "auto");
            }
        } else if (strcmp(current_name, "print-color-mode-supported") == 0) {
            target->job_template_capabilities |=
                ESPRESSO_JOB_CAP_PRINT_COLOR_MODE;
            target->legacy_output_mode = false;
            if (value_contains(value, value_length, "color") ||
                value_contains(value, value_length, "rgb") ||
                value_contains(value, value_length, "cmyk")) {
                target->color = true;
            }
        } else if (strcmp(current_name, "output-mode-supported") == 0) {
            if (!(target->job_template_capabilities &
                  ESPRESSO_JOB_CAP_PRINT_COLOR_MODE)) {
                target->legacy_output_mode = true;
            }
            if (value_contains(value, value_length, "color") ||
                value_contains(value, value_length, "rgb") ||
                value_contains(value, value_length, "cmyk")) {
                target->color = true;
            }
        } else if (strcmp(current_name, "sides-supported") == 0) {
            target->job_template_capabilities |= ESPRESSO_JOB_CAP_SIDES;
            if (value_contains(value, value_length, "two-sided")) {
                target->duplex = true;
            }
        } else if (strcmp(current_name, "copies-supported") == 0 &&
                   tag == IPP_TAG_RANGE && value_length == 8) {
            uint32_t upper = read_u32(value + 4);
            if (upper > UINT16_MAX) {
                upper = UINT16_MAX;
            }
            target->copies_upper = (uint16_t)upper;
            target->copies = upper > 1;
            target->job_template_capabilities |= ESPRESSO_JOB_CAP_COPIES;
        } else if (strcmp(current_name, "finishings-supported") == 0) {
            target->job_template_capabilities |= ESPRESSO_JOB_CAP_FINISHINGS;
        } else if (strcmp(current_name, "orientation-requested-supported") == 0) {
            target->job_template_capabilities |= ESPRESSO_JOB_CAP_ORIENTATION;
        } else if (strcmp(current_name, "output-bin-supported") == 0) {
            target->job_template_capabilities |= ESPRESSO_JOB_CAP_OUTPUT_BIN;
        } else if (strcmp(current_name, "print-quality-supported") == 0) {
            target->job_template_capabilities |= ESPRESSO_JOB_CAP_PRINT_QUALITY;
        } else if ((strcmp(current_name, "sheet-collate-supported") == 0 ||
                    strcmp(current_name, "multiple-document-handling-supported") == 0) &&
                   value_contains(value, value_length, "collated") &&
                   !value_contains(value, value_length, "uncollated")) {
            target->collate = true;
        } else if (strcmp(current_name, "operations-supported") == 0 &&
                   tag == IPP_TAG_ENUM && value_length == 4) {
            uint32_t operation = read_u32(value);
            if (operation < 64) {
                target->operations_supported |= 1ULL << operation;
            }
        } else if (strcmp(current_name, "printer-state") == 0 &&
                   tag == IPP_TAG_ENUM && value_length == 4) {
            uint32_t state = read_u32(value);
            if (state <= UINT8_MAX) {
                target->printer_state = (uint8_t)state;
            }
        } else if (strcmp(current_name, "printer-is-accepting-jobs") == 0 &&
                   tag == IPP_TAG_BOOLEAN && value_length == 1) {
            target->accepting_jobs = value[0] != 0;
            target->accepting_jobs_known = true;
        } else if (strcmp(current_name, "printer-state-reasons") == 0 &&
                   tag == IPP_TAG_KEYWORD) {
            csv_add_split(target->state_reasons, sizeof(target->state_reasons),
                          value, value_length);
        }
    }
    return IPP_CODEC_INCOMPLETE;
}

uint16_t ipp_codec_message_code(const uint8_t *input, size_t input_length)
{
    return input && input_length >= 4 ? read_u16(input + 2) : UINT16_MAX;
}

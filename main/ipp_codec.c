#include "ipp_codec.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IPP_HEADER_LENGTH 8
#define IPP_NAME_MAX 255

#define IPP_TAG_OPERATION_ATTRIBUTES 0x01
#define IPP_TAG_END_ATTRIBUTES 0x03
#define IPP_TAG_PRINTER_ATTRIBUTES 0x04
#define IPP_TAG_INTEGER 0x21
#define IPP_TAG_BOOLEAN 0x22
#define IPP_TAG_ENUM 0x23
#define IPP_TAG_RESOLUTION 0x32
#define IPP_TAG_RANGE 0x33
#define IPP_TAG_BEGIN_COLLECTION 0x34
#define IPP_TAG_END_COLLECTION 0x37
#define IPP_TAG_TEXT 0x41
#define IPP_TAG_NAME 0x42
#define IPP_TAG_KEYWORD 0x44
#define IPP_TAG_URI 0x45
#define IPP_TAG_CHARSET 0x47
#define IPP_TAG_LANGUAGE 0x48
#define IPP_TAG_MIMETYPE 0x49
#define IPP_TAG_MEMBER_NAME 0x4a

#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000b
#define IPP_STATUS_ERROR_BAD_REQUEST 0x0400

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
} byte_buffer_t;

typedef enum {
    PRESENT_VERSIONS = 1ULL << 0,
    PRESENT_OPERATIONS = 1ULL << 1,
    PRESENT_PRINTER_URI = 1ULL << 2,
    PRESENT_URI_AUTH = 1ULL << 3,
    PRESENT_URI_SECURITY = 1ULL << 4,
    PRESENT_UUID = 1ULL << 5,
    PRESENT_NAME = 1ULL << 6,
    PRESENT_INFO = 1ULL << 7,
    PRESENT_MAKE_MODEL = 1ULL << 8,
    PRESENT_LOCATION = 1ULL << 9,
    PRESENT_FORMATS = 1ULL << 10,
    PRESENT_FORMAT_DEFAULT = 1ULL << 11,
    PRESENT_URF = 1ULL << 12,
    PRESENT_COLOR = 1ULL << 13,
    PRESENT_SIDES = 1ULL << 14,
    PRESENT_COPIES = 1ULL << 15,
    PRESENT_MEDIA = 1ULL << 16,
    PRESENT_COMPRESSION = 1ULL << 17,
    PRESENT_MULTIPLE_DOCUMENTS = 1ULL << 18,
    PRESENT_MEDIA_COL_DATABASE = 1ULL << 19,
    PRESENT_MEDIA_COL_DEFAULT = 1ULL << 20,
    PRESENT_PRINT_COLOR_MODE = 1ULL << 21,
    PRESENT_PRINT_COLOR_DEFAULT = 1ULL << 22,
    PRESENT_RESOLUTION = 1ULL << 23,
    PRESENT_RESOLUTION_DEFAULT = 1ULL << 24,
} present_attribute_t;

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

static void csv_remove(char *csv, size_t csv_size, const char *unwanted)
{
    char filtered[ESPRESSO_PDL_MAX] = {0};
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
        if (end > start && !equal_span(start, (size_t)(end - start), unwanted,
                                       strlen(unwanted))) {
            csv_add(filtered, sizeof(filtered), (const uint8_t *)start,
                    (size_t)(end - start));
        }
    }
    snprintf(csv, csv_size, "%s", filtered);
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

static char *rewrite_uri(const char *name, const uint8_t *value, size_t value_length,
                         const char *printer_uri, const char *authority)
{
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
    if (!(present & PRESENT_OPERATIONS) && target->operations_supported) {
        bool first = true;
        for (uint32_t operation = 0; operation < 64; ++operation) {
            if ((target->operations_supported & (1ULL << operation)) != 0) {
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
    if (!(present & PRESENT_FORMATS) && target->pdl[0] &&
        !append_csv_attributes(buffer, IPP_TAG_MIMETYPE,
                               "document-format-supported", target->pdl)) {
        return false;
    }
    if (!(present & PRESENT_FORMAT_DEFAULT) &&
        csv_contains(target->pdl, "image/urf", strlen("image/urf")) &&
        !append_string(buffer, IPP_TAG_MIMETYPE, "document-format-default", "image/urf")) {
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
    if (!(present & PRESENT_MEDIA) && target->media[0] &&
        !append_csv_attributes(buffer, IPP_TAG_KEYWORD, "media-supported", target->media)) {
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
             strcmp(current_name, "printer-uuid") == 0 ||
             strcmp(current_name, "uri-authentication-supported") == 0 ||
             strcmp(current_name, "uri-security-supported") == 0);
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
    if (!major || !printer_uri || !output || !output_length) {
        return IPP_CODEC_MALFORMED;
    }
    *output = NULL;
    *output_length = 0;
    uint8_t header[] = {
        major, minor,
        (uint8_t)(IPP_OP_GET_PRINTER_ATTRIBUTES >> 8),
        (uint8_t)IPP_OP_GET_PRINTER_ATTRIBUTES,
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

void ipp_codec_finalize_profile(printer_target_t *target)
{
    if (!target) {
        return;
    }
    csv_remove(target->pdl, sizeof(target->pdl), "application/octet-stream");
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
            tag == IPP_TAG_MIMETYPE &&
            !value_contains(value, value_length, "application/octet-stream")) {
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
        } else if ((strcmp(current_name, "print-color-mode-supported") == 0 ||
                    strcmp(current_name, "output-mode-supported") == 0) &&
                   (value_contains(value, value_length, "color") ||
                    value_contains(value, value_length, "rgb") ||
                    value_contains(value, value_length, "cmyk"))) {
            target->color = true;
        } else if (strcmp(current_name, "sides-supported") == 0 &&
                   value_contains(value, value_length, "two-sided")) {
            target->duplex = true;
        } else if (strcmp(current_name, "copies-supported") == 0 &&
                   tag == IPP_TAG_RANGE && value_length == 8) {
            uint32_t upper = read_u32(value + 4);
            if (upper > UINT16_MAX) {
                upper = UINT16_MAX;
            }
            target->copies_upper = (uint16_t)upper;
            target->copies = upper > 1;
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
        }
    }
    return IPP_CODEC_INCOMPLETE;
}

uint16_t ipp_codec_message_code(const uint8_t *input, size_t input_length)
{
    return input && input_length >= 4 ? read_u16(input + 2) : UINT16_MAX;
}

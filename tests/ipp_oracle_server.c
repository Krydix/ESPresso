#include "ipp_codec.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static size_t add_value(uint8_t *message, size_t cursor, uint8_t tag,
                        const char *name, const void *value, size_t value_length)
{
    size_t name_length = name ? strlen(name) : 0;
    message[cursor++] = tag;
    message[cursor++] = (uint8_t)(name_length >> 8);
    message[cursor++] = (uint8_t)name_length;
    if (name_length) {
        memcpy(message + cursor, name, name_length);
        cursor += name_length;
    }
    message[cursor++] = (uint8_t)(value_length >> 8);
    message[cursor++] = (uint8_t)value_length;
    if (value_length) {
        memcpy(message + cursor, value, value_length);
        cursor += value_length;
    }
    return cursor;
}

static size_t add_string(uint8_t *message, size_t cursor, uint8_t tag,
                         const char *name, const char *value)
{
    return add_value(message, cursor, tag, name, value, strlen(value));
}

static int send_all(int socket_fd, const void *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(socket_fd, (const char *)data + sent, length - sent, 0);
        if (result <= 0) {
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

static int handle_client(int client)
{
    uint8_t request[16384];
    size_t request_length = 0;
    uint8_t *body = NULL;
    size_t content_length = 0;
    while (request_length < sizeof(request)) {
        ssize_t received = recv(client, request + request_length,
                                sizeof(request) - request_length, 0);
        if (received <= 0) {
            break;
        }
        request_length += (size_t)received;
        for (size_t i = 3; i < request_length; ++i) {
            if (memcmp(request + i - 3, "\r\n\r\n", 4) == 0) {
                body = request + i + 1;
                size_t header_length = (size_t)(body - request);
                if (header_length < 4096) {
                    char headers[4096];
                    memcpy(headers, request, header_length);
                    headers[header_length] = '\0';
                    char *length_header = strstr(headers, "Content-Length:");
                    if (length_header) {
                        content_length = strtoul(length_header + 15, NULL, 10);
                    }
                }
                break;
            }
        }
        if (body && content_length >= 8 &&
            (size_t)(request + request_length - body) >= content_length) {
            break;
        }
    }
    if (!body || (size_t)(request + request_length - body) < 8) {
        return 1;
    }

    uint16_t operation = ipp_codec_message_code(body, content_length);
    uint32_t request_id = ((uint32_t)body[4] << 24) |
                          ((uint32_t)body[5] << 16) |
                          ((uint32_t)body[6] << 8) | body[7];
    if (operation == 0x0003) {
        uint8_t *unsupported = NULL;
        size_t unsupported_length = 0;
        if (ipp_codec_build_status_response(
                body[0], body[1], IPP_STATUS_SERVER_ERROR_OPERATION_NOT_SUPPORTED,
                request_id, "Operation is not safe to relay", &unsupported,
                &unsupported_length) != IPP_CODEC_OK) {
            return 1;
        }
        char headers[256];
        int header_length = snprintf(headers, sizeof(headers),
                                     "HTTP/1.1 200 OK\r\n"
                                     "Content-Type: application/ipp\r\n"
                                     "Content-Length: %zu\r\n"
                                     "Connection: close\r\n\r\n",
                                     unsupported_length);
        int result = send_all(client, headers, (size_t)header_length) ||
                     send_all(client, unsupported, unsupported_length);
        free(unsupported);
        return result ? 1 : 0;
    }

    uint8_t legacy[4096] = {1, 1, 0, 0, body[4], body[5], body[6], body[7], 1};
    size_t length = 9;
    length = add_string(legacy, length, 0x47, "attributes-charset", "utf-8");
    length = add_string(legacy, length, 0x48, "attributes-natural-language", "en");
    if (operation == IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        legacy[length++] = 4;
        length = add_string(legacy, length, 0x44, "ipp-versions-supported", "1.1");
        length = add_string(legacy, length, 0x45, "printer-uri-supported",
                            "ipp://legacy.local:631/ipp/print");
        length = add_string(legacy, length, 0x45, "printer-uuid",
                            "urn:uuid:physical-printer");
        length = add_string(legacy, length, 0x44, "uri-security-supported", "tls");
        length = add_string(legacy, length, 0x49, "document-format-supported",
                            "image/urf");
        length = add_string(legacy, length, 0x44, "urf-supported", "W8");
        length = add_string(legacy, length, 0x44, NULL, "SRGB24");
        length = add_string(legacy, length, 0x44, NULL, "RS300-600");
        length = add_string(legacy, length, 0x44, "media-supported",
                            "iso_a4_210x297mm");
        length = add_string(legacy, length, 0x44, NULL, "na_letter_8.5x11in");
        length = add_string(legacy, length, 0x44, "media-default",
                            "iso_a4_210x297mm");
        const uint32_t operations[] = {2, 3, 4, 5, 6, 8, 9, 10, 11};
        for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
            uint8_t value[] = {0, 0, 0, (uint8_t)operations[i]};
            length = add_value(legacy, length, 0x23,
                               i ? NULL : "operations-supported", value,
                               sizeof(value));
        }
        uint8_t state[] = {0, 0, 0, 3};
        length = add_value(legacy, length, 0x23, "printer-state", state,
                           sizeof(state));
        uint8_t accepting[] = {1};
        length = add_value(legacy, length, 0x22,
                           "printer-is-accepting-jobs", accepting,
                           sizeof(accepting));
        length = add_string(legacy, length, 0x44, "printer-state-reasons", "none");
    } else if (operation == IPP_OPERATION_PRINT_JOB ||
               operation == IPP_OPERATION_CREATE_JOB ||
               operation == IPP_OPERATION_GET_JOB_ATTRIBUTES ||
               operation == IPP_OPERATION_GET_JOBS) {
        legacy[length++] = 2;
        uint8_t job_id[] = {0, 0, 0, 42};
        length = add_value(legacy, length, 0x21, "job-id", job_id,
                           sizeof(job_id));
        length = add_string(legacy, length, 0x45, "job-uri",
                            "ipp://legacy.local:631/jobs/42");
        uint8_t job_state[] = {0, 0, 0, 3};
        length = add_value(legacy, length, 0x23, "job-state", job_state,
                           sizeof(job_state));
    }
    legacy[length++] = 3;

    printer_target_t target = {0};
    snprintf(target.instance, sizeof(target.instance), "Legacy fixture");
    snprintf(target.label, sizeof(target.label), "TestCo Legacy 500");
    snprintf(target.pdl, sizeof(target.pdl), "image/urf");
    snprintf(target.urf, sizeof(target.urf), "W8,SRGB24,RS300-600");
    snprintf(target.media, sizeof(target.media),
             "iso_a4_210x297mm,na_letter_8.5x11in");
    snprintf(target.media_default, sizeof(target.media_default),
             "iso_a4_210x297mm");
    target.color = true;
    target.printer_state = 3;
    target.accepting_jobs = true;
    target.accepting_jobs_known = true;
    target.operations_supported = (1ULL << IPP_OPERATION_PRINT_JOB) |
                                  (1ULL << 3) |
                                  (1ULL << IPP_OPERATION_VALIDATE_JOB) |
                                  (1ULL << IPP_OPERATION_CREATE_JOB) |
                                  (1ULL << IPP_OPERATION_SEND_DOCUMENT) |
                                  (1ULL << IPP_OPERATION_CANCEL_JOB) |
                                  (1ULL << IPP_OPERATION_GET_JOB_ATTRIBUTES) |
                                  (1ULL << IPP_OPERATION_GET_JOBS) |
                                  (1ULL << IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    ipp_codec_finalize_profile(&target);

    uint8_t *normalized = NULL;
    size_t normalized_length = 0;
    size_t attributes_length = 0;
    ipp_codec_result_t codec;
    if (operation == IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        codec = ipp_codec_normalize_printer_response(
            legacy, length, "ipp://127.0.0.1:18631/ipp/print",
            "ipp://127.0.0.1:18631", "oracle-bridge-uuid", &target,
            &normalized, &normalized_length, &attributes_length);
    } else {
        codec = ipp_codec_rewrite(
            legacy, length, "ipp://127.0.0.1:18631/ipp/print",
            "ipp://127.0.0.1:18631", &normalized, &normalized_length,
            &attributes_length);
    }
    if (codec != IPP_CODEC_OK) {
        return 1;
    }
    normalized[0] = body[0];
    normalized[1] = body[1];

    char headers[256];
    int header_length = snprintf(headers, sizeof(headers),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/ipp\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n",
                                 normalized_length);
    int result = send_all(client, headers, (size_t)header_length) ||
                 send_all(client, normalized, normalized_length);
    free(normalized);
    return result ? 1 : 0;
}

int main(void)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        return 1;
    }
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(18631),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 4) != 0) {
        close(server);
        return 1;
    }
    while (1) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            close(server);
            return errno == EINTR ? 0 : 1;
        }
        int result = handle_client(client);
        close(client);
        if (result != 0) {
            close(server);
            return result;
        }
    }
}

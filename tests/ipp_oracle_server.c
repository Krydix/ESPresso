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
        listen(server, 1) != 0) {
        close(server);
        return 1;
    }

    int client = accept(server, NULL, NULL);
    if (client < 0) {
        close(server);
        return 1;
    }
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
        close(client);
        close(server);
        return 1;
    }

    uint8_t legacy[4096] = {1, 1, 0, 0, body[4], body[5], body[6], body[7], 1};
    size_t length = 9;
    length = add_string(legacy, length, 0x47, "attributes-charset", "utf-8");
    length = add_string(legacy, length, 0x48, "attributes-natural-language", "en");
    legacy[length++] = 4;
    length = add_string(legacy, length, 0x44, "ipp-versions-supported", "1.1");
    length = add_string(legacy, length, 0x45, "printer-uri-supported",
                        "ipp://legacy.local:631/ipp/print");
    length = add_string(legacy, length, 0x45, "printer-uuid",
                        "urn:uuid:physical-printer");
    length = add_string(legacy, length, 0x44, "uri-security-supported", "tls");
    length = add_string(legacy, length, 0x49, "document-format-supported", "image/urf");
    length = add_string(legacy, length, 0x44, "urf-supported", "W8");
    length = add_string(legacy, length, 0x44, NULL, "SRGB24");
    length = add_string(legacy, length, 0x44, NULL, "RS300-600");
    length = add_string(legacy, length, 0x44, "media-supported",
                        "iso_a4_210x297mm");
    length = add_string(legacy, length, 0x44, NULL, "na_letter_8.5x11in");
    length = add_string(legacy, length, 0x44, "media-default",
                        "iso_a4_210x297mm");
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
    ipp_codec_finalize_profile(&target);

    uint8_t *normalized = NULL;
    size_t normalized_length = 0;
    size_t attributes_length = 0;
    if (ipp_codec_normalize_printer_response(
            legacy, length, "ipp://127.0.0.1:18631/ipp/print",
            "ipp://127.0.0.1:18631", "oracle-bridge-uuid", &target,
            &normalized, &normalized_length, &attributes_length) != IPP_CODEC_OK) {
        close(client);
        close(server);
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
    close(client);
    close(server);
    return result ? 1 : 0;
}

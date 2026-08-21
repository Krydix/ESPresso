/* Captive-portal DNS responder, derived from ESP-IDF's CC0 example. */
#include "dns_server.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_PACKET_MAX 512
#define DNS_TYPE_A 1

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t name_pointer;
    uint16_t type;
    uint16_t class_code;
    uint32_t ttl;
    uint16_t data_length;
    uint32_t address;
} dns_answer_t;

struct dns_server {
    volatile bool running;
    TaskHandle_t task;
    char netif_key[20];
};

static const char *TAG = "espresso_dns";

static int make_reply(uint8_t *packet, size_t request_length, size_t capacity,
                      uint32_t address)
{
    if (request_length < sizeof(dns_header_t) || request_length > capacity) {
        return -1;
    }
    dns_header_t *header = (dns_header_t *)packet;
    if (ntohs(header->questions) != 1 || (ntohs(header->flags) & 0x7800) != 0) {
        return -1;
    }

    size_t cursor = sizeof(dns_header_t);
    while (cursor < request_length && packet[cursor] != 0) {
        size_t label_length = packet[cursor];
        if (label_length > 63 || cursor + label_length + 1 >= request_length) {
            return -1;
        }
        cursor += label_length + 1;
    }
    if (cursor + 5 > request_length) {
        return -1;
    }
    cursor++;
    uint16_t query_type;
    memcpy(&query_type, packet + cursor, sizeof(query_type));
    if (ntohs(query_type) != DNS_TYPE_A) {
        return -1;
    }
    if (request_length + sizeof(dns_answer_t) > capacity) {
        return -1;
    }

    header->flags = htons(ntohs(header->flags) | 0x8000 | 0x0080);
    header->answers = htons(1);
    dns_answer_t answer = {
        .name_pointer = htons(0xc00c),
        .type = htons(DNS_TYPE_A),
        .class_code = htons(1),
        .ttl = htonl(30),
        .data_length = htons(4),
        .address = address,
    };
    memcpy(packet + request_length, &answer, sizeof(answer));
    return (int)(request_length + sizeof(answer));
}

static void dns_task(void *context)
{
    struct dns_server *server = context;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        server->running = false;
        server->task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "bind failed: errno %d", errno);
        close(sock);
        server->running = false;
        server->task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[DNS_PACKET_MAX];
    while (server->running) {
        struct sockaddr_storage source;
        socklen_t source_length = sizeof(source);
        int length = recvfrom(sock, packet, sizeof(packet) - sizeof(dns_answer_t), 0,
                              (struct sockaddr *)&source, &source_length);
        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(server->netif_key);
        esp_netif_ip_info_t info = {0};
        if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK) {
            continue;
        }
        int reply_length = make_reply(packet, (size_t)length, sizeof(packet), info.ip.addr);
        if (reply_length > 0) {
            sendto(sock, packet, (size_t)reply_length, 0,
                   (struct sockaddr *)&source, source_length);
        }
    }
    close(sock);
    server->task = NULL;
    vTaskDelete(NULL);
}

dns_server_handle_t dns_server_start(const char *netif_key)
{
    struct dns_server *server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }
    snprintf(server->netif_key, sizeof(server->netif_key), "%s", netif_key);
    server->running = true;
    if (xTaskCreate(dns_task, "espresso_dns", 4096, server, 4, &server->task) != pdPASS) {
        free(server);
        return NULL;
    }
    return server;
}

void dns_server_stop(dns_server_handle_t server)
{
    if (!server) {
        return;
    }
    server->running = false;
    for (int i = 0; i < 20 && server->task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (server->task) {
        vTaskDelete(server->task);
    }
    free(server);
}

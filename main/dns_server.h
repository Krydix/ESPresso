#pragma once

typedef struct dns_server *dns_server_handle_t;

dns_server_handle_t dns_server_start(const char *netif_key);
void dns_server_stop(dns_server_handle_t handle);

#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct espresso_tls_connection espresso_tls_connection_t;

/* Load or generate the device-local, persistent espresso.local identity. */
esp_err_t tls_identity_init(void);
esp_err_t tls_identity_accept(int socket_fd,
                              espresso_tls_connection_t **connection);
int tls_identity_read(espresso_tls_connection_t *connection, void *data,
                      size_t length);
int tls_identity_write(espresso_tls_connection_t *connection,
                       const void *data, size_t length);
void tls_identity_close(espresso_tls_connection_t *connection);
const char *tls_identity_certificate(size_t *length);

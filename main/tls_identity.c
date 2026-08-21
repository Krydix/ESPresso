#include "tls_identity.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

#define CERT_CAPACITY 2048
#define KEY_CAPACITY 1024

struct espresso_tls_connection {
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
};

static const char *TAG = "espresso_tls";
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static mbedtls_x509_crt s_certificate;
static mbedtls_pk_context s_key;
static mbedtls_ssl_config s_config;
static unsigned char *s_certificate_pem;
static unsigned char *s_key_pem;
static bool s_ready;

static void log_mbedtls(const char *operation, int error)
{
    char message[96];
    mbedtls_strerror(error, message, sizeof(message));
    ESP_LOGE(TAG, "%s failed: -0x%04x (%s)", operation, -error, message);
}

static esp_err_t persist_identity(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("espresso", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, "tls_cert", s_certificate_pem,
                       strlen((const char *)s_certificate_pem) + 1);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "tls_key", s_key_pem,
                           strlen((const char *)s_key_pem) + 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t generate_identity(void)
{
    s_certificate_pem = calloc(1, CERT_CAPACITY);
    s_key_pem = calloc(1, KEY_CAPACITY);
    if (!s_certificate_pem || !s_key_pem) {
        return ESP_ERR_NO_MEM;
    }
    int result = mbedtls_pk_setup(
        &s_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (result == 0) {
        result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                     mbedtls_pk_ec(s_key),
                                     mbedtls_ctr_drbg_random, &s_drbg);
    }
    if (result == 0) {
        result = mbedtls_pk_write_key_pem(&s_key, s_key_pem, KEY_CAPACITY);
    }
    if (result != 0) {
        log_mbedtls("private key generation", result);
        return ESP_FAIL;
    }

    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&writer, &s_key);
    mbedtls_x509write_crt_set_issuer_key(&writer, &s_key);
    static const char name[] = "CN=espresso.local,O=ESPresso";
    unsigned char serial[16];
    result = mbedtls_ctr_drbg_random(&s_drbg, serial, sizeof(serial));
    mbedtls_x509_san_list san = {0};
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)"espresso.local";
    san.node.san.unstructured_name.len = strlen("espresso.local");
    if (result == 0) {
        result = mbedtls_x509write_crt_set_subject_name(&writer, name);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_issuer_name(&writer, name);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_serial_raw(&writer, serial,
                                                       sizeof(serial));
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_validity(
            &writer, "20250101000000", "20450101000000");
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_basic_constraints(&writer, 0, -1);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_subject_alternative_name(&writer,
                                                                    &san);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_set_key_usage(
            &writer, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                         MBEDTLS_X509_KU_KEY_AGREEMENT);
    }
    if (result == 0) {
        result = mbedtls_x509write_crt_pem(
            &writer, s_certificate_pem, CERT_CAPACITY,
            mbedtls_ctr_drbg_random, &s_drbg);
    }
    mbedtls_x509write_crt_free(&writer);
    if (result != 0) {
        log_mbedtls("certificate generation", result);
        return ESP_FAIL;
    }
    result = mbedtls_x509_crt_parse(&s_certificate, s_certificate_pem,
                                    strlen((const char *)s_certificate_pem) + 1);
    if (result != 0) {
        log_mbedtls("generated certificate parse", result);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "generated a persistent espresso.local TLS identity");
    return persist_identity();
}

static esp_err_t load_identity(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("espresso", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t certificate_length = 0;
    size_t key_length = 0;
    err = nvs_get_blob(nvs, "tls_cert", NULL, &certificate_length);
    if (err == ESP_OK) {
        err = nvs_get_blob(nvs, "tls_key", NULL, &key_length);
    }
    if (err != ESP_OK || !certificate_length || !key_length ||
        certificate_length > CERT_CAPACITY || key_length > KEY_CAPACITY) {
        nvs_close(nvs);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    s_certificate_pem = calloc(1, certificate_length);
    s_key_pem = calloc(1, key_length);
    if (!s_certificate_pem || !s_key_pem) {
        nvs_close(nvs);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(nvs, "tls_cert", s_certificate_pem,
                       &certificate_length);
    if (err == ESP_OK) {
        err = nvs_get_blob(nvs, "tls_key", s_key_pem, &key_length);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }
    int result = mbedtls_x509_crt_parse(&s_certificate, s_certificate_pem,
                                        certificate_length);
    if (result == 0) {
        result = mbedtls_pk_parse_key(&s_key, s_key_pem, key_length, NULL, 0,
                                      mbedtls_ctr_drbg_random, &s_drbg);
    }
    if (result != 0) {
        log_mbedtls("persisted identity parse", result);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t tls_identity_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    mbedtls_x509_crt_init(&s_certificate);
    mbedtls_pk_init(&s_key);
    mbedtls_ssl_config_init(&s_config);
    static const unsigned char personalization[] = "ESPresso IPPS identity";
    int result = mbedtls_ctr_drbg_seed(
        &s_drbg, mbedtls_entropy_func, &s_entropy, personalization,
        sizeof(personalization) - 1);
    if (result != 0) {
        log_mbedtls("random seed", result);
        return ESP_FAIL;
    }
    esp_err_t err = load_identity();
    if (err != ESP_OK) {
        free(s_certificate_pem);
        free(s_key_pem);
        s_certificate_pem = NULL;
        s_key_pem = NULL;
        mbedtls_x509_crt_free(&s_certificate);
        mbedtls_pk_free(&s_key);
        mbedtls_x509_crt_init(&s_certificate);
        mbedtls_pk_init(&s_key);
        err = generate_identity();
    }
    if (err != ESP_OK) {
        return err;
    }
    result = mbedtls_ssl_config_defaults(
        &s_config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0) {
        mbedtls_ssl_conf_rng(&s_config, mbedtls_ctr_drbg_random, &s_drbg);
        result = mbedtls_ssl_conf_own_cert(&s_config, &s_certificate, &s_key);
    }
    if (result != 0) {
        log_mbedtls("TLS server configuration", result);
        return ESP_FAIL;
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t tls_identity_accept(int socket_fd,
                              espresso_tls_connection_t **connection)
{
    if (!s_ready || !connection) {
        return ESP_ERR_INVALID_STATE;
    }
    *connection = calloc(1, sizeof(**connection));
    if (!*connection) {
        return ESP_ERR_NO_MEM;
    }
    mbedtls_ssl_init(&(*connection)->ssl);
    mbedtls_net_init(&(*connection)->net);
    (*connection)->net.fd = socket_fd;
    int result = mbedtls_ssl_setup(&(*connection)->ssl, &s_config);
    if (result == 0) {
        mbedtls_ssl_set_bio(&(*connection)->ssl, &(*connection)->net,
                            mbedtls_net_send, mbedtls_net_recv,
                            mbedtls_net_recv_timeout);
        do {
            result = mbedtls_ssl_handshake(&(*connection)->ssl);
        } while (result == MBEDTLS_ERR_SSL_WANT_READ ||
                 result == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    if (result != 0) {
        log_mbedtls("TLS handshake", result);
        tls_identity_close(*connection);
        *connection = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

int tls_identity_read(espresso_tls_connection_t *connection, void *data,
                      size_t length)
{
    if (!connection || length > INT_MAX) {
        return -1;
    }
    int result;
    do {
        result = mbedtls_ssl_read(&connection->ssl, data, length);
    } while (result == MBEDTLS_ERR_SSL_WANT_READ ||
             result == MBEDTLS_ERR_SSL_WANT_WRITE);
    return result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ? 0 : result;
}

int tls_identity_write(espresso_tls_connection_t *connection,
                       const void *data, size_t length)
{
    if (!connection || length > INT_MAX) {
        return -1;
    }
    int result;
    do {
        result = mbedtls_ssl_write(&connection->ssl, data, length);
    } while (result == MBEDTLS_ERR_SSL_WANT_READ ||
             result == MBEDTLS_ERR_SSL_WANT_WRITE);
    return result;
}

void tls_identity_close(espresso_tls_connection_t *connection)
{
    if (!connection) {
        return;
    }
    mbedtls_ssl_close_notify(&connection->ssl);
    mbedtls_ssl_free(&connection->ssl);
    /* The owning worker closes the socket. */
    connection->net.fd = -1;
    free(connection);
}

const char *tls_identity_certificate(size_t *length)
{
    if (length) {
        *length = s_certificate_pem ?
                      strlen((const char *)s_certificate_pem) : 0;
    }
    return (const char *)s_certificate_pem;
}

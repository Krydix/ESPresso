idf_component_register(
    SRCS
        "app_main.c"
        "app_state.c"
        "dns_server.c"
        "ipp_codec.c"
        "ipp_proxy.c"
        "ipp_proxy_core.c"
        "ipp_stream.c"
        "ota_update.c"
        "printer_capabilities.c"
        "printer_advertisement.c"
        "printer_discovery.c"
        "printer_identity.c"
        "web_server.c"
        "wifi_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        esp_event
        esp_app_format
        esp_http_client
        esp_http_server
        esp_https_ota
        esp_netif
        esp_wifi
        json
        lwip
        mdns
        mbedtls
        nvs_flash
    EMBED_FILES
        "../frontend/index.html"
        "../favicon.svg"
)

set(ESPRESSO_SETUP_SUFFIX "$ENV{ESPRESSO_SETUP_SUFFIX}")
if(ESPRESSO_SETUP_SUFFIX STREQUAL "")
    set(ESPRESSO_SETUP_SUFFIX "DEV0")
endif()
string(TOUPPER "${ESPRESSO_SETUP_SUFFIX}" ESPRESSO_SETUP_SUFFIX)
if(NOT ESPRESSO_SETUP_SUFFIX MATCHES "^[A-Z0-9][A-Z0-9][A-Z0-9][A-Z0-9]$")
    message(FATAL_ERROR "ESPRESSO_SETUP_SUFFIX must contain exactly four letters or digits")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ESPRESSO_SETUP_SUFFIX="${ESPRESSO_SETUP_SUFFIX}"
)

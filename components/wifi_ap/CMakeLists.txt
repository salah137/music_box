set(SOURCES
    "ap_init.c"
    "https/main_http.c"
    "https/uri_handlers/check/check.c"
    "https/uri_handlers/upload/upload.c"
)

idf_component_register(
    SRCS ${SOURCES}
    EMBED_TXTFILES
        certs/servercert.pem
        certs/prvtkey.pem
    PRIV_REQUIRES
    REQUIRES
        esp_wifi
        nvs_flash
        esp_https_server
        cjson
        esp_driver_gpio
    INCLUDE_DIRS
        "include"
)

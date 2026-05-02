#include "string.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "uri_handlers/check/check.h"
#include "uri_handlers/upload/upload.h"


const char *TAG = "http_server";

static const httpd_uri_t check_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = check,
    .user_ctx = NULL};


static const httpd_uri_t upload_uri = {
    .uri  ="/upload",
    .method = HTTP_POST,
    .handler = upload_music_handler,
    .user_ctx = NULL
};

httpd_handle_t start_https_server(void)
{
    /* 
    ESP_LOGI(TAG, "starting the https ..........");
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();

    extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
    extern const uint8_t servercert_pem_end[] asm("_binary_servercert_pem_end");
    extern const uint8_t prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
    extern const uint8_t prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");

    config.servercert = servercert_pem_start;
    config.servercert_len = servercert_pem_end - servercert_pem_start;
    config.prvtkey_pem = prvtkey_pem_start;
    config.prvtkey_len = prvtkey_pem_end - prvtkey_pem_start;
    */

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 80;
    

    httpd_handle_t server = NULL;

    esp_err_t ret = httpd_start(&server,&config);

    if (ret == ESP_OK)
    {
        httpd_register_uri_handler(server, &check_uri);
        httpd_register_uri_handler(server,&upload_uri);
        ESP_LOGI(TAG, "HTTPS server started on port 443");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start HTTPS server: %s", esp_err_to_name(ret));
    }

    return server;
}

#include "esp_log.h"
#include "esp_https_server.h"
#include "cJSON.h"

esp_err_t check(httpd_req_t *req)
{

    cJSON* response = cJSON_CreateObject();

    cJSON_AddItemToObject(response,"status",cJSON_CreateString("exists"));

    const char* json = cJSON_Print(response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));

    return ESP_OK;
}
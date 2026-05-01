#ifndef CHECK_H
#define CHECK_H
#include "esp_log.h"
#include "esp_https_server.h"

esp_err_t check(httpd_req_t *req);

#endif
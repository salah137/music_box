#ifndef UPLOAD_H
#define UPLOAD_H
#include "esp_log.h"
#include "esp_https_server.h"

esp_err_t upload_music_handler(httpd_req_t* req);

#endif
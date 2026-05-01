#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "string.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h> 
#include <stdlib.h>
#include <string.h>

static char *TAG_http = "HTTP_LOG";

typedef enum {
  FINDING_BOUNDRIES_STATE,
  READING_HEADER_STATE,
  READING_NAME_STATE,
  READING_FIELD_STATE,
  READING_FILE_STATE,
  DONE_STATE,
} parsing_state_t;

#define CHUNCK_SIZE 2048
#define BOUNDRY_SIZE 128
#define OVERLAP BOUNDRY_SIZE * 2

esp_err_t upload_music_handler(httpd_req_t *req) {
  // getting the bounderies
  size_t ct_size = httpd_req_get_hdr_value_len(req, "Content-Type");
  char content_type[ct_size];
  httpd_req_get_hdr_value_str(req, "Content-Type", content_type, ct_size);
  char *b = strstr(content_type, "boundary=");
  if (!b) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not valid headers");
    return ESP_FAIL;
  }

  char boundary[BOUNDRY_SIZE];
  snprintf(boundary, sizeof(boundary), "\r\n--%s", b + 9);

  int buff_size = CHUNCK_SIZE + OVERLAP;
  char *buff = malloc(buff_size);

  int total_len = req->content_len;
  int recived = 0;
  int carry_len = 0;
  if (!buff) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "The Heap is coocked");
    ESP_LOGE(TAG_http, "The Heap is coocked");
    return ESP_FAIL;
  }

  FILE *out_file = NULL;
  parsing_state_t current_state = FINDING_BOUNDRIES_STATE;
  char field_name[128] = {0};
  char field_value[128] = {0};
  char fiel_val_len = 0;

  int result = ESP_FAIL;

  while (recived < total_len) {
    int to_read = fmin(total_len - recived, buff_size);
    int ret = httpd_req_recv(req, buff, to_read);

    if (ret == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (ret < 0) {
      result = ESP_FAIL;
      break;
    }

    recived += ret;
    int i = 0;
    while (i < buff_size) {
      if (current_state == FINDING_BOUNDRIES_STATE) {
        char *found =
            memmem(buff + i, buff_size - i, boundary, sizeof(boundary));

        if (found) {
          i = (buff - found) + sizeof(boundary);
          if (i + 2 < buff_size)
            i += 2;

          current_state = READING_HEADER_STATE;
          memset(field_name, 0, sizeof(field_name));
        } else {
          i = buff_size - OVERLAP;
          if (i < 0)
            i = 0;
          break;
        }
      } else if (current_state == READING_HEADER_STATE) {
        char *hdr_end = memmem(buff + i, buff_size - 1, "\r\n", 2);
        if (!hdr_end) {
          break;
        }

        *hdr_end = '\0';
        char *header = buff + i;

        char *name_ptr = strstr(buff + i, "name=\"");
        if (name_ptr) {
          name_ptr += 6;
          char *name_end = strchr(buff + i, '"');
          int len = fmin(name_end - name_ptr, sizeof(field_name));
          strncpy(field_name, name_ptr, len);
          field_name[len] = '\0';
        }
        char *fn_ptr = strstr(header, "filename=\"");
        if (fn_ptr) {
          fn_ptr += 9;
          char file_name[32] = "upload.mp3";

          char *fn_end = strchr(fn_ptr, '"');

          if (fn_end) {
            size_t fn_len = fmin(fn_end - fn_ptr, sizeof(file_name));
            strncpy(file_name, fn_ptr, fn_len);

            char out_path[64];

            snprintf(out_path, 64, "/files/%s", file_name);

            out_file = fopen(out_path, "wb");
            if (!out_file) {
              ESP_LOGE(TAG_http, "enable to open the file %s", out_path);
              goto cleanup;
              result = ESP_FAIL;
              break;
            }

            ESP_LOGI(TAG_http, "saving the file");
            current_state = READING_FILE_STATE;

          } else {
            break;
          }
        }
      } else if (current_state == READING_FILE_STATE ||
                 current_state == READING_FIELD_STATE) {
        char *next =
            memmem(buff + i, buff_size - i, boundary, sizeof(boundary));

        if (next) {
          if (current_state == READING_FILE_STATE && out_file) {
            fwrite(buff + i, 1, next - (buff + i), out_file);
            fclose(out_file);
            out_file = NULL;
            ESP_LOGI(TAG_http, "file saved at position", "%s");

          } else if (current_state == READING_FIELD_STATE) {
            size_t len = fmin(next - (buff + i), sizeof(field_value) - 1);
            strncpy(field_value, buff + i, len);
            field_value[len] = '\0';

            i = (next - buff) + sizeof(boundary) + 2; // skip boundary + \r\n
            current_state = READING_HEADER_STATE;
            memset(field_name, 0, sizeof(field_name));
          }
        } else {
          int safe_len = buff_size - i - OVERLAP;
          if (safe_len > 0) {
            if (current_state == READING_FIELD_STATE) {
              fwrite(buff + i, 1, safe_len, out_file);
            } else if (current_state == READING_FIELD_STATE) {
              int copy =
                  fmin(safe_len, (int)sizeof(field_value) - fiel_val_len - 1);
              strncpy(field_value + fiel_val_len, buff + i, copy);
              fiel_val_len += copy;
            }

            i += safe_len;
          }
          break;
        }
      }
    }

    carry_len = buff_size - i;

    if (carry_len > 0 && carry_len <= OVERLAP) {
      memmove(buff, buff + i, carry_len);
    } else {
      carry_len = 0;
    }
  }

cleanup:
  if (out_file)
    fclose(out_file);

  free(buff);

  if (result != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    return ESP_FAIL;
  }

  httpd_resp_sendstr(req, "Upload OK");
  return ESP_OK;
}

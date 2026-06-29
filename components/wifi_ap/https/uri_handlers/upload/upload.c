#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

static const char *TAG_http = "HTTP_LOG";
// 7553086
// 4069220
#define CHUNK_SIZE 8192 
#define BOUNDARY_SIZE 128
#define OVERLAP 128

typedef enum {
  SEARCHING_BOUNDRY,
  READING_THROUGH_HEADER,
  READING_FILE_TYPE,
  READING_BINARY,
  FINISHED
} state;

esp_err_t upload_music_handler(httpd_req_t *req) {
  bool *changed = (bool *)req->user_ctx;

  char *content_type = NULL;
  char *name = NULL;
  char *file_name = NULL;
  char *content_type_l = NULL;
  FILE *out_file = NULL;

  int content_type_len = httpd_req_get_hdr_value_len(req, "Content-Type");
  if (content_type_len >= 1) {
    content_type = malloc(content_type_len + 1);
    if (!content_type) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    content_type_len + 1) == ESP_OK) {
      ESP_LOGI(TAG_http, "Got the Content type: %s", content_type);
    } else {
      ESP_LOGI(TAG_http, "didn't get the Content type");
      free(content_type);
      httpd_resp_send_408(req);
      return ESP_FAIL;
    }
  } else {
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  char *boundary = strstr(content_type, "boundary=");
  if (!boundary) {
    ESP_LOGE(TAG_http, "no boundary found");
    free(content_type);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }
  boundary += strlen("boundary=");
  size_t boundary_len = strlen(boundary);
  ESP_LOGI(TAG_http, "boundary='%s' len=%d", boundary, boundary_len);

  if (boundary_len > BOUNDARY_SIZE) {
    ESP_LOGE(TAG_http, "boundary too long: %d", boundary_len);
    free(content_type);
    httpd_resp_send_408(req);
    return ESP_FAIL;
  }

  char end_mark[BOUNDARY_SIZE + 3];
  snprintf(end_mark, BOUNDARY_SIZE + 3, "\r\n%s", boundary);

  char rcv_buffer[CHUNK_SIZE + OVERLAP];
  size_t content_len = req->content_len;
  size_t rcv_len = 0;
  size_t resived = 0;
  state current_state = SEARCHING_BOUNDRY;
  int carry = 0;
  int chunk_count = 0;

  ESP_LOGI(TAG_http, "content_len=%d", content_len);

  bool done = false;

  while (!done && rcv_len <= content_len) {
    ESP_LOGI(TAG_http, "--- chunk #%d | rcv_len=%d carry=%d", chunk_count++,
             rcv_len, carry);

    resived = httpd_req_recv(req, rcv_buffer + carry, CHUNK_SIZE);
    ESP_LOGI(TAG_http, "httpd_req_recv returned %d", resived);

    if (resived <= 0) {
      *changed = true;
      ESP_LOGW(TAG_http, "recv error or connection closed: %d", resived);
      break;
    }

    rcv_len += resived;
    int total = carry + resived;
    int i = 0;

    ESP_LOGI(TAG_http, "total bytes in buffer=%d", total);

    while (i < total) {
      ESP_LOGD(TAG_http, "inner loop: i=%d total=%d state=%d", i, total,
               current_state);

      if (current_state == SEARCHING_BOUNDRY) {
        ESP_LOGI(TAG_http, "SEARCHING_BOUNDRY: i=%d", i);
        char *c_boundry = strstr(rcv_buffer + i, boundary);
        if (c_boundry) {
          ESP_LOGI(TAG_http, "boundary string found at offset=%d",
                   (int)(c_boundry - rcv_buffer));
          c_boundry += boundary_len;
          if (strncmp(c_boundry, "\r\n", 2) == 0) {
            i = (c_boundry - rcv_buffer) + 2;

            ESP_LOGI(TAG_http, "BOUNDARY FOUND, i now=%d", i);
            current_state = READING_THROUGH_HEADER;
          } else {
            ESP_LOGW(TAG_http,
                     "boundary found but not followed by CRLF, skipping");
            i++;
          }
        } else {
          ESP_LOGW(TAG_http, "boundary not found in chunk, setting carry=%d",
                   total - i);
          carry = total - i;
          break;
        }

      } else if (current_state == READING_THROUGH_HEADER) {
        ESP_LOGI(TAG_http, "READING_THROUGH_HEADER: i=%d", i);
        char *line_start = rcv_buffer + i;
        char *line_end = strstr(line_start, "\r\n");
        if (!line_end) {
          ESP_LOGW(TAG_http,
                   "no CRLF found, line split across chunks, carry=%d",
                   total - i);
          carry = total - i;
          break;
        }
        ESP_LOGI(TAG_http, "line_end found at offset=%d",
                 (int)(line_end - rcv_buffer));

        char *name_start = strstr(line_start, "name=\"");
        if (!name_start || name_start > line_end) {
          ESP_LOGW(TAG_http, "name= not found in line, carry=%d", total - i);
          carry = total - i;
          break;
        }
        name_start += strlen("name=\"");
        char *name_end = memchr(name_start, '"', line_end - name_start);
        if (!name_end) {
          ESP_LOGW(TAG_http, "closing quote for name not found, carry=%d",
                   total - i);
          carry = total - i;
          break;
        }

        free(name);
        size_t name_len = name_end - name_start;
        name = malloc(name_len + 1);
        if (!name) {
          free(content_type);
          free(file_name);
          httpd_resp_send_500(req);
          return ESP_FAIL;
        }
        memcpy(name, name_start, name_len);
        name[name_len] = '\0';
        ESP_LOGI(TAG_http, "extracted name='%s'", name);

        char *fname_start = strstr(name_end, "filename=\"");
        if (!fname_start) {
          ESP_LOGW(TAG_http, "filename= not found in line, carry=%d",
                   total - i);
          carry = total - i;
          break;
        }
        fname_start += strlen("filename=\"");
        char *fname_end = memchr(fname_start, '"', line_end - fname_start);
        if (!fname_end) {
          ESP_LOGW(TAG_http, "closing quote for filename not found, carry=%d",
                   total - i);
          carry = total - i;
          break;
        }

        free(file_name);
        size_t fname_len = fname_end - fname_start;
        file_name = malloc(fname_len + 1);
        if (!file_name) {
          free(content_type);
          free(name);
          httpd_resp_send_500(req);
          return ESP_FAIL;
        }
        memcpy(file_name, fname_start, fname_len);
        file_name[fname_len] = '\0';
        ESP_LOGI(TAG_http, "extracted filename='%s'", file_name);

        i = (line_end - rcv_buffer) + 2;
        ESP_LOGI(TAG_http, "READING_THROUGH_HEADER done, i now=%d", i);
        current_state = READING_FILE_TYPE;

      }

      else if (current_state == READING_FILE_TYPE) {
        ESP_LOGI(TAG_http, "READING_FILE_TYPE: i=%d", i);
        char *content_type_h = strstr(rcv_buffer + i, "Content-Type: ");
        if (content_type_h) {
          ESP_LOGI(TAG_http, "Content-Type header found at offset=%d",
                   (int)(content_type_h - rcv_buffer));
          content_type_h += strlen("Content-Type: ");
          char *content_type_end = strstr(content_type_h, "\r\n");

          if (!content_type_end) {
            ESP_LOGW(TAG_http, "no CRLF after Content-Type, carry=%d",
                     total - i);
            carry = total - i;
            break;
          }

          size_t ct_len = content_type_end - content_type_h;
          content_type_l = malloc(ct_len + 1);
          if (!content_type_l) {
            free(content_type);
            free(name);
            free(file_name);
            httpd_resp_send_500(req);
            return ESP_FAIL;
          }

          int j = 0;
          while (content_type_h + j < content_type_end) {
            content_type_l[j] = content_type_h[j];
            j++;
          }
          content_type_l[j] = '\0';
          ESP_LOGI(TAG_http, "file Content-Type='%s'", content_type_l);

          i = (content_type_end - rcv_buffer) + 2;
          ESP_LOGI(TAG_http, "after Content-Type line, i=%d", i);

          if (i + 2 <= total && strncmp(rcv_buffer + i, "\r\n", 2) == 0) {
            i += 2;
            ESP_LOGI(TAG_http, "skipped blank separator line, i=%d", i);
          } else {
            ESP_LOGW(TAG_http, "no blank separator line found at i=%d", i);
          }

          if (strncmp(content_type_l, "audio/mpeg", strlen("audio/mpeg")) !=
              0) {
            ESP_LOGE(TAG_http, "not valid file type: %s", content_type_l);
            free(content_type);
            free(name);
            free(file_name);
            free(content_type_l);
            httpd_resp_send_408(req);
            return ESP_FAIL;
          }

          FILE *names_file = fopen("/files/names.txt", "a");
          if (!names_file) {
            free(content_type);
            free(name);
            free(file_name);
            free(content_type_l);
            ESP_LOGE(TAG_http, "can't open /files/names.txt");
            httpd_resp_send_500(req);
            return ESP_FAIL;
          }

          char *out_path = malloc(8 + strlen(file_name) + 1);
          snprintf(out_path, strlen("/sdcard/") + strlen(file_name) + 1,
                   "/sdcard/%s", file_name);

          ESP_LOGI(TAG_http,
                   "path = %s, file_name = %s, out_path len = %d, size : %d",
                   out_path, file_name,
                   8 + strlen(file_name) + 1);

          fprintf(names_file, "%s\n", out_path);
          fclose(names_file);
          ESP_LOGI(TAG_http, "wrote name to names.txt");

          out_file = fopen(out_path, "wb");

          if (!out_file) {
            ESP_LOGE(TAG_http, "can't create file <%s>", out_path);
            free(out_path);
            free(content_type);
            free(name);
            free(file_name);
            free(content_type_l);
            httpd_resp_send_500(req);
            return ESP_FAIL;
          }
          free(out_path);
          ESP_LOGI(TAG_http, "READING_FILE_TYPE done, file opened, i=%d", i);
          current_state = READING_BINARY;

        } else {
          ESP_LOGW(TAG_http, "Content-Type header not found yet, carry=%d",
                   total - i);
          carry = total - i;
          break;
        }

      } else if (current_state == READING_BINARY) {
        ESP_LOGD(TAG_http, "READING_BINARY: i=%d total=%d", i, total);
        char *end_line =
            memmem(rcv_buffer + i, total - i, end_mark, boundary_len + 2);
        int written;

        if (end_line) {
          ESP_LOGI(TAG_http, "end boundary found at offset=%d",
                   (int)(end_line - rcv_buffer));
          written = fwrite(rcv_buffer + i, 1,
                           (long)(end_line - (rcv_buffer + i)), out_file);
          ESP_LOGI(TAG_http, "written %d bytes to <%s>", written, name);
          ESP_LOGW(TAG_http, "FINISHED");
          current_state = FINISHED;
          break;
        } else {
          written = fwrite(rcv_buffer + i, 1, total - i, out_file);
          ESP_LOGI(TAG_http, "written %d bytes to <%s> (partial)", written,
                   name);
          i += written;
        }

      } else if (current_state == FINISHED) {
        ESP_LOGI(TAG_http, "state=FINISHED, breaking inner loop");
        done = true;
        break;
      }
    }

    if (carry > 0) {
      ESP_LOGI(TAG_http, "moving %d carry bytes to buffer start", carry);
      memmove(rcv_buffer, rcv_buffer + (total - carry), carry);
    }
    carry = 0;
  }

  ESP_LOGI(TAG_http, "loop done: rcv_len=%d content_len=%d", rcv_len,
           content_len);

  free(content_type);
  free(name);
  free(file_name);
  free(content_type_l);

  if (out_file != NULL)
    fclose(out_file);

  httpd_resp_sendstr(req, "UPLOAD DONE");
  return ESP_OK;
}

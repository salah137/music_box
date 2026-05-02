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

#define CHUNCK_SIZE 1024
#define BOUNDRY_SIZE 64
#define OVERLAP BOUNDRY_SIZE * 2

esp_err_t upload_music_handler(httpd_req_t *req) {
  // getting the boundaries
  size_t ct_size = httpd_req_get_hdr_value_len(req, "Content-Type");

  // FIX 1: allocate ct_size + 1 to have room for the null terminator
  char content_type[ct_size + 1];
  httpd_req_get_hdr_value_str(req, "Content-Type", content_type, ct_size + 1);

  char *b = strstr(content_type, "boundary=");
  if (!b) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not valid headers");
    return ESP_FAIL;
  }

  char boundary[BOUNDRY_SIZE];
  snprintf(boundary, sizeof(boundary), "\r\n--%s", b + 9);
  // boundary_len used everywhere instead of sizeof(boundary)
  size_t boundary_len = strlen(boundary); // FIX 2: use actual string length, not array size

  int buff_size = CHUNCK_SIZE + OVERLAP;
  char *buff = malloc(buff_size);

  int total_len = req->content_len;
  int received = 0;
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
  int fiel_val_len = 0; // FIX 3: was char (max 127), use int

  int result = ESP_OK;

  while (received < total_len) {
    // FIX 4: read into buff + carry_len so the overlap region is preserved;
    //        limit to_read accordingly
    int space = buff_size - carry_len;
    int to_read = fmin(total_len - received, space);
    int ret = httpd_req_recv(req, buff + carry_len, to_read);

    if (ret == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (ret < 0) {
      result = ESP_FAIL;
      break;
    }

    received += ret;
    int data_len = carry_len + ret; // actual valid bytes in buffer this round
    int i = 0;

    while (i < data_len) {
      if (current_state == FINDING_BOUNDRIES_STATE) {
        // FIX 2 (applied): boundary_len instead of sizeof(boundary)
        char *found = memmem(buff + i, data_len - i, boundary, boundary_len);

        if (found) {
          // FIX 5: was (buff - found) which is negative; correct direction
          i = (found - buff) + boundary_len;
          if (i + 2 <= data_len)
            i += 2; // skip \r\n after boundary

          current_state = READING_HEADER_STATE;
          memset(field_name, 0, sizeof(field_name));
        } else {
          // keep the overlap tail for the next chunk
          i = data_len - OVERLAP;
          if (i < 0)
            i = 0;
          break;
        }

      } else if (current_state == READING_HEADER_STATE) {
        char *hdr_end = memmem(buff + i, data_len - i, "\r\n", 2);
        if (!hdr_end) {
          break;
        }

        // empty line = end of headers, next state depends on what we found
        if (hdr_end == buff + i) {
          i += 2;
          if (out_file)
            current_state = READING_FILE_STATE;
          else if (field_name[0])
            current_state = READING_FIELD_STATE;
          continue;
        }

        *hdr_end = '\0';
        char *header = buff + i;

        char *name_ptr = strstr(header, "name=\"");
        if (name_ptr) {
          name_ptr += 6;
          // FIX 6: was strchr(buff + i, '"') — starts at wrong place, finds the
          //        opening quote. Must start from name_ptr to find the closing one.
          char *name_end = strchr(name_ptr, '"');
          if (name_end) {
            int len = fmin(name_end - name_ptr, (int)sizeof(field_name) - 1);
            strncpy(field_name, name_ptr, len);
            field_name[len] = '\0';

            FILE* files_names = fopen("/files/file_names.txt","a");
            fprintf(files_names, "%s",field_name);
            fclose(files_names);
            
            ESP_LOGI(TAG_http, "HONOROOOOBLE");
          }
        }

        char *fn_ptr = strstr(header, "filename=\"");
        if (fn_ptr) {
          // FIX 7: "filename=\"" is 10 chars, not 9
          fn_ptr += 10;
          char file_name[32] = "upload.mp3";

          char *fn_end = strchr(fn_ptr, '"');
          if (fn_end) {
            size_t fn_len = fmin(fn_end - fn_ptr, (int)sizeof(file_name) - 1);
            strncpy(file_name, fn_ptr, fn_len);
            file_name[fn_len] = '\0';

            char out_path[64];
            snprintf(out_path, sizeof(out_path), "/sdcard/%s", file_name);

            out_file = fopen(out_path, "wb");
            if (!out_file) {
              ESP_LOGE(TAG_http, "unable to open the file %s", out_path);
              // FIX 8: set result before goto so cleanup returns the right code
              result = ESP_FAIL;
              goto cleanup;
            }

            ESP_LOGI(TAG_http, "saving the file %s", out_path);
            // state is set when the blank header line is reached (above)
          } else {
            break;
          }
        }

        // FIX 9: advance i past this header line so we don't re-parse it forever
        i = (hdr_end - buff) + 2;

      } else if (current_state == READING_FILE_STATE ||
                 current_state == READING_FIELD_STATE) {
        // FIX 2 (applied): boundary_len
        char *next = memmem(buff + i, data_len - i, boundary, boundary_len);

        if (next) {
          if (current_state == READING_FILE_STATE && out_file) {
            fwrite(buff + i, 1, next - (buff + i), out_file);
            fclose(out_file);
            out_file = NULL;
            // FIX 10: corrected ESP_LOGI argument order (tag first, then format)
            ESP_LOGI(TAG_http, "file saved");
          } else if (current_state == READING_FIELD_STATE) {
            size_t len = fmin(next - (buff + i), (int)sizeof(field_value) - 1);
            strncpy(field_value, buff + i, len);
            field_value[len] = '\0';
          }

          i = (next - buff) + boundary_len + 2; // skip boundary + \r\n
          current_state = READING_HEADER_STATE;
          memset(field_name, 0, sizeof(field_name));

        } else {
          int safe_len = data_len - i - OVERLAP;
          if (safe_len > 0) {
            // FIX 11: was duplicate READING_FIELD_STATE check; second branch
            //         must be READING_FILE_STATE to actually write file data
            if (current_state == READING_FILE_STATE && out_file) {
              fwrite(buff + i, 1, safe_len, out_file);
            } else if (current_state == READING_FIELD_STATE) {
              int copy = fmin(safe_len,
                              (int)sizeof(field_value) - fiel_val_len - 1);
              if (copy > 0) {
                strncpy(field_value + fiel_val_len, buff + i, copy);
                fiel_val_len += copy;
              }
            }
            i += safe_len;
          }
          break;
        }
      }
    } // inner while

    // FIX 4 (cont.): carry the tail bytes to the front for the next iteration
    carry_len = data_len - i;
    if (carry_len > 0 && carry_len <= OVERLAP) {
      memmove(buff, buff + i, carry_len);
    } else {
      carry_len = 0;
    }
  } // outer while

cleanup:
  if (out_file)
    fclose(out_file);

  free(buff);

  if (result != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    return ESP_FAIL;
  }

  httpd_resp_sendstr(req, "Upload OK \n");
  return ESP_OK;
}
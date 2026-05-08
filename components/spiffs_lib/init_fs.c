#include "esp_log.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "spiffs";

void init_fs() {
  ESP_LOGI(TAG, "preparing spiff");
  esp_vfs_spiffs_conf_t config = {.base_path = "/files",
                                  .partition_label = NULL,
                                  .max_files = 5,
                                  .format_if_mount_failed = false};

  esp_err_t result = esp_vfs_spiffs_register(&config);

  if (result != ESP_OK) {
    ESP_LOGE(TAG, "enable to initialize the SPIFF (%s)",
             esp_err_to_name(result));
  }

  size_t total = 0, used = 0;

  result = esp_spiffs_info(config.partition_label, &total, &used);

  if (result != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get partition infos %s", esp_err_to_name(result));
  } else {
    ESP_LOGI(TAG, "used %d, free %d", used, total);
  }
}

char **get_names(int * count) {
    DIR *dir = opendir("/files");
    if (dir) {
        ESP_LOGI(TAG, "helllo");
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "found file: %s", entry->d_name);
        }
        closedir(dir);
    } else {
        ESP_LOGE(TAG, "failed to open /files directory");
    }

    
    int max_arr_len = 4;
    int current_arr_len = 0;
    char **arr = malloc((max_arr_len + 1) * sizeof(char *));
    if (!arr) return NULL;

    FILE *names = fopen("/files/names.txt", "r");
    
    if (!names) {
        ESP_LOGE(TAG, "unable to open names.txt");
        free(arr);
        return NULL;
    }

    int c;
    char current[100];
    int c_pin = 0;
    *current = 0;
    
    while ((c = fgetc(names)) != EOF) {
        if ((char)c == '\n') {
            if (c_pin == 0) continue;  // skip empty lines
            current[c_pin] = '\0';
            ESP_LOGI(TAG, "name%d: %s", current_arr_len, current);
            *count += 1;
            
            if (current_arr_len >= max_arr_len) {
                max_arr_len += 4;
                char **re = realloc(arr, (max_arr_len + 1) * sizeof(char *));
                if (!re) {
                    ESP_LOGE(TAG, "realloc failed");
                    fclose(names);
                    free(arr);
                    return NULL;
                }
                arr = re;
            }

            arr[current_arr_len] = malloc(c_pin + 1);
            if (!arr[current_arr_len]) {
                fclose(names);
                free(arr);
                return NULL;
            }
            memcpy(arr[current_arr_len], current, c_pin + 1);
            current_arr_len++;
            c_pin = 0;

        } else {
            if (c_pin >= 99) {
                ESP_LOGE(TAG, "name too long, truncating");
                c_pin = 99;
                continue;
            }
            current[c_pin++] = (unsigned char)c;
        }
    }

    fclose(names);
    arr[current_arr_len] = NULL;  // NULL sentinel
    return arr;
}
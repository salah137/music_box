#include "esp_spiffs.h"
#include "esp_log.h"

static const char* TAG = "spiffs";

void init_fs(){
    ESP_LOGI(TAG,"preparing spiff");
    esp_vfs_spiffs_conf_t config = {
        .base_path = "/files",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };

    esp_err_t result = esp_vfs_spiffs_register(&config);
    
    if(result != ESP_OK){
        ESP_LOGE(TAG,"enable to initialize the SPIFF (%s)", esp_err_to_name(result));
    }

    size_t total = 0, used = 0;

    result = esp_spiffs_info(config.partition_label,&total, &used);

    if(result != ESP_OK){
        ESP_LOGE(TAG,"Failed to get partition infos %s", esp_err_to_name(result));
    }else {
        ESP_LOGI(TAG,"used %d, free %d", used,total);
    }

}
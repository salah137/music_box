idf_component_register(SRCS "main.c" "ui/ui_components.c" "reusable/reusable.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES i2c_oled wifi_ap spiffs_lib bt_comp  types sd_card
                    REQUIRES esp_https_server  nvs_flash esp_driver_gpio 

)

spiffs_create_partition_image(files ../partition FLASH_IN_PROJECT)
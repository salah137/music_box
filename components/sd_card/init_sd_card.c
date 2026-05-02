#include "esp_err.h"
#include "hal/spi_types.h"
#include "sd_protocol_types.h"
#include "stdio.h"
#include "string.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"

static const char *TAG = "sd_spi";

#define MOUNT_PATH "/sdcard"
#define MISO  CONFIG_MISO
#define MOSI  CONFIG_MOSI
#define CS    CONFIG_CS
#define SCLK  CONFIG_CLK

void init_sd_card(void) {
    esp_err_t ret;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *sd_card;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = MOSI,
        .miso_io_num     = MISO,
        .sclk_io_num     = SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,   // ← fixed from 400
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    ESP_ERROR_CHECK(ret);          // ← added

    sdspi_device_config_t slot_device = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_device.gpio_cs  = CS;
    slot_device.host_id  = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 400;       // ← start slow for debugging

    ret = esp_vfs_fat_sdspi_mount(MOUNT_PATH, &host, &slot_device, &mount_cfg, &sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return;
    }

    sdmmc_card_print_info(stdout, sd_card);
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_PATH);
}
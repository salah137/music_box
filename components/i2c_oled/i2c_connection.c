#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SSD1306_ADRESS 0x3c
#define FRQ 400000

#define COMMAND 0x00
#define DATA 0x40

struct {
  i2c_master_bus_handle_t master_handler;
  i2c_master_dev_handle_t handler;
} typedef screen_t;

bool init_bus(int sda, int scl, screen_t *screen) {
  i2c_master_bus_config_t master_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = sda,
      .scl_io_num = scl,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };

  ESP_ERROR_CHECK(i2c_new_master_bus(&master_config, &screen->master_handler));

  printf("Probing SSD1306...\n");
  if (i2c_master_probe(screen->master_handler, SSD1306_ADRESS,
                       pdMS_TO_TICKS(100)) != ESP_OK) {
    printf("SSD1306 NOT detected\n");
    return false;
  }

  printf("SSD1306 detected\n");

  i2c_device_config_t dev_config = {
      .dev_addr_length = I2C_ADDR_BIT_7,
      .device_address = SSD1306_ADRESS,
      .scl_speed_hz = FRQ,
  };

  ESP_ERROR_CHECK(i2c_master_bus_add_device(screen->master_handler, &dev_config,
                                            &screen->handler));
  return true;
}

void ssd1306_command(uint8_t command, screen_t *screen) {
  static WORD_ALIGNED_ATTR uint8_t mode = COMMAND;
  static WORD_ALIGNED_ATTR uint8_t data;

  data = command;

  static WORD_ALIGNED_ATTR i2c_master_transmit_multi_buffer_info_t infos[2];

  infos[0].write_buffer = &mode;
  infos[0].buffer_size = 1;
  infos[1].write_buffer = &data;
  infos[1].buffer_size = 1;

  ESP_ERROR_CHECK(i2c_master_multi_buffer_transmit(screen->handler, infos, 2,
                                                   pdMS_TO_TICKS(200)));
}

void ssd1306_data(uint8_t *data, int len, screen_t *screen) {
  if (!data || len <= 0)
    return;

  static WORD_ALIGNED_ATTR uint8_t mode = DATA;
  static WORD_ALIGNED_ATTR i2c_master_transmit_multi_buffer_info_t infos[2];

  infos[0].write_buffer = &mode;
  infos[0].buffer_size = 1;
  infos[1].write_buffer = data;
  infos[1].buffer_size = len;

  ESP_ERROR_CHECK(i2c_master_multi_buffer_transmit(screen->handler, infos, 2,
                                                   pdMS_TO_TICKS(200)));
}

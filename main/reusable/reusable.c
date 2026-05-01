#include "driver/gpio.h"

gpio_config_t set_up_interrupt(int pin) {
  gpio_config_t io_config = {.pin_bit_mask = 1ULL << pin,
                             .mode = GPIO_MODE_INPUT,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .intr_type = GPIO_INTR_ANYEDGE};
  return io_config;
}

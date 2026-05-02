#include "ap_init.h"
#include "drawing.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "i2c_connection.h"
#include "init_bt.h"
#include "init_fs.h"
#include "local_types.h"
#include "portmacro.h"
#include "reusable/reusable.h"
#include "ui/ui_components.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "init_sd_card.h"
/* ---------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------*/
#define SDA_PIN 21
#define SCL_PIN 22
#define PAGE_CAPACITY 3
#define MUSIC_COUNT 5

#define BOTTOM_RIGHT CONFIG_BOTTOM_RIGHT
#define BOTTOM_LEFT CONFIG_BOTTOM_LEFT
#define TOP_RIGHT CONFIG_TOP_RIGHT
#define TOP_LEFT CONFIG_TOP_LEFT

/* Debounce / UI timing (ms) */
#define DEBOUNCE_MS 100
#define DOUBLE_CLICK_MS 250
#define VOLUME_SHOW_MS 2000

/* ---------------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------------*/
typedef enum { SELECT_AUDIO, LISTNING_AUDIO, LISTING_BT } app_mode_t;

/* ---------------------------------------------------------------------------
 * Shared state – grouped by concern
 * -------------------------------------------------------------------------*/

/* Synchronisation primitives */
static QueueHandle_t s_input_queue;
static SemaphoreHandle_t s_scanning_mutex;
static SemaphoreHandle_t s_bt_mutex;

/* UI / navigation */
static app_mode_t s_mode = SELECT_AUDIO;
static int s_pinned = 0;
static int s_start_index = 0;

/* Audio */
static music_t s_music[MUSIC_COUNT] = {0};
static int s_music_len = MUSIC_COUNT;
static music_t s_selected = {0};
static int s_volume_level = 50;
static bool s_paused = false;
static int s_volume_changed_at = 0; /* timestamp of last volume change   */

/* Bluetooth */
static bt_device_t **s_bt_devices = NULL;
static int s_bt_len = 0;
static int s_bt_max_len = 4;
static int s_bt_selected = -1;
static bool s_bt_connected = false;
static bool s_scanning = true;

/* Wi-Fi */
static bool s_wifi_connected = false;

/* ISR helpers */
static int s_last_push = 0;
static bool s_set_high = true;

/* ---------------------------------------------------------------------------
 * Timing helper
 * -------------------------------------------------------------------------*/
static inline int millis(void) {
  return (int)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---------------------------------------------------------------------------
 * Scanning flag (mutex-protected)
 * -------------------------------------------------------------------------*/
static void set_scanning(bool val) {
  xSemaphoreTake(s_scanning_mutex, portMAX_DELAY);
  s_scanning = val;
  xSemaphoreGive(s_scanning_mutex);
}

static bool get_scanning(void) {
  xSemaphoreTake(s_scanning_mutex, portMAX_DELAY);
  bool val = s_scanning;
  xSemaphoreGive(s_scanning_mutex);
  return val;
}

/* ---------------------------------------------------------------------------
 * List navigation helpers
 * -------------------------------------------------------------------------*/
static void nav_next(int list_len) {
  if (s_pinned >= list_len - 1) {
    s_pinned = 0;
    s_start_index = 0;
  } else {
    s_pinned++;
  }
  if (s_pinned >= PAGE_CAPACITY)
    s_start_index++;

  ESP_LOGI("nav", "pinned=%d  start=%d  len=%d", s_pinned, s_start_index,
           list_len);
}

static void nav_prev(void) {
  if (s_pinned == 0)
    return;

  s_pinned--;
  if ((s_pinned % 3 == 1 || s_start_index == 1) && s_start_index > 0)
    s_start_index--;

  ESP_LOGI("nav", "pinned=%d  start=%d", s_pinned, s_start_index);
}

/* ---------------------------------------------------------------------------
 * ISR
 * -------------------------------------------------------------------------*/
static void IRAM_ATTR isr_handler(void *args) {
  int now = millis();
  if (s_last_push != 0 && now - s_last_push <= DEBOUNCE_MS)
    return;

  int pin = (int)args;
  BaseType_t hp = pdFALSE;

  if (!gpio_get_level(pin)) {
    s_set_high = false;
  } else {
    s_set_high = true;
    xQueueSendFromISR(s_input_queue, &pin, &hp);
  }

  s_last_push = now;
  xQueueSendFromISR(s_input_queue, &pin, &hp);
  if (hp)
    portYIELD_FROM_ISR();
}

/* ---------------------------------------------------------------------------
 * Input handlers – one function per mode for readability
 * -------------------------------------------------------------------------*/
static void handle_select_audio(int pressed) {
  switch (pressed) {
  case TOP_RIGHT:
    nav_next(s_music_len);
    break;
  case TOP_LEFT:
    nav_prev();
    break;
  case BOTTOM_RIGHT:
    s_selected = s_music[s_pinned];
    s_mode = LISTNING_AUDIO;
    break;
  case BOTTOM_LEFT:
    s_pinned = 0;
    s_start_index = 0;
    s_mode = LISTING_BT;
    set_scanning(true);
    break;
  default:
    break;
  }
}

static void handle_listening_audio(int pressed, int press_count) {
  static int volume_start_ms = 0;

  switch (pressed) {
  case BOTTOM_RIGHT:
    s_paused = !s_paused;
    break;
  case BOTTOM_LEFT:
    s_mode = SELECT_AUDIO;
    break;

  case TOP_RIGHT:
    if (press_count == 0) {
      if (!s_set_high) {
        volume_start_ms = millis();
      } else {
        s_volume_changed_at = millis();
        int delta = (s_volume_changed_at - volume_start_ms) / 100;
        if (s_volume_level < 100)
          s_volume_level += delta;
      }
    } else if (press_count == 1) {
      if (s_pinned == s_music_len)
        s_pinned = 0;
      s_selected = s_music[++s_pinned];
    }
    break;

  case TOP_LEFT:
    if (press_count == 0) {
      if (!s_set_high) {
        volume_start_ms = millis();
      } else {
        s_volume_changed_at = millis();
        int delta = (s_volume_changed_at - volume_start_ms) / 100;
        if (s_volume_level > 0)
          s_volume_level -= delta;
      }
    } else if (press_count == 1) {
      if (s_pinned == 0)
        s_pinned = s_music_len;
      s_selected = s_music[--s_pinned];
    }
    break;

  default:
    break;
  }
}

static void handle_listing_bt(int pressed, int press_count) {
  switch (pressed) {
  case BOTTOM_LEFT:
    if (press_count == 0) {
      set_scanning(true);
    } else if (press_count == 1) {
      s_mode = SELECT_AUDIO;
    }
    break;

  case TOP_RIGHT:
    xSemaphoreTake(s_bt_mutex, portMAX_DELAY);
    nav_next(s_bt_len);
    xSemaphoreGive(s_bt_mutex);
    break;

  case TOP_LEFT:
    nav_prev();
    break;

  case BOTTOM_RIGHT:
    if (press_count == 0) {
      xSemaphoreTake(s_bt_mutex, portMAX_DELAY);
      s_bt_selected = s_pinned;
      set_scanning(true);
      xSemaphoreGive(s_bt_mutex);
    } else if (press_count == 1) {
      disconnect();
      s_bt_selected = -1;
      s_bt_connected = false;
    }
    break;

  default:
    break;
  }
}

/* ---------------------------------------------------------------------------
 * Input task
 * -------------------------------------------------------------------------*/
static void handle_input(void *args) {
  /* Configure GPIO interrupts for all four buttons. */
  gpio_config_t tr = set_up_interrupt(TOP_RIGHT);
  gpio_config_t tl = set_up_interrupt(TOP_LEFT);
  gpio_config_t bl = set_up_interrupt(BOTTOM_LEFT);
  gpio_config_t br = set_up_interrupt(BOTTOM_RIGHT);

  gpio_config(&tr);
  gpio_config(&tl);
  gpio_config(&bl);
  gpio_config(&br);

  gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  gpio_isr_handler_add(TOP_RIGHT, isr_handler, (void *)TOP_RIGHT);
  gpio_isr_handler_add(TOP_LEFT, isr_handler, (void *)TOP_LEFT);
  gpio_isr_handler_add(BOTTOM_LEFT, isr_handler, (void *)BOTTOM_LEFT);
  gpio_isr_handler_add(BOTTOM_RIGHT, isr_handler, (void *)BOTTOM_RIGHT);

  int last_pressed = 0;
  int last_click = 0;
  int press_count = 0;
  int pressed = 0;

  while (1) {
    if (xQueueReceive(s_input_queue, &pressed, pdMS_TO_TICKS(100)) != pdTRUE)
      continue;

    /* Detect double-click (second press of same button within window). */
    if (millis() - last_click < DOUBLE_CLICK_MS && pressed == last_pressed &&
        !s_set_high) {
      press_count++;
    }

    switch (s_mode) {
    case SELECT_AUDIO:
      handle_select_audio(pressed);
      break;
    case LISTNING_AUDIO:
      handle_listening_audio(pressed, press_count);
      break;
    case LISTING_BT:
      handle_listing_bt(pressed, press_count);
      break;
    default:
      break;
    }

    last_pressed = pressed;
    press_count = 0;
    last_click = millis();

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* ---------------------------------------------------------------------------
 * Draw task
 * -------------------------------------------------------------------------*/
static void draw_ui_task(void *args) {
  screen_t oled;
  init(SDA_PIN, SCL_PIN, &oled);

  float progress = 0.0f;

  while (1) {
    int now = millis();
    ssd1306_clear_fb();

    /* Header: show connected BT device name or "none". */
    const char *bt_name = (s_bt_connected && s_bt_selected >= 0)
                              ? s_bt_devices[s_bt_selected]->name
                              : "none";
    draw_header((char *)bt_name, s_wifi_connected, 70);

    switch (s_mode) {
    case SELECT_AUDIO:
      draw_select_music(s_music, s_start_index, s_music_len - 1, s_pinned);
      break;

    case LISTNING_AUDIO: {
      bool show_volume = (s_volume_changed_at != 0 &&
                          now - s_volume_changed_at < VOLUME_SHOW_MS);
      draw_music_control(s_selected.name, progress, s_selected.duration,
                         s_paused, show_volume, s_volume_level);
      if (!s_paused) {
        progress += 0.1f;
        if (progress >= 100.0f)
          progress = 0.0f;
      }
      break;
    }

    case LISTING_BT:
      xSemaphoreTake(s_bt_mutex, portMAX_DELAY);
      bt_device_arr_ui(s_bt_devices, s_start_index, s_bt_len, s_pinned);
      xSemaphoreGive(s_bt_mutex);
      break;

    default:
      ESP_LOGE("draw", "Unknown mode: %d", s_mode);
      break;
    }

    ssd1306_update(&oled);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* ---------------------------------------------------------------------------
 * BT task
 * -------------------------------------------------------------------------*/
static void bt_task(void *args) {
  s_bt_devices = malloc(sizeof(bt_device_t *) * s_bt_max_len);
  init_bt(&s_bt_devices, &s_bt_max_len, &s_bt_len, &s_bt_selected,
          &s_bt_connected);

  while (1) {
    xSemaphoreTake(s_bt_mutex, portMAX_DELAY);
    if (get_scanning()) {
      s_bt_len = 0;
      scan_refresh();
      set_scanning(false);
    }
    xSemaphoreGive(s_bt_mutex);

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/* ---------------------------------------------------------------------------
 * Wi-Fi task
 * -------------------------------------------------------------------------*/
static void wifi_task(void *args) {
  wifi_softap_init(&s_wifi_connected);
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/* SD Card task */
static void sdcard_task(void* args){
    init_sd_card();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ---------------------------------------------------------------------------
 * Static data initialisation
 * -------------------------------------------------------------------------*/
static void fill_music_library(void) {
  s_music[0] = (music_t){.name = "I came", .duration = 400};
  s_music[1] = (music_t){.name = "I saw", .duration = 500};
  s_music[2] = (music_t){.name = "I wanted", .duration = 500};
  s_music[3] = (music_t){.name = "I conquered", .duration = 500};
  s_music[4] = (music_t){.name = "I failed", .duration = 500};
}

/* ---------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------*/
void app_main(void) {
  s_input_queue = xQueueCreate(10, sizeof(int));
  s_scanning_mutex = xSemaphoreCreateMutex();
  s_bt_mutex = xSemaphoreCreateMutex();

  fill_music_library();
  init_fs();

  xTaskCreatePinnedToCore(bt_task, "bt_task", 4096, NULL, 6, NULL, 0);
  xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, NULL, 5, NULL, 1);
  xTaskCreate(draw_ui_task, "draw_ui", 2048, NULL, 5, NULL);
  xTaskCreate(handle_input, "handle_input", 2048, NULL, 4, NULL);
  xTaskCreate(sdcard_task, "sd_card", 4096, NULL, 3, NULL);
  
}

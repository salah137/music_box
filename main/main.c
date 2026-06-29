#include "ap_init.h"
#include "drawing.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/ringbuf.h"
#include "i2c_connection.h"
#include "init_bt.h"
#include "init_fs.h"
#include "init_sd_card.h"
#include "local_types.h"
#include "portmacro.h"
#include "reusable/reusable.h"
#include "ui/ui_components.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

/* ---------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------*/
#define SDA_PIN 21
#define SCL_PIN 22
#define PAGE_CAPACITY 3

#define BOTTOM_RIGHT CONFIG_BOTTOM_RIGHT
#define BOTTOM_LEFT CONFIG_BOTTOM_LEFT
#define TOP_RIGHT CONFIG_TOP_RIGHT
#define TOP_LEFT CONFIG_TOP_LEFT

/* Debounce / UI timing (ms) */
#define DEBOUNCE_MS 150
#define DOUBLE_CLICK_MS 250
#define VOLUME_SHOW_MS 2000

#define MP3_BUF_SIZE 1024

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
static SemaphoreHandle_t s_rm_mutex;

/* UI / navigation */
static app_mode_t s_mode = SELECT_AUDIO;
static int s_pinned = 0;
static int s_start_index = 0;

/* Audio */
static int music_count = 0;
music_t **s_music = {0};
static int s_music_len = 0;
static music_t *s_selected = {0};

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
static bool s_changed = false;
static bool *w_params[2] = {&s_wifi_connected, &s_changed};

/* ISR helpers */
static int s_last_push = 0;
static bool s_set_high = true;


RingbufHandle_t rb;

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
  case BOTTOM_LEFT: {
    s_selected = NULL;
    s_mode = SELECT_AUDIO;
    break;
  }
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
      draw_select_music(s_music, s_start_index, s_music_len, s_pinned);
      break;

    case LISTNING_AUDIO: {
      bool show_volume = (s_volume_changed_at != 0 &&
                          now - s_volume_changed_at < VOLUME_SHOW_MS);
      draw_music_control(s_selected->name + strlen("/sdcard/"), progress,
                         s_selected->duration, s_paused, show_volume,
                         s_volume_level);
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
  if (rb != NULL) {
    init_bt(&s_bt_devices, &s_bt_max_len, &s_bt_len, &s_bt_selected,
            &s_bt_connected, &s_paused, &rb);
  }

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

  xSemaphoreTake(s_rm_mutex, portMAX_DELAY);
  wifi_softap_init(w_params);
  xSemaphoreGive(s_rm_mutex);

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/* ---------------------------------------------------------------------------
 * SD Card task
 * ---------------------------------------------------------------------------*/
static mp3dec_t mp3d;
static uint8_t mp3_buf[4096];
static int     mp3_valid = 0;

static void sdcard_task(void *args) {
    init_sd_card();
    mp3dec_init(&mp3d);

    FILE *music_file = NULL;
    char *current    = NULL;

    while (1) {
        if (s_selected == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Open or reopen on track change
        if (music_file == NULL || current == NULL ||
            strcmp(current, s_selected->name) != 0) {

            if (music_file) fclose(music_file);
            free(current);
            mp3_valid = 0;  // flush input buffer on track change
            mp3dec_init(&mp3d);  // reset decoder state

            music_file = fopen(s_selected->name, "rb");
            current    = malloc(strlen(s_selected->name) + 1);
            strcpy(current, s_selected->name);

            if (!music_file) {
                ESP_LOGE("HOLA", "failed to open %s", s_selected->name);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        // Refill MP3 input buffer
        if (mp3_valid < (int)sizeof(mp3_buf)) {
            memmove(mp3_buf, mp3_buf, mp3_valid);  // already at front
            int space = sizeof(mp3_buf) - mp3_valid;
            int read  = fread(mp3_buf + mp3_valid, 1, space, music_file);
            if (read == 0) {
                // EOF — loop
                fseek(music_file, 0, SEEK_SET);
                mp3_valid = 0;
                continue;
            }
            mp3_valid += read;
        }

        // Decode one frame
        static mp3dec_frame_info_t info;
        static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
        int samples = mp3dec_decode_frame(&mp3d, mp3_buf, mp3_valid, pcm, &info);

        if (info.frame_bytes == 0) {
            // Decoder needs more data — shouldn't happen with 4KB buffer
            mp3_valid = 0;
            continue;
        }

        // Advance input buffer
        mp3_valid -= info.frame_bytes;
        memmove(mp3_buf, mp3_buf + info.frame_bytes, mp3_valid);

        if (samples > 0) {
            // Send PCM to ring buffer (bytes = samples * channels * sizeof(int16_t))
            size_t pcm_bytes = samples * info.channels * sizeof(int16_t);
            xRingbufferSend(rb, pcm, pcm_bytes, portMAX_DELAY);
        }
    }
}


static int fill_music_library(char **names, int count) {
  if (s_music == NULL || music_count == 0) {
    ESP_LOGI("HH", "%d", count);
    s_music = malloc(count * sizeof(music_t *));

    if (!s_music) {
      ESP_LOGE("hh", "unable to malloc heap for <s_music>");
      return -1;
    }

    s_music_len = count;
  } else {
    for (int i = 0; i < music_count; i++) {
      free(s_music[i]);
    }
    s_music = realloc(s_music, count * sizeof(music_t *));
    if (!s_music) {
      ESP_LOGE("hh", "unable to realloc heap for <s_music>");
      return -1;
    }

    s_music_len = count;
  }

  for (int i = 0; i < count; i++) {
    music_t *mus = malloc(sizeof(music_t));
    ESP_LOGI("hh", "sizeof music_t = %d", (int)sizeof(music_t));
    ESP_LOGI("hh", "free heap before loop = %d", (int)esp_get_free_heap_size());
    if (!mus) {
      ESP_LOGE("hh", "unable to malloc heap for <mus>");
      return -1;
    }
    mus->name = names[i];
    s_music[i] = mus;
    ESP_LOGI("hh", "noo %s", s_music[i]->name);
  }

  music_count = count;
  return 0;
}

static void fs_task(void *args) {
  int count = 0;
  init_fs();

  if (fill_music_library(get_names(&count), count) == -1) {
    ESP_LOGE("WTF", "idk what is wrong");
  } else {
    ESP_LOGI("WTF", "the data is got");
  }

  while (1) {
    xSemaphoreTake(s_rm_mutex, portMAX_DELAY);

    if (s_changed == true) {
      if (fill_music_library(get_names(&count), count) == -1) {
        ESP_LOGE("WTF", "idk what is wrong");
      } else {
        ESP_LOGI("WTF", "the data is got");
      }
      s_changed = false;
    }

    xSemaphoreGive(s_rm_mutex);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* ---------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------*/
void app_main(void) {
  s_input_queue = xQueueCreate(10, sizeof(int));
  s_scanning_mutex = xSemaphoreCreateMutex();
  s_bt_mutex = xSemaphoreCreateMutex();
  s_rm_mutex = xSemaphoreCreateMutex();

  ESP_LOGI("MEM", "Heap at start: %lu", esp_get_free_heap_size());
  
  rb = xRingbufferCreate(1024 * 8, RINGBUF_TYPE_NOSPLIT);

  xTaskCreatePinnedToCore(bt_task, "bt_task", 4096, NULL, 6, NULL, 0);
  ESP_LOGI("MEM", "Heap after bt_task: %lu", esp_get_free_heap_size());
  
  xTaskCreatePinnedToCore(wifi_task, "wifi_task", 8192 * 4, NULL, 3, NULL, 1);
  ESP_LOGI("MEM", "Heap after wifi_task: %lu", esp_get_free_heap_size());

  
  xTaskCreate(draw_ui_task, "draw_ui", 2048, NULL, 5, NULL);
  ESP_LOGI("MEM", "Heap after draw_ui task start: %lu", esp_get_free_heap_size());

  xTaskCreate(handle_input, "handle_input", 2048, NULL, 4, NULL);
  ESP_LOGI("MEM", "Heap after handle_input task start: %lu", esp_get_free_heap_size());

  xTaskCreate(sdcard_task, "sd_card", 8192, NULL, 7, NULL);
  ESP_LOGI("MEM", "Heap after sd_card task: %lu", esp_get_free_heap_size());

  xTaskCreate(fs_task, "fs_task", 2048, NULL, 3, NULL);
  ESP_LOGI("MEM", "Heap after fs_take: %lu", esp_get_free_heap_size());

}

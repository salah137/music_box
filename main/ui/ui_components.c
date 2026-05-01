#include "drawing.h"
#include "esp_log.h"
#include "local_types.h"

void draw_music_label(char *name, int index, int pinned) {
  ssd1306_draw_horizental_line(5, 16 * (1 + index), 128, 14, pinned ? 1 : 0);
  draw_string(5, 17 * (1 + index), name, 1, pinned ? 0 : 1);
  ssd1306_draw_horizental_line(5, (17 * (1 + index)) + 10, 128, 1, 1);
}

void draw_select_music(music_t *music, int start_index, int max, int pinned) {
  for (int i = start_index; i < 3 + start_index; i++) {

    draw_music_label(music[i].name, i - start_index, (int)(pinned == i));
  }
}

static void continuing_icon() {
  ssd1306_draw_verticale_line(40, 25, 30, 15, 1);
  ssd1306_draw_verticale_line(70, 25, 30, 15, 1);
}

static void paused_icon() {
  for (int i = 0; i < 40; i++) {
    ssd1306_draw_verticale_line(50 + i * 1, 25 + i / 2, 30 - 1 * i, 1, 1);
  }
}

void draw_music_control(char *name, float progress, int duration, bool stopped,
                        bool changed_volume, int volume_level) {
  draw_string(5, 17, name, 1, 1);
  if (stopped)
    paused_icon();
  else
    continuing_icon();

  ssd1306_draw_horizental_line(1, 60, (int)(progress * 100) / duration, 3, 1);

  if (changed_volume) {
      
    int vy = 64 - (volume_level * 40) / 100;
    ESP_LOGI("volume", "%d", volume_level);
    ESP_LOGI("volume_y", "%d",vy);
    ssd1306_draw_verticale_line(120, vy, 64 - vy, 5, 1);
  }
}

void draw_header(char *bt_name, bool wifi, int battery) {
  draw_string(1, 1, bt_name, 1, 1);

  if (wifi)
    draw_string(40, 1, "Wifi", 1, 1);
  else
    draw_string(40, 1, "no Wifi", 1, 1);

  ssd1306_draw_horizental_line(96, 4, 3, 3, 1);
  ssd1306_draw_rectangle_empty(100, 1 , 10, 30);
  ssd1306_draw_horizental_line(100, 1, battery * 0.01 * 30, 10, 1);
}

// bt UI section
void bt_device_label(char *name, int index, int pinned) {
  ssd1306_draw_horizental_line(5, 16 * (1 + index), 128, 14, pinned ? 1 : 0);
  draw_string(5, 17 * (1 + index), name, 1, pinned ? 0 : 1);
  ssd1306_draw_horizental_line(5, (17 * (1 + index)) + 10, 128, 1, 1);
}

void bt_device_arr_ui(bt_device_t **bt_dv, int start_index, int max, int pinned) {
  for (int i = start_index; i < 3 + start_index && i < max; i++) {
      bt_device_label(bt_dv[i]->name, i - start_index, (int)(pinned == i));
  }
}

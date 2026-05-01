#ifndef UI_COMPONENTS_H
#define UI_COMPONENTS_H

#include "local_types.h"

void draw_music_label(char *name, int index, int pinned);
void draw_select_music(music_t* musics, int start_index, int max,
                       int pinned);
void draw_music_control(char *name, float progress, int duration, bool stopped,
                        bool changed_volume, int volume_level);
void draw_header(char* bt_name, bool wifi, int battery);

void bt_device_arr_ui(bt_device_t **bt_dv, int start_index, int max, int pinned);
void bt_device_label(char *name, int index, int pinned);

 #endif
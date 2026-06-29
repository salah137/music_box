#ifndef  INIT_H
#define INIT_H

#include <local_types.h>
#include "freertos/ringbuf.h"

void init_bt(bt_device_t ***arr, int *arr_max_len, int *arr_len,int* selected_dv,bool* connected, bool* paused,RingbufHandle_t* r);
void scan_refresh();
void disconnect(void);
void sine_table_init(void);

#endif

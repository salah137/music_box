#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "local_types.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "minimp3.h"

/* ---------------------------------------------------------------------------
 * Constants & Tags
 * -------------------------------------------------------------------------*/
#define TAG        "BT_SCAN"
#define TAG_A2DP   "A2DP"

/* ---------------------------------------------------------------------------
 * Module state
 * -------------------------------------------------------------------------*/
typedef struct {
    bt_device_t ***array;
    int          *len;
    int          *max_len;
    int          *selected;
    bool         *connected;
    bool         *paused;
    esp_bd_addr_t peer;
} bt_state_t;

RingbufHandle_t *rb_t;
static bt_state_t s = { 0 };

/* ---------------------------------------------------------------------------
 * Carry buffer for BT audio callback
 * -------------------------------------------------------------------------*/
static uint8_t s_carry[MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t)];
static size_t  s_carry_len = 0;

static int32_t bt_audio_data_cb(uint8_t *data, int32_t len)
{
    if (*(s.paused)) { memset(data, 0, len); return len; }

    size_t filled = 0;

    if (s_carry_len > 0) {
        size_t take = s_carry_len < (size_t)len ? s_carry_len : (size_t)len;
        memcpy(data, s_carry, take);
        memmove(s_carry, s_carry + take, s_carry_len - take);
        s_carry_len -= take;
        filled += take;
    }

    while (filled < (size_t)len) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(*rb_t, &item_size, 0);
        if (item == NULL) break;

        size_t need = (size_t)len - filled;
        if (item_size <= need) {
            memcpy(data + filled, item, item_size);
            filled += item_size;
        } else {
            memcpy(data + filled, item, need);
            size_t overflow = item_size - need;
            memcpy(s_carry, (uint8_t *)item + need, overflow);
            s_carry_len = overflow;
            filled += need;
        }
        vRingbufferReturnItem(*rb_t, item);
    }

    if (filled < (size_t)len) memset(data + filled, 0, len - filled);
    return len;
}

/* ---------------------------------------------------------------------------
 * Device list helpers
 * -------------------------------------------------------------------------*/
static char *bda_to_str_alloc(const esp_bd_addr_t bda)
{
    char *buf = malloc(18);
    if (!buf) return NULL;
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return buf;
}

static bool device_exists(const char *bda_str)
{
    for (int i = 0; i < *s.len; i++) {
        bt_device_t *dv = (*s.array)[i];
        if (dv && dv->bda && strcmp(dv->bda, bda_str) == 0)
            return true;
    }
    return false;
}

static bool grow_devices_array(void)
{
    int          new_max = (*s.max_len) + 4;
    bt_device_t **tmp    = realloc(*s.array, (size_t)new_max * sizeof(bt_device_t *));
    if (!tmp) {
        ESP_LOGE(TAG, "realloc failed – device list is full");
        return false;
    }
    *s.array   = tmp;
    *s.max_len = new_max;
    return true;
}

static void append_device(char *bda_str, char *name)
{
    if (device_exists(bda_str)) {
        free(bda_str);
        free(name);
        return;
    }

    if (*s.len >= *s.max_len - 1 && !grow_devices_array()) {
        free(bda_str);
        free(name);
        return;
    }

    bt_device_t *dv = malloc(sizeof(bt_device_t));
    if (!dv) {
        ESP_LOGE(TAG, "malloc failed for bt_device_t");
        free(bda_str);
        free(name);
        return;
    }

    dv->bda  = bda_str;
    dv->name = name;
    (*s.array)[(*s.len)++] = dv;
}

/* ---------------------------------------------------------------------------
 * A2DP callback
 * -------------------------------------------------------------------------*/
static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG_A2DP, "A2DP connected – starting media");
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG_A2DP, "A2DP disconnected");
        }
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG_A2DP, "Audio state: %d", param->audio_stat.state);
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG_A2DP, "Audio config event");
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Classic BT GAP callback
 * -------------------------------------------------------------------------*/
static char *name_from_eir_alloc(uint8_t *eir)
{
    if (!eir) return NULL;

    uint8_t  len = 0;
    uint8_t *ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (!ptr)
        ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    if (!ptr || len == 0) return NULL;

    if (len > ESP_BT_GAP_MAX_BDNAME_LEN)
        len = ESP_BT_GAP_MAX_BDNAME_LEN;
    return strndup((char *)ptr, len);
}

static void try_autoconnect(const char *eir_name, const esp_bd_addr_t bda)
{
    if (*s.selected == -1 || !eir_name) return;

    const char *target = (*s.array)[*s.selected]->name;
    if (!target || !strstr(eir_name, target)) return;

    esp_bt_gap_cancel_discovery();
    memcpy(s.peer, bda, sizeof(esp_bd_addr_t));
    esp_a2d_source_connect(s.peer);
    *s.connected = true;
}

static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        char *bda_str = bda_to_str_alloc(param->disc_res.bda);
        if (!bda_str) break;

        ESP_LOGI(TAG, "Classic BT device: %s", bda_str);
        char *eir_name = NULL;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
            if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
                free(eir_name);
                eir_name = name_from_eir_alloc(prop->val);
                if (eir_name) ESP_LOGI(TAG, "  Name: %s", eir_name);
                try_autoconnect(eir_name, param->disc_res.bda);
            }
        }

        append_device(bda_str, eir_name);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        ESP_LOGI(TAG, "Discovery %s",
                 param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED
                     ? "STARTED" : "STOPPED");
        break;
    default:
        ESP_LOGI(TAG, "Unhandled GAP event: %d", event);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/
void disconnect(void)
{
    esp_a2d_source_disconnect(s.peer);
}

void init_bt(bt_device_t ***arr, int *arr_max_len, int *arr_len,
             int *selected_dv, bool *connected, bool *paused, RingbufHandle_t *r)
{
    s.array    = arr;
    s.max_len  = arr_max_len;
    s.len      = arr_len;
    s.selected = selected_dv;
    s.connected = connected;
    s.paused   = paused;
    rb_t       = r;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)); // ← Classic only
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_gap_callback));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_audio_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    esp_bt_gap_set_device_name("ESP32-Scanner");
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
}

void scan_refresh(void)
{
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}
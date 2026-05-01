#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "local_types.h"
#include "nvs_flash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Constants & Tags
 * -------------------------------------------------------------------------*/
#define TAG        "BT_SCAN"
#define TAG_A2DP   "A2DP"

#define SINE_TABLE_SIZE 256

/* ---------------------------------------------------------------------------
 * Module state – grouped into a single struct for clarity
 * -------------------------------------------------------------------------*/
typedef struct {
    bt_device_t ***array;       /* pointer to the caller's device array      */
    int          *len;          /* current number of entries                  */
    int          *max_len;      /* allocated capacity                         */
    int          *selected;     /* index of the device to auto-connect, or -1 */
    bool         *connected;    /* set to true once A2DP connects             */
    esp_bd_addr_t peer;         /* BD address of the connected peer           */
} bt_state_t;

static bt_state_t s = { 0 };

/* ---------------------------------------------------------------------------
 * Audio: sine-wave generator
 * -------------------------------------------------------------------------*/
static int16_t  s_sine_table[SINE_TABLE_SIZE];
static uint32_t s_phase = 0;

static void sine_table_init(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        s_sine_table[i] = (int16_t)(32767.0f * sinf(2.0f * M_PI * i / SINE_TABLE_SIZE));
    }
}

static int32_t bt_audio_data_cb(uint8_t *data, int32_t len)
{
    int16_t *samples    = (int16_t *)data;
    int      num_frames = len / (2 * sizeof(int16_t)); /* stereo int16 pairs */

    for (int i = 0; i < num_frames; i++) {
        int16_t val = s_sine_table[s_phase % SINE_TABLE_SIZE];
        samples[2 * i]     = val; /* L */
        samples[2 * i + 1] = val; /* R */
        s_phase++;
    }
    return len;
}

/* ---------------------------------------------------------------------------
 * Device list helpers
 * -------------------------------------------------------------------------*/

/** Convert a BD address to a heap-allocated "xx:xx:xx:xx:xx:xx" string.
 *  Caller owns the returned pointer. Returns NULL on allocation failure. */
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

/** Double the device array capacity. Returns true on success. */
static bool grow_devices_array(void)
{
    int          new_max = (*s.max_len) * 2;
    bt_device_t **tmp    = realloc(*s.array, (size_t)new_max * sizeof(bt_device_t *));
    if (!tmp) {
        ESP_LOGE(TAG, "realloc failed – device list is full");
        return false;
    }
    *s.array   = tmp;
    *s.max_len = new_max;
    return true;
}

/** Append a new device to the list, growing it as needed.
 *  Takes ownership of both heap-allocated strings on success;
 *  frees them on failure. */
static void append_device(char *bda_str, char *name)
{
    if (device_exists(bda_str)) {
        /* Duplicate – free the strings we were given and bail out. */
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
 * A2DP source callback
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
        ESP_LOGI(TAG_A2DP, "Audio config event (informational)");
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Classic BT helpers & GAP callback
 * -------------------------------------------------------------------------*/

/** Extract the device name from EIR data (complete name, then short name).
 *  Returns a heap-allocated string or NULL. */
static char *name_from_eir_alloc(uint8_t *eir)
{
    if (!eir) return NULL;

    uint8_t  len = 0;
    uint8_t *ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
    if (!ptr)
        ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
    if (!ptr || len == 0)
        return NULL;

    if (len > ESP_BT_GAP_MAX_BDNAME_LEN)
        len = ESP_BT_GAP_MAX_BDNAME_LEN;
    return strndup((char *)ptr, len);
}

/** Attempt auto-connect if the discovered EIR name matches the selected device. */
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

        ESP_LOGI(TAG, "Classic BT device found: %s", bda_str);

        char *eir_name = NULL;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
            switch (prop->type) {
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                ESP_LOGI(TAG, "  Name: %s", (char *)prop->val);
                break;
            case ESP_BT_GAP_DEV_PROP_COD:
                ESP_LOGI(TAG, "  CoD: 0x%" PRIx32, *(uint32_t *)prop->val);
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                ESP_LOGI(TAG, "  RSSI: %d dBm", *(int8_t *)prop->val);
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                free(eir_name); /* drop earlier value if EIR seen twice */
                eir_name = name_from_eir_alloc(prop->val);
                if (eir_name) ESP_LOGI(TAG, "  EIR Name: %s", eir_name);
                try_autoconnect(eir_name, param->disc_res.bda);
                break;
            default:
                break;
            }
        }

        append_device(bda_str, eir_name);
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        ESP_LOGI(TAG, "Classic BT discovery %s",
                 param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED
                     ? "STARTED" : "STOPPED");
        break;

    default:
        ESP_LOGI(TAG, "Unhandled GAP event: %d", event);
        break;
    }
}

/* ---------------------------------------------------------------------------
 * BLE scanning
 * -------------------------------------------------------------------------*/

static void ble_gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            esp_ble_gap_start_scanning(10);
        } else {
            ESP_LOGE(TAG, "BLE scan param set failed: %d", param->scan_param_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        char *bda_str = bda_to_str_alloc(param->scan_rst.bda);
        if (!bda_str) break;

        ESP_LOGI(TAG, "BLE device: %s  RSSI: %d", bda_str, param->scan_rst.rssi);

        uint8_t  name_len = 0;
        uint8_t *name_ptr = esp_ble_resolve_adv_data(
            param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);

        char *name = (name_ptr && name_len > 0)
                         ? strndup((char *)name_ptr, name_len)
                         : NULL;
        if (name) ESP_LOGI(TAG, "  Name: %s", name);

        append_device(bda_str, name);
        break;
    }

    default:
        break;
    }
}

static void start_ble_scan(void)
{
    static bool callback_registered = false;
    if (!callback_registered) {
        esp_ble_gap_register_callback(ble_gap_callback);
        callback_registered = true;
    }

    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    esp_ble_gap_set_scan_params(&scan_params);
}

/* ---------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------*/

void disconnect(void)
{
    esp_a2d_source_disconnect(s.peer);
}

void init_bt(bt_device_t ***arr, int *arr_max_len, int *arr_len,
             int *selected_dv, bool *connected)
{
    /* Bind module state to the caller's variables. */
    s.array    = arr;
    s.max_len  = arr_max_len;
    s.len      = arr_len;
    s.selected = selected_dv;
    s.connected = connected;

    /* NVS – required by the BT stack. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* BT controller (BTDM = Classic + BLE). */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

    /* Bluedroid host stack. */
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    /* Callbacks & A2DP source setup. */
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_gap_callback));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_audio_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    sine_table_init();
    start_ble_scan();

    esp_bt_gap_set_device_name("ESP32-Scanner");
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
}

void scan_refresh(void)
{
    esp_ble_gap_stop_scanning();
    start_ble_scan();
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}
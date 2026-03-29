/*
 * BLE HID keyboard backend — 5-state machine
 */

#if defined(CONFIG_INPUT_BLE) || defined(CONFIG_INPUT_AUTO)

#include "input_hal_internal.h"
#include "ble_keyboard.h"
#include "hid_keymap.h"
#include "storage.h"
#include "vterm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"

#include <string.h>

static const char *TAG = "ble_kbd";

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static volatile ble_state_t s_state = BLE_IDLE;

/* Protects s_scan_results / s_scan_count between BT task and app task */
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

/* Registry loaded at init — addresses we try to reconnect to */
static ble_device_info_t s_registry[STORAGE_BLE_MAX];
static int               s_registry_count = 0;

/* Scan results accumulated during BLE_PAIRING_SCAN */
static ble_device_info_t s_scan_results[STORAGE_BLE_MAX];
static int               s_scan_count = 0;

/* BDA of currently connected device (valid in BLE_CONNECTED) */
static uint8_t s_connected_bda[6];

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

ble_state_t ble_keyboard_get_state(void)      { return s_state; }

int ble_keyboard_get_scan_results(ble_device_info_t *out, int max)
{
    taskENTER_CRITICAL(&s_scan_mux);
    int n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan_results, n * sizeof(ble_device_info_t));
    taskEXIT_CRITICAL(&s_scan_mux);
    return n;
}

void ble_keyboard_enter_pairing(void)
{
    ESP_LOGI(TAG, "Entering pairing mode");
    esp_ble_gap_stop_scanning();
    s_scan_count = 0;
    s_state = BLE_PAIRING_SCAN;
    esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
}

void ble_keyboard_reconnect_start(void)
{
    if (s_state == BLE_CONNECTED) return;
    if (s_registry_count == 0)   return;
    ESP_LOGI(TAG, "Reconnect scan for %d known device(s)", s_registry_count);
    s_state = BLE_RECONNECT;
    esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
}

void ble_keyboard_select_device(const uint8_t addr[6], uint8_t addr_type)
{
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    ESP_LOGI(TAG, "Connecting to selected device %s", mac);
    esp_ble_gap_stop_scanning();
    s_state = BLE_CONNECTING;
    esp_hidh_dev_open((uint8_t *)addr, ESP_HID_TRANSPORT_BLE, addr_type);
}

void ble_keyboard_forget_device(const uint8_t addr[6])
{
    storage_ble_remove(addr);
    /* Reload registry */
    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
    if (s_state == BLE_CONNECTED &&
        memcmp(s_connected_bda, addr, 6) == 0) {
        /* TODO: disconnect active connection */
        ESP_LOGW(TAG, "forget_device: disconnect not yet implemented");
    }
}

/* ------------------------------------------------------------------ */
/*  GAP / scan helpers                                                 */
/* ------------------------------------------------------------------ */

static bool ad_has_hid_uuid(uint8_t *adv_data, uint8_t adv_data_len)
{
    uint8_t *p   = adv_data;
    uint8_t *end = adv_data + adv_data_len;
    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0x02 || type == 0x03) {
            uint8_t *uuids = p + 2;
            uint8_t  count = (len - 1) / 2;
            for (uint8_t i = 0; i < count; i++) {
                uint16_t uuid = (uint16_t)(uuids[2*i]) |
                                ((uint16_t)(uuids[2*i+1]) << 8);
                if (uuid == 0x1812) return true;
            }
        }
        p += 1 + len;
    }
    return false;
}

/* Extract advertised name from AD structures into dst[dstsz] */
static void ad_get_name(uint8_t *adv, uint8_t adv_len,
                        char *dst, size_t dstsz)
{
    uint8_t *p   = adv;
    uint8_t *end = adv + adv_len;
    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0x08 || type == 0x09) {   /* Short / Complete Local Name */
            size_t nlen = (size_t)(len - 1);
            if (nlen >= dstsz) nlen = dstsz - 1;
            memcpy(dst, p + 2, nlen);
            dst[nlen] = '\0';
            return;
        }
        p += 1 + len;
    }
    snprintf(dst, dstsz, "Unknown");
}

static bool addr_in_registry(const uint8_t addr[6])
{
    for (int i = 0; i < s_registry_count; i++) {
        if (memcmp(s_registry[i].addr, addr, 6) == 0) return true;
    }
    return false;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        /* Initial scan params set — start reconnect or wait for explicit call */
        if (s_registry_count > 0) {
            ble_keyboard_reconnect_start();
        } else {
            ESP_LOGI(TAG, "No paired devices — awaiting pairing mode trigger");
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        struct ble_scan_result_evt_param *r = &param->scan_rst;
        if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
        if (!ad_has_hid_uuid(r->ble_adv, r->adv_data_len)) break;

        if (s_state == BLE_RECONNECT) {
            if (addr_in_registry(r->bda)) {
                ESP_LOGI(TAG, "Known device found, connecting");
                esp_ble_gap_stop_scanning();
                s_state = BLE_CONNECTING;
                esp_hidh_dev_open(r->bda, ESP_HID_TRANSPORT_BLE,
                                  r->ble_addr_type);
            }
        } else if (s_state == BLE_PAIRING_SCAN) {
            /* Add to scan results if not already present */
            char discovered_name[64] = {0};
            taskENTER_CRITICAL(&s_scan_mux);
            bool dup = false;
            for (int i = 0; i < s_scan_count; i++) {
                if (memcmp(s_scan_results[i].addr, r->bda, 6) == 0) {
                    dup = true; break;
                }
            }
            if (!dup && s_scan_count < STORAGE_BLE_MAX) {
                ble_device_info_t *d = &s_scan_results[s_scan_count++];
                memcpy(d->addr, r->bda, 6);
                d->addr_type = r->ble_addr_type;
                ad_get_name(r->ble_adv, r->adv_data_len,
                            d->name, sizeof(d->name));
                d->last_seen = 0;
                memcpy(discovered_name, d->name, sizeof(discovered_name));
            }
            taskEXIT_CRITICAL(&s_scan_mux);
            if (discovered_name[0] != '\0')
                ESP_LOGI(TAG, "Discovered: '%s'", discovered_name);
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan stopped (state=%d)", (int)s_state);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "Auth complete: success");
        } else {
            ESP_LOGW(TAG, "Auth complete: failed (reason=0x%02x)",
                     param->ble_security.auth_cmpl.fail_reason);
            s_state = BLE_RECONNECT;
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  HID host callback                                                  */
/* ------------------------------------------------------------------ */

static void hidh_callback(void *handler_args, esp_event_base_t base,
                          int32_t id, void *event_data)
{
    esp_hidh_event_t       event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *data  = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVT: {
        if (data->open.status != ESP_OK) {
            ESP_LOGW(TAG, "HIDH open failed (status=0x%x), returning to reconnect",
                     data->open.status);
            s_state = BLE_RECONNECT;
            esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
            break;
        }
        const uint8_t *bda = esp_hidh_dev_bda_get(data->open.dev);
        if (!bda) {
            ESP_LOGE(TAG, "HIDH open: bda_get returned NULL");
            s_state = BLE_RECONNECT;
            esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
            break;
        }
        ESP_LOGI(TAG, "Connected: %02X:%02X:%02X:%02X:%02X:%02X",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        memcpy(s_connected_bda, bda, 6);
        s_state = BLE_CONNECTED;
        esp_ble_set_encryption((uint8_t *)bda, ESP_BLE_SEC_ENCRYPT_MITM);

        /* Save/update device in registry */
        ble_device_info_t dev = {0};
        memcpy(dev.addr, bda, 6);
        /* Find name from scan results if available */
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_results[i].addr, bda, 6) == 0) {
                dev.addr_type = s_scan_results[i].addr_type;
                memcpy(dev.name, s_scan_results[i].name, sizeof(dev.name));
                break;
            }
        }
        if (dev.name[0] == '\0') snprintf(dev.name, sizeof(dev.name), "Unknown HID");
        storage_ble_save(&dev);
        storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
        break;
    }

    case ESP_HIDH_CLOSE_EVT:
        ESP_LOGI(TAG, "Keyboard disconnected, restarting reconnect scan");
        s_state = BLE_RECONNECT;
        esp_ble_gap_stop_scanning();
        esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
        break;

    case ESP_HIDH_INPUT_EVT: {
        if (data->input.usage != ESP_HID_USAGE_KEYBOARD) break;
        if (data->input.length < 3) break;

        const uint8_t *report    = data->input.data;
        uint8_t        modifiers = report[0];
        uint8_t n = (data->input.length < 8) ? (uint8_t)data->input.length : 8u;

        for (uint8_t i = 2; i < n; i++) {
            uint8_t kc = report[i];
            if (kc == 0x00 || kc == 0x01) continue;

            uint8_t buf[INPUT_EVENT_MAX_LEN];
            uint8_t len = hid_keymap_translate(kc, modifiers,
                                               vterm_app_cursor_keys(), buf);
            if (len == 0) continue;

            input_event_t ev = { .type = INPUT_EVENT_KEY, .len = len };
            for (uint8_t j = 0; j < len; j++) ev.buf[j] = buf[j];
            input_hal_post_event(&ev);
        }
        break;
    }

    case ESP_HIDH_BATTERY_EVT:
        ESP_LOGI(TAG, "Battery: %d%%", data->battery.level);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Backend init                                                       */
/* ------------------------------------------------------------------ */

esp_err_t ble_keyboard_backend_init(void)
{
    esp_err_t ret;

    /* Load existing paired device registry */
    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
    ESP_LOGI(TAG, "Registry: %d known device(s)", s_registry_count);

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "mem_release: %s", esp_err_to_name(ret));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "BT controller already initialised");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt_controller_enable: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t   io_cap   = ESP_IO_CAP_NONE;
    uint8_t            key_size = 16;
    uint8_t            init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t            rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                   &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                   &io_cap,   sizeof(io_cap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                   &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                   &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                   &rsp_key,  sizeof(rsp_key));

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gap_register_callback: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_hidh_config_t hidh_cfg = {
        .callback         = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg     = NULL,
    };
    ret = esp_hidh_init(&hidh_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hidh_init: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Scan params → triggers SCAN_PARAM_SET_COMPLETE → reconnect or wait */
    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BLE keyboard backend initialised");
    return ESP_OK;
}

#else

#include "input_hal_internal.h"
#include "ble_keyboard.h"
esp_err_t   ble_keyboard_backend_init(void)                           { return ESP_OK; }
ble_state_t ble_keyboard_get_state(void)                              { return BLE_IDLE; }
void        ble_keyboard_enter_pairing(void)                          {}
void        ble_keyboard_reconnect_start(void)                        {}
int         ble_keyboard_get_scan_results(ble_device_info_t *o, int m){ (void)o;(void)m; return 0; }
void        ble_keyboard_select_device(const uint8_t a[6], uint8_t t) { (void)a;(void)t; }
void        ble_keyboard_forget_device(const uint8_t a[6])            { (void)a; }

#endif

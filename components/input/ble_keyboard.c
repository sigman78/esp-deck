/*
 * BLE HID keyboard backend
 *
 * Scans for BLE HID devices (UUID 0x1812), connects, and forwards
 * HID reports through hid_keymap_translate() → input_hal_post_event().
 *
 * Only compiled when CONFIG_INPUT_BLE or CONFIG_INPUT_AUTO is set.
 */

#if defined(CONFIG_INPUT_BLE) || defined(CONFIG_INPUT_AUTO)

#include "input_hal_internal.h"
#include "hid_keymap.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"

static const char *TAG = "ble_kbd";

/* Set to true while a GAP scan is active to avoid double-starts */
static volatile bool s_scanning = false;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */
static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param);
static void hidh_callback(void *handler_args, esp_event_base_t base,
                          int32_t id, void *event_data);

/* ------------------------------------------------------------------ */
/*  GAP / scan helpers                                                 */
/* ------------------------------------------------------------------ */

static void start_scan(void)
{
    ESP_LOGI(TAG, "Scanning for BLE HID keyboard...");
    s_scanning = true;
    esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
}

static bool ad_has_hid_uuid(uint8_t *adv_data, uint8_t adv_data_len)
{
    /* Walk AD structures looking for UUID16 list containing 0x1812 (HID) */
    uint8_t *p   = adv_data;
    uint8_t *end = adv_data + adv_data_len;

    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];

        /* Types 0x02 (Incomplete) and 0x03 (Complete) UUID16 lists */
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

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        start_scan();
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        struct ble_scan_result_evt_param *r = &param->scan_rst;
        if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;

        if (ad_has_hid_uuid(r->ble_adv, r->adv_data_len)) {
            ESP_LOGI(TAG, "HID device found, connecting...");
            esp_ble_gap_stop_scanning();
            s_scanning = false;
            esp_hidh_dev_open(r->bda, ESP_HID_TRANSPORT_BLE, r->ble_addr_type);
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "BLE scan stopped");
        s_scanning = false;
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
        const uint8_t *bda = esp_hidh_dev_bda_get(data->open.dev);
        ESP_LOGI(TAG, "Connected: %02X:%02X:%02X:%02X:%02X:%02X",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        esp_ble_set_encryption((uint8_t *)bda, ESP_BLE_SEC_ENCRYPT_MITM);
        break;
    }

    case ESP_HIDH_CLOSE_EVT:
        ESP_LOGI(TAG, "Keyboard disconnected, restarting scan");
        if (!s_scanning) {
            start_scan();
        }
        break;

    case ESP_HIDH_INPUT_EVT: {
        if (data->input.usage != ESP_HID_USAGE_KEYBOARD) break;
        if (data->input.length < 3) break;

        const uint8_t *report    = data->input.data;
        uint8_t        modifiers = report[0];
        /* report[1] is reserved; keycodes begin at report[2] */
        uint8_t n = (data->input.length < 8) ? (uint8_t)data->input.length : 8u;

        for (uint8_t i = 2; i < n; i++) {
            uint8_t kc = report[i];
            if (kc == 0x00 || kc == 0x01) continue;  /* no key / rollover */

            uint8_t buf[INPUT_EVENT_MAX_LEN];
            uint8_t len = hid_keymap_translate(kc, modifiers, buf);
            if (len == 0) continue;

            input_event_t ev = { .len = len };
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

    /* Release Classic BT memory — ESP32-S3 is BLE-only */
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mem_release: %s", esp_err_to_name(ret));
    }

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

    /* Security: Just Works bonding, no I/O capability */
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

    /* HID host — creates its own event task internally */
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

    /* Set scan params — GAP fires SCAN_PARAM_SET_COMPLETE, then start_scan() */
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

#else /* CONFIG_INPUT_BLE || CONFIG_INPUT_AUTO not set */

#include "input_hal_internal.h"
esp_err_t ble_keyboard_backend_init(void) { return ESP_OK; }

#endif /* CONFIG_INPUT_BLE || CONFIG_INPUT_AUTO */

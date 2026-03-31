/*
 * BLE HID keyboard backend — 5-state machine (NimBLE)
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
#include "esp_hidh.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "store/config/ble_store_config.h"

#include <string.h>

static const char *TAG = "ble_kbd";

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static volatile ble_state_t s_state          = BLE_IDLE;
static volatile bool        s_nimble_synced  = false;

/* Protects s_scan_results / s_scan_count between NimBLE task and app task */
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

/* Registry — addresses we auto-reconnect to on boot */
static ble_device_info_t s_registry[STORAGE_BLE_MAX];
static int               s_registry_count = 0;

/* Scan results accumulated during BLE_PAIRING_SCAN */
static ble_device_info_t s_scan_results[STORAGE_BLE_MAX];
static int               s_scan_count = 0;

/* BDA of connected device (NimBLE little-endian: val[0] = LSB) */
static uint8_t s_connected_bda[6];

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_scan(int32_t duration_ms);

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* NimBLE stores addresses little-endian; reverse for human-readable log */
static void log_addr(const char *prefix, const uint8_t addr[6])
{
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix,
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static bool addr_in_registry(const uint8_t addr[6])
{
    for (int i = 0; i < s_registry_count; i++) {
        if (memcmp(s_registry[i].addr, addr, 6) == 0) return true;
    }
    return false;
}

/* AD structure parsers — raw adv data, stack-agnostic */
static bool ad_has_hid_uuid(const uint8_t *adv_data, uint8_t adv_len)
{
    const uint8_t *p   = adv_data;
    const uint8_t *end = adv_data + adv_len;
    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0x02 || type == 0x03) {
            const uint8_t *uuids = p + 2;
            uint8_t count = (len - 1) / 2;
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

static void ad_get_name(const uint8_t *adv, uint8_t adv_len,
                        char *dst, size_t dstsz)
{
    const uint8_t *p   = adv;
    const uint8_t *end = adv + adv_len;
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

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

ble_state_t ble_keyboard_get_state(void) { return s_state; }

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
    if (!s_nimble_synced) {
        ESP_LOGW(TAG, "enter_pairing: NimBLE not yet synced");
        return;
    }
    ESP_LOGI(TAG, "Entering pairing mode");
    ble_gap_disc_cancel();
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan_count = 0;
    taskEXIT_CRITICAL(&s_scan_mux);
    s_state = BLE_PAIRING_SCAN;
    start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
}

void ble_keyboard_reconnect_start(void)
{
    if (s_state == BLE_CONNECTED) return;
    if (s_registry_count == 0)   return;
    if (!s_nimble_synced)        return;   /* on_sync will start scan */
    ESP_LOGI(TAG, "Reconnect scan for %d known device(s)", s_registry_count);
    s_state = BLE_RECONNECT;
    start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
}

void ble_keyboard_select_device(const uint8_t addr[6], uint8_t addr_type)
{
    log_addr("Connecting to", addr);
    ble_gap_disc_cancel();
    s_state = BLE_CONNECTING;
    esp_hidh_dev_open((uint8_t *)addr, ESP_HID_TRANSPORT_BLE, addr_type);
}

void ble_keyboard_forget_device(const uint8_t addr[6])
{
    storage_ble_remove(addr);
    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
    if (s_state == BLE_CONNECTED && memcmp(s_connected_bda, addr, 6) == 0) {
        /* TODO: disconnect active connection */
        ESP_LOGW(TAG, "forget_device: disconnect not yet implemented");
    }
}

/* ------------------------------------------------------------------ */
/*  Scan helper                                                        */
/* ------------------------------------------------------------------ */

static void start_scan(int32_t duration_ms)
{
    struct ble_gap_disc_params p = {
        .itvl              = 0x50,
        .window            = 0x30,
        .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
        .limited           = 0,
        .passive           = 0,   /* active scan — fetch device names */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_ms, &p,
                          gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
    }
}

/* ------------------------------------------------------------------ */
/*  NimBLE GAP event callback                                         */
/* ------------------------------------------------------------------ */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *d = &event->disc;
        if (!ad_has_hid_uuid(d->data, d->length_data)) break;

        if (s_state == BLE_RECONNECT) {
            if (addr_in_registry(d->addr.val)) {
                log_addr("Known device found, connecting", d->addr.val);
                ble_gap_disc_cancel();
                s_state = BLE_CONNECTING;
                esp_hidh_dev_open((uint8_t *)d->addr.val,
                                  ESP_HID_TRANSPORT_BLE, d->addr.type);
            }
        } else if (s_state == BLE_PAIRING_SCAN) {
            char discovered_name[64] = {0};
            taskENTER_CRITICAL(&s_scan_mux);
            bool dup = false;
            for (int i = 0; i < s_scan_count; i++) {
                if (memcmp(s_scan_results[i].addr, d->addr.val, 6) == 0) {
                    dup = true; break;
                }
            }
            if (!dup && s_scan_count < STORAGE_BLE_MAX) {
                ble_device_info_t *dev = &s_scan_results[s_scan_count++];
                memcpy(dev->addr, d->addr.val, 6);
                dev->addr_type = d->addr.type;
                ad_get_name(d->data, d->length_data,
                            dev->name, sizeof(dev->name));
                dev->last_seen = 0;
                memcpy(discovered_name, dev->name, sizeof(discovered_name));
            }
            taskEXIT_CRITICAL(&s_scan_mux);
            if (discovered_name[0] != '\0')
                ESP_LOGI(TAG, "Discovered: '%s'", discovered_name);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete (state=%d)", (int)s_state);
        if (s_state == BLE_RECONNECT || s_state == BLE_PAIRING_SCAN) {
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Encryption established");
        } else {
            ESP_LOGW(TAG, "Encryption failed (status=%d)",
                     event->enc_change.status);
            s_state = BLE_RECONNECT;
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  HIDH callback (stack-agnostic)                                    */
/* ------------------------------------------------------------------ */

static void hidh_callback(void *handler_args, esp_event_base_t base,
                          int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_t       event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *data  = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVT: {
        if (data->open.status != ESP_OK) {
            ESP_LOGW(TAG, "HIDH open failed (status=0x%x), returning to reconnect",
                     data->open.status);
            s_state = BLE_RECONNECT;
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
            break;
        }
        const uint8_t *bda = esp_hidh_dev_bda_get(data->open.dev);
        if (!bda) {
            ESP_LOGE(TAG, "HIDH open: bda_get returned NULL");
            s_state = BLE_RECONNECT;
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
            break;
        }
        log_addr("Connected:", bda);
        memcpy(s_connected_bda, bda, 6);
        s_state = BLE_CONNECTED;

        /* Save/update device in registry */
        ble_device_info_t dev = {0};
        memcpy(dev.addr, bda, 6);
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_results[i].addr, bda, 6) == 0) {
                dev.addr_type = s_scan_results[i].addr_type;
                memcpy(dev.name, s_scan_results[i].name, sizeof(dev.name));
                break;
            }
        }
        if (dev.name[0] == '\0')
            snprintf(dev.name, sizeof(dev.name), "Unknown HID");
        storage_ble_save(&dev);
        storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
        break;
    }

    case ESP_HIDH_CLOSE_EVT:
        ESP_LOGI(TAG, "Keyboard disconnected, restarting reconnect scan");
        s_state = BLE_RECONNECT;
        ble_gap_disc_cancel();
        start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
        break;

    case ESP_HIDH_INPUT_EVT: {
        if (data->input.usage != ESP_HID_USAGE_KEYBOARD) break;
        if (data->input.length < 3) break;

        const uint8_t *report    = data->input.data;
        uint8_t        modifiers = report[0];
        uint8_t n = (data->input.length < 8) ? (uint8_t)data->input.length : 8u;

        for (uint8_t i = 2; i < n; i++) {
            uint8_t kc = report[i];
            if (kc == 0x00 || kc == 0x01) continue;   /* 0x01 = ErrorRollOver */

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
/*  NimBLE host task + sync/reset callbacks                           */
/* ------------------------------------------------------------------ */

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_nimble_synced = true;
    ESP_LOGI(TAG, "NimBLE synced");
    if (s_registry_count > 0) {
        ESP_LOGI(TAG, "Auto-reconnect: %d device(s) in registry",
                 s_registry_count);
        s_state = BLE_RECONNECT;
        start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
    } else {
        ESP_LOGI(TAG, "No paired devices — awaiting pairing trigger");
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset (reason=%d)", reason);
    s_state        = BLE_IDLE;
    s_nimble_synced = false;
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();   /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ */
/*  Backend init                                                       */
/* ------------------------------------------------------------------ */

esp_err_t ble_keyboard_backend_init(void)
{
    esp_err_t ret;

    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
    ESP_LOGI(TAG, "Registry: %d known device(s)", s_registry_count);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Security manager: bonding + MITM + Secure Connections, no display/keyboard */
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 0;   /* Just Works — compatible with NO_IO capability */
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_io_cap        = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sync_cb           = on_sync;
    ble_hs_cfg.reset_cb          = on_reset;

    /* Persist bonding keys in NVS */
    ble_store_config_init();

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

    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE keyboard backend initialised (NimBLE)");
    return ESP_OK;
}

#else

#include "input_hal_internal.h"
#include "ble_keyboard.h"
esp_err_t   ble_keyboard_backend_init(void)                            { return ESP_OK; }
ble_state_t ble_keyboard_get_state(void)                               { return BLE_IDLE; }
void        ble_keyboard_enter_pairing(void)                           {}
void        ble_keyboard_reconnect_start(void)                         {}
int         ble_keyboard_get_scan_results(ble_device_info_t *o, int m) { (void)o; (void)m; return 0; }
void        ble_keyboard_select_device(const uint8_t a[6], uint8_t t)  { (void)a; (void)t; }
void        ble_keyboard_forget_device(const uint8_t a[6])             { (void)a; }

#endif

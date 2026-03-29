#include "pairing_overlay.h"
#include "ble_keyboard.h"
#include "storage.h"
#include "vterm.h"
#include "input_hal.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"

#define PAIRING_OVERLAY_TIMEOUT_MS  30000   /* 30 s no-activity timeout */
#define MAX_SCAN_ENTRIES            STORAGE_BLE_MAX

static const char *TAG = "pairing_overlay";

static void ov_write(const char *s) { vterm_write(s, strlen(s)); }

/* Render the overlay box.
 * Box origin: row 2, col 10 (1-based ANSI coordinates).
 * Layout:
 *   row 2:              +------ BLE Pairing ------+
 *   row 3..2+n:         |  [name]                 |
 *   row 3+n:            +-------------------------+
 *   row 4+n:            | ESC=cancel  ENTER=connect |
 */
static void render_overlay(const ble_device_info_t *devs, int ndevs, int sel)
{
    char buf[80];

    /* Save cursor */
    ov_write("\033[s");

    /* Top border */
    snprintf(buf, sizeof(buf), "\033[2;10H+------ BLE Pairing ------+");
    ov_write(buf);

    if (ndevs == 0) {
        /* Single "Scanning..." row */
        snprintf(buf, sizeof(buf), "\033[3;10H|   %-22s |", "  Scanning...");
        ov_write(buf);
        /* Bottom border at row 4 */
        snprintf(buf, sizeof(buf), "\033[4;10H+-------------------------+");
        ov_write(buf);
        /* Status line at row 5 */
        snprintf(buf, sizeof(buf), "\033[5;10H| ESC=cancel  ENTER=connect |");
        ov_write(buf);
    } else {
        int row;
        for (int i = 0; i < ndevs && i < MAX_SCAN_ENTRIES; i++) {
            row = 3 + i;
            if (i == sel) {
                snprintf(buf, sizeof(buf), "\033[%d;10H| > %-22s |", row, devs[i].name);
            } else {
                snprintf(buf, sizeof(buf), "\033[%d;10H|   %-22s |", row, devs[i].name);
            }
            ov_write(buf);
        }
        /* Bottom border */
        row = 3 + ndevs;
        snprintf(buf, sizeof(buf), "\033[%d;10H+-------------------------+", row);
        ov_write(buf);
        /* Status line */
        row = 4 + ndevs;
        snprintf(buf, sizeof(buf), "\033[%d;10H| ESC=cancel  ENTER=connect |", row);
        ov_write(buf);
    }

    /* Restore cursor */
    ov_write("\033[u");
}

/* Clear the overlay area by overwriting with spaces. */
static void clear_overlay(int ndevs)
{
    char buf[80];

    ov_write("\033[s");

    /* Number of rows drawn: 1 (top) + max(ndevs,1) + 1 (bottom) + 1 (status) */
    int entries = (ndevs > 0) ? ndevs : 1;
    int total_rows = 1 + entries + 1 + 1; /* top + device rows + bottom + status */

    for (int i = 0; i < total_rows; i++) {
        snprintf(buf, sizeof(buf), "\033[%d;10H%-28s", 2 + i, "");
        ov_write(buf);
    }

    ov_write("\033[u");
}

esp_err_t pairing_overlay_run(void)
{
    ble_keyboard_enter_pairing();

    int sel   = 0;
    int ndevs = 0;
    ble_device_info_t devs[MAX_SCAN_ENTRIES];
    memset(devs, 0, sizeof(devs));

    render_overlay(devs, ndevs, sel);

    int64_t last_activity_ms = (int64_t)(esp_timer_get_time() / 1000);
    esp_err_t result = ESP_ERR_TIMEOUT;

    while (1) {
        /* Check total timeout */
        int64_t now = (int64_t)(esp_timer_get_time() / 1000);
        if (now - last_activity_ms > PAIRING_OVERLAY_TIMEOUT_MS) {
            ESP_LOGW(TAG, "pairing overlay timed out");
            result = ESP_ERR_TIMEOUT;
            break;
        }

        /* Refresh scan results */
        ble_device_info_t new_devs[MAX_SCAN_ENTRIES];
        int new_ndevs = ble_keyboard_get_scan_results(new_devs, MAX_SCAN_ENTRIES);

        bool list_changed = (new_ndevs != ndevs);
        if (new_ndevs > 0 && new_ndevs == ndevs) {
            /* Check if any name changed */
            for (int i = 0; i < new_ndevs; i++) {
                if (memcmp(new_devs[i].addr, devs[i].addr, 6) != 0 ||
                    strncmp(new_devs[i].name, devs[i].name, sizeof(devs[i].name)) != 0) {
                    list_changed = true;
                    break;
                }
            }
        }

        if (list_changed) {
            memcpy(devs, new_devs, new_ndevs * sizeof(ble_device_info_t));
            ndevs = new_ndevs;
            if (sel >= ndevs && ndevs > 0) {
                sel = ndevs - 1;
            }
            render_overlay(devs, ndevs, sel);
        }

        /* Wait for input event (200 ms poll interval) */
        input_event_t ev;
        bool got_event = input_hal_read(&ev, 200);
        if (!got_event) {
            /* No event — loop back to check timeout */
            continue;
        }

        /* Reset activity timer on any event */
        last_activity_ms = (int64_t)(esp_timer_get_time() / 1000);

        if (ev.type == INPUT_EVENT_KEY) {
            uint8_t *b = ev.buf;
            int len    = ev.len;

            if (len >= 3 && b[0] == 0x1B && b[1] == '[' && b[2] == 'A') {
                /* Up arrow */
                if (sel > 0) sel--;
                render_overlay(devs, ndevs, sel);
            } else if (len >= 3 && b[0] == 0x1B && b[1] == '[' && b[2] == 'B') {
                /* Down arrow */
                if (ndevs > 0 && sel < ndevs - 1) sel++;
                render_overlay(devs, ndevs, sel);
            } else if (len >= 1 && (b[0] == '\r' || b[0] == '\n')) {
                /* Enter — select device */
                if (ndevs > 0) {
                    ESP_LOGI(TAG, "selecting device: %s", devs[sel].name);
                    ble_keyboard_select_device(devs[sel].addr, devs[sel].addr_type);
                    result = ESP_OK;
                    break;
                }
            } else if (len == 1 && b[0] == 0x1B) {
                /* Bare Escape — dismiss */
                result = ESP_ERR_NOT_FOUND;
                break;
            }
        } else if (ev.type == INPUT_EVENT_TAP) {
            /* Map touch y coordinate to terminal row (16px per row) */
            int touch_row = ev.y / 16;
            int dev_idx   = touch_row - 3;  /* device rows start at terminal row 3 */
            if (dev_idx >= 0 && dev_idx < ndevs) {
                sel = dev_idx;
                render_overlay(devs, ndevs, sel);
                /* Treat tap as immediate selection */
                ESP_LOGI(TAG, "touch-selecting device: %s", devs[sel].name);
                ble_keyboard_select_device(devs[sel].addr, devs[sel].addr_type);
                result = ESP_OK;
                break;
            } else {
                /* Tap outside device list — just re-render to show highlight */
                render_overlay(devs, ndevs, sel);
            }
        }
    }

    /* Clear overlay regardless of exit path */
    clear_overlay(ndevs);

    return result;
}

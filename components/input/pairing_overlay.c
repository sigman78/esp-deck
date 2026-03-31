/*
 * pairing_overlay.c — BLE pairing menu rendered as a display overlay.
 *
 * Writes directly into a DRAM overlay buffer composited by the display ISR
 * on top of the primary (SSH/vterm) terminal buffer.  Non-zero overlay cells
 * are rendered with overlay fg/bg colors; zero cells are transparent.
 *
 * No ANSI escape sequences — the SSH vterm path is completely untouched.
 */

#include "pairing_overlay.h"
#include "ble_keyboard.h"
#include "storage.h"
#include "display.h"
#include "input_hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

#define PAIRING_OVERLAY_TIMEOUT_MS  30000   /* 30 s no-activity timeout */
#define MAX_SCAN_ENTRIES            STORAGE_BLE_MAX

/* Overlay buffer dimensions — fixed to match 800×480 / 8×16 font terminal */
#define OV_COLS      100
#define OV_ROWS       30
#define OV_FONT_H     16   /* FONT_HEIGHT from font component */

/* Box layout: 54 chars wide, centered on 100-col terminal */
#define OV_BOX_W     54    /* total box width (inner = 52)                */
#define OV_ORIG_COL  23    /* start column: (100-54)/2 = 23               */
#define OV_ORIG_ROW   2    /* start row (leave 2 rows of terminal at top) */
#define OV_INNER     (OV_BOX_W - 2)  /* 52 inner chars */

/* Unicode box-drawing codepoints (all present in terminus8x16 font) */
#define BOX_H   0x2500u   /* ─  BOX DRAWINGS LIGHT HORIZONTAL             */
#define BOX_V   0x2502u   /* │  BOX DRAWINGS LIGHT VERTICAL               */
#define BOX_TL  0x250Cu   /* ┌  BOX DRAWINGS LIGHT DOWN AND RIGHT         */
#define BOX_TR  0x2510u   /* ┐  BOX DRAWINGS LIGHT DOWN AND LEFT          */
#define BOX_BL  0x2514u   /* └  BOX DRAWINGS LIGHT UP AND RIGHT           */
#define BOX_BR  0x2518u   /* ┘  BOX DRAWINGS LIGHT UP AND LEFT            */
#define BOX_ML  0x251Cu   /* ├  BOX DRAWINGS LIGHT VERTICAL AND RIGHT     */
#define BOX_MR  0x2524u   /* ┤  BOX DRAWINGS LIGHT VERTICAL AND LEFT      */

static const char *TAG = "pairing_overlay";

/* Overlay cell buffer — DRAM_ATTR so the display ISR can read it safely */
static DRAM_ATTR display_overlay_cell_t s_ov_buf[OV_COLS * OV_ROWS];

/* ------------------------------------------------------------------ */
/* Low-level overlay primitives                                         */
/* ------------------------------------------------------------------ */

static void ov_clear(void)
{
    memset(s_ov_buf, 0, sizeof(s_ov_buf));
}

static void ov_putch(int col, int row, uint16_t cp)
{
    if (col >= 0 && col < OV_COLS && row >= 0 && row < OV_ROWS)
        s_ov_buf[row * OV_COLS + col].cp = cp;
}

/* Write ASCII string (safe: uint8_t cast, so high bytes ignored) */
static void ov_puts(int col, int row, const char *s)
{
    while (*s && col < OV_COLS)
        ov_putch(col++, row, (uint8_t)*s++);
}

/* Draw a horizontal rule: left_cp ─────── right_cp */
static void ov_hline(int col, int row, int width,
                     uint16_t left_cp, uint16_t fill_cp, uint16_t right_cp)
{
    ov_putch(col, row, left_cp);
    for (int i = 1; i < width - 1; i++)
        ov_putch(col + i, row, fill_cp);
    ov_putch(col + width - 1, row, right_cp);
}

/* Draw │ + 52 ASCII chars (inner_text must be exactly OV_INNER chars) + │ */
static void ov_text_row(int col, int row, const char *inner_text)
{
    ov_putch(col, row, BOX_V);
    ov_puts(col + 1, row, inner_text);
    ov_putch(col + OV_BOX_W - 1, row, BOX_V);
}

/* Draw top border with title centered in the horizontal rule:
 *   ┌──────────── title ───────────────┐
 */
static void ov_title_border(int col, int row, const char *title)
{
    ov_hline(col, row, OV_BOX_W, BOX_TL, BOX_H, BOX_TR);
    int tlen  = (int)strlen(title);
    int toff  = (OV_INNER - tlen) / 2;  /* left-pad within inner area */
    ov_puts(col + 1 + toff, row, title);
}

/* ------------------------------------------------------------------ */
/* Menu rendering                                                       */
/* ------------------------------------------------------------------ */

static void render_overlay(const ble_device_info_t *devs, int ndevs, int sel)
{
    ov_clear();
    const int c = OV_ORIG_COL;
    int r = OV_ORIG_ROW;

    /* ┌──────── BLE Pairing ────────┐ */
    ov_title_border(c, r, " BLE Pairing ");
    r++;

    if (ndevs == 0) {
        char inner[OV_INNER + 1];
        snprintf(inner, sizeof(inner), " %-*s ", OV_INNER - 2, "Scanning...");
        ov_text_row(c, r, inner);
        r++;
    } else {
        char inner[OV_INNER + 1];
        for (int i = 0; i < ndevs && i < MAX_SCAN_ENTRIES; i++, r++) {
            snprintf(inner, sizeof(inner), " %c %-*s ",
                     (i == sel) ? '>' : ' ',
                     OV_INNER - 4,   /* 52 - 1(sp) - 1(sel) - 1(sp) - 1(sp) */
                     devs[i].name);
            ov_text_row(c, r, inner);
        }
    }

    /* └─────────────────────────────┘ */
    ov_hline(c, r, OV_BOX_W, BOX_BL, BOX_H, BOX_BR);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t pairing_overlay_run(void)
{
    ble_keyboard_enter_pairing();

    int sel   = 0;
    int ndevs = 0;
    ble_device_info_t devs[MAX_SCAN_ENTRIES];
    memset(devs, 0, sizeof(devs));

    /* Activate overlay */
    int ov_cols = OV_COLS, ov_rows = OV_ROWS;
    display_get_text_size(&ov_cols, &ov_rows);
    display_set_overlay_colors(COLOR_BLACK, COLOR_CYAN);
    display_set_overlay_buffer(s_ov_buf, ov_cols, ov_rows);
    display_set_cursor(-1, -1, CURSOR_NONE);   /* hide SSH cursor under overlay */

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
        if (!list_changed && new_ndevs > 0) {
            for (int i = 0; i < new_ndevs; i++) {
                if (memcmp(new_devs[i].addr, devs[i].addr, 6) != 0 ||
                    strncmp(new_devs[i].name, devs[i].name,
                            sizeof(devs[i].name)) != 0) {
                    list_changed = true;
                    break;
                }
            }
        }

        if (list_changed) {
            memcpy(devs, new_devs, new_ndevs * sizeof(ble_device_info_t));
            ndevs = new_ndevs;
            if (sel >= ndevs && ndevs > 0) sel = ndevs - 1;
            render_overlay(devs, ndevs, sel);
        }

        /* Wait for input event (200 ms poll interval) */
        input_event_t ev;
        if (!input_hal_read(&ev, 200)) continue;

        last_activity_ms = (int64_t)(esp_timer_get_time() / 1000);

        if (ev.type == INPUT_EVENT_KEY) {
            uint8_t *b  = ev.buf;
            int      len = ev.len;

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
            /* Map screen pixel Y to terminal row, then to device index.
             * Device rows start at OV_ORIG_ROW+1 (one below the top border). */
            int touch_row = ev.y / OV_FONT_H;
            int dev_idx   = touch_row - (OV_ORIG_ROW + 1);
            if (dev_idx >= 0 && dev_idx < ndevs) {
                sel = dev_idx;
                render_overlay(devs, ndevs, sel);   /* flash selection */
                ESP_LOGI(TAG, "touch-selecting device: %s", devs[sel].name);
                ble_keyboard_select_device(devs[sel].addr, devs[sel].addr_type);
                result = ESP_OK;
            } else {
                /* Tap outside device list (border, gaps, or off-menu) — dismiss */
                result = ESP_ERR_NOT_FOUND;
            }
            break;
        }
    }

    /* Deactivate overlay — primary buffer resumes */
    display_set_overlay_buffer(NULL, 0, 0);
    ov_clear();

    return result;
}

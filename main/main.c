/*
 * Cyberdeck SSH Terminal - Main Application
 *
 * ESP32-S3 based portable SSH terminal with BLE keyboard and 7" display.
 * Uses the vterm component (libtsm backend) for full VT/ANSI output.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// Component headers
#include "esp_heap_caps.h"
#include "display.h"
#include "vterm.h"
#include "ssh_client.h"
#include "ble_keyboard.h"
#include "wifi_manager.h"
#include "font.h"

static const char *TAG = "cyberdeck";

/* Log both total and internal-DRAM heap in one line. */
static void log_heap(const char *label)
{
    ESP_LOGI(TAG, "Heap %-30s  total=%7u  int=%7u  int_blk=%7u",
             label,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// Event group for system state
static EventGroupHandle_t s_system_event_group;

#define WIFI_CONNECTED_BIT  BIT0
#define SSH_CONNECTED_BIT   BIT1
#define BLE_PAIRED_BIT      BIT2

/* -------------------------------------------------------------------------
 * ANSI helper macros — same shorthand used in sim/main.c
 * ---------------------------------------------------------------------- */
#define ESC  "\x1b"
#define CSI  ESC "["
#define RST  CSI "0m"

/* Write literal string via vterm */
#define vw(s)  vterm_write((s), sizeof(s) - 1)

/* Write formatted string via vterm */
static void vf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        vterm_write(buf, (size_t)n);
}

/* Set ANSI-256 foreground / background colour */
static void fg(int n) { vf(CSI "38;5;%dm", n); }
static void bg(int n) { vf(CSI "48;5;%dm", n); }

/* Newline */
#define nl()  vw("\r\n")

/* Reset colours then newline */
#define rnl() vw(RST "\r\n")

/* -------------------------------------------------------------------------
 * Initialize NVS flash storage
 * ---------------------------------------------------------------------- */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");
}

/* -------------------------------------------------------------------------
 * Initialize network stack
 * ---------------------------------------------------------------------- */
static void init_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Network stack initialized");
}

/* -------------------------------------------------------------------------
 * Splash screen — colour capability showcase.
 *
 * Uses full ANSI escape sequences via vterm; the same libtsm/display
 * pipeline used for SSH session output.
 *
 * Layout (100 × 30 terminal):
 *   Row  0     : blank
 *   Row  1     : title bar (full-width, bright cyan on dark blue)
 *   Row  2     : subtitle
 *   Row  3     : blank
 *   Row  4-5   : ANSI-16 FG colour strip
 *   Row  6-7   : ANSI-16 BG colour strip
 *   Row  8     : SGR attributes
 *   Row  9     : blank
 *   Row 10     : colour cube label
 *   Row 11-16  : 6 × 6×6 RGB cube rows (6 green groups × 6 blue, 2-char ea)
 *   Row 17     : blank
 *   Row 18     : grayscale label
 *   Row 19     : 24-step grayscale ramp (3-char ea, 72 chars)
 *   Row 20     : blank
 *   Row 21     : status / ready line
 * ---------------------------------------------------------------------- */
static void show_splash_screen(void)
{
    /* Clear screen, home cursor */
    vw(CSI "2J" CSI "H");

    /* ── Title bar (full width) ─────────────────────────────────────── */
    fg(14); bg(17);  /* bright cyan on bright black (dark grey) */
    vw(CSI "1m");    /* bold */
    /* 100 chars: 2 sp + left pad + text + right pad + 2 sp */
    vw("  " "\xe2\x95\x94");   /* ╔ */
    for (int i = 0; i < 94; i++) vw("\xe2\x95\x90"); /* ═ ×94 */
    vw("\xe2\x95\x97" "  "); rnl(); /* ╗ */

    fg(14); bg(17); vw(CSI "1m");
    vw("  " "\xe2\x95\x91"); /* ║ */
    fg(15); bg(17); vw(CSI "1m");
    vw("          CYBERDECK SSH TERMINAL v0.1"
       "          ESP32-S3 \xc2\xb7 Terminus 8\xc3\x97" "16          ");
    fg(14); bg(17); vw(CSI "1m");
    vw("\xe2\x95\x91" "  "); rnl(); /* ║ */

    fg(14); bg(17);
    vw("  " "\xe2\x95\x9a");   /* ╚ */
    for (int i = 0; i < 94; i++) vw("\xe2\x95\x90"); /* ═ ×94 */
    vw("\xe2\x95\x9d" "  "); rnl(); /* ╝ */
    vw(RST); nl();

    /* ── ANSI-16 foreground colours ─────────────────────────────────── */
    fg(7); bg(0);
    vw(CSI "1m" "  FG: " RST);
    for (int c = 0; c < 16; c++) {
        vf(CSI "38;5;%dm", c);
        bg(c < 8 ? 0 : 8);  /* dark half on black, bright half on dark grey */
        vw(" \xe2\x96\x88\xe2\x96\x88 ");  /* ██ */
    }
    vw(RST); nl();

    /* ── ANSI-16 background colours ─────────────────────────────────── */
    fg(7); bg(0);
    vw(CSI "1m" "  BG: " RST);
    for (int c = 0; c < 16; c++) {
        fg(c < 8 ? 15 : 0);
        vf(CSI "48;5;%dm", c);
        vf(" %2d ", c);
    }
    vw(RST); nl(); nl();

    /* ── SGR attribute demo ──────────────────────────────────────────── */
    fg(7); bg(0);
    vw("  " CSI "1m" "Bold" RST
       "  " CSI "4m" "Underline" RST
       "  " CSI "7m" "Reverse" RST
       "  " CSI "1;32m" "Bold+Green" RST
       "  " CSI "4;33m" "Underline+Yellow" RST
       "  " CSI "7;35m" "Reverse+Magenta" RST);
    nl(); nl();

    /* ── 6×6×6 colour cube (ANSI 16-231) ───────────────────────────── */
    fg(7); bg(0);
    vw(CSI "1m" "  256-color cube:" RST);
    nl();

    for (int r = 0; r < 6; r++) {
        vw("  ");
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int idx = 16 + r * 36 + g * 6 + b;
                /* pick fg for legibility */
                int light = (r > 2 || g > 2 || (r + g + b) > 6);
                vf(CSI "38;5;%dm" CSI "48;5;%dm", light ? 0 : 15, idx);
                vw("  ");
            }
            if (g < 5) { fg(0); bg(0); vw(" "); }  /* gap between green groups */
        }
        vw(RST); nl();
    }
    nl();

    /* ── Grayscale ramp (ANSI 232-255) ──────────────────────────────── */
    fg(7); bg(0);
    vw(CSI "1m" "  Grayscale:" RST);
    vw("  ");
    for (int i = 0; i < 24; i++) {
        vf(CSI "38;5;%dm" CSI "48;5;%dm", (i < 12) ? 15 : 0, 232 + i);
        vf("%3d", 232 + i);
    }
    vw(RST); nl(); nl();

    /* ── Status line ─────────────────────────────────────────────────── */
    fg(10); bg(0); vw(CSI "1m");
    vw("  Initializing system...");
    vw(RST); nl();
}

/* -------------------------------------------------------------------------
 * Coloured status helpers
 *   info  — cyan   [*]
 *   ok    — green  [✓]
 *   fail  — red    [✗]
 * ---------------------------------------------------------------------- */
static void status_info(const char *msg)
{
    fg(6); bg(0); vw(CSI "1m");
    vw("  [");
    fg(14); vw("*");
    fg(6);  vw("] ");
    vw(RST); fg(7); bg(0);
    vterm_write(msg, strlen(msg));
    vw(RST); nl();
}

static void status_ok(const char *msg)
{
    fg(2); bg(0); vw(CSI "1m");
    vw("  [");
    fg(10); vw("\xe2\x9c\x93");  /* ✓ */
    fg(2);  vw("] ");
    vw(RST); fg(7); bg(0);
    vterm_write(msg, strlen(msg));
    vw(RST); nl();
}

static void status_fail(const char *msg)
{
    fg(1); bg(0); vw(CSI "1m");
    vw("  [");
    fg(9);  vw("\xe2\x9c\x97");  /* ✗ */
    fg(1);  vw("] ");
    vw(RST); fg(7); bg(0);
    vterm_write(msg, strlen(msg));
    vw(RST); nl();
}

/* -------------------------------------------------------------------------
 * Main application task
 * ---------------------------------------------------------------------- */
static void main_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Main task started");

    // Show splash screen
    ESP_LOGI(TAG, "Calling show_splash_screen");
    show_splash_screen();
    ESP_LOGI(TAG, "Splash done");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Initialize WiFi
    log_heap("before wifi_connect");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    status_info("Connecting to WiFi...");
    wifi_manager_connect();

    // Wait for WiFi connection
    EventBits_t bits = xEventGroupWaitBits(s_system_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        status_ok("WiFi connected");

        // Initialize BLE keyboard (optional)
        status_info("Waiting for BLE keyboard...");
        ble_keyboard_init();

        // Wait a bit for BLE pairing
        vTaskDelay(pdMS_TO_TICKS(3000));

        // Connect to SSH server
        nl();
        status_info("Connecting to SSH server...");

        ssh_config_t ssh_cfg = {
            .host     = CONFIG_SSH_DEFAULT_HOST,
            .port     = CONFIG_SSH_DEFAULT_PORT,
            .username = CONFIG_SSH_DEFAULT_USER,
            .password = "",  /* TODO: prompt for password */
        };

        if (ssh_client_connect(&ssh_cfg) == ESP_OK) {
            status_ok("SSH connected");
            xEventGroupSetBits(s_system_event_group, SSH_CONNECTED_BIT);

            /* Clear screen and hand off to SSH callbacks */
            vTaskDelay(pdMS_TO_TICKS(1000));
            vw(CSI "2J" CSI "H");

            /* Main loop — actual I/O handled by SSH client callbacks */
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else {
            status_fail("SSH connection failed");
        }
    } else {
        status_fail("WiFi connection timeout");
    }

    // Cleanup
    ESP_LOGI(TAG, "Main task ending");
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Application entry point
 * ---------------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Cyberdeck SSH Terminal");
    ESP_LOGI(TAG, "ESP32-S3 @ %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "===========================================");

    // Create system event group
    s_system_event_group = xEventGroupCreate();

    // Initialize subsystems
    log_heap("boot");
    init_nvs();
    log_heap("after nvs_init");
    init_network();
    log_heap("after netif_init");

    ESP_LOGI(TAG, "Initializing display...");
    display_init();
    log_heap("after display_init");

    ESP_LOGI(TAG, "Initializing font renderer...");
    font_init();
    log_heap("after font_init");

    ESP_LOGI(TAG, "Initializing vterm...");
    vterm_init(CONFIG_TERMINAL_WIDTH, CONFIG_TERMINAL_HEIGHT);
    log_heap("after vterm_init");

    /* Try to allocate the task stack from SPIRAM first so that scarce
     * internal DRAM is not exhausted.  Fall back to internal DRAM with
     * a smaller stack if no SPIRAM is present.                          */
#define MAIN_TASK_STACK  16384
    StackType_t  *task_stack = heap_caps_malloc(MAIN_TASK_STACK,
                                                MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_8BIT);
    if (!task_stack) {
        ESP_LOGW(TAG, "No SPIRAM for task stack, trying internal DRAM");
        task_stack = heap_caps_malloc(MAIN_TASK_STACK,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static StaticTask_t s_main_task_tcb;   /* TCB must be static / long-lived */

    if (!task_stack) {
        ESP_LOGE(TAG, "Cannot allocate task stack (%u B) — halting",
                 MAIN_TASK_STACK);
    } else {
        ESP_LOGI(TAG, "Task stack @ %p (%u B)", task_stack, MAIN_TASK_STACK);
        xTaskCreateStaticPinnedToCore(
            main_task,
            "main_task",
            MAIN_TASK_STACK / sizeof(StackType_t),
            NULL,
            5,
            task_stack,
            &s_main_task_tcb,
            1   /* Pin to core 1 */
        );
    }
#undef MAIN_TASK_STACK

    ESP_LOGI(TAG, "System initialized");
}

/*
 * Cyberdeck SSH Terminal — Application Entry Point
 *
 * ESP32-S3 portable SSH terminal with BLE keyboard and 7" display.
 * State machine: BOOT → WIFI_WAIT → SSH_CONNECT → SESSION → (loop)
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_heap_caps.h"

#include "display.h"
#include "font.h"
#include "vterm.h"
#include "input_hal.h"
#include "wifi_manager.h"
#include "ssh_client.h"
#include "splash.h"

static const char *TAG = "cyberdeck";

/* -------------------------------------------------------------------------
 * Heap diagnostic helper
 * ---------------------------------------------------------------------- */
static void log_heap(const char *label)
{
    ESP_LOGI(TAG, "Heap %-30s  total=%7u  int=%7u  int_blk=%7u",
             label,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

/* -------------------------------------------------------------------------
 * System init helpers
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

static void init_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Network stack initialized");
}

/* -------------------------------------------------------------------------
 * Application state machine
 * ---------------------------------------------------------------------- */
typedef enum {
    STATE_BOOT,
    STATE_WIFI_WAIT,
    STATE_SSH_CONNECT,
    STATE_SESSION,
    STATE_ERROR,
} app_state_t;

static void main_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Main task started");

    app_state_t state = STATE_BOOT;
    bool wifi_started = false;
    TickType_t wifi_timeout_tick = 0;

    for (;;) {
        switch (state) {

        /* ── BOOT: show splash, then wait for WiFi ─────────────────── */
        case STATE_BOOT:
            splash_show();
            vTaskDelay(pdMS_TO_TICKS(2000));
            vterm_bench_report();
            state = STATE_WIFI_WAIT;
            break;

        /* ── WIFI_WAIT: kick WiFi once, poll until connected ────────── */
        case STATE_WIFI_WAIT:
            if (!wifi_started) {
                splash_status_info("Connecting to WiFi...");
                wifi_manager_connect();
                wifi_started = true;
                wifi_timeout_tick = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
            }
            if (wifi_manager_is_connected()) {
                splash_status_ok("WiFi connected");
                state = STATE_SSH_CONNECT;
            } else if (xTaskGetTickCount() >= wifi_timeout_tick) {
                splash_status_fail("WiFi connection timeout");
                state = STATE_ERROR;
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            break;

        /* ── SSH_CONNECT: attempt connection, retry on failure ──────── */
        case STATE_SSH_CONNECT: {
            /* If WiFi dropped while we were here, go back to WIFI_WAIT */
            if (!wifi_manager_is_connected()) {
                ESP_LOGW(TAG, "WiFi lost, waiting for reconnect");
                wifi_started = false;
                state = STATE_WIFI_WAIT;
                break;
            }

            ssh_config_t ssh_cfg = {
                .host        = CONFIG_SSH_DEFAULT_HOST,
                .port        = CONFIG_SSH_DEFAULT_PORT,
                .username    = CONFIG_SSH_DEFAULT_USER,
                .password    = CONFIG_SSH_DEFAULT_PASSWORD,
                .private_key = NULL,
            };

            splash_status_info("Connecting to SSH server...");
            if (ssh_client_connect(&ssh_cfg) == ESP_OK) {
                splash_status_ok("SSH connected");
                vTaskDelay(pdMS_TO_TICKS(500));
                /* Clear screen and hand off to SSH session */
                vterm_write("\e[2J\e[H", 7);
                vterm_flush();
                state = STATE_SESSION;
            } else {
                splash_status_fail("SSH connection failed — retrying in 5 s...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                /* stay in STATE_SSH_CONNECT */
            }
            break;
        }

        /* ── SESSION: forward keyboard input until remote exits ─────── */
        case STATE_SESSION: {
            input_event_t ev;
            while (ssh_client_is_connected()) {
                if (input_hal_read(&ev, 100))
                    ssh_client_send(ev.buf, ev.len);
            }
#if CONFIG_SSH_AUTO_RECONNECT
            ESP_LOGI(TAG, "SSH session ended, reconnecting...");
            state = STATE_SSH_CONNECT;
#else
            splash_status_fail("SSH session ended");
            state = STATE_ERROR;
#endif
            break;
        }

        /* ── ERROR: display message and halt ────────────────────────── */
        case STATE_ERROR:
            splash_status_fail("Fatal error — system halted");
            for (;;) vTaskDelay(portMAX_DELAY);
            break;
        }
    }
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

    ESP_LOGI(TAG, "Initializing input HAL...");
    if (input_hal_init() != ESP_OK)
        ESP_LOGW(TAG, "Input HAL init failed (non-fatal)");
    log_heap("after input_hal_init");

    /* Try to allocate task stack from SPIRAM; fall back to internal DRAM. */
#define MAIN_TASK_STACK  16384
    StackType_t *task_stack = heap_caps_malloc(MAIN_TASK_STACK,
                                               MALLOC_CAP_SPIRAM |
                                               MALLOC_CAP_8BIT);
    if (!task_stack) {
        ESP_LOGW(TAG, "No SPIRAM for task stack, falling back to internal DRAM");
        task_stack = heap_caps_malloc(MAIN_TASK_STACK,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static StaticTask_t s_main_task_tcb;

    if (!task_stack) {
        ESP_LOGE(TAG, "Cannot allocate task stack (%u B) — halting", MAIN_TASK_STACK);
        for (;;) vTaskDelay(portMAX_DELAY);
    }

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
#undef MAIN_TASK_STACK

    ESP_LOGI(TAG, "System initialized");
}

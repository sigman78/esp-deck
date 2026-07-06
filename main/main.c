/*
 * Cyberdeck SSH Terminal — device composition root.
 *
 * Hardware/IDF initialization stays here. Shared app orchestration lives in
 * the cyberdeck_app component.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "ble_keyboard.h"
#include "cyberdeck_app.h"
#include "display.h"
#include "font.h"
#include "input_hal.h"
#include "pairing_overlay.h"
#include "splash.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"

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

static void app_status_info(const char *msg, void *user)
{
    (void)user;
    splash_status_info(msg);
}

static void app_status_ok(const char *msg, void *user)
{
    (void)user;
    splash_status_ok(msg);
}

static void app_status_fail(const char *msg, void *user)
{
    (void)user;
    splash_status_fail(msg);
}

static void app_request_pairing(void *user)
{
    (void)user;
    pairing_overlay_run();
}

static void main_task(void *pvParameters)
{
    cyberdeck_app_t *app = (cyberdeck_app_t *)pvParameters;

    ESP_LOGI(TAG, "Main task started");
    while (cyberdeck_app_is_running(app)) {
        cyberdeck_app_tick(app, (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS);

        input_event_t ev;
        if (!input_hal_read(&ev, 100)) {
            continue;
        }

        if (ev.type == INPUT_EVENT_KEY) {
            cyberdeck_app_send_bytes(app, ev.buf, ev.len);
        } else if (ev.type == INPUT_EVENT_LONG_PRESS) {
            cyberdeck_app_handle_pairing_request(app);
        }
    }

    vterm_bench_report();
    splash_status_fail("Fatal error — system halted");
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* -------------------------------------------------------------------------
 * Application entry point
 * ---------------------------------------------------------------------- */
void app_main(void)
{
    static cyberdeck_app_t s_app;

    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Cyberdeck SSH Terminal");
    ESP_LOGI(TAG, "ESP32-S3 @ %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "===========================================");

    log_heap("boot");
    init_nvs();
    log_heap("after nvs_init");

    if (storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "Storage init failed — using Kconfig defaults");
    }
    log_heap("after storage_init");

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
    splash_show();
    log_heap("after vterm_init");

    ESP_LOGI(TAG, "Initializing input HAL...");
    if (input_hal_init() != ESP_OK) {
        ESP_LOGW(TAG, "Input HAL init failed (non-fatal)");
    }
    log_heap("after input_hal_init");

    if (ssh_client_init() != ESP_OK) {
        ESP_LOGW(TAG, "SSH client init failed (non-fatal)");
    }

    cyberdeck_app_config_t app_cfg = {
        .boot_delay_ms = 2000,
        .wifi_timeout_ms = 30000,
        .ssh_retry_delay_ms = 5000,
        .auto_reconnect = CONFIG_SSH_AUTO_RECONNECT,
        .prefer_explicit_connection = false,
        .default_host = CONFIG_SSH_DEFAULT_HOST,
        .default_port = CONFIG_SSH_DEFAULT_PORT,
        .default_user = CONFIG_SSH_DEFAULT_USER,
        .default_password = CONFIG_SSH_DEFAULT_PASSWORD,
        .status_info = app_status_info,
        .status_ok = app_status_ok,
        .status_fail = app_status_fail,
        .request_pairing = app_request_pairing,
        .user = NULL,
    };

    ESP_ERROR_CHECK(cyberdeck_app_init(&s_app, &app_cfg, 0));

    /* Try to allocate task stack from SPIRAM; fall back to internal DRAM. */
#define MAIN_TASK_STACK 16384
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
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    ESP_LOGI(TAG, "Task stack @ %p (%u B)", task_stack, MAIN_TASK_STACK);
    xTaskCreateStaticPinnedToCore(
        main_task,
        "main_task",
        MAIN_TASK_STACK / sizeof(StackType_t),
        &s_app,
        5,
        task_stack,
        &s_main_task_tcb,
        1
    );
#undef MAIN_TASK_STACK

    ESP_LOGI(TAG, "System initialized");
}

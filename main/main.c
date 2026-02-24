/*
 * Cyberdeck SSH Terminal - Main Application
 *
 * ESP32-S3 based portable SSH terminal with BLE keyboard and 7" display
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"

// Component headers
#include "display.h"
#include "terminal.h"
#include "ssh_client.h"
#include "ble_keyboard.h"
#include "wifi_manager.h"
#include "font.h"

static const char *TAG = "cyberdeck";

// Event group for system state
static EventGroupHandle_t s_system_event_group;

#define WIFI_CONNECTED_BIT  BIT0
#define SSH_CONNECTED_BIT   BIT1
#define BLE_PAIRED_BIT      BIT2

/**
 * Initialize NVS flash storage
 */
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

/**
 * Initialize network stack
 */
static void init_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "Network stack initialized");
}

/**
 * Display splash screen
 */
static void show_splash_screen(void)
{
    terminal_clear();
    terminal_set_cursor(0, 0);

    terminal_print("\n");
    terminal_print("  ╔═══════════════════════════════════════════════════════╗\n");
    terminal_print("  ║                                                       ║\n");
    terminal_print("  ║              CYBERDECK SSH TERMINAL v0.1              ║\n");
    terminal_print("  ║                                                       ║\n");
    terminal_print("  ║         ESP32-S3 Portable Terminal Device             ║\n");
    terminal_print("  ║                                                       ║\n");
    terminal_print("  ╚═══════════════════════════════════════════════════════╝\n");
    terminal_print("\n");
    terminal_print("  Initializing system...\n\n");
}

/**
 * Main application task
 */
static void main_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Main task started");

    // Show splash screen
    show_splash_screen();
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Initialize WiFi
    terminal_print("  [*] Connecting to WiFi...\n");
    wifi_manager_connect();

    // Wait for WiFi connection
    EventBits_t bits = xEventGroupWaitBits(s_system_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        terminal_print("  [✓] WiFi connected\n");

        // Initialize BLE keyboard (optional)
        terminal_print("  [*] Waiting for BLE keyboard...\n");
        ble_keyboard_init();

        // Wait a bit for BLE pairing
        vTaskDelay(pdMS_TO_TICKS(3000));

        // Connect to SSH server
        terminal_print("\n  [*] Connecting to SSH server...\n");

        ssh_config_t ssh_cfg = {
            .host = CONFIG_SSH_DEFAULT_HOST,
            .port = CONFIG_SSH_DEFAULT_PORT,
            .username = CONFIG_SSH_DEFAULT_USER,
            .password = "",  // TODO: Prompt for password
        };

        if (ssh_client_connect(&ssh_cfg) == ESP_OK) {
            terminal_print("  [✓] SSH connected\n");
            xEventGroupSetBits(s_system_event_group, SSH_CONNECTED_BIT);

            // Clear screen and start interactive session
            vTaskDelay(pdMS_TO_TICKS(1000));
            terminal_clear();

            // Main loop - handled by SSH client callbacks
            while (1) {
                // Keep task alive, actual I/O handled by callbacks
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else {
            terminal_print("  [✗] SSH connection failed\n");
        }
    } else {
        terminal_print("  [✗] WiFi connection timeout\n");
    }

    // Cleanup
    ESP_LOGI(TAG, "Main task ending");
    vTaskDelete(NULL);
}

/**
 * Application entry point
 */
void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Cyberdeck SSH Terminal POC");
    ESP_LOGI(TAG, "ESP32-S3 @ %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "===========================================");

    // Create system event group
    s_system_event_group = xEventGroupCreate();

    // Initialize subsystems
    init_nvs();
    init_network();

    // Initialize display and terminal
    ESP_LOGI(TAG, "Initializing display...");
    display_init();

    ESP_LOGI(TAG, "Initializing font renderer...");
    font_init();

    ESP_LOGI(TAG, "Initializing terminal...");
    terminal_init(CONFIG_TERMINAL_WIDTH, CONFIG_TERMINAL_HEIGHT);

    // Create main application task
    xTaskCreatePinnedToCore(
        main_task,
        "main_task",
        8192,
        NULL,
        5,
        NULL,
        1  // Pin to core 1
    );

    ESP_LOGI(TAG, "System initialized, entering main loop");
}

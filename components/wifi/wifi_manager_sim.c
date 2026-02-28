#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "wifi_manager.h"
#include "esp_log.h"

static const char *TAG = "wifi_sim";

esp_err_t wifi_manager_init(void)
{
    /* Initialize Winsock — required before any socket/getaddrinfo calls. */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        ESP_LOGE(TAG, "WSAStartup failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_connect(void)    { ESP_LOGI(TAG, "WiFi mocked (simulator)"); return ESP_OK; }
esp_err_t wifi_manager_disconnect(void) { WSACleanup(); return ESP_OK; }
bool wifi_manager_is_connected(void)    { return true; }
const char *wifi_manager_get_ip(void)   { return "127.0.0.1"; }

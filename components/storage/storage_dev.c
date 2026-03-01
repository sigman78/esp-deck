/*
 * storage_dev.c — LittleFS platform backend (ESP-IDF only).
 *
 * Mounts the "storage" LittleFS partition via esp_vfs_littlefs_register so
 * that /littlefs/... paths work with standard fopen/fread/fwrite/fclose.
 * Creates /littlefs/keys/ if it does not already exist.
 */

#include "storage.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define MOUNT_POINT  "/littlefs"
#define PART_LABEL   "storage"

static const char *TAG = "storage_dev";

const char *storage_platform_mount_point(void)
{
    return MOUNT_POINT;
}

esp_err_t storage_platform_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = MOUNT_POINT,
        .partition_label        = PART_LABEL,
        .format_if_mount_failed = true,   /* auto-format blank partition */
        .dont_mount             = false,
        .grow_on_mount          = true,
        .read_only              = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS (partition '%s'): %d",
                 PART_LABEL, ret);
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(PART_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at '%s': %zu KB total, %zu KB used",
             MOUNT_POINT, total / 1024, used / 1024);

    /* Ensure keys/ subdirectory exists */
    struct stat st;
    if (stat(MOUNT_POINT "/keys", &st) != 0) {
        if (mkdir(MOUNT_POINT "/keys", 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create '%s/keys': errno=%d",
                     MOUNT_POINT, errno);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created directory '%s/keys'", MOUNT_POINT);
    }

    return ESP_OK;
}

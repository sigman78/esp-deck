/*
 * storage_sim.c — local directory platform backend (simulator only).
 *
 * Creates sim_storage/ and sim_storage/keys/ in the current working
 * directory (i.e. build-sim/) if they do not already exist.
 */

#include "storage.h"
#include "esp_log.h"

#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  include <direct.h>
#  define FS_MKDIR(path)  _mkdir(path)
#else
#  include <sys/types.h>
#  define FS_MKDIR(path)  mkdir((path), 0755)
#endif

#define SIM_MOUNT "sim_storage"

static const char *TAG = "storage_sim";

const char *storage_platform_mount_point(void)
{
    return SIM_MOUNT;
}

static esp_err_t ensure_dir(const char *path)
{
    if (FS_MKDIR(path) == 0) {
        ESP_LOGI(TAG, "Created directory '%s'", path);
        return ESP_OK;
    }
    if (errno == EEXIST)
        return ESP_OK;
    ESP_LOGE(TAG, "mkdir '%s' failed: errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t storage_platform_init(void)
{
    esp_err_t ret;
    ret = ensure_dir(SIM_MOUNT);
    if (ret != ESP_OK) return ret;
    ret = ensure_dir(SIM_MOUNT "/keys");
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "Sim storage ready at '%s'", SIM_MOUNT);
    return ESP_OK;
}

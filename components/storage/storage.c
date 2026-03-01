/*
 * storage.c — INI parser, CRUD, and key I/O.
 *
 * Compiled on BOTH device and simulator.
 * All I/O uses standard C stdio (fopen/fgets/fclose/fprintf/fread/fwrite).
 * After storage_platform_init() the VFS (LittleFS or host FS) presents the
 * mount point as a normal directory to the C runtime.
 */

#include "storage.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "storage";

/* -------------------------------------------------------------------------
 * Internal path helpers
 * ---------------------------------------------------------------------- */

static void profiles_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/profiles.ini", storage_platform_mount_point());
}

static void key_path(const char *key_id, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/keys/%s.pem",
             storage_platform_mount_point(), key_id);
}

/* -------------------------------------------------------------------------
 * INI parse helpers
 * ---------------------------------------------------------------------- */

/* Remove trailing \r, \n, space in place */
static void rtrim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 &&
           (s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == ' '))
        s[--len] = '\0';
}

/* Extract section name from "[section]" into dst[dstsz].
 * Returns 1 on success, 0 if not a section header. */
static int parse_section(const char *line, char *dst, size_t dstsz)
{
    if (line[0] != '[') return 0;
    const char *end = strchr(line + 1, ']');
    if (!end) return 0;
    size_t len = (size_t)(end - (line + 1));
    if (len == 0 || len >= dstsz) return 0;
    memcpy(dst, line + 1, len);
    dst[len] = '\0';
    return 1;
}

/* Split "key=value" line into key and val strings.
 * Both buffers must be at least as large as the source line.
 * Returns 1 on success, 0 if no '=' found. */
static int parse_kv(const char *line, char *key, char *val)
{
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    size_t klen = (size_t)(eq - line);
    memcpy(key, line, klen);
    key[klen] = '\0';
    strcpy(val, eq + 1);
    return 1;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

esp_err_t storage_init(void)
{
    esp_err_t ret = storage_platform_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Platform init failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "Storage ready at '%s'", storage_platform_mount_point());
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Profile load
 * ---------------------------------------------------------------------- */

esp_err_t storage_load_profiles(conn_profile_t *out, int *count, int max)
{
    if (!out || !count || max <= 0) return ESP_ERR_INVALID_ARG;
    *count = 0;

    char path[128];
    profiles_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "profiles.ini not found at '%s' (first boot?)", path);
        return ESP_OK;   /* absent file is not an error */
    }

    char line[256];
    int  cur_idx = -1;

    while (fgets(line, sizeof(line), f)) {
        rtrim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;

        char section[32];
        if (parse_section(line, section, sizeof(section))) {
            if (*count >= max) {
                ESP_LOGW(TAG, "Max profiles (%d) reached, skipping [%s]",
                         max, section);
                cur_idx = -1;
                continue;
            }
            cur_idx = *count;
            memset(&out[cur_idx], 0, sizeof(conn_profile_t));
            snprintf(out[cur_idx].name, sizeof(out[cur_idx].name), "%s", section);
            out[cur_idx].port = 22;
            (*count)++;
            continue;
        }

        if (cur_idx < 0) continue;

        char key[128], val[128];
        if (!parse_kv(line, key, val)) continue;

        conn_profile_t *p = &out[cur_idx];

        if (strcmp(key, "host") == 0) {
            snprintf(p->host, sizeof(p->host), "%s", val);
        } else if (strcmp(key, "port") == 0) {
            int v = atoi(val);
            p->port = (v > 0 && v <= 65535) ? (uint16_t)v : 22;
        } else if (strcmp(key, "user") == 0) {
            snprintf(p->user, sizeof(p->user), "%s", val);
        } else if (strcmp(key, "auth") == 0) {
            p->auth = (strcmp(val, "key") == 0)
                      ? STORAGE_AUTH_KEY : STORAGE_AUTH_PASSWORD;
        } else if (strcmp(key, "password") == 0) {
            snprintf(p->password, sizeof(p->password), "%s", val);
        } else if (strcmp(key, "key_id") == 0) {
            snprintf(p->key_id, sizeof(p->key_id), "%s", val);
        }
        /* Unknown keys silently ignored for forward compatibility */
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d profile(s) from '%s'", *count, path);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Profile save
 * ---------------------------------------------------------------------- */

esp_err_t storage_save_profiles(const conn_profile_t *profiles, int count)
{
    if (!profiles && count > 0) return ESP_ERR_INVALID_ARG;

    char path[128];
    profiles_path(path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open '%s' for write: errno=%d", path, errno);
        return ESP_FAIL;
    }

    for (int i = 0; i < count; i++) {
        const conn_profile_t *p = &profiles[i];
        fprintf(f, "[%s]\n", p->name);
        fprintf(f, "host=%s\n", p->host);
        fprintf(f, "port=%u\n", (unsigned)p->port);
        fprintf(f, "user=%s\n", p->user);
        fprintf(f, "auth=%s\n",
                p->auth == STORAGE_AUTH_KEY ? "key" : "password");
        if (p->auth == STORAGE_AUTH_KEY)
            fprintf(f, "key_id=%s\n", p->key_id);
        else
            fprintf(f, "password=%s\n", p->password);
        fprintf(f, "\n");
    }

    fclose(f);
    ESP_LOGI(TAG, "Saved %d profile(s) to '%s'", count, path);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Find profile by name
 * ---------------------------------------------------------------------- */

const conn_profile_t *storage_find_profile(const conn_profile_t *profiles,
                                            int count,
                                            const char *name)
{
    if (!profiles || !name) return NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(profiles[i].name, name) == 0)
            return &profiles[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Key get
 * ---------------------------------------------------------------------- */

esp_err_t storage_get_key(const char *key_id,
                           char       *buf,
                           size_t      buf_len,
                           size_t     *written)
{
    if (!key_id || !buf || buf_len == 0 || !written)
        return ESP_ERR_INVALID_ARG;

    char path[160];
    key_path(key_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Key '%s' not found at '%s'", key_id, path);
        return ESP_FAIL;
    }

    size_t n = fread(buf, 1, buf_len - 1, f);
    fclose(f);

    if (n == 0) {
        ESP_LOGE(TAG, "Key file '%s' is empty", path);
        return ESP_FAIL;
    }
    if (n == buf_len - 1)
        ESP_LOGW(TAG, "Key '%s' may be truncated (buf_len=%zu)", key_id, buf_len);

    buf[n] = '\0';
    *written = n;
    ESP_LOGI(TAG, "Read key '%s' (%zu bytes)", key_id, n);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Key set
 * ---------------------------------------------------------------------- */

esp_err_t storage_set_key(const char *key_id, const char *pem, size_t len)
{
    if (!key_id || !pem || len == 0) return ESP_ERR_INVALID_ARG;

    char path[160];
    key_path(key_id, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open '%s' for write: errno=%d", path, errno);
        return ESP_FAIL;
    }

    size_t n = fwrite(pem, 1, len, f);
    fclose(f);

    if (n != len) {
        ESP_LOGE(TAG, "Short write: %zu of %zu bytes for key '%s'",
                 n, len, key_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wrote key '%s' (%zu bytes)", key_id, len);
    return ESP_OK;
}

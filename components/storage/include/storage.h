/*
 * storage.h — Persistent profile and key storage for the Cyberdeck.
 *
 * Provides:
 *   - INI-based SSH connection profiles (load/save)
 *   - PEM key blob storage (get/set)
 *
 * Does NOT depend on the ssh component. Callers translate conn_profile_t
 * to ssh_config_t themselves.
 *
 * Platform backends:
 *   - Device: LittleFS mounted at /littlefs  (storage_dev.c)
 *   - Sim:    local directory sim_storage/   (storage_sim.c)
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef enum {
    STORAGE_AUTH_PASSWORD = 0,
    STORAGE_AUTH_KEY,
} storage_auth_t;

typedef struct {
    char            name[32];       /* Profile name, e.g. "default"  */
    char            host[64];       /* Hostname or IP                 */
    uint16_t        port;           /* TCP port, typically 22         */
    char            user[32];       /* Username                       */
    storage_auth_t  auth;           /* Authentication method          */
    char            password[64];   /* Password  (auth == PASSWORD)   */
    char            key_id[32];     /* Key identifier (auth == KEY)   */
} conn_profile_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * Initialize the storage subsystem.
 *
 * On device: mounts LittleFS, ensures keys/ directory exists.
 * On sim:    ensures sim_storage/ and sim_storage/keys/ exist.
 *
 * Must be called before any other storage_* function.
 *
 * @return ESP_OK on success, ESP_FAIL on mount/mkdir failure.
 */
esp_err_t storage_init(void);

/* -------------------------------------------------------------------------
 * Connection profiles
 * ---------------------------------------------------------------------- */

/**
 * Load all profiles from <mount_point>/profiles.ini.
 *
 * Parses the INI file into @p out. At most @p max profiles are written.
 * If the file does not exist, sets *count = 0 and returns ESP_OK.
 *
 * @param out   Output array of conn_profile_t, caller-allocated.
 * @param count Set to the number of profiles actually parsed.
 * @param max   Maximum number of profiles to write into @p out.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_FAIL.
 */
esp_err_t storage_load_profiles(conn_profile_t *out, int *count, int max);

/**
 * Save profiles to <mount_point>/profiles.ini, overwriting any prior file.
 *
 * @param profiles  Array of profiles to write.
 * @param count     Number of profiles in the array.
 * @return ESP_OK or ESP_FAIL.
 */
esp_err_t storage_save_profiles(const conn_profile_t *profiles, int count);

/**
 * Find a profile by name within an already-loaded array.
 *
 * Linear scan, case-sensitive.
 *
 * @return Pointer to the matching profile, or NULL if not found.
 */
const conn_profile_t *storage_find_profile(const conn_profile_t *profiles,
                                           int count,
                                           const char *name);

/* -------------------------------------------------------------------------
 * SSH key blobs
 * ---------------------------------------------------------------------- */

/**
 * Read a PEM key from <mount_point>/keys/<key_id>.pem.
 *
 * @param key_id    Key identifier (filename stem, no path, no extension).
 * @param buf       Caller-supplied buffer.
 * @param buf_len   Size of @p buf in bytes.
 * @param written   Set to the number of bytes written (excluding NUL).
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_FAIL.
 */
esp_err_t storage_get_key(const char *key_id,
                           char       *buf,
                           size_t      buf_len,
                           size_t     *written);

/**
 * Write a PEM key to <mount_point>/keys/<key_id>.pem, overwriting prior.
 *
 * @param key_id    Key identifier (filename stem, no path, no extension).
 * @param pem       PEM data (need not be NUL-terminated).
 * @param len       Length of @p pem in bytes.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_FAIL.
 */
esp_err_t storage_set_key(const char *key_id, const char *pem, size_t len);

/* -------------------------------------------------------------------------
 * Platform seam — implemented in storage_dev.c / storage_sim.c
 * ---------------------------------------------------------------------- */

/** Platform-specific initialisation. Called only by storage_init(). */
esp_err_t   storage_platform_init(void);

/** Mount-point string, e.g. "/littlefs" or "sim_storage". Valid forever. */
const char *storage_platform_mount_point(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */

# BLE Keyboard Connectivity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Commission the never-tested BLE keyboard pipeline: formal state machine, persistent device registry, pairing overlay UI, touch long-press trigger, and main app integration.

**Architecture:** Extend `ble_keyboard.c` with a 5-state machine; add `ble_device_info_t` + `storage_ble_*` to the storage component (INI file, same pattern as profiles); render a modal pairing overlay via `vterm_write()`; add GT911 tap/long-press detection in `touch_input.c`; wire everything into two new `main.c` states (BLE_INIT, PAIRING).

**Tech Stack:** ESP-IDF 5.5+, esp_hidh BLE HID host, Bluedroid NVS bonding, I2C master (new API), LittleFS via stdio, FreeRTOS queues, vterm ANSI rendering.

---

## File Map

| File | Action | Responsibility |
|------|--------|---------------|
| `components/input/hid_keymap.h` | Modify | Add `bool app_cursor` param |
| `components/input/hid_keymap.c` | Modify | Dynamic arrow key mode |
| `components/input/include/input_hal.h` | Modify | Add `type`, `x`, `y` to `input_event_t`; event type constants |
| `components/input/input_hal.c` | Modify | Call `touch_input_backend_init()` |
| `components/input/input_hal_internal.h` | Modify | Add `touch_input_backend_init()` decl |
| `components/input/input_uart.c` | Modify | Set `ev.type = INPUT_EVENT_KEY` |
| `components/input/ble_keyboard.h` | Rewrite | New `ble_state_t` API replacing old stub API |
| `components/input/ble_keyboard.c` | Rewrite | 5-state machine, registry integration, cursor mode |
| `components/input/touch_input.c` | Implement | GT911 I2C tap + long-press |
| `components/input/Kconfig.projbuild` | Modify | GT911 pin + address Kconfig |
| `components/input/pairing_overlay.h` | Create | `pairing_overlay_run()` declaration |
| `components/input/pairing_overlay.c` | Create | Modal pairing UI via vterm |
| `components/input/CMakeLists.txt` | Modify | Add `pairing_overlay.c`; add `vterm` to REQUIRES |
| `components/storage/include/storage.h` | Modify | Add `ble_device_info_t`, `storage_ble_*` |
| `components/storage/storage.c` | Modify | Implement `storage_ble_*` |
| `main/main.c` | Modify | Add `STATE_BLE_INIT`, `STATE_PAIRING`; long-press handling |

---

## Task 1: Fix Arrow Key Application Cursor Mode

**Why first:** Correctness bug that affects every arrow key press; independent change, easy to verify.

**Files:**
- Modify: `components/input/hid_keymap.h`
- Modify: `components/input/hid_keymap.c`
- Modify: `components/input/ble_keyboard.c` (call site)
- Modify: `components/input/CMakeLists.txt` (add vterm dependency)

- [ ] **Step 1: Add `vterm` to input component REQUIRES**

Edit `components/input/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS
        "input_hal.c"
        "hid_keymap.c"
        "ble_keyboard.c"
        "input_uart.c"
        "touch_input.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        bt
        esp_hid
        nvs_flash
        driver
        freertos
        esp_event
        vterm
)
```

- [ ] **Step 2: Update `hid_keymap_translate()` signature**

Replace full content of `components/input/hid_keymap.h`:
```c
/*
 * HID Usage ID → terminal byte sequence translator — private header
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Translate a HID keyboard keycode + modifier byte into terminal bytes.
 *
 * @param keycode    HID Usage ID (e.g. 0x04 = 'a')
 * @param modifiers  HID modifier byte (bit0=LCtrl, bit1=LShift, bit2=LAlt, …)
 * @param app_cursor true = application cursor key mode (arrows → ESC O A/B/C/D)
 *                   false = normal cursor key mode    (arrows → ESC [ A/B/C/D)
 * @param buf        Output buffer — must be at least INPUT_EVENT_MAX_LEN bytes
 * @return           Number of bytes written; 0 if keycode is unrecognised
 */
uint8_t hid_keymap_translate(uint8_t keycode, uint8_t modifiers,
                             bool app_cursor, uint8_t *buf);
```

- [ ] **Step 3: Remove arrow keys from s_specials and handle them dynamically**

In `components/input/hid_keymap.c`, replace the `s_specials[]` array (remove the 4 arrow entries at lines 77-80) and update the function signature + add dynamic arrow handling:

Replace lines 75-152 with:
```c
static const hid_special_t s_specials[] = {
    /* F1–F4 (SS3 sequences) */
    { 0x3A, 3, { 0x1B, 'O', 'P'           } },
    { 0x3B, 3, { 0x1B, 'O', 'Q'           } },
    { 0x3C, 3, { 0x1B, 'O', 'R'           } },
    { 0x3D, 3, { 0x1B, 'O', 'S'           } },
    /* F5–F12 (CSI Pn ~) */
    { 0x3E, 5, { 0x1B, '[', '1', '5', '~' } },
    { 0x3F, 5, { 0x1B, '[', '1', '7', '~' } },
    { 0x40, 5, { 0x1B, '[', '1', '8', '~' } },
    { 0x41, 5, { 0x1B, '[', '1', '9', '~' } },
    { 0x42, 5, { 0x1B, '[', '2', '0', '~' } },
    { 0x43, 5, { 0x1B, '[', '2', '1', '~' } },
    { 0x44, 5, { 0x1B, '[', '2', '3', '~' } },
    { 0x45, 5, { 0x1B, '[', '2', '4', '~' } },
    /* Navigation cluster */
    { 0x49, 4, { 0x1B, '[', '2', '~'      } },  /* Insert  */
    { 0x4C, 4, { 0x1B, '[', '3', '~'      } },  /* Delete  */
    { 0x4A, 3, { 0x1B, '[', 'H'           } },  /* Home    */
    { 0x4D, 3, { 0x1B, '[', 'F'           } },  /* End     */
    { 0x4B, 5, { 0x1B, '[', '5', '~', 0   } },  /* Page Up */
    { 0x4E, 5, { 0x1B, '[', '6', '~', 0   } },  /* Page Dn */
    /* Numpad Enter */
    { 0x58, 1, { '\r'                      } },
};

#define SPECIALS_CNT  (sizeof(s_specials) / sizeof(s_specials[0]))

/* Arrow key HID codes and their ANSI suffix characters */
static const uint8_t s_arrow_hid[]  = { 0x52, 0x51, 0x4F, 0x50 };
static const char    s_arrow_char[] = { 'A',  'B',  'C',  'D'  };
#define ARROW_CNT  4

uint8_t hid_keymap_translate(uint8_t keycode, uint8_t modifiers,
                             bool app_cursor, uint8_t *buf)
{
    /* --- printable range --- */
    if (keycode >= PRINTABLE_MIN && keycode <= PRINTABLE_MAX) {
        uint8_t idx = keycode - PRINTABLE_MIN;
        uint8_t ch  = SHIFT(modifiers) ? s_printable[idx][1]
                                       : s_printable[idx][0];

        if (CTRL(modifiers)) {
            if (keycode == 0x1F) {   /* '2' unshifted → NUL */
                ch = 0x00;
            } else {
                ch = ch & 0x1Fu;
            }
            if (ALT(modifiers)) {
                buf[0] = 0x1B;
                buf[1] = ch;
                return 2;
            }
            buf[0] = ch;
            return 1;
        }

        if (ALT(modifiers)) {
            buf[0] = 0x1B;
            buf[1] = ch;
            return 2;
        }

        buf[0] = ch;
        return 1;
    }

    /* --- arrow keys — mode-aware --- */
    for (uint8_t i = 0; i < ARROW_CNT; i++) {
        if (keycode == s_arrow_hid[i]) {
            buf[0] = 0x1B;
            buf[1] = app_cursor ? 'O' : '[';
            buf[2] = s_arrow_char[i];
            return 3;
        }
    }

    /* --- other special keys --- */
    for (uint8_t i = 0; i < SPECIALS_CNT; i++) {
        if (s_specials[i].hid == keycode) {
            uint8_t len = s_specials[i].len;
            memcpy(buf, s_specials[i].seq, len);
            return len;
        }
    }

    return 0;   /* unrecognised */
}
```

- [ ] **Step 4: Update call site in `ble_keyboard.c`**

Add `#include "vterm.h"` near the top of `ble_keyboard.c` (after the existing includes):
```c
#include "vterm.h"
```

In `hidh_callback`, replace:
```c
uint8_t len = hid_keymap_translate(kc, modifiers, buf);
```
with:
```c
uint8_t len = hid_keymap_translate(kc, modifiers,
                                   vterm_app_cursor_keys(), buf);
```

- [ ] **Step 5: Build to verify no compile errors**

```bash
cd D:/esp32/unbreezy/cyberdeck
idf.py build 2>&1 | tail -20
```
Expected: `Build successful` — no errors about `hid_keymap_translate`.

- [ ] **Step 6: Commit**

```bash
git add components/input/hid_keymap.h components/input/hid_keymap.c \
        components/input/ble_keyboard.c components/input/CMakeLists.txt
git commit -m "fix(input): arrow keys respect application cursor mode via vterm_app_cursor_keys()"
```

---

## Task 2: Extend `input_event_t` for Type and Touch Coordinates

**Files:**
- Modify: `components/input/include/input_hal.h`
- Modify: `components/input/input_uart.c`
- Modify: `components/input/ble_keyboard.c`

- [ ] **Step 1: Update `input_hal.h`**

Replace the `input_event_t` typedef and add event type constants. Full new content of the `input_event_t` block in `components/input/include/input_hal.h`:

```c
/* Event type identifiers */
#define INPUT_EVENT_KEY        0   /* keyboard byte sequence: buf[0..len-1] */
#define INPUT_EVENT_TAP        1   /* touch tap: x and y are valid */
#define INPUT_EVENT_LONG_PRESS 2   /* touch long-press: x and y are valid */

#define INPUT_EVENT_MAX_LEN  8

typedef struct {
    uint8_t  type;                  /* INPUT_EVENT_KEY / TAP / LONG_PRESS   */
    uint8_t  len;                   /* byte count in buf (KEY events only)  */
    uint8_t  buf[INPUT_EVENT_MAX_LEN];
    uint16_t x;                     /* touch X coordinate (touch events)    */
    uint16_t y;                     /* touch Y coordinate (touch events)    */
} input_event_t;
```

(The `type` field defaults to 0 = `INPUT_EVENT_KEY` when zero-initialised, so existing code posting key events remains correct with `{ .len = n }` or `{ .buf = {b}, .len = 1 }`.)

- [ ] **Step 2: Explicitly set type in `input_uart.c`**

In `components/input/input_uart.c`, find the event construction inside `uart_input_task`. Change:
```c
input_event_t ev = { .buf = {byte}, .len = 1 };
```
to:
```c
input_event_t ev = { .type = INPUT_EVENT_KEY, .buf = {byte}, .len = 1 };
```

(Exact line may vary — search for `input_event_t ev` in that file.)

- [ ] **Step 3: Explicitly set type in `ble_keyboard.c`**

In `components/input/ble_keyboard.c`, inside `hidh_callback` `ESP_HIDH_INPUT_EVT` case, change:
```c
input_event_t ev = { .len = len };
for (uint8_t j = 0; j < len; j++) ev.buf[j] = buf[j];
```
to:
```c
input_event_t ev = { .type = INPUT_EVENT_KEY, .len = len };
for (uint8_t j = 0; j < len; j++) ev.buf[j] = buf[j];
```

- [ ] **Step 4: Build to verify**

```bash
idf.py build 2>&1 | tail -20
```
Expected: `Build successful`.

- [ ] **Step 5: Commit**

```bash
git add components/input/include/input_hal.h \
        components/input/input_uart.c \
        components/input/ble_keyboard.c
git commit -m "feat(input): extend input_event_t with type field and touch x/y coordinates"
```

---

## Task 3: BLE Device Registry in Storage Component

**Files:**
- Modify: `components/storage/include/storage.h`
- Modify: `components/storage/storage.c`

Storage format — `<mount>/ble_devices.ini`:
```ini
[AA:BB:CC:DD:EE:FF]
addr_type=0
name=Keychron K3 Pro
last_seen=1711584000
```
Section name = MAC address in `XX:XX:XX:XX:XX:XX` format. Follows the same INI pattern as `profiles.ini`.

- [ ] **Step 1: Add `ble_device_info_t` and `storage_ble_*` declarations to `storage.h`**

Append before the final `#endif` in `components/storage/include/storage.h`:

```c
/* -------------------------------------------------------------------------
 * BLE paired device registry
 * ---------------------------------------------------------------------- */

#define STORAGE_BLE_MAX  8    /* maximum paired BLE devices stored */

typedef struct {
    uint8_t  addr[6];         /* BLE device address (little-endian) */
    uint8_t  addr_type;       /* 0 = public, 1 = random */
    char     name[64];        /* advertised device name, or "Unknown" */
    uint32_t last_seen;       /* unix timestamp or 0 if unavailable */
} ble_device_info_t;

/**
 * Add or update a paired BLE device record (matched by addr).
 * Writes entire list back to <mount>/ble_devices.ini.
 */
esp_err_t storage_ble_save(const ble_device_info_t *dev);

/**
 * Load all paired BLE device records.
 *
 * @param out    Caller-allocated array of at least @p max entries.
 * @param max    Maximum entries to write into @p out.
 * @param count  Set to number of entries written.
 * @return ESP_OK (missing file → count=0, not an error).
 */
esp_err_t storage_ble_list(ble_device_info_t *out, int max, int *count);

/**
 * Remove a paired device by address.  No-op if address not found.
 */
esp_err_t storage_ble_remove(const uint8_t addr[6]);

/**
 * Delete ble_devices.ini entirely (factory reset BLE pairing list).
 */
esp_err_t storage_ble_clear(void);
```

- [ ] **Step 2: Implement `storage_ble_*` in `storage.c`**

Append to the end of `components/storage/storage.c`:

```c
/* -------------------------------------------------------------------------
 * BLE device registry — INI persistence
 * ---------------------------------------------------------------------- */

static void ble_devices_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/ble_devices.ini",
             storage_platform_mount_point());
}

/* Format MAC address as "AA:BB:CC:DD:EE:FF" */
static void fmt_addr(const uint8_t addr[6], char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

/* Parse "AA:BB:CC:DD:EE:FF" into addr[6]. Returns 1 on success. */
static int parse_addr(const char *s, uint8_t addr[6])
{
    unsigned a, b, c, d, e, f;
    if (sscanf(s, "%02X:%02X:%02X:%02X:%02X:%02X",
               &a, &b, &c, &d, &e, &f) != 6) return 0;
    addr[0]=(uint8_t)a; addr[1]=(uint8_t)b; addr[2]=(uint8_t)c;
    addr[3]=(uint8_t)d; addr[4]=(uint8_t)e; addr[5]=(uint8_t)f;
    return 1;
}

esp_err_t storage_ble_list(ble_device_info_t *out, int max, int *count)
{
    if (!out || !count || max <= 0) return ESP_ERR_INVALID_ARG;
    *count = 0;

    char path[128];
    ble_devices_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return ESP_OK;   /* absent = no paired devices */

    char line[128];
    int  cur = -1;

    while (fgets(line, sizeof(line), f)) {
        rtrim(line);
        if (line[0] == '\0' || line[0] == ';') continue;

        char section[24];
        if (parse_section(line, section, sizeof(section))) {
            if (*count >= max) { cur = -1; continue; }
            cur = *count;
            memset(&out[cur], 0, sizeof(ble_device_info_t));
            if (!parse_addr(section, out[cur].addr)) { cur = -1; continue; }
            snprintf(out[cur].name, sizeof(out[cur].name), "Unknown");
            (*count)++;
            continue;
        }
        if (cur < 0) continue;

        char key[64], val[64];
        if (!parse_kv(line, key, val)) continue;

        if      (strcmp(key, "addr_type") == 0)
            out[cur].addr_type = (uint8_t)atoi(val);
        else if (strcmp(key, "name") == 0)
            snprintf(out[cur].name, sizeof(out[cur].name), "%s", val);
        else if (strcmp(key, "last_seen") == 0)
            out[cur].last_seen = (uint32_t)strtoul(val, NULL, 10);
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d BLE device(s)", *count);
    return ESP_OK;
}

esp_err_t storage_ble_save(const ble_device_info_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;

    ble_device_info_t list[STORAGE_BLE_MAX];
    int count = 0;
    storage_ble_list(list, STORAGE_BLE_MAX, &count);

    /* Update existing entry or append */
    int idx = -1;
    for (int i = 0; i < count; i++) {
        if (memcmp(list[i].addr, dev->addr, 6) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (count >= STORAGE_BLE_MAX) {
            ESP_LOGW(TAG, "BLE device list full, dropping oldest");
            memmove(&list[0], &list[1],
                    sizeof(ble_device_info_t) * (STORAGE_BLE_MAX - 1));
            count = STORAGE_BLE_MAX - 1;
        }
        idx = count++;
    }
    list[idx] = *dev;

    char path[128];
    ble_devices_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write '%s': errno=%d", path, errno);
        return ESP_FAIL;
    }

    char mac[18];
    for (int i = 0; i < count; i++) {
        fmt_addr(list[i].addr, mac);
        fprintf(f, "[%s]\n", mac);
        fprintf(f, "addr_type=%u\n", (unsigned)list[i].addr_type);
        fprintf(f, "name=%s\n", list[i].name);
        fprintf(f, "last_seen=%lu\n", (unsigned long)list[i].last_seen);
        fprintf(f, "\n");
    }
    fclose(f);
    ESP_LOGI(TAG, "Saved BLE device %s ('%s')", mac, dev->name);
    return ESP_OK;
}

esp_err_t storage_ble_remove(const uint8_t addr[6])
{
    ble_device_info_t list[STORAGE_BLE_MAX];
    int count = 0;
    storage_ble_list(list, STORAGE_BLE_MAX, &count);

    int found = -1;
    for (int i = 0; i < count; i++) {
        if (memcmp(list[i].addr, addr, 6) == 0) { found = i; break; }
    }
    if (found < 0) return ESP_OK;   /* not present, not an error */

    memmove(&list[found], &list[found + 1],
            sizeof(ble_device_info_t) * (count - found - 1));
    count--;

    char path[128];
    ble_devices_path(path, sizeof(path));
    if (count == 0) { remove(path); return ESP_OK; }

    FILE *f = fopen(path, "w");
    if (!f) return ESP_FAIL;
    char mac[18];
    for (int i = 0; i < count; i++) {
        fmt_addr(list[i].addr, mac);
        fprintf(f, "[%s]\n", mac);
        fprintf(f, "addr_type=%u\n", (unsigned)list[i].addr_type);
        fprintf(f, "name=%s\n", list[i].name);
        fprintf(f, "last_seen=%lu\n", (unsigned long)list[i].last_seen);
        fprintf(f, "\n");
    }
    fclose(f);
    return ESP_OK;
}

esp_err_t storage_ble_clear(void)
{
    char path[128];
    ble_devices_path(path, sizeof(path));
    remove(path);
    ESP_LOGI(TAG, "BLE device list cleared");
    return ESP_OK;
}
```

- [ ] **Step 3: Build to verify**

```bash
idf.py build 2>&1 | tail -20
```
Expected: `Build successful`.

- [ ] **Step 4: Quick simulator smoke test (optional but fast)**

Build and run the simulator; verify storage doesn't crash on init:
```bash
cmake -B build-sim sim/ -DBUILD_SIMULATOR=1
cmake --build build-sim
# Run briefly; check no crash on storage_init()
```

- [ ] **Step 5: Commit**

```bash
git add components/storage/include/storage.h components/storage/storage.c
git commit -m "feat(storage): add BLE paired device registry (INI persistence)"
```

---

## Task 4: BLE State Machine Refactor

**Files:**
- Rewrite: `components/input/include/ble_keyboard.h`
- Rewrite: `components/input/ble_keyboard.c`

Replace the existing stub public API (`ble_keyboard_init/scan/pair/is_connected/get_report`) with the new state machine API. The internal `ble_keyboard_backend_init()` function (called by `input_hal.c`) remains unchanged.

- [ ] **Step 1: Rewrite `ble_keyboard.h`**

Full replacement of `components/input/include/ble_keyboard.h`:

```c
/*
 * BLE HID keyboard — public API
 *
 * Exposes the 5-state connection lifecycle and pairing primitives.
 * The backend is driven by esp_hidh and Bluedroid GAP callbacks.
 * Call ble_keyboard_backend_init() (via input_hal_init()) to start.
 */

#pragma once

#include "esp_err.h"
#include "storage.h"   /* ble_device_info_t */
#include <stdint.h>
#include <stdbool.h>

/* Connection state machine */
typedef enum {
    BLE_IDLE,          /* stack up, not scanning */
    BLE_RECONNECT,     /* scanning for a known (bonded) device */
    BLE_PAIRING_SCAN,  /* scanning for any HID device — pairing mode */
    BLE_CONNECTING,    /* connection in progress */
    BLE_CONNECTED,     /* keyboard active, input flowing */
} ble_state_t;

/** Current connection state (thread-safe read). */
ble_state_t ble_keyboard_get_state(void);

/**
 * Switch to BLE_PAIRING_SCAN mode.
 * Can be called from any state. Disconnects an active connection first.
 * Clears the scan results buffer.
 */
void ble_keyboard_enter_pairing(void);

/**
 * Start reconnect scan for devices already in the storage registry.
 * Called automatically by backend_init if registry is non-empty.
 * No-op if already connected.
 */
void ble_keyboard_reconnect_start(void);

/**
 * Copy at most @p max discovered devices from the pairing scan buffer.
 * Only valid during BLE_PAIRING_SCAN state.
 *
 * @return Number of entries written into @p out.
 */
int ble_keyboard_get_scan_results(ble_device_info_t *out, int max);

/**
 * Connect and bond with the device at @p addr.
 * Transitions: BLE_PAIRING_SCAN → BLE_CONNECTING.
 * On successful bond, saves the device to the storage registry.
 */
void ble_keyboard_select_device(const uint8_t addr[6], uint8_t addr_type);

/**
 * Remove @p addr from the storage registry and (if connected) disconnect.
 */
void ble_keyboard_forget_device(const uint8_t addr[6]);
```

- [ ] **Step 2: Rewrite `ble_keyboard.c`**

Full replacement of `components/input/ble_keyboard.c` (inside the `#if defined(CONFIG_INPUT_BLE) || defined(CONFIG_INPUT_AUTO)` guard):

```c
/*
 * BLE HID keyboard backend — 5-state machine
 */

#if defined(CONFIG_INPUT_BLE) || defined(CONFIG_INPUT_AUTO)

#include "input_hal_internal.h"
#include "ble_keyboard.h"
#include "hid_keymap.h"
#include "storage.h"
#include "vterm.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"

#include <string.h>

static const char *TAG = "ble_kbd";

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static volatile ble_state_t s_state = BLE_IDLE;

/* Registry loaded at init — addresses we try to reconnect to */
static ble_device_info_t s_registry[STORAGE_BLE_MAX];
static int               s_registry_count = 0;

/* Scan results accumulated during BLE_PAIRING_SCAN */
static ble_device_info_t s_scan_results[STORAGE_BLE_MAX];
static int               s_scan_count = 0;

/* BDA of currently connected device (valid in BLE_CONNECTED) */
static uint8_t s_connected_bda[6];

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

ble_state_t ble_keyboard_get_state(void)      { return s_state; }

int ble_keyboard_get_scan_results(ble_device_info_t *out, int max)
{
    int n = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan_results, n * sizeof(ble_device_info_t));
    return n;
}

void ble_keyboard_enter_pairing(void)
{
    ESP_LOGI(TAG, "Entering pairing mode");
    esp_ble_gap_stop_scanning();
    s_scan_count = 0;
    s_state = BLE_PAIRING_SCAN;
    esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
}

void ble_keyboard_reconnect_start(void)
{
    if (s_state == BLE_CONNECTED) return;
    if (s_registry_count == 0)   return;
    ESP_LOGI(TAG, "Reconnect scan for %d known device(s)", s_registry_count);
    s_state = BLE_RECONNECT;
    esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
}

void ble_keyboard_select_device(const uint8_t addr[6], uint8_t addr_type)
{
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    ESP_LOGI(TAG, "Connecting to selected device %s", mac);
    esp_ble_gap_stop_scanning();
    s_state = BLE_CONNECTING;
    esp_hidh_dev_open((uint8_t *)addr, ESP_HID_TRANSPORT_BLE, addr_type);
}

void ble_keyboard_forget_device(const uint8_t addr[6])
{
    storage_ble_remove(addr);
    /* Reload registry */
    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
    if (s_state == BLE_CONNECTED &&
        memcmp(s_connected_bda, addr, 6) == 0) {
        /* TODO: disconnect active connection */
        ESP_LOGW(TAG, "forget_device: disconnect not yet implemented");
    }
}

/* ------------------------------------------------------------------ */
/*  GAP / scan helpers                                                 */
/* ------------------------------------------------------------------ */

static bool ad_has_hid_uuid(uint8_t *adv_data, uint8_t adv_data_len)
{
    uint8_t *p   = adv_data;
    uint8_t *end = adv_data + adv_data_len;
    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0x02 || type == 0x03) {
            uint8_t *uuids = p + 2;
            uint8_t  count = (len - 1) / 2;
            for (uint8_t i = 0; i < count; i++) {
                uint16_t uuid = (uint16_t)(uuids[2*i]) |
                                ((uint16_t)(uuids[2*i+1]) << 8);
                if (uuid == 0x1812) return true;
            }
        }
        p += 1 + len;
    }
    return false;
}

/* Extract advertised name from AD structures into dst[dstsz] */
static void ad_get_name(uint8_t *adv, uint8_t adv_len,
                        char *dst, size_t dstsz)
{
    uint8_t *p   = adv;
    uint8_t *end = adv + adv_len;
    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0x08 || type == 0x09) {   /* Short / Complete Local Name */
            size_t nlen = (size_t)(len - 1);
            if (nlen >= dstsz) nlen = dstsz - 1;
            memcpy(dst, p + 2, nlen);
            dst[nlen] = '\0';
            return;
        }
        p += 1 + len;
    }
    snprintf(dst, dstsz, "Unknown");
}

static bool addr_in_registry(const uint8_t addr[6])
{
    for (int i = 0; i < s_registry_count; i++) {
        if (memcmp(s_registry[i].addr, addr, 6) == 0) return true;
    }
    return false;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        /* Initial scan params set — start reconnect or wait for explicit call */
        if (s_registry_count > 0) {
            ble_keyboard_reconnect_start();
        } else {
            ESP_LOGI(TAG, "No paired devices — awaiting pairing mode trigger");
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        struct ble_scan_result_evt_param *r = &param->scan_rst;
        if (r->search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
        if (!ad_has_hid_uuid(r->ble_adv, r->adv_data_len)) break;

        if (s_state == BLE_RECONNECT) {
            if (addr_in_registry(r->bda)) {
                ESP_LOGI(TAG, "Known device found, connecting");
                esp_ble_gap_stop_scanning();
                s_state = BLE_CONNECTING;
                esp_hidh_dev_open(r->bda, ESP_HID_TRANSPORT_BLE,
                                  r->ble_addr_type);
            }
        } else if (s_state == BLE_PAIRING_SCAN) {
            /* Add to scan results if not already present */
            bool dup = false;
            for (int i = 0; i < s_scan_count; i++) {
                if (memcmp(s_scan_results[i].addr, r->bda, 6) == 0) {
                    dup = true; break;
                }
            }
            if (!dup && s_scan_count < STORAGE_BLE_MAX) {
                ble_device_info_t *d = &s_scan_results[s_scan_count++];
                memcpy(d->addr, r->bda, 6);
                d->addr_type = r->ble_addr_type;
                ad_get_name(r->ble_adv, r->adv_data_len,
                            d->name, sizeof(d->name));
                d->last_seen = 0;
                ESP_LOGI(TAG, "Discovered: %s ('%s')",
                         /* mac via format */
                         d->name, d->name);
            }
        }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan stopped (state=%d)", (int)s_state);
        if (s_state == BLE_RECONNECT) {
            /* Retry after a brief pause — handled by restarting in CLOSE_EVT */
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  HID host callback                                                  */
/* ------------------------------------------------------------------ */

static void hidh_callback(void *handler_args, esp_event_base_t base,
                          int32_t id, void *event_data)
{
    esp_hidh_event_t       event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *data  = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(data->open.dev);
        ESP_LOGI(TAG, "Connected: %02X:%02X:%02X:%02X:%02X:%02X",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        memcpy(s_connected_bda, bda, 6);
        s_state = BLE_CONNECTED;
        esp_ble_set_encryption((uint8_t *)bda, ESP_BLE_SEC_ENCRYPT_MITM);

        /* Save/update device in registry */
        ble_device_info_t dev = {0};
        memcpy(dev.addr, bda, 6);
        /* Find name from scan results if available */
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_results[i].addr, bda, 6) == 0) {
                dev.addr_type = s_scan_results[i].addr_type;
                memcpy(dev.name, s_scan_results[i].name, sizeof(dev.name));
                break;
            }
        }
        if (dev.name[0] == '\0') snprintf(dev.name, sizeof(dev.name), "Unknown HID");
        storage_ble_save(&dev);
        storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
        break;
    }

    case ESP_HIDH_CLOSE_EVT:
        ESP_LOGI(TAG, "Keyboard disconnected, restarting reconnect scan");
        s_state = BLE_RECONNECT;
        esp_ble_gap_start_scanning(CONFIG_INPUT_BLE_SCAN_DURATION);
        break;

    case ESP_HIDH_INPUT_EVT: {
        if (data->input.usage != ESP_HID_USAGE_KEYBOARD) break;
        if (data->input.length < 3) break;

        const uint8_t *report    = data->input.data;
        uint8_t        modifiers = report[0];
        uint8_t n = (data->input.length < 8) ? (uint8_t)data->input.length : 8u;

        for (uint8_t i = 2; i < n; i++) {
            uint8_t kc = report[i];
            if (kc == 0x00 || kc == 0x01) continue;

            uint8_t buf[INPUT_EVENT_MAX_LEN];
            uint8_t len = hid_keymap_translate(kc, modifiers,
                                               vterm_app_cursor_keys(), buf);
            if (len == 0) continue;

            input_event_t ev = { .type = INPUT_EVENT_KEY, .len = len };
            for (uint8_t j = 0; j < len; j++) ev.buf[j] = buf[j];
            input_hal_post_event(&ev);
        }
        break;
    }

    case ESP_HIDH_BATTERY_EVT:
        ESP_LOGI(TAG, "Battery: %d%%", data->battery.level);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Backend init                                                       */
/* ------------------------------------------------------------------ */

esp_err_t ble_keyboard_backend_init(void)
{
    esp_err_t ret;

    /* Load existing paired device registry */
    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
    ESP_LOGI(TAG, "Registry: %d known device(s)", s_registry_count);

    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "mem_release: %s", esp_err_to_name(ret));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "BT controller already initialised");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt_controller_enable: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid_init: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bluedroid_enable: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t   io_cap   = ESP_IO_CAP_NONE;
    uint8_t            key_size = 16;
    uint8_t            init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t            rsp_key  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                   &auth_req, sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                   &io_cap,   sizeof(io_cap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                   &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                   &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                   &rsp_key,  sizeof(rsp_key));

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gap_register_callback: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_hidh_config_t hidh_cfg = {
        .callback         = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg     = NULL,
    };
    ret = esp_hidh_init(&hidh_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hidh_init: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Scan params → triggers SCAN_PARAM_SET_COMPLETE → start_scan or wait */
    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set_scan_params: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BLE keyboard backend initialised");
    return ESP_OK;
}

#else

#include "input_hal_internal.h"
#include "ble_keyboard.h"
esp_err_t  ble_keyboard_backend_init(void) { return ESP_OK; }
ble_state_t ble_keyboard_get_state(void)   { return BLE_IDLE; }
void ble_keyboard_enter_pairing(void)      {}
void ble_keyboard_reconnect_start(void)    {}
int  ble_keyboard_get_scan_results(ble_device_info_t *o, int m) { (void)o;(void)m; return 0; }
void ble_keyboard_select_device(const uint8_t a[6], uint8_t t)  { (void)a;(void)t; }
void ble_keyboard_forget_device(const uint8_t a[6])             { (void)a; }

#endif
```

- [ ] **Step 3: Build to verify**

```bash
idf.py build 2>&1 | tail -20
```
Expected: `Build successful`.

- [ ] **Step 4: Flash and verify BLE init in serial log**

```bash
idf.py flash monitor
```
Expected serial log output:
```
I (xxx) ble_kbd: Registry: 0 known device(s)   ← or N if NVS has bonds
I (xxx) ble_kbd: BLE keyboard backend initialised
I (xxx) ble_kbd: No paired devices — awaiting pairing mode trigger
```

- [ ] **Step 5: Commit**

```bash
git add components/input/include/ble_keyboard.h components/input/ble_keyboard.c
git commit -m "feat(ble): 5-state BLE keyboard machine with storage registry integration"
```

---

## Task 5: Touch Input — GT911 Tap and Long-Press Detection

**Files:**
- Modify: `components/input/Kconfig.projbuild`
- Rewrite: `components/input/touch_input.c`
- Modify: `components/input/input_hal_internal.h`
- Modify: `components/input/input_hal.c`

**GT911 hardware notes for Waveshare ESP32-S3-Touch-LCD-7:**
- I2C address: 0x5D (verify against your board schematic)
- SDA: GPIO8, SCL: GPIO9, INT: GPIO4 (verify against schematic)
- Status register: 0x814E (read 1 byte; lower nibble = touch count)
- Point data starts at: 0x8150 (8 bytes per point: track, x_lo, x_hi, y_lo, y_hi, size_lo, size_hi, reserved)
- Clear interrupt: write 0x00 to 0x814E after reading
- Polling period: 50ms is sufficient for UI interaction

- [ ] **Step 1: Add GT911 Kconfig options**

Append to `components/input/Kconfig.projbuild` (inside the existing `menu "Input HAL"`):

```kconfig
    config INPUT_TOUCH_I2C_SDA
        int "GT911 I2C SDA GPIO"
        default 8
        help
            GPIO number for GT911 touch controller I2C SDA.
            Verify against your board schematic.

    config INPUT_TOUCH_I2C_SCL
        int "GT911 I2C SCL GPIO"
        default 9
        help
            GPIO number for GT911 touch controller I2C SCL.

    config INPUT_TOUCH_I2C_ADDR
        hex "GT911 I2C address"
        default 0x5D
        help
            GT911 I2C address: 0x5D or 0x14 depending on INT pin
            state during power-on reset.

    config INPUT_TOUCH_LONG_PRESS_MS
        int "Long-press duration (milliseconds)"
        default 1000
        range 500 3000
        help
            How long a touch must be held to trigger INPUT_EVENT_LONG_PRESS.
```

- [ ] **Step 2: Add `touch_input_backend_init()` to `input_hal_internal.h`**

Append to `components/input/input_hal_internal.h`:
```c
esp_err_t touch_input_backend_init(void);
```

- [ ] **Step 3: Call `touch_input_backend_init()` from `input_hal.c`**

In `components/input/input_hal.c`, after the call to `input_uart_backend_init()`, add:

```c
    r = touch_input_backend_init();
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "Touch input init failed: %s", esp_err_to_name(r));
        /* non-fatal — touch is optional */
    }
```

Also add the include at the top if not already present (it's covered by `input_hal_internal.h`).

- [ ] **Step 4: Implement `touch_input.c`**

Full replacement of `components/input/touch_input.c`:

```c
/*
 * Touch input — GT911 capacitive touch controller
 *
 * Detects tap and long-press gestures; posts INPUT_EVENT_TAP and
 * INPUT_EVENT_LONG_PRESS to the input HAL queue.
 *
 * Only compiled on device (not in simulator build).
 */

#ifndef BUILD_SIMULATOR

#include "input_hal_internal.h"
#include "input_hal.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define GT911_ADDR       CONFIG_INPUT_TOUCH_I2C_ADDR
#define GT911_REG_STATUS 0x814E
#define GT911_REG_POINTS 0x8150

static i2c_master_bus_handle_t  s_bus  = NULL;
static i2c_master_dev_handle_t  s_dev  = NULL;

/* ------------------------------------------------------------------ */
/*  GT911 register access                                              */
/* ------------------------------------------------------------------ */

static esp_err_t gt911_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t reg_buf[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    esp_err_t r = i2c_master_transmit(s_dev, reg_buf, 2, 100);
    if (r != ESP_OK) return r;
    return i2c_master_receive(s_dev, buf, len, 100);
}

static esp_err_t gt911_write_byte(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_dev, buf, 3, 100);
}

/* ------------------------------------------------------------------ */
/*  Touch polling task                                                 */
/* ------------------------------------------------------------------ */

static void touch_task(void *arg)
{
    TickType_t   touch_start  = 0;
    bool         touching     = false;
    uint16_t     touch_x      = 0;
    uint16_t     touch_y      = 0;
    const uint32_t long_press_ms = CONFIG_INPUT_TOUCH_LONG_PRESS_MS;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));

        uint8_t status = 0;
        if (gt911_read(GT911_REG_STATUS, &status, 1) != ESP_OK) continue;

        uint8_t count = status & 0x0F;

        if (count > 0 && (status & 0x80)) {
            /* At least one touch point */
            uint8_t pt[8] = {0};
            gt911_read(GT911_REG_POINTS, pt, 8);
            gt911_write_byte(GT911_REG_STATUS, 0x00);   /* clear buffer-ready */

            uint16_t x = (uint16_t)pt[1] | ((uint16_t)pt[2] << 8);
            uint16_t y = (uint16_t)pt[3] | ((uint16_t)pt[4] << 8);

            if (!touching) {
                touching    = true;
                touch_start = xTaskGetTickCount();
                touch_x     = x;
                touch_y     = y;
            } else {
                /* Check long-press threshold */
                uint32_t held_ms = pdTICKS_TO_MS(xTaskGetTickCount() - touch_start);
                if (held_ms >= long_press_ms) {
                    input_event_t ev = {
                        .type = INPUT_EVENT_LONG_PRESS,
                        .x    = touch_x,
                        .y    = touch_y,
                    };
                    input_hal_post_event(&ev);
                    /* Wait for release before firing again */
                    touching = false;
                }
            }
        } else {
            if (count == 0 && (status & 0x80)) {
                gt911_write_byte(GT911_REG_STATUS, 0x00);
            }
            if (touching) {
                /* Released — was it a tap? */
                uint32_t held_ms = pdTICKS_TO_MS(xTaskGetTickCount() - touch_start);
                if (held_ms < long_press_ms) {
                    input_event_t ev = {
                        .type = INPUT_EVENT_TAP,
                        .x    = touch_x,
                        .y    = touch_y,
                    };
                    input_hal_post_event(&ev);
                }
                touching = false;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Backend init                                                       */
/* ------------------------------------------------------------------ */

esp_err_t touch_input_backend_init(void)
{
    esp_err_t ret;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = I2C_NUM_0,
        .sda_io_num          = CONFIG_INPUT_TOUCH_I2C_SDA,
        .scl_io_num          = CONFIG_INPUT_TOUCH_I2C_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = GT911_ADDR,
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Quick probe — verify GT911 responds */
    uint8_t status = 0;
    ret = gt911_read(GT911_REG_STATUS, &status, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GT911 not responding at 0x%02X (SDA=%d SCL=%d) — "
                 "check Kconfig pin assignments against board schematic",
                 GT911_ADDR,
                 CONFIG_INPUT_TOUCH_I2C_SDA,
                 CONFIG_INPUT_TOUCH_I2C_SCL);
        return ret;
    }

    xTaskCreatePinnedToCore(touch_task, "touch", 2048, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "GT911 touch input initialised (addr=0x%02X)", GT911_ADDR);
    return ESP_OK;
}

#else /* BUILD_SIMULATOR */

#include "input_hal_internal.h"
esp_err_t touch_input_backend_init(void) { return ESP_OK; }

#endif /* BUILD_SIMULATOR */
```

- [ ] **Step 5: Build to verify**

```bash
idf.py build 2>&1 | tail -20
```
Expected: `Build successful`.

- [ ] **Step 6: Flash and verify GT911 probe**

```bash
idf.py flash monitor
```
Expected serial log:
```
I (xxx) touch: GT911 touch input initialised (addr=0x5D)
```
If instead you see `GT911 not responding` — run `idf.py menuconfig` → Input HAL and verify the SDA/SCL/address values against your board schematic.

- [ ] **Step 7: Verify tap event in SESSION (temporary log)**

Add a temporary `ESP_LOGI` in `main.c` STATE_SESSION to confirm touch events arrive:
```c
if (ev.type == INPUT_EVENT_TAP)
    ESP_LOGI("test", "TAP at (%u, %u)", ev.x, ev.y);
```
Touch the screen; expected: `TAP at (X, Y)` in serial output.
Remove the temporary log after confirming.

- [ ] **Step 8: Commit**

```bash
git add components/input/touch_input.c \
        components/input/Kconfig.projbuild \
        components/input/input_hal_internal.h \
        components/input/input_hal.c
git commit -m "feat(touch): GT911 tap and long-press detection via I2C master"
```

---

## Task 6: Pairing Overlay

**Files:**
- Create: `components/input/pairing_overlay.h`
- Create: `components/input/pairing_overlay.c`
- Modify: `components/input/CMakeLists.txt`

The overlay renders via `vterm_write()` — same as `splash.c`. It is a blocking call that runs its own mini event loop polling BLE scan results and input HAL events.

- [ ] **Step 1: Create `pairing_overlay.h`**

Create `components/input/pairing_overlay.h`:
```c
/*
 * pairing_overlay — modal BLE pairing screen rendered via vterm ANSI output.
 *
 * Blocks until a device is paired or the user cancels (long-press).
 */

#pragma once
#include <stdbool.h>

/**
 * Show the pairing overlay.
 *
 * Calls ble_keyboard_enter_pairing() internally.
 * Blocks until:
 *   - A device is selected and bonded  → returns true
 *   - User long-presses to cancel      → returns false
 *
 * Callers must disconnect SSH before calling and reconnect after.
 */
bool pairing_overlay_run(void);
```

- [ ] **Step 2: Create `pairing_overlay.c`**

Create `components/input/pairing_overlay.c`:

```c
/*
 * Pairing overlay — modal BLE device selection screen.
 *
 * Renders a full-screen ANSI UI via vterm_write().
 * Polls ble_keyboard_get_scan_results() every 2s.
 * Accepts INPUT_EVENT_TAP for selection and INPUT_EVENT_LONG_PRESS to cancel.
 * Arrow keys + Enter also navigate if a keyboard is connected.
 */

#include "pairing_overlay.h"
#include "ble_keyboard.h"
#include "input_hal.h"
#include "storage.h"
#include "vterm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pairing_ui";

/* Terminal geometry — must match CONFIG_TERMINAL_WIDTH / HEIGHT */
#define COLS  100
#define ROWS  30

/* Overlay box dimensions */
#define BOX_W   60   /* inner width (excluding borders)  */
#define BOX_X   20   /* left column of border            */
#define BOX_Y    8   /* top row of border                */
#define BOX_H   12   /* total height including borders   */

/* Row heights within the box */
#define TITLE_ROW   (BOX_Y + 1)
#define STATUS_ROW  (BOX_Y + 2)
#define SEP1_ROW    (BOX_Y + 3)
#define LIST_START  (BOX_Y + 4)
#define LIST_ROWS    5            /* max visible list entries */
#define SEP2_ROW    (BOX_Y + 9)
#define HINT_ROW    (BOX_Y + 10)

/* ANSI helpers */
#define ANSI_RESET   "\x1b[0m"
#define ANSI_BOLD    "\x1b[1m"
#define ANSI_REV     "\x1b[7m"
#define ANSI_BLU_BG  "\x1b[44m"
#define ANSI_WHT_FG  "\x1b[97m"
#define ANSI_GRY_FG  "\x1b[90m"
#define ANSI_CYN_FG  "\x1b[36m"
#define ANSI_HIDE_CUR "\x1b[?25l"
#define ANSI_SHOW_CUR "\x1b[?25h"
#define ANSI_CLR_SCR  "\x1b[2J\x1b[H"

/* Move cursor: row/col are 1-based for ANSI */
static void move(int row, int col)
{
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row + 1, col + 1);
    vterm_write(buf, (size_t)n);
}

static void write_str(const char *s)
{
    vterm_write(s, strlen(s));
}

static void draw_hline(int row, int col, int width,
                       const char *left, const char *mid, const char *right)
{
    move(row, col);
    write_str(left);
    for (int i = 0; i < width; i++) write_str(mid);
    write_str(right);
}

static void draw_frame(void)
{
    write_str(ANSI_BLU_BG ANSI_WHT_FG);

    /* Top border */
    draw_hline(BOX_Y, BOX_X, BOX_W, "╔", "═", "╗");

    /* Side borders and blank interior */
    for (int r = BOX_Y + 1; r < BOX_Y + BOX_H - 1; r++) {
        move(r, BOX_X);      write_str("║");
        move(r, BOX_X + 1);
        for (int i = 0; i < BOX_W; i++) write_str(" ");
        move(r, BOX_X + BOX_W + 1); write_str("║");
    }

    /* Separators */
    draw_hline(SEP1_ROW, BOX_X, BOX_W, "╠", "═", "╣");
    draw_hline(SEP2_ROW, BOX_X, BOX_W, "╠", "═", "╣");

    /* Bottom border */
    draw_hline(BOX_Y + BOX_H - 1, BOX_X, BOX_W, "╚", "═", "╝");

    /* Title */
    move(TITLE_ROW, BOX_X + 2);
    write_str(ANSI_BOLD "      PAIR BLUETOOTH KEYBOARD       " ANSI_RESET ANSI_BLU_BG ANSI_WHT_FG);

    /* Hint */
    move(HINT_ROW, BOX_X + 2);
    write_str(ANSI_GRY_FG "  Tap to pair  •  Long-press: cancel  " ANSI_RESET ANSI_BLU_BG ANSI_WHT_FG);

    write_str(ANSI_RESET);
}

static void draw_status(const char *msg)
{
    move(STATUS_ROW, BOX_X + 2);
    write_str(ANSI_BLU_BG ANSI_CYN_FG);
    char line[BOX_W + 1];
    snprintf(line, sizeof(line), "  %-*s", BOX_W - 2, msg);
    write_str(line);
    write_str(ANSI_RESET);
}

static void draw_list(const ble_device_info_t *devs, int count, int selected)
{
    for (int i = 0; i < LIST_ROWS; i++) {
        move(LIST_START + i, BOX_X + 2);
        write_str(ANSI_BLU_BG ANSI_WHT_FG);

        if (i < count) {
            char line[BOX_W + 1];
            const char *sel = (i == selected) ? "► " : "  ";
            /* Truncate device address to 6 chars for display */
            const ble_device_info_t *d = &devs[i];
            char mac_short[9];
            snprintf(mac_short, sizeof(mac_short), "%02X:%02X:%02X",
                     d->addr[3], d->addr[4], d->addr[5]);
            snprintf(line, sizeof(line), "%s%-36.36s [..%s]",
                     sel, d->name, mac_short);
            /* Highlight selected row */
            if (i == selected) write_str(ANSI_REV);
            write_str(line);
            if (i == selected) write_str(ANSI_RESET ANSI_BLU_BG ANSI_WHT_FG);
        } else {
            char blank[BOX_W + 1];
            memset(blank, ' ', BOX_W - 2);
            blank[BOX_W - 2] = '\0';
            write_str(blank);
        }
        write_str(ANSI_RESET);
    }
    vterm_flush();
}

/* Map touch Y coordinate to list index. Returns -1 if outside list. */
static int touch_y_to_index(uint16_t y)
{
    /* Cell height = 16px (BOUNCE_BUFFER_HEIGHT). Row 0 = top. */
    int row = (int)(y / 16);
    int idx = row - LIST_START;
    if (idx < 0 || idx >= LIST_ROWS) return -1;
    return idx;
}

bool pairing_overlay_run(void)
{
    ESP_LOGI(TAG, "Pairing overlay started");

    write_str(ANSI_HIDE_CUR ANSI_CLR_SCR);
    ble_keyboard_enter_pairing();
    draw_frame();
    draw_status("Scanning for HID keyboards...");

    ble_device_info_t devs[STORAGE_BLE_MAX];
    int   dev_count  = 0;
    int   selected   = 0;
    bool  done       = false;
    bool  paired     = false;
    TickType_t last_refresh = 0;

    while (!done) {
        /* Refresh scan results every 2s */
        TickType_t now = xTaskGetTickCount();
        if (now - last_refresh > pdMS_TO_TICKS(2000)) {
            last_refresh = now;
            int new_count = ble_keyboard_get_scan_results(devs, STORAGE_BLE_MAX);
            if (new_count != dev_count) {
                dev_count = new_count;
                draw_list(devs, dev_count, selected);
            }
            if (dev_count == 0) {
                draw_status("Scanning for HID keyboards...");
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "%d device(s) found — tap to pair", dev_count);
                draw_status(msg);
            }
        }

        /* Poll input events (100ms timeout) */
        input_event_t ev;
        if (!input_hal_read(&ev, 100)) continue;

        if (ev.type == INPUT_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "Long-press: cancelling pairing");
            done = true;
            paired = false;

        } else if (ev.type == INPUT_EVENT_TAP && dev_count > 0) {
            int idx = touch_y_to_index(ev.y);
            if (idx >= 0 && idx < dev_count) {
                selected = idx;
                draw_list(devs, dev_count, selected);
                draw_status("Connecting...");
                ble_keyboard_select_device(devs[idx].addr, devs[idx].addr_type);
                /* Wait briefly for connection */
                for (int w = 0; w < 50; w++) {   /* up to 5s */
                    vTaskDelay(pdMS_TO_TICKS(100));
                    if (ble_keyboard_get_state() == BLE_CONNECTED) {
                        paired = true;
                        break;
                    }
                }
                done = true;
            }

        } else if (ev.type == INPUT_EVENT_KEY && ev.len == 1) {
            /* Keyboard navigation if already connected */
            if (ev.buf[0] == '\r' && dev_count > 0) {
                /* Enter — select highlighted */
                draw_status("Connecting...");
                ble_keyboard_select_device(devs[selected].addr,
                                           devs[selected].addr_type);
                for (int w = 0; w < 50; w++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                    if (ble_keyboard_get_state() == BLE_CONNECTED) {
                        paired = true;
                        break;
                    }
                }
                done = true;
            } else if (ev.len == 3 && ev.buf[0] == 0x1B && ev.buf[1] == '[') {
                if (ev.buf[2] == 'A' && selected > 0)              /* Up */
                    selected--;
                else if (ev.buf[2] == 'B' && selected < dev_count-1) /* Down */
                    selected++;
                draw_list(devs, dev_count, selected);
            }
        }
    }

    if (paired) {
        draw_status("Paired! Returning to session...");
    } else {
        draw_status("Pairing cancelled.");
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Restore terminal */
    write_str(ANSI_SHOW_CUR ANSI_CLR_SCR ANSI_RESET);
    vterm_flush();

    ESP_LOGI(TAG, "Pairing overlay done: %s", paired ? "paired" : "cancelled");
    return paired;
}
```

- [ ] **Step 3: Add `pairing_overlay.c` to CMakeLists.txt**

In `components/input/CMakeLists.txt`, add `"pairing_overlay.c"` to the SRCS list:
```cmake
    SRCS
        "input_hal.c"
        "hid_keymap.c"
        "ble_keyboard.c"
        "input_uart.c"
        "touch_input.c"
        "pairing_overlay.c"
```

- [ ] **Step 4: Build to verify**

```bash
idf.py build 2>&1 | tail -20
```
Expected: `Build successful`.

- [ ] **Step 5: Commit**

```bash
git add components/input/pairing_overlay.h \
        components/input/pairing_overlay.c \
        components/input/CMakeLists.txt
git commit -m "feat(input): pairing overlay modal UI via vterm ANSI rendering"
```

---

## Task 7: Main App Integration

**Files:**
- Modify: `main/main.c`

Add `STATE_BLE_INIT` and `STATE_PAIRING` states; handle `INPUT_EVENT_LONG_PRESS` in `STATE_SESSION`; update the SESSION forward loop to skip non-KEY events.

- [ ] **Step 1: Update includes in `main.c`**

Add to the includes block at the top of `main/main.c`:
```c
#include "ble_keyboard.h"
#include "pairing_overlay.h"
```

- [ ] **Step 2: Add new states to the enum**

Replace the `app_state_t` enum:
```c
typedef enum {
    STATE_BOOT,
    STATE_BLE_INIT,      /* NEW: check registry, decide pairing vs reconnect */
    STATE_PAIRING,       /* NEW: run pairing overlay */
    STATE_WIFI_WAIT,
    STATE_SSH_CONNECT,
    STATE_SESSION,
    STATE_ERROR,
} app_state_t;
```

- [ ] **Step 3: Add BLE_INIT and PAIRING cases; update BOOT, SESSION**

In `main_task()`, replace:
```c
        case STATE_BOOT:
            //splash_show();
            vTaskDelay(pdMS_TO_TICKS(2000));
            vterm_bench_report();
            state = STATE_WIFI_WAIT;
            break;
```
with:
```c
        case STATE_BOOT:
            //splash_show();
            vTaskDelay(pdMS_TO_TICKS(2000));
            vterm_bench_report();
            state = STATE_BLE_INIT;
            break;

        case STATE_BLE_INIT: {
            ble_device_info_t reg[STORAGE_BLE_MAX];
            int reg_count = 0;
            storage_ble_list(reg, STORAGE_BLE_MAX, &reg_count);
            if (reg_count == 0) {
                ESP_LOGI(TAG, "No paired BLE devices — entering pairing mode");
                state = STATE_PAIRING;
            } else {
                ESP_LOGI(TAG, "%d paired device(s) — starting reconnect scan",
                         reg_count);
                ble_keyboard_reconnect_start();
                state = STATE_WIFI_WAIT;
            }
            break;
        }

        case STATE_PAIRING:
            /* pairing_overlay_run() blocks until paired or cancelled */
            pairing_overlay_run();
            /* Whether paired or cancelled, proceed to WiFi/SSH */
            wifi_started = false;
            state = STATE_WIFI_WAIT;
            break;
```

Also replace the `STATE_SESSION` case entirely:
```c
        case STATE_SESSION: {
            input_event_t ev;
            while (ssh_client_is_connected()) {
                if (!input_hal_read(&ev, 100)) continue;

                if (ev.type == INPUT_EVENT_KEY && ev.len > 0) {
                    ssh_client_send(ev.buf, ev.len);
                } else if (ev.type == INPUT_EVENT_LONG_PRESS) {
                    ESP_LOGI(TAG, "Long-press: entering pairing mode");
                    ssh_client_disconnect();
                    state = STATE_PAIRING;
                    goto session_exit;
                }
                /* INPUT_EVENT_TAP: ignore during normal session */
            }
            vterm_bench_report();
session_exit:
            if (state == STATE_PAIRING) break;
#if CONFIG_SSH_AUTO_RECONNECT
            ESP_LOGI(TAG, "SSH session ended, reconnecting...");
            state = STATE_SSH_CONNECT;
#else
            splash_status_fail("SSH session ended");
            state = STATE_ERROR;
#endif
            break;
        }
```

- [ ] **Step 4: Add `STORAGE_BLE_MAX` usage — ensure the macro is available**

`STORAGE_BLE_MAX` is defined in `storage.h` which is already included. Verify `#include "storage.h"` is present (it is in the original `main.c`). No change needed.

- [ ] **Step 5: Build to verify**

```bash
idf.py build 2>&1 | tail -20
```
Expected: `Build successful`.

- [ ] **Step 6: End-to-end hardware test — Stage 3 (pairing at boot)**

With a clean NVS (`idf.py erase-flash` first), flash and observe:
```bash
idf.py erase-flash flash monitor
```
Expected flow:
```
I (xxx) cyberdeck: STATE_BLE_INIT
I (xxx) cyberdeck: No paired BLE devices — entering pairing mode
I (xxx) pairing_ui: Pairing overlay started
```
→ Pairing screen appears on display
→ Turn on BLE keyboard
→ Device appears in list
→ Tap its name
```
I (xxx) ble_kbd: Connected: XX:XX:XX:XX:XX:XX
I (xxx) storage: Saved BLE device XX:XX:XX:XX:XX:XX
I (xxx) pairing_ui: Pairing overlay done: paired
```
→ Session proceeds to WiFi → SSH

- [ ] **Step 7: End-to-end hardware test — Stage 5 (registry persistence)**

Power cycle the device. Expected:
```
I (xxx) ble_kbd: Registry: 1 known device(s)
I (xxx) cyberdeck: 1 paired device(s) — starting reconnect scan
I (xxx) ble_kbd: Known device found, connecting
I (xxx) ble_kbd: Connected: XX:XX:XX:XX:XX:XX
```
→ Pairing screen never appears; session starts directly.

- [ ] **Step 8: End-to-end hardware test — Stage 4 (mid-session pairing)**

During active SSH session, long-press the screen for ≥1s. Expected:
```
I (xxx) cyberdeck: Long-press: entering pairing mode
I (xxx) pairing_ui: Pairing overlay started
```
→ Pairing screen appears; after pairing → SSH reconnects.

- [ ] **Step 9: Commit**

```bash
git add main/main.c
git commit -m "feat(main): BLE_INIT + PAIRING states; long-press mid-session pairing trigger"
```

---

## Self-Review Checklist

**Spec coverage:**
- ✅ BLE state machine (5 states, public API) — Task 4
- ✅ Device registry (storage_ble_*) — Task 3
- ✅ Pairing overlay (modal, vterm, tap selection) — Task 6
- ✅ Touch input (GT911 tap + long-press) — Task 5
- ✅ Boot-time pairing prompt — Task 7 STATE_BLE_INIT → STATE_PAIRING
- ✅ Mid-session pairing (long-press) — Task 7 STATE_SESSION
- ✅ Auto-reconnect on boot — Task 4 ble_keyboard_reconnect_start() + Task 7 STATE_BLE_INIT
- ✅ App cursor mode fix (known gap from spec) — Task 1
- ✅ input_event_t type field — Task 2

**Type consistency:**
- `ble_device_info_t` defined in `storage.h`, used consistently in `ble_keyboard.h`, `ble_keyboard.c`, `storage.c`, `pairing_overlay.c`, `main.c`
- `STORAGE_BLE_MAX` defined in `storage.h`, used in `ble_keyboard.c`, `pairing_overlay.c`, `main.c`
- `INPUT_EVENT_KEY / TAP / LONG_PRESS` defined in `input_hal.h`, used in `ble_keyboard.c`, `touch_input.c`, `pairing_overlay.c`, `main.c`
- `ble_keyboard_enter_pairing()` called in `pairing_overlay.c` — matches declaration in `ble_keyboard.h` ✅
- `vterm_flush()` called in `pairing_overlay.c` — present in `vterm.h` (verify during build)

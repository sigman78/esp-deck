# BLE Keyboard Connectivity Design

**Date:** 2026-03-28
**Status:** Approved
**Scope:** Phase 1 — BLE pairing/connection flow (input fidelity is Phase 2)

---

## Context

The `components/input/` subsystem has a full BLE HID host implementation (`ble_keyboard.c`), HID keycode translator (`hid_keymap.c`), and FreeRTOS queue dispatcher (`input_hal.c`) — but it has never been tested on real hardware. The main app wires `input_hal_read() → ssh_client_send()` in `STATE_SESSION`, so the end-to-end path exists on paper.

This design commissions that pipeline: formalises the BLE state machine, adds persistent device registry, implements a pairing overlay UI, and integrates everything into the main app boot/session flow.

---

## Architecture

### Approach: Pragmatic enhancement of existing component

- Extend `ble_keyboard.c` with an explicit state machine (same component, no new component)
- Add device registry to the existing `storage` component (alongside SSH profiles)
- Add `pairing_overlay.c` inside `components/input/`
- Add one new state (`STATE_PAIRING`) and one new state (`STATE_BLE_INIT`) to `main.c`
- Implement minimal GT911 touch detection in `touch_input.c` (long-press only)

---

## Components

### 1. BLE State Machine (`components/input/ble_keyboard.c` + `ble_keyboard.h`)

**Five states:**

```
BLE_IDLE
  ├─ registry empty  ──────────────────────► BLE_PAIRING_SCAN
  ├─ registry non-empty ──► BLE_RECONNECT     │ user selects device
  │                            │              ▼
  │                   found ──► BLE_CONNECTING ◄─ (both paths)
  │                             │
  │                             ▼
  │                        BLE_CONNECTED
  │                             │ disconnect
  └─────────────────────────────► BLE_RECONNECT (auto-retry)

  Mid-session: ble_keyboard_enter_pairing() ──► BLE_PAIRING_SCAN (from any state)
```

- `BLE_RECONNECT`: scans only for addresses in the device registry; connects silently
- `BLE_PAIRING_SCAN`: scans for any HID UUID 0x1812; populates scan results buffer

**New public API additions to `ble_keyboard.h`:**

```c
typedef enum {
    BLE_IDLE,
    BLE_PAIRING_SCAN,
    BLE_RECONNECT,
    BLE_CONNECTING,
    BLE_CONNECTED,
} ble_state_t;

ble_state_t  ble_keyboard_get_state(void);
void         ble_keyboard_enter_pairing(void);
int          ble_keyboard_get_scan_results(ble_device_info_t *out, int max);
void         ble_keyboard_select_device(const uint8_t addr[6], uint8_t addr_type);
void         ble_keyboard_forget_device(const uint8_t addr[6]);
```

---

### 2. Device Registry (`components/storage/`)

Two-layer persistence:
- **Bluedroid NVS bonding** (automatic): security keys managed by ESP-IDF, no code needed
- **Storage component device list**: human-readable records in LittleFS at `/storage/ble_devices.json`

**New type in `storage.h`:**

```c
typedef struct {
    uint8_t  addr[6];
    uint8_t  addr_type;       // BLE_ADDR_TYPE_PUBLIC or RANDOM
    char     name[64];        // advertised name, or "Unknown"
    uint32_t last_seen;       // unix timestamp or boot count
} ble_device_info_t;
```

**New functions in `storage.c`:**

```c
esp_err_t storage_ble_save(const ble_device_info_t *dev);       // add or update by addr
esp_err_t storage_ble_list(ble_device_info_t *out, int max, int *count);
esp_err_t storage_ble_remove(const uint8_t addr[6]);
esp_err_t storage_ble_clear(void);
```

The BLE backend calls `storage_ble_save()` on successful bond and `storage_ble_list()` on init to seed the reconnect address filter.

---

### 3. Pairing Overlay (`components/input/pairing_overlay.c`)

Modal full-screen takeover rendered via `vterm_write()` (same mechanism as `splash.c`).

**Visual layout (100×30 cells):**
```
╔══════════════════════════════════════╗
║      PAIR BLUETOOTH KEYBOARD         ║
║  Scanning for HID devices...         ║
╠══════════════════════════════════════╣
║  ► Keychron K3 Pro     [AA:BB:CC...] ║
║    HHKB Studio         [DD:EE:FF...] ║
║    Unknown HID         [11:22:33...] ║
╠══════════════════════════════════════╣
║  Tap to pair  •  Long-press: cancel  ║
╚══════════════════════════════════════╝
```

**Public API:**

```c
// Blocks until paired or cancelled. Returns true if a device was paired.
bool pairing_overlay_run(void);
```

Internally polls `ble_keyboard_get_scan_results()` every 2 seconds, re-renders updated list. Devices not seen for >10s are dropped from the list.

**Input handling during overlay:**
- Touch tap on a list row → `ble_keyboard_select_device()`
- Touch long-press (≥1s) → cancel, return false
- If a keyboard is already connected: arrow keys + Enter also navigate the list via `input_hal_read()`

---

### 4. Touch Input — Minimal Implementation (`components/input/touch_input.c`)

Currently a stub. Needs only enough GT911 I2C implementation to:
- Detect touch down + duration
- Post `INPUT_EVENT_LONG_PRESS` to input queue when press ≥1s
- Post `INPUT_EVENT_TAP` with (x, y) coordinates

Full gesture recognition (scroll, multi-touch) remains a future task.

**New event types added to `input_hal.h`:**

```c
#define INPUT_EVENT_KEY       0   // existing: keyboard byte sequence
#define INPUT_EVENT_TAP       1   // touch tap: ev.x, ev.y populated
#define INPUT_EVENT_LONG_PRESS 2  // long press: ev.x, ev.y populated

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t buf[INPUT_EVENT_MAX_LEN];
    uint16_t x, y;   // for touch events
} input_event_t;
```

---

### 5. Main App Integration (`main/main.c`)

Two new states inserted before WiFi:

```
STATE_BOOT
  └─► STATE_BLE_INIT

STATE_BLE_INIT
  ├─ storage_ble_list() → empty?
  │     yes ──► STATE_PAIRING
  │     no  ──► ble_keyboard_reconnect_start()
  │             └─► STATE_WIFI_WAIT  (BLE reconnects in background)

STATE_PAIRING
  ├─ pairing_overlay_run()
  ├─ on true  (paired) ──► STATE_WIFI_WAIT
  └─ on false (cancel) ──► STATE_WIFI_WAIT  (keyboard optional; UART still works)

STATE_WIFI_WAIT → STATE_SSH_CONNECT → STATE_SESSION  (unchanged)

STATE_SESSION
  ├─ INPUT_EVENT_LONG_PRESS received
  │     └─► ssh_client_disconnect()
  │         STATE_PAIRING
  │         after return ──► STATE_SSH_CONNECT (auto-reconnects)
  └─ normal key input loop  (unchanged)
```

---

## Data Flow

```
BLE Keyboard (HW)
  │  HID reports (6-key rollover + modifier byte)
  ▼
esp_hidh callback in ble_keyboard.c
  │  hid_keymap_translate() → byte sequence
  ▼
input_hal_post_event()  →  FreeRTOS queue (16 entries)
  │
  ▼
main.c STATE_SESSION: input_hal_read()
  │
  ▼
ssh_client_send()  →  libssh2_channel_write()  →  SSH server
```

---

## Known Gap to Fix (Phase 1 includes this)

`hid_keymap.c` always emits `ESC [ A/B/C/D` for arrow keys. The simulator's `translate_key()` correctly checks `vterm_app_cursor_keys()` to switch between `ESC O` (application mode) and `ESC [ ` (normal mode). Fix `hid_keymap.c` to accept a `bool app_cursor_mode` parameter, populated from `vterm_app_cursor_keys()` at the call site in `ble_keyboard.c`.

---

## Files to Create or Modify

| File | Change |
|------|--------|
| `components/input/ble_keyboard.h` | Add `ble_state_t`, new API functions, `ble_device_info_t` |
| `components/input/ble_keyboard.c` | Implement 5-state machine, registry integration |
| `components/input/pairing_overlay.c` | New file — modal pairing UI via vterm |
| `components/input/pairing_overlay.h` | New header |
| `components/input/touch_input.c` | Minimal GT911: tap + long-press events |
| `components/input/input_hal.h` | Add `type`, `x`, `y` to `input_event_t`; new event type constants |
| `components/input/input_hal.c` | Handle new event types in post/read |
| `components/input/hid_keymap.c` | Fix arrow key mode via `app_cursor_mode` param |
| `components/input/hid_keymap.h` | Update `hid_keymap_translate()` signature |
| `components/input/CMakeLists.txt` | Add `pairing_overlay.c` |
| `components/storage/include/storage.h` | Add `ble_device_info_t`, new `storage_ble_*` functions |
| `components/storage/storage.c` | Implement `storage_ble_*` functions |
| `main/main.c` | Add `STATE_BLE_INIT`, `STATE_PAIRING`; long-press handling in SESSION |

---

## Verification Plan

**Stage 1 — BLE connection baseline**
- Flash, check serial logs: BLE stack init → scan start → HID UUID found → connect
- Confirm `input_hal_post_event()` fires on keypress (temporary ESP_LOGI)

**Stage 2 — Input flow end-to-end**
- SSH connected, type on BLE keyboard → characters appear in terminal
- Test: printable keys, Shift, Ctrl+C/D/Z, arrows, Backspace, Tab, Enter, F-keys

**Stage 3 — Pairing overlay at boot**
- Flash with empty NVS → pairing screen appears → scan discovers keyboard → tap → bond → SSH session starts

**Stage 4 — Mid-session pairing**
- Long-press during SSH session → overlay appears, SSH disconnects → pair → SSH reconnects

**Stage 5 — Registry persistence**
- Reboot after pairing → auto-reconnects silently
- `storage_ble_remove()` → next boot shows pairing screen again

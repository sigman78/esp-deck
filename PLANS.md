# CyberDeck — ESP32 SSH Terminal Architecture

## 1. System Overview

```

┌─────────────────────────────────────────────────────────────┐
│                        SHELL (UI)                           │
│   Status bar · Onboarding · Profile picker · Settings       │
├──────────┬──────────┬───────────┬──────────┬────────────────┤
│  VTerm   │   SSH    │   WiFi    │  Input   │   Storage      │
│ (libtsm) │(libssh2) │(esp-wifi) │(BLE/UART)│ (NVS/LittleFS) │
├──────────┴──────────┴───────────┴──────────┴────────────────┤
│                     Terminal                                │
│          Direct console draw (DOS-style)                    │
├─────────────────────────────────────────────────────────────┤
│                     Display                                 │
│         Character/attr display. Font renderer.              │
├─────────────────────────────────────────────────────────────┤
│                      HAL                                    │
│ ESP: LCD/WIFI/BLE/UART/VFS │ PC: SDL and mocks              │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Module Inventory

### 2.1 Display (bounce-buffer rendering, no pixel framebuffer)

**Responsibility:** Owns the text cell buffer (cols × rows of `{glyph, fg, bg, attrs}`) and renders scanlines on-the-fly via a bounce buffer — no full pixel framebuffer exists in memory.

**How it works:** The ESP32 RGB LCD peripheral drives the panel via DMA. When a DMA buffer needs filling, an ISR callback fires and the bounce buffer renderer converts the relevant rows of the cell buffer + bitmap font into pixel data directly into the DMA bounce buffer. The cell buffer, font data, and bounce buffer all reside in **IRAM** (accessed from ISR context, must not be in flash or PSRAM).

| Sublayer | Description |
|----------|-------------|
| `cell_buffer` | The logical cols×rows grid of text cells. Located in IRAM. No dirty tracking needed — the LCD continuously rescans and the bounce buffer renderer always produces current content. |
| `bounce_renderer` | Converts cell rows + bitmap font → pixel scanlines into a small bounce buffer. **Platform-independent core logic** — same function used on ESP (called from DMA ISR) and PC (called to fill SDL surface). |
| `font_data` | Bitmap font glyphs in IRAM. Accessed by bounce renderer at ISR time. |

| Platform | Integration |
|----------|-------------|
| **ESP32** | RGB LCD peripheral → DMA ISR → `display_render_chunk()` → DMA buffer → panel |
| **PC/SDL** | Main loop calls `display_render_chunk()` → fills SDL surface pixels → `SDL_UpdateTexture()` + `SDL_RenderPresent()` |

This design eliminates the need for a ~750KB pixel framebuffer entirely. Memory cost is just the cell buffer (~24KB) + font (~4-8KB) + small bounce buffer (~few KB), all in IRAM.

### 2.2 Terminal

**Responsibility:** DOS-style direct-write console — `terminal_write`, `terminal_set_color`, etc. Writes directly into the cell grid. 
Used by Shell for menus, status bar, onboarding screens.

Platform-independent (operates on cell_grid).

### 2.3 VTerm (libtsm wrapper)

**Responsibility:** Full VT220/xterm emulation. Feeds raw bytes from SSH into libtsm, reads back the TSM screen into cell_grid.

| Function | Description |
|----------|-------------|
| `vterm_init(cols, rows)` | Create TSM screen + VTE |
| `vterm_feed(data, len)` | Push SSH output bytes into VTE |
| `vterm_get_cell(col, row)` | Read cell state (for sync to cell_grid) |
| `vterm_input_key(keysym, mods)` | Translate keypress → VT escape sequence → SSH channel |
| `vterm_resize(cols, rows)` | Handle terminal resize |

Platform-independent.

### 2.4 Input (HAL)

**Responsibility:** Abstracts keyboard input across BLE HID (Using NimBLE), USB HID, and UART/JTAG debug console.

```c
typedef struct {
    uint16_t keycode;     // HID usage / raw scancode
    uint16_t unicode;     // Translated UTF-16 codepoint (0 if not translatable)
    uint8_t  modifiers;   // Shift/Ctrl/Alt/Meta bitmask
    bool     pressed;     // key down vs key up
} input_event_t;

// HAL interface
void hal_input_init(void);
bool hal_input_poll(input_event_t *evt);  // non-blocking
void hal_input_set_callback(void (*cb)(input_event_t *evt));
```

| Backend (ESP) | Notes |
|---------------|-------|
| BLE HID Host | `esp_hidh` — pairs with BLE keyboards, decodes HID reports |
| USB HID Host | Future: USB-OTG host on S3 |
| UART | Debug/fallback — reads from JTAG-USB console |

| Backend (PC) | Notes |
|--------------|-------|
| SDL keyboard | `SDL_KEYDOWN` / `SDL_KEYUP` → `input_event_t` |

### 2.5 WiFi

**Responsibility:** STA-mode WiFi lifecycle — scan, connect, reconnect, report status.

```c
typedef enum {
    WIFI_STATE_OFF,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,  // was connected, lost
    WIFI_STATE_FAILED,
} wifi_state_t;

void wifi_init(void);
void wifi_connect(const char *ssid, const char *pass);
void wifi_disconnect(void);
wifi_state_t wifi_get_state(void);
const char *wifi_get_ip(void);
```

ESP-only. On PC simulator, this module is stubbed — always returns `WIFI_STATE_CONNECTED` with a fake IP, since the PC already has networking.

### 2.6 SSH (libssh2 wrapper)

**Responsibility:** Session lifecycle — TCP connect, host-key verify, authenticate (password / pubkey), open shell channel, PTY request, read/write channel data, keepalive.

```c
typedef enum {
    SSH_STATE_DISCONNECTED,
    SSH_STATE_TCP_CONNECTING,
    SSH_STATE_HANDSHAKE,
    SSH_STATE_AUTH,
    SSH_STATE_CHANNEL_OPEN,
    SSH_STATE_READY,
    SSH_STATE_ERROR,
} ssh_state_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;        // NULL if using key
    const char *privkey_path;    // NULL if using password
    uint16_t term_cols;
    uint16_t term_rows;
} ssh_connect_params_t;

void ssh_init(void);
void ssh_connect(const ssh_connect_params_t *params);
void ssh_disconnect(void);
ssh_state_t ssh_get_state(void);
int  ssh_channel_read(uint8_t *buf, size_t len);    // non-blocking
int  ssh_channel_write(const uint8_t *buf, size_t len);
void ssh_channel_resize(uint16_t cols, uint16_t rows);
void ssh_poll(void);  // call from main loop — handles keepalive, reconnect
```

Platform-independent (libssh2 uses BSD sockets). On PC, connects via normal TCP. On ESP, uses lwIP sockets.

### 2.7 Storage

**Responsibility:** Persistent config — SSH profiles, WiFi credentials, known hosts, SSH keys, user preferences in simple ini file.

```c
// Profile management
int     storage_get_profile_count(void);
bool    storage_get_profile(int index, ssh_profile_t *out);
bool    storage_save_profile(int index, const ssh_profile_t *profile);
bool    storage_delete_profile(int index);

// WiFi credentials
bool    storage_get_wifi_creds(wifi_creds_t *out);
bool    storage_save_wifi_creds(const wifi_creds_t *creds);

// SSH keys (LittleFS for file-like access)
bool    storage_read_file(const char *path, uint8_t *buf, size_t *len);
bool    storage_write_file(const char *path, const uint8_t *buf, size_t len);
```

| Backend (ESP) | Use |
|---------------|-----|
| NVS | Small KV: profiles, WiFi creds, preferences |
| LittleFS | Files: SSH private keys, known_hosts |

| Backend (PC) | Use |
|--------------|-----|
| JSON file on disk | `~/.cyberdeck/profiles.json` |
| Local filesystem | `~/.cyberdeck/keys/` |

### 2.8 Shell (UI Controller)

**Responsibility:** The "application" layer — orchestrates all modules, owns the state machine, renders UI via the Terminal module.

Not a Unix shell — think of it as the firmware's main UI/UX controller.

---

## 3. Event Bus / Inter-Module Communication

Modules communicate through a lightweight **event bus** rather than direct cross-calls. This keeps modules decoupled and makes PC simulation straightforward.

```c
typedef enum {
    EVT_INPUT_KEY,           // input_event_t payload
    EVT_WIFI_STATE_CHANGED,  // wifi_state_t payload
    EVT_SSH_STATE_CHANGED,   // ssh_state_t payload
    EVT_SSH_DATA_READY,      // bytes available on channel
    EVT_BLE_DEVICE_FOUND,    // BLE scan result
    EVT_BLE_CONNECTED,
    EVT_BLE_DISCONNECTED,
    EVT_DISPLAY_RESIZED,     // new cols/rows (future)
    EVT_STORAGE_ERROR,
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        input_event_t   key;
        wifi_state_t    wifi_state;
        ssh_state_t     ssh_state;
        struct { uint8_t *data; size_t len; } ssh_data;
        // ...
    };
} event_t;

// Subscribe / publish
typedef void (*event_handler_t)(const event_t *evt);
void event_subscribe(event_type_t type, event_handler_t handler);
void event_publish(const event_t *evt);
void event_pump(void);  // dispatch pending events (call from main loop)
```

### 3.1 RTOS Minimization Strategy

The PC simulator is single-threaded with no RTOS. To keep the shared `common/` code buildable on both platforms, **RTOS primitives must not leak into common code**. The rule is:

> **Common code calls the event bus API. Only platform code touches RTOS primitives.**

| Primitive | ESP32 (platform) | PC mock |
|-----------|-------------------|---------|
| Event queue | `xQueueSend` / `xQueueReceive` inside `event_bus_esp.c` | Simple ring buffer in `event_bus_pc.c`, pumped from main loop |
| Mutex (for cell buffer if SSH task writes) | `SemaphoreHandle_t` in a thin `platform_mutex.h` wrapper | No-op or `pthread_mutex` if needed |
| Task creation | `xTaskCreatePinnedToCore` in `main.c` (ESP) | Not used — everything runs in main loop |
| Delays | `vTaskDelay` in `main.c` (ESP) | `SDL_Delay` in `main.c` (PC) |
| SSH blocking reads | Dedicated `ssh_task` on Core 1 posts `EVT_SSH_DATA_READY` | `ssh_poll()` uses non-blocking `libssh2_channel_read()` in main loop (set `libssh2_session_set_blocking(session, 0)`) |

**Minimal platform abstraction needed:**

```c
// platform.h — the ONLY RTOS-adjacent header common code may include
// (implementations in esp/ and pc/ directories)

typedef struct platform_mutex_s *platform_mutex_t;
platform_mutex_t platform_mutex_create(void);
void platform_mutex_lock(platform_mutex_t m);
void platform_mutex_unlock(platform_mutex_t m);
void platform_mutex_destroy(platform_mutex_t m);

uint32_t platform_millis(void);   // monotonic ms (esp_timer / SDL_GetTicks)
void platform_sleep_ms(uint32_t ms);
```

This is likely all you need. The cell buffer might need a mutex if `ssh_task` (ESP) writes VTerm output to it concurrently with the DMA ISR reading it — but on PC it's single-threaded so the mutex is a no-op.

**Key design point:** `ssh_poll()` in common code must be non-blocking. On ESP, the dedicated `ssh_task` calls the blocking `libssh2_channel_read()` and publishes events. On PC, `ssh_poll()` is called from the main loop using libssh2's non-blocking mode. The common Shell code doesn't care — it just reacts to `EVT_SSH_DATA_READY`.

---

## 4. Application State Machine & Lifecycle

```
                    ┌──────────┐
         Power on → │  BOOT    │ init display, font, storage
                    └────┬─────┘
                         │
                   ┌─────▼─────┐
          ┌─────── │  SHELL    │ ◄──── ESC / disconnect
          │        │  (idle)   │
          │        └────┬──┬───┘
          │             │  │
     no stored     has  │  │ user picks profile
     WiFi creds   creds │  │
          │             │  │
     ┌────▼─────┐  ┌────▼──▼───┐
     │  WIFI    │  │  WIFI     │
     │  ONBOARD │  │  CONNECT  │
     │  (scan+  │  └────┬──────┘
     │   pick)  │       │ connected
     └────┬─────┘  ┌────▼──────┐
          │        │  SSH      │
          └───────►│  CONNECT  │ handshake + auth
                   └────┬──────┘
                        │ ready
                   ┌────▼──────┐
                   │  SESSION  │ VTerm active, SSH piped
                   │  (active) │
                   └───────────┘
```

### Main Loop (simplified)

```c
// ESP32 — main task (other tasks for SSH, BLE run separately)
void app_main(void) {
    hal_display_init();    // starts RGB LCD + DMA, bounce buffer self-refreshes
    font_init();
    storage_init();
    input_init();
    event_init();
    shell_init();          // enters BOOT → SHELL transition

    while (1) {
        input_poll();      // check for key events
        ssh_poll();        // keepalive, read channel (or done in ssh_task)
        event_pump();      // dispatch all pending events
        shell_tick();      // state machine update + UI writes to cell buffer
        // No display_flush() — bounce buffer ISR renders continuously
        vTaskDelay(pdMS_TO_TICKS(10));  // ~100 Hz
    }
}
```

```c
// PC simulator — single-threaded, no RTOS
int main(void) {
    SDL_Init(SDL_INIT_VIDEO);
    hal_display_init();    // creates SDL window + texture
    font_init();
    storage_init();
    input_init();
    event_init();
    shell_init();

    while (!quit) {
        sdl_poll_events();     // SDL_PollEvent → input_event_t → event_publish
        input_poll();
        ssh_poll();            // same libssh2 code, blocking handled differently (see §8)
        event_pump();
        shell_tick();
        hal_display_present(); // bounce_render_lines() → SDL surface → present
        SDL_Delay(16);         // ~60 FPS
    }
}
```

---

## 5. Data Flow: Keystroke to Remote Shell

```
 BLE Keyboard
      │
      ▼
 hal_input (BLE HID decode)
      │
      ▼
 EVT_INPUT_KEY published
      │
      ▼
 shell_tick() receives event
      │
      ├─ if in SHELL state → navigate menu (Terminal direct-draw)
      │
      └─ if in SESSION state:
            │
            ▼
         vterm_input_key(keysym, mods)
            │
            ▼
         libtsm VTE generates escape sequence
            │
            ▼
         ssh_channel_write(esc_bytes)
            │
            ▼
         libssh2 → TCP → remote host
```

### Data Flow: Remote Output to Screen

```
 remote host → TCP → libssh2
      │
      ▼
 ssh_channel_read(buf) → EVT_SSH_DATA_READY
      │
      ▼
 shell (SESSION state) receives event
      │
      ▼
 vterm_feed(buf, len)
      │
      ▼
 libtsm processes escape codes, updates internal screen
      │
      ▼
 vterm_sync_to_cellgrid()  — diff TSM screen → cell_grid dirty flags
      │
      ▼
 display_flush() — render only dirty cells → LCD
```

---

## 6. UI Interactions (Brief)

### 6.1 Status Bar (persistent, top or bottom row)

Always visible. Shows icons/text for:
- WiFi status (disconnected / connecting / signal strength)
- BLE keyboard (paired / searching / disconnected)
- SSH state (disconnected / connecting / connected)
- Battery (if applicable)

Rendered by Shell via Terminal direct-draw, overlaid on VTerm area when in SESSION.

### 6.2 Onboarding Flow (first boot / no config)

1. **Welcome screen** — "CyberDeck v1.0" splash, "Press any key"
2. **WiFi setup** — scan networks, display list, select, enter password (on-screen or via keyboard)
3. **Create SSH profile** — host, port, username, auth method, optional key import
4. **Done** → return to Shell home

### 6.3 Shell Home / Profile Picker

- List of stored SSH profiles (hostname, user, last-connected)
- Arrow keys to navigate, Enter to connect
- Shortcut keys: `[N]ew` profile, `[E]dit`, `[D]elete`, `[W]iFi` settings, `[S]ettings`

### 6.4 In-Session Overlay

- **ESC+ESC** (double-tap or configurable chord) → drop to Shell menu overlay
  - Disconnect / Switch profile / WiFi info / Back to session
- **Ctrl+Shift+R** → force redraw

### 6.5 Error / Disconnect Handling

- SSH disconnect → notification banner, option to reconnect or return to Shell
- WiFi lost → persistent warning in status bar, auto-reconnect attempts
- Auth failure → return to Shell with error message, option to re-enter credentials

---

## 7. Technical Implementation Notes

### 7.1 Threading Model (ESP32)

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| `main_task` | 0 | 5 | Main loop: event pump, shell, input poll |
| `ssh_task` | 1 | 6 | Blocking `libssh2_channel_read()` → publishes `EVT_SSH_DATA_READY` |
| BLE (internal) | 0 | — | `esp_hidh` manages its own task internally |

**Deliberately minimal.** WiFi runs event-driven via esp-netif callbacks (no dedicated task needed). BLE HID host stack manages its own tasks. Only SSH needs a separate task because libssh2 blocking reads would stall the UI.

**Cell buffer concurrency:** The DMA ISR reads the cell buffer on every scanline. If `ssh_task` triggers VTerm output that modifies the cell buffer, you may see partial-cell tearing on rare frames. In practice this is invisible for text, but if it matters, a `platform_mutex` around cell buffer writes (locked briefly by `vterm_sync_to_cellbuffer()`, ISR skips if locked and uses stale data) can fix it.

**PC simulator:** Everything runs single-threaded. SSH uses `libssh2_session_set_blocking(session, 0)` and `ssh_poll()` is called from the main loop. No tasks, no mutexes needed.

### 7.2 Memory Budget (ESP32-S3, 512KB SRAM)

| Consumer | Location | Estimate |
|----------|----------|----------|
| Cell buffer (100×30 × 8 bytes) | **IRAM** (ISR access) | ~24KB |
| Bitmap font | **IRAM** (ISR access) | ~4-8KB |
| Bounce buffer (DMA) | **IRAM** (ISR access) | ~2-4KB |
| libtsm screen state | SRAM | ~50KB |
| libssh2 session | SRAM | ~30KB |
| SSH read/write buffers | SRAM | ~16KB |
| BLE stack | SRAM | ~40KB |
| WiFi stack | SRAM | ~40KB |
| FreeRTOS tasks + stacks | SRAM | ~32KB |
| **Total SRAM** | | **~240KB** |

No PSRAM framebuffer needed — bounce buffer rendering eliminates the ~750KB framebuffer entirely. IRAM budget (~30-35KB for display) is the tightest constraint.

### 7.3 SSH Authentication Flow

```
1. TCP connect (lwIP socket)
2. libssh2_session_handshake()
3. libssh2_knownhost_readfile() → verify host fingerprint
   - Unknown host: prompt user "Trust this host? [fingerprint]"
   - Mismatch: warn user, block connection
4. Try auth methods in order:
   a. libssh2_userauth_publickey_fromfile() (if key configured)
   b. libssh2_userauth_password() (if password stored)
   c. Keyboard-interactive (future)
5. libssh2_channel_open_session()
6. libssh2_channel_request_pty_ex("xterm-256color", cols, rows)
7. libssh2_channel_shell()
8. → SSH_STATE_READY
```

### 7.4 Display Architecture Notes

No dirty tracking or explicit flush step exists. The LCD RGB peripheral continuously rescans the panel, and the DMA ISR callback renders fresh pixel data from the cell buffer on every scan. Writing to the cell buffer is "instant" — the next scanline pass picks up the change.

**IRAM pressure:** Cell buffer + font + bounce buffer must all fit in IRAM since they're accessed from ISR context. On ESP32-S3 with 512KB SRAM, budget ~30-35KB for these (cell buffer ~24KB, font ~8KB, bounce buffer ~2-4KB). This is the tightest constraint in the system.

**PC simulator:** The same `bounce_render_lines()` function is called in a simple loop to fill the SDL surface, then presented. No DMA, no ISR — just a direct call from the SDL main loop. This is what makes the rendering code truly shared.

---

## 8. PC Simulator Architecture

### 8.1 Project Layout (IDF Component Model)

The project follows ESP-IDF's recommended component layout, with a custom CMake macro (`cyberdeck_component_register`) that dispatches to `idf_component_register` for ESP builds and standard CMake for PC simulator builds. Each component declares platform-specific sources and dependencies via `SRCS_DEV`/`SRCS_SIM` and `REQUIRES_DEV`/`REQUIRES_SIM`.

```
cyberdeck/
├── assets/                    ← fonts, bitmaps, static data
│
├── components/                ← IDF-style components (the bulk of the project)
│   ├── display/
│   │   ├── CMakeLists.txt         cyberdeck_component_register(
│   │   │                            SRCS         display_render.c    ← bounce renderer (shared)
│   │   │                            SRCS_DEV     lcd_driver.c        ← ESP RGB LCD + DMA
│   │   │                            SRCS_SIM     display_sdl.c       ← SDL surface fill
│   │   │                            REQUIRES_DEV esp_lcd driver esp_timer
│   │   │                            REQUIRES_SIM SDL2::SDL2-static)
│   │   └── include/
│   │       └── display.h
│   │
│   ├── font/
│   │   ├── CMakeLists.txt
│   │   └── include/
│   │
│   ├── terminal/              ← DOS-style direct console (shared, no platform deps)
│   │   ├── CMakeLists.txt         SRCS terminal.c
│   │   └── include/               REQUIRES display font
│   │
│   ├── vterm/                 ← libtsm wrapper (shared)
│   │   ├── CMakeLists.txt         SRCS vterm.c
│   │   └── include/               REQUIRES libtsm display
│   │
│   ├── input/
│   │   ├── CMakeLists.txt         SRCS         input.c             ← common dispatch
│   │   │                          SRCS_DEV     input_ble.c input_uart.c
│   │   │                          SRCS_SIM     input_sdl.c
│   │   │                          REQUIRES_DEV esp_hidh bt
│   │   │                          REQUIRES_SIM SDL2::SDL2-static
│   │   └── include/
│   │       └── input.h
│   │
│   ├── wifi/
│   │   ├── CMakeLists.txt         SRCS_DEV     wifi.c
│   │   │                          SRCS_SIM     wifi_stub.c
│   │   │                          REQUIRES_DEV esp_wifi esp_netif
│   │   └── include/
│   │
│   ├── ssh/                   ← libssh2 wrapper (shared — BSD sockets)
│   │   ├── CMakeLists.txt         SRCS ssh.c
│   │   └── include/               REQUIRES libssh2
│   │
│   ├── storage/
│   │   ├── CMakeLists.txt         SRCS_DEV     storage_nvs.c
│   │   │                          SRCS_SIM     storage_file.c
│   │   │                          REQUIRES_DEV nvs_flash littlefs
│   │   └── include/
│   │
│   ├── shell/                 ← UI controller + state machine (shared)
│   │   ├── CMakeLists.txt         SRCS shell.c shell_ui.c
│   │   └── include/               REQUIRES terminal vterm input ssh wifi storage event_bus
│   │
│   ├── event_bus/
│   │   ├── CMakeLists.txt         SRCS         event_bus.c         ← shared subscribe/dispatch
│   │   │                          SRCS_DEV     event_queue_rtos.c  ← FreeRTOS queue backend
│   │   │                          SRCS_SIM     event_queue_ring.c  ← ring buffer backend
│   │   │                          REQUIRES_DEV freertos
│   │   └── include/
│   │
│   ├── platform/              ← minimal RTOS abstraction (mutex, millis)
│   │   ├── CMakeLists.txt         SRCS_DEV platform_esp.c
│   │   │                          SRCS_SIM platform_pc.c
│   │   └── include/
│   │       └── platform.h
│   │
│   ├── libtsm/                ← git submodule or vendored
│   └── libssh2/               ← IDF component wrapper
│
├── idfsim/                    ← mock IDF headers for PC sim build
│   ├── esp_log.h                  (maps ESP_LOGx → printf)
│   ├── freertos/                  (empty stubs or thin typedefs)
│   ├── esp_err.h
│   └── ...
│
├── main/                      ← ESP app entry point
│   ├── CMakeLists.txt             REQUIRES shell display input ...
│   └── main.c                     app_main() — FreeRTOS tasks, ESP init
│
├── sim/                       ← PC simulator entry point
│   ├── CMakeLists.txt
│   └── main.c                     SDL init, single-threaded main loop
│
├── tests/                     ← unit tests (run on PC)
│
├── cmake/
│   └── cyberdeck_component.cmake  ← the SRCS_DEV/SRCS_SIM dispatch macro
│
└── CMakeLists.txt             ← top-level (IDF project or PC build)
```

### 8.2 How `cyberdeck_component_register` Works

The macro is the key enabler for dual-platform builds. On ESP it delegates to `idf_component_register`; on PC it creates a normal CMake library:

```cmake
# Pseudocode of the dispatch logic:
if(IDF_TARGET)  # building under idf.py
    idf_component_register(
        SRCS ${SRCS} ${SRCS_DEV}
        INCLUDE_DIRS ${INCLUDE_DIRS}
        REQUIRES ${REQUIRES} ${REQUIRES_DEV}
    )
else()           # PC simulator build
    add_library(${component_name} ${SRCS} ${SRCS_SIM})
    target_include_directories(${component_name} PUBLIC ${INCLUDE_DIRS})
    target_link_libraries(${component_name} PUBLIC ${REQUIRES} ${REQUIRES_SIM})
    target_include_directories(${component_name} SYSTEM PRIVATE ${IDFSIM_DIR})
endif()
```

This means components that are fully platform-independent (like `terminal`, `vterm`, `shell`, `ssh`) just use `SRCS` and `REQUIRES` — no `_DEV`/`_SIM` variants at all. Only components with hardware-touching code need the split.

### 8.3 What the PC Simulator Gives You

| Capability | Benefit |
|------------|---------|
| Shell UI development | Iterate on menus, profile picker, onboarding without flashing |
| VTerm correctness | Test escape sequence handling against local shells |
| SSH integration | Test SSH connections from PC (same libssh2 code, non-blocking mode) |
| Font tuning | Adjust glyph rendering with instant visual feedback |
| Event bus testing | Simulate WiFi/BLE events, test state machine transitions |
| **Bounce renderer reuse** | Same `bounce_render_lines()` produces pixels on both platforms |

### 8.4 RTOS Mock Layer (PC)

The PC simulator doesn't use FreeRTOS. The thin `platform.h` abstraction (see §3.1) handles the gap:

```c
// platform_pc.c — minimal mocks
#include "platform.h"
#include <SDL.h>
#include <stdlib.h>

// Mutex — no-op for single-threaded PC sim
struct platform_mutex_s { int dummy; };
platform_mutex_t platform_mutex_create(void)   { return calloc(1, sizeof(struct platform_mutex_s)); }
void platform_mutex_lock(platform_mutex_t m)   { (void)m; }
void platform_mutex_unlock(platform_mutex_t m) { (void)m; }
void platform_mutex_destroy(platform_mutex_t m){ free(m); }

uint32_t platform_millis(void)        { return SDL_GetTicks(); }
void platform_sleep_ms(uint32_t ms)   { SDL_Delay(ms); }
```

```c
// event_bus_pc.c — ring buffer, no queue
#define EVT_RING_SIZE 64
static event_t ring[EVT_RING_SIZE];
static int ring_head = 0, ring_tail = 0;

void event_publish(const event_t *evt) {
    ring[ring_head] = *evt;
    ring_head = (ring_head + 1) % EVT_RING_SIZE;
}

void event_pump(void) {
    while (ring_tail != ring_head) {
        dispatch(&ring[ring_tail]);  // call registered handlers
        ring_tail = (ring_tail + 1) % EVT_RING_SIZE;
    }
}
```

**SSH on PC:** Uses non-blocking libssh2. `ssh_poll()` called from main loop attempts reads, publishes `EVT_SSH_DATA_READY` when data arrives. No threading needed.

### 8.5 The `idfsim/` Mock Layer

The `idfsim/` directory provides stub headers so that shared component code can `#include` IDF headers without `#ifdef` guards everywhere. This covers things like:

- `esp_log.h` → maps `ESP_LOGI` / `ESP_LOGW` / `ESP_LOGE` to `printf`
- `esp_err.h` → typedefs `esp_err_t`, defines `ESP_OK` etc.
- `freertos/FreeRTOS.h` → empty or minimal typedefs (only needed if any shared code accidentally pulls it in)
- `esp_timer.h` → wraps `SDL_GetTicks` or `clock_gettime`

The goal is **not** to reimplement IDF — just provide enough type/macro stubs that shared code compiles. Platform-specific code (`SRCS_DEV`) never builds on PC, so its deeper IDF dependencies don't matter.

### 8.6 Simulating ESP-Only Peripherals

```c
// wifi_stub.c — PC simulator WiFi
static wifi_state_t state = WIFI_STATE_CONNECTED;

void wifi_init(void) {}
void wifi_connect(const char *ssid, const char *pass) {
    (void)ssid; (void)pass;
    state = WIFI_STATE_CONNECTED;
    event_t evt = { .type = EVT_WIFI_STATE_CHANGED, .wifi_state = state };
    event_publish(&evt);
}
wifi_state_t wifi_get_state(void) { return state; }
const char *wifi_get_ip(void) { return "192.168.1.100"; }
```

For BLE keyboard simulation, SDL keyboard events go through `hal_input_sdl.c` → same `input_event_t` → same event bus → Shell doesn't know the difference.

---

## 9. Suggested Implementation Order

| Phase | Modules | Milestone |
|-------|---------|-----------|
| **0 — Current** | Display, Font, VTerm, Terminal | ✅ ESC text rendering works |
| **1 — Input + Event Bus** | `event_bus`, `hal_input` (SDL first, then BLE) | Keypresses flow through event bus |
| **2 — Shell Skeleton** | `shell` state machine, `terminal` menus | Navigate menus on screen |
| **3 — Storage** | `storage` (PC file backend first) | Save/load SSH profiles |
| **4 — SSH Integration** | `ssh` module, wire to VTerm + event bus | Connect to SSH server from PC sim |
| **5 — WiFi** | `wifi` module (ESP), stub (PC) | Connect to real WiFi on ESP |
| **6 — BLE Input** | `hal_input_ble` (ESP) | Type on BLE keyboard → SSH |
| **7 — Polish** | Status bar, reconnect logic, error handling, host-key UI | Usable product |
| **8 — Extras** | USB HID, mDNS discovery, SFTP, multiple sessions | Nice-to-haves |

---

## 10. Things You Might Have Missed

**TERM environment / PTY size negotiation** — Send `SIGWINCH`-equivalent via `ssh_channel_resize()` if display size ever changes. Set `TERM=xterm-256color` in PTY request.

**Keepalive** — libssh2 supports `libssh2_keepalive_config()`. Essential for NAT traversal and detecting dead connections. Run from ssh_poll().

**Host key management** — `known_hosts` file in LittleFS. First-connection TOFU prompt ("The authenticity of host X can't be established. Fingerprint: SHA256:... Connect anyway?"). Critical for security.

**Power management** — ESP32 light sleep between display refreshes if idle. Wake on BLE/UART interrupt. Significant battery impact if battery-powered.

**OTA updates** — ESP32 supports OTA via HTTPS. Could add an "Update firmware" option in Shell settings.

**Scrollback buffer** — libtsm supports scrollback. Map Shift+PageUp/PageDown to scroll. Configurable depth (memory permitting).

**Clipboard** — Could support local selection/copy from scrollback via Shift+arrow keys. Paste via Shift+Insert → inject into SSH channel.

**mDNS/Avahi discovery** — Auto-discover SSH servers on local network. Nice for onboarding.
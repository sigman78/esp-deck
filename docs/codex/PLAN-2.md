# Platform-HAL + Structured App Architecture

## Summary

Use exactly two top-level architectural domains:

- `platform` = HAL and platform adaptation code
- `app` = everything product-specific above HAL

Within that, do not flatten the app into one blob. The app should still be internally structured into a few concrete subsystems with clear ownership, but without overengineering them into a forest of generic interfaces. The rule is:

- abstraction boundary is strict at `platform -> app`
- inside `app`, use concrete module boundaries first
- introduce extra interfaces only where two real implementations or async ownership require them

This keeps the architecture aligned with your intent: HAL is the platform seam, and everything else belongs to the app layer.

## Vision

The shared product should be a single cyberdeck application that runs on top of a narrow HAL. ESP32 hardware and SDL2 emulation live entirely under `platform`. Above that line, the app owns terminal behavior, SSH session behavior, UI flow, pairing flow, routing, and state.

The structure to plan toward is:

```text
+-----------------------------------------------------------+
|                         APP LAYER                         |
|                                                           |
|  +------------------ app_shell ------------------------+  |
|  | boot/session/ui state machine                      |  |
|  | focus, modal routing, reconnect policy             |  |
|  +-------------------+----------------+---------------+  |
|                      |                |                  |
|          +-----------+--+      +------+-----------+      |
|          | app_terminal |      | app_session_ssh |      |
|          | tsm wrapper  |<---->| raw byte stream |      |
|          | cell buffer  |      | connect/read/write     |
|          | key encoding |      | PTY resize/status |    |
|          +------+-------+      +-----------+------+      |
|                 |                              |          |
|         +-------+--------+          +----------+-----+   |
|         | app_ui / modal |          | app_data/store |   |
|         | splash, pairing|          | profiles, keys |   |
|         | status, menus  |          | app-side policy|   |
|         +----------------+          +----------------+   |
|                                                           |
+------------------------------^----------------------------+
                               |
                      strict HAL boundary
                               |
+------------------------------v----------------------------+
|                      PLATFORM / HAL                       |
|                                                           |
|  hal_display   hal_input   hal_net   hal_storage         |
|  hal_time      hal_system  hal_random/log if needed      |
|                                                           |
|   ESP32 impl: esp-idf, lcd, nimble, wifi, littlefs       |
|   SDL2 impl : sdl window, keyboard/mouse, host fs, stub  |
|                                                           |
+-----------------------------------------------------------+
```

Core intent of the diagram:
- `app_shell` is the composition root of the app layer.
- `app_terminal` and `app_session_ssh` are peer subsystems, not direct owners of each other.
- `platform` provides capabilities, not product behavior.
- simulator and hardware differ only under HAL.

## Key Changes

### 1. Define the top-level boundary clearly
Adopt these hard rules:

- `app` must not include ESP-IDF, SDL2, NimBLE, panel driver, or board-specific headers.
- `platform` must not include `tsm`, SSH UI logic, overlays, or app state machine code.
- `main/main.c` and `sim/main.c` become thin platform bootstraps that init HAL and enter one shared `app_shell`.

Planned directory intent:
- `platform/display`, `platform/input`, `platform/net`, `platform/storage`, `platform/system`
- `app/shell`, `app/terminal`, `app/session`, `app/ui`, `app/data`

### 2. Keep the app internally structured, but concrete
Inside `app`, use subsystem boundaries rather than generic service abstractions.

Recommended app subsystems:
- `app_shell`
  - top-level state machine
  - routes events and owns focus/modal state
  - decides when to connect WiFi/SSH and when to show overlays
- `app_terminal`
  - wraps `tsm`
  - owns screen model, cursor state, dirty copy, terminal modes, key-to-sequence encoding
- `app_session_ssh`
  - owns libssh2 session lifecycle and background read pumping
  - exposes raw byte in/out and connection state
- `app_ui`
  - splash, status area, pairing modal, future menu/profile picker
- `app_data`
  - profile/key loading policy and app-side selection logic

This is still “App layer”, but now organized enough that responsibilities do not smear across files.

### 3. Make HAL coarse, not over-abstracted
Use a small number of HAL modules with simple, stable APIs. Avoid a generic interface hierarchy.

HAL modules to plan:
- `hal_display`
  - init, register text surface, register overlay surface, cursor update, present/pump
- `hal_input`
  - init and deliver normalized input events
- `hal_net`
  - init, connect, disconnect, status/events
- `hal_storage`
  - profile/key/device persistence
- `hal_time`
  - monotonic ms, sleep/delay
- `hal_system`
  - startup/shutdown hooks, heap info, optional logging helpers

Guidance:
- keep one public header per HAL module
- allow two implementations only where they exist now: ESP32 and SDL2/sim
- do not add “interface objects” unless a module truly needs runtime-swappable implementations

### 4. Fix the current coupling by moving decisions upward into app
These are the main architectural corrections:

- `ssh_client` must stop feeding `vterm` directly.
  - Instead: SSH emits session bytes/state to `app_shell`, which forwards bytes to `app_terminal`.
- BLE keyboard backend must stop querying terminal mode.
  - Instead: HAL input emits raw key/modifier events; `app_terminal` encodes them using current terminal mode.
- pairing overlay must stop reading the global input queue itself.
  - Instead: `app_shell` routes events to the active modal UI.
- WiFi logic must stop being a polling global owned by random callers.
  - Instead: HAL net reports status; `app_shell` owns connection policy.

This keeps platform code dumb and app code in control.

### 5. Place shared rendering logic carefully
The display rendering split should follow ownership, not implementation convenience:

- rendering that converts terminal cell surfaces into pixels belongs under `platform/display`
  - because it is part of the HAL display contract
- terminal semantics and screen-state mutation belong under `app_terminal`
  - because they are product behavior

So the current renderer split evolves into:
- `app_terminal` produces text/overlay surfaces and cursor state
- `hal_display` consumes those surfaces and renders them on ESP32 or SDL2

### 6. Keep one foreground app loop and minimal background workers
Use this concurrency model:

- one foreground `app_shell` event loop owns app state and routing
- background workers are allowed only where platform/library constraints require them:
  - SSH read loop
  - BLE/NimBLE internals
  - display driver internals if needed

All worker outputs should become plain events delivered upward. Avoid direct callbacks from worker code into unrelated app modules.

Recommended app event types:
- input event
- net status changed
- session data received
- session status changed
- timer/timeout

That is enough structure without building a heavyweight event framework.

## Public APIs / Interfaces To Plan

Important API surfaces to converge on:

- `hal_display.h`
  - surface registration, cursor, present/pump
- `hal_input.h`
  - normalized raw input events
- `hal_net.h`
  - platform connectivity state and actions
- `hal_storage.h`
  - persistence primitives only
- `app_terminal.h`
  - feed remote bytes, query terminal mode, encode local input, reset/resize
- `app_session_ssh.h`
  - connect/disconnect/send/poll session events
- `app_shell.h`
  - shared app entrypoint and lifecycle

Intent for types:
- HAL types are platform-neutral C structs/enums
- terminal transport remains raw byte stream `uint8_t* + len`
- app UI state stays app-private
- avoid broad callback surfaces unless needed for async delivery

## Test Plan

- Keep the `tsm` host tests as the base regression suite.
- Fix or retire the current legacy `terminal` host test harness before further refactoring it; it is not a reliable architecture guard as-is.
- Add app-level tests for:
  - boot -> WiFi ready -> SSH connect -> session active
  - session disconnect -> reconnect path
  - modal pairing focus capture without leaking input to remote session
  - raw key/modifier input encoded by `app_terminal`
- Add HAL contract tests for simulator/device parity:
  - normalized input event shape
  - storage behavior
  - net status semantics
  - display surface registration behavior
- Add one simulator smoke test proving the shared app stack runs through HAL only.

## Assumptions and Defaults

- Top level is exactly `platform` and `app`.
- Internal structure inside `app` is required and intentional, not a violation of the two-layer split.
- `tsm` remains and lives under `app_terminal`.
- SSH remains the primary interactive session model.
- SDL2 emulation must satisfy the same HAL contract as device code, not invent a separate app path.
- Favor concrete subsystem APIs over generic interface patterns unless multiple real implementations justify more abstraction.

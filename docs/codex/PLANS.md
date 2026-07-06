# Cyberdeck Terminal Architecture Review and Improvement Plan

## Summary

Current architecture is workable but the boundaries are inconsistent: `main` is the real composition root, while several components also reach across layers directly. The strongest example is transport and input code depending on terminal state details: `components/ssh/ssh_client.c` writes straight into `vterm`, and `components/input/ble_keyboard.c` queries `vterm_app_cursor_keys()` during key translation. That makes session, terminal, and device input hard to evolve independently.

The terminal/rendering core is the healthiest part today. `tsm` is well covered by host tests and should remain the terminal engine. By contrast, the separate `terminal` component is effectively legacy and should be retired from the main architecture rather than expanded. There is also a simulator/device split in orchestration logic between `main/main.c` and `sim/main.c`, which should be collapsed into one shared app controller.

This plan assumes an incremental, SSH-first refactor: preserve current behavior, keep `tsm`, keep the display pipeline, and improve API surface and ownership so later additions like local UI sessions, profile picker, or non-SSH backends do not require another structural rewrite.

## Key Changes

### 1. Establish explicit layers and ownership
Create four layers and keep dependencies one-way only:

- `platform`: display driver, BLE backend, touch backend, WiFi backend, storage backend, simulator shims.
- `services`: `ssh_session`, `wifi_service`, `input_service`, `profile_store`.
- `terminal`: `tsm` adapter plus terminal presentation state.
- `app`: one `cyberdeck_app` controller that owns state machine, focus, overlays, and service wiring.

Rules to enforce:
- `platform` must not include `vterm`, `ssh_client`, or app code.
- `services` must not render directly.
- `terminal` must not know about SSH, BLE, or WiFi.
- `app` is the only layer allowed to connect components together.

### 2. Replace direct coupling with typed interfaces
Define these public interfaces and migrate all cross-module calls through them:

- `session_iface`
  - `start(config)`, `stop()`, `send(bytes,len)`, `resize(cols,rows)`, `is_connected()`
  - event callback or queue for `DATA`, `CONNECTED`, `DISCONNECTED`, `ERROR`
- `terminal_iface`
  - `feed_remote(bytes,len)`, `reset()`, `resize(cols,rows)`
  - callback for `WRITE_REMOTE` terminal responses and `BELL`
  - query for current terminal modes needed by key encoding
- `input_router`
  - consumes raw `input_event_t`
  - outputs either `app_action_t` or encoded terminal input bytes
- `overlay_controller`
  - owns modal overlays and focus capture
  - takes actions and render state, but never reads input directly from the global queue

Concrete refactors:
- `ssh_client` stops including `vterm.h`; it only emits session events.
- BLE keyboard backend stops including `vterm.h`; it emits raw key/modifier intent only.
- `pairing_overlay` stops running its own input loop; app controller gives it focus and routes events to it.
- `main/main.c` and `sim/main.c` become thin bootstraps that construct the same `cyberdeck_app`.

### 3. Normalize terminal and rendering responsibilities
Keep `tsm` as the canonical screen model and make `vterm` the only terminal implementation. Remove the architectural role of the older `terminal` module.

Target split inside terminal/display path:
- `terminal_core`: wraps `tsm`, owns cell buffer, dirty-region copy, cursor state, response callback.
- `terminal_keymap`: converts key/modifier input into terminal byte sequences using terminal mode state.
- `display_render`: remains a pure renderer over a registered text surface plus overlay surface.

API changes:
- Rename `vterm` to a less transport-specific name such as `terminal_core` or `terminal_session` in the long-lived API surface.
- Add `resize(cols,rows)` now, even if backed by recreate-on-resize initially, because PTY resize is a required architectural seam.
- Make the terminal response path explicit so DA/DSR/CPR traffic flows `terminal -> app -> session`, never `terminal -> ssh` directly.

### 4. Move orchestration into a shared app controller
Introduce a single `cyberdeck_app` state machine with explicit substates:

- boot
- wifi_connecting
- session_connecting
- session_active
- pairing_modal
- error

Responsibilities:
- choose active profile from storage
- initiate WiFi and SSH
- route session output into terminal
- route terminal responses back into active session
- route input either to overlay/app shortcuts or to terminal key encoding
- handle reconnect policy and cursor/overlay visibility

This removes today’s split ownership where `main` handles app state, `ssh_client` owns read task behavior, and overlays consume the same input queue opportunistically.

### 5. Tighten service contracts and concurrency model
Adopt one concurrency rule: services may have their own task/thread, but all cross-task interaction must be message-based.

Required changes:
- `ssh_session` owns its socket/libssh2 task(s) and posts events to app; no direct rendering side effects.
- `wifi_service` becomes event-driven instead of “call connect then poll global bool”.
- `input_service` owns device backends and exposes one queue/channel of normalized raw events.
- storage remains synchronous for now, but app/service code owns profile selection and secret lookup.

Keep singletons only at bootstrap level. Internal module state should move behind opaque handles where practical, but this refactor does not require fully multi-instance support everywhere.

## Test Plan

- Keep and continue running the `tsm` host suites as regression gates; they currently pass and are the strongest safety net.
- Fix the broken host `terminal` test harness first or remove it if the legacy `terminal` module is retired. Right now it does not link because the display stub omits `display_ansi_to_rgb565`.
- Add host tests for new boundaries:
  - `session_iface` event ordering on connect/disconnect/error
  - `input_router` mapping of BLE/UART/touch input to app actions vs terminal bytes
  - modal overlay focus rules so pairing cannot leak keystrokes into SSH
  - app-controller state transitions for WiFi loss, SSH reconnect, and overlay entry/exit
- Add one simulator integration test path or scripted smoke run for: boot -> connect -> remote output -> keyboard input -> disconnect.
- Add a resize contract test even if device resolution is fixed today.

## Assumptions and Defaults

- Scope is incremental, not a ground-up rewrite.
- Product model is SSH-first; local UI is secondary and should sit beside, not inside, the transport layer.
- `tsm` remains the terminal engine; do not replace it with `libvterm`.
- No UI framework migration is included; keep the current display/overlay renderer.
- Profile storage format can stay file-based for now; architecture work is about ownership and interfaces, not storage redesign.

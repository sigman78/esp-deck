# Simulator Input Latency — Analysis

Three independent polling intervals stack on top of network RTT.
A real terminal (e.g. Windows Terminal) has <2 ms local overhead for all three.

---

## Source 1 — Input polling delay (up to 16 ms average ~8 ms)

`sim/main.c` calls `SDL_Delay(16)` at the end of every loop.
A keypress sits waiting until the next `SDL_PollEvent()` before it is forwarded to
`ssh_client_send()`.

**Note:** user already improved this with `SDL_Delay(gotInput ? 1 : 16)` so input
events now drain in ~1 ms. The 16 ms idle delay remains when no input is active.

---

## Source 2 — ssh_read_task EAGAIN sleep (up to 10 ms, average ~5 ms)

`ssh_client.c`:
```c
vTaskDelay(n > 0 ? 1 : pdMS_TO_TICKS(10));
```
libssh2 runs in non-blocking mode. On `LIBSSH2_ERROR_EAGAIN` (channel empty) the
task calls `Sleep(10)`. If the server response arrives 1 ms into that sleep, the
task won't process it for another 9 ms.

**Fix when ready:** reduce EAGAIN sleep to 1 ms, or use
`WaitForSingleObject(event, 10)` on a handle signalled by an async socket callback.
Alternatively switch to blocking mode (no non-blocking flag after shell open).

---

## Source 3 — Render decoupled from vterm flush (up to 16 ms, average ~8 ms)

`vterm_flush()` → `flush_buf()` → `refresh_display()` updates the cell buffer
but **never calls `display_render_frame()`**.  Pixels only reach the SDL window
when the main loop calls `display_render_frame()` — up to 16 ms later.

**Fix when ready:** add a `#ifdef BUILD_SIMULATOR` block inside `refresh_display()`
(in `vterm.c`) that calls `display_render_frame()` directly, or post a user event
via `SDL_PushEvent()` from the SSH read thread to wake the main loop immediately.

---

## Worst-case stack (before gotInput fix)

| Source                    | Max   | Average |
|---------------------------|-------|---------|
| Input polling (SDL_Delay) | 16 ms |   8 ms  |
| EAGAIN sleep              | 10 ms |   5 ms  |
| Render delay              | 16 ms |   8 ms  |
| **Total local overhead**  | **42 ms** | **21 ms** |

A real terminal adds ~1–2 ms total across all three paths.

---

## Files involved

| File | Role |
|------|------|
| `sim/main.c`                      | SDL_Delay value, gotInput optimization |
| `components/ssh/ssh_client.c:188` | `vTaskDelay(n > 0 ? 1 : pdMS_TO_TICKS(10))` |
| `components/vterm/vterm.c`        | `flush_buf()` → `refresh_display()` — no render call |

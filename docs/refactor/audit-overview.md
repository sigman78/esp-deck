# Audit Overview

## Current layout
- `main/`: ESP-IDF composition root and device startup.
- `sim/`: host simulator composition root and SDL event loop.
- `components/display`: shared renderer plus device/SDL backends.
- `components/tsm`: VT parser and terminal model.
- `components/vterm`: adapter from parser/model to display buffer.
- `components/ssh`: transport plus direct terminal coupling.
- `components/input`: hardware input backends, BLE lifecycle, and pairing overlay UI.
- `components/storage`: profiles, keys, BLE registry, with device/host storage backends.
- `components/wifi`: device and host Wi-Fi backends.

## Main issues
- Device and host duplicate app orchestration logic.
- `main/main.c` is a god entrypoint that owns nearly every subsystem.
- `ssh_client` feeds terminal output directly into `vterm`, so transport and presentation are coupled.
- `input` owns UI behavior (`pairing_overlay`) and depends on terminal mode through `vterm`.
- `display.h` owns the text-cell contract used by higher layers.
- Host build portability is weak; simulator CMake assumes GCC-like flags.

## Strong seams to preserve
- Shared display rendering core with target-specific backends.
- `storage` device/host split behind one API.
- `tsm` as the terminal core.

# Progress Log

## 2026-04-10
- Added `docs/refactor/` workspace for audit, target architecture, and migration notes.
- Introduced `components/cyberdeck_app` as the first shared orchestration layer.
- Moved device and simulator boot/session state machines into `cyberdeck_app`.
- Reduced `main/main.c` and `sim/main.c` to composition roots plus event pumps.
- Fixed simulator build portability for MSVC by gating GCC-specific warning flags.
- Fixed the host terminal test seam by providing a stub `display_ansi_to_rgb565`.
- Verified:
  - `tests/terminal`
  - `tests/tsm`
  - host simulator build via `cmake -S . -B build-sim-check -DBUILD_SIMULATOR=ON`

## Next
- Separate SSH transport from terminal presentation.
- Move pairing overlay/UI behavior out of `components/input`.
- Pull the text-cell contract out of `display.h` into a renderer-neutral surface API.

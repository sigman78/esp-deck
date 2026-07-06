# Migration Plan

## Active sequence
1. Add refactor documentation workspace.
2. Introduce shared app runtime and move orchestration out of device/host entrypoints.
3. Fix host build portability so the simulator remains a valid architecture target.
4. Continue separating transport, terminal, input, and display ownership.

## Near-term boundaries
- `cyberdeck_app`: shared orchestration layer.
- `main/` and `sim/`: composition roots and event pumps only.
- `pairing_overlay`: future move from input into app/UI.
- `ssh_client`: future move from terminal-aware transport to session transport.
- `display.h`: future reduction to renderer/display contract only.

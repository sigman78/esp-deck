# Target Architecture

## Layering
1. Platform adapters
   - Device: IDF, LCD, BLE HID, touch, Wi-Fi, filesystem, sockets.
   - Host: SDL, host filesystem, simulated/stubbed Wi-Fi, host sockets.
2. Core services
   - Terminal core (`tsm`).
   - Terminal/session adapter.
   - Storage service.
   - Wi-Fi service.
   - Input normalization.
   - SSH/session transport.
3. App/runtime
   - Shared state machine and orchestration.
   - Shared boot/session lifecycle.
   - UI overlays and app commands.

## Rules
- Device behavior is the source of truth.
- Host support may simulate, patch, or skip unsupported hardware behavior, but the shared app/runtime API stays the same.
- Composition roots stay thin.
- Stateful boundaries should move toward explicit runtime/context ownership.
- UI behavior must not live inside hardware/input backends.

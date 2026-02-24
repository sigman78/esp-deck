# Cyberdeck SSH Terminal - POC

ESP32-S3 based portable SSH terminal with 7-inch display and BLE keyboard support.

## Project Structure

```
cyberdeck/
├── main/                   # Main application
│   ├── main.c             # Application entry point
│   ├── include/           # Main headers
│   └── Kconfig.projbuild  # Configuration options
├── components/            # Project components
│   ├── display/           # LCD display driver
│   ├── font/              # Font rendering (Terminus 8x16)
│   ├── terminal/          # Terminal emulator
│   ├── ssh/               # SSH client
│   ├── input/             # BLE keyboard & touch
│   └── wifi/              # WiFi manager
└── CMakeLists.txt         # Root build config
```

## Quick Start

### Prerequisites

1. **Install ESP-IDF v5.1 or later** (required for RGB LCD support)
   ```bash
   # Clone ESP-IDF
   git clone --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf
   git checkout v5.1  # or later (v5.2, v5.3, etc.)
   ./install.sh esp32s3
   ```

2. Set up ESP-IDF environment:
   ```bash
   . $HOME/esp-idf/export.sh
   ```

   **Important**: ESP-IDF v5.0 and earlier do not have full RGB LCD panel support.
   You must use v5.1 or later for this project.

### Build & Flash

1. Configure project:
   ```bash
   idf.py menuconfig
   ```
   - Set WiFi SSID/password in "Cyberdeck Configuration"
   - Set SSH server details
   - Adjust terminal size if needed

2. Build:
   ```bash
   idf.py build
   ```

3. Flash:
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

## Configuration

All configuration is in `idf.py menuconfig` → "Cyberdeck Configuration":

- Terminal dimensions (default: 100×30)
- Scrollback buffer size
- WiFi credentials
- SSH server details
- Colors

## Key Features

### 🚀 Bounce Buffer Rendering
- **No framebuffer needed** - saves 768KB PSRAM!
- Small 25.6KB bounce buffer in fast SRAM
- Dirty row tracking for efficient updates
- **15-180× faster** than traditional framebuffer approach
- See [BOUNCE_BUFFER_RENDERING.md](BOUNCE_BUFFER_RENDERING.md) for details

## Current Status

### ✅ Working
- [x] **Display driver** - RGB LCD with bounce buffer rendering (ISR callback-based)
- [x] **Font rendering** - Real Terminus 8x16 font integrated
- [x] **WiFi manager** - Connection and event handling
- [x] **Terminal buffer** - 100×30 cell buffer with dirty row tracking
- [x] **Project structure** - All components organized and building

### ⚠️ Stub/Placeholder
- [ ] **SSH client** - Interface defined, mock implementation only
- [ ] **BLE keyboard** - Interface defined, not implemented
- [ ] **Touch input** - Not implemented
- [ ] **Terminal emulation** - Basic text only, no ANSI sequences yet

### 📋 Next Steps
1. Integrate libvterm for full terminal emulation (256 colors, alt screen, etc.)
2. Implement SSH client (libssh or custom with mbedTLS)
3. Connect terminal rendering to bounce buffer callback
4. Implement BLE HID host for keyboard input
5. Add touch UI for WiFi config
6. Power management and OTA updates

## Hardware

**Board**: Waveshare ESP32-S3-Touch-LCD-7

- **Display**: 800×480 RGB LCD (16-bit parallel interface)
- **Touch**: GT911 capacitive (not yet implemented)
- **MCU**: ESP32-S3 dual-core @ 240MHz
- **RAM**: 512KB SRAM + 8MB PSRAM
- **Flash**: 16MB

**Display Configuration** (in `lcd_driver.c`):
- PCLK: 16MHz
- Backlight: GPIO 2
- Pin mapping configured for actual board (may differ from Waveshare documentation)
- Bounce buffer: 25.6KB in SRAM (800×16 pixels)

## Documentation

- [Full Documentation](../CYBERDECK_POC_DOCUMENTATION.md) - Complete implementation guide
- [Technical Specs](../TECHNICAL_SPECS.md) - Hardware/software specifications
- [Quick Start Guide](../QUICK_START.md) - Setup and build instructions
- [Bounce Buffer Rendering](BOUNCE_BUFFER_RENDERING.md) - **Rendering architecture details** ⭐
- [Troubleshooting Guide](TROUBLESHOOTING.md) - **Common errors and solutions** 🔧

## License

TBD

## Contributing

This is a POC project. Contributions welcome!

---

*For detailed implementation guide, see CYBERDECK_POC_DOCUMENTATION.md*

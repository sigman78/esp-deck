# Troubleshooting Guide

## Build Errors

### Error: `esp_lcd_panel_rgb.h: No such file or directory`

**Symptom**:
```
cyberdeck/components/display/lcd_driver.c:10:10: fatal error: esp_lcd_panel_rgb.h: No such file or directory
```

**Cause**: You're using ESP-IDF v5.0 or earlier, which doesn't have full RGB LCD panel support.

**Solution**: Upgrade to ESP-IDF v5.1 or later

```bash
# Check current ESP-IDF version
idf.py --version

# If version is less than 5.1.0, upgrade:

# 1. Navigate to your ESP-IDF directory
cd ~/esp-idf  # or wherever you installed ESP-IDF

# 2. Fetch latest versions
git fetch

# 3. Checkout v5.1 or later
git checkout v5.1   # or v5.2, v5.3, etc.

# 4. Update submodules
git submodule update --init --recursive

# 5. Reinstall tools
./install.sh esp32s3

# 6. Re-source environment
. ./export.sh

# 7. Verify version
idf.py --version
# Should show: ESP-IDF v5.1.0 or higher
```

**Alternative** (if you can't upgrade ESP-IDF):

The Waveshare ESP32-S3-Touch-LCD-7 board uses RGB parallel interface which requires ESP-IDF 5.1+. If you must use an older version, you'll need to:

1. Use a different display module (SPI-based LCD)
2. Write a custom driver
3. Wait for ESP-IDF 5.1+ support in your environment

---

### Error: `undefined reference to 'esp_lcd_new_rgb_panel'`

**Symptom**:
```
undefined reference to `esp_lcd_new_rgb_panel'
```

**Cause**: ESP-IDF version mismatch or missing component dependency.

**Solution 1**: Verify ESP-IDF version
```bash
idf.py --version
# Must be 5.1.0 or higher
```

**Solution 2**: Clean and rebuild
```bash
idf.py fullclean
idf.py build
```

**Solution 3**: Check component requirements
Ensure `components/display/CMakeLists.txt` includes:
```cmake
REQUIRES
    esp_lcd
    driver
```

---

### Error: `Failed to allocate bounce buffer`

**Symptom**:
```
E (123) lcd_driver: Failed to allocate bounce buffer
```

**Cause**: Not enough internal SRAM available (26KB needed).

**Solution**: Free up SRAM by:

1. **Reduce FreeRTOS heap**:
   ```
   idf.py menuconfig
   → Component config → ESP32S3-Specific
   → Set "Minimum free heap size" to lower value
   ```

2. **Move some buffers to PSRAM** (if needed):
   In `terminal.c`, change allocation:
   ```c
   // If SRAM is tight, use PSRAM for terminal buffer
   term.buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
   ```

3. **Check actual SRAM usage**:
   ```bash
   idf.py size-components
   ```

---

### Error: Build fails with memory errors

**Symptom**:
```
region `iram0_0_seg' overflowed by XXX bytes
region `dram0_0_seg' overflowed by XXX bytes
```

**Cause**: Code or data segments too large.

**Solutions**:

1. **Enable PSRAM**:
   ```
   idf.py menuconfig
   → Component config → ESP32S3-Specific
   → Support for external, SPI-connected RAM: [✓]
   ```

2. **Optimize code size**:
   ```
   idf.py menuconfig
   → Compiler options
   → Optimization Level → Optimize for size (-Os)
   ```

3. **Move large arrays to flash**:
   Add `const` keyword to font data:
   ```c
   const uint8_t terminus_8x16_font[...] = { ... };
   ```

---

### Error: `No WiFi connection`

**Symptom**:
```
E (5000) wifi_manager: WiFi connection timeout
```

**Solutions**:

1. **Check credentials**:
   ```
   idf.py menuconfig
   → Cyberdeck Configuration
   → WiFi SSID: [your-network]
   → WiFi Password: [your-password]
   ```

2. **Verify WiFi antenna**:
   - Check antenna is connected to board
   - Check WiFi is enabled in your environment

3. **Check network**:
   - Is 2.4GHz WiFi available? (ESP32-S3 doesn't support 5GHz)
   - Is WPA2 encryption used?

---

### Error: Display shows nothing (blank screen)

**Symptom**: LCD stays black or white, no output.

**Possible Causes & Solutions**:

1. **Backlight not enabled**:
   - Check GPIO2 is set correctly
   - Verify `LCD_BK_LIGHT_ON_LEVEL` is correct (1 or 0)

2. **Wrong pin configuration**:
   - Double-check pin numbers match your board
   - Refer to Waveshare schematic

3. **Timing issues**:
   - Try adjusting `LCD_PIXEL_CLOCK_HZ` (lower if unstable)
   - Check hsync/vsync parameters

4. **Power issue**:
   - Ensure 5V/2A power supply
   - Check current draw isn't exceeded

**Debug steps**:
```c
// Add to display_init():
ESP_LOGI(TAG, "Testing backlight...");
gpio_set_level(PIN_NUM_BK_LIGHT, 1);
vTaskDelay(pdMS_TO_TICKS(2000));
gpio_set_level(PIN_NUM_BK_LIGHT, 0);
```

---

### Error: Display shows corrupted/distorted image

**Symptom**: Display works but shows wrong colors, tearing, or corruption.

**Solutions**:

1. **Check pixel clock**:
   ```c
   // Try reducing clock speed
   #define LCD_PIXEL_CLOCK_HZ  (10 * 1000 * 1000)  // 10MHz instead of 20MHz
   ```

2. **Verify DMA alignment**:
   - Bounce buffer should be 4-byte aligned
   - Check `MALLOC_CAP_DMA` is used

3. **Check color format**:
   - Ensure RGB565 format is correct
   - Verify byte order (MSB/LSB)

4. **Memory corruption**:
   ```bash
   # Enable heap poisoning
   idf.py menuconfig
   → Component config → Heap memory debugging
   → Heap corruption detection: Light/Comprehensive
   ```

---

### Error: Terminal text is garbled

**Symptom**: Characters display incorrectly or are unreadable.

**Solutions**:

1. **Font data missing**:
   - Check `terminus_8x16.c` has actual font data
   - Currently uses placeholder - need to convert real Terminus font

2. **Wrong font dimensions**:
   - Verify `FONT_WIDTH = 8` and `FONT_HEIGHT = 16`
   - Check terminal dimensions (100×30)

3. **Color mapping issue**:
   - Check `ansi_to_rgb565()` function
   - Verify foreground/background colors

---

## Common Questions

### Q: Can I use ESP-IDF 5.0?
**A**: No, RGB LCD panel support requires ESP-IDF 5.1 or later. Please upgrade.

### Q: How much memory does this use?
**A**:
- SRAM: ~38KB (terminal buffer 12KB + bounce buffer 26KB)
- PSRAM: ~320KB (scrollback, SSH, TLS buffers)
- Flash: ~2MB (firmware)

### Q: Can I use a different display?
**A**: Yes, but you'll need to write a driver for it. SPI displays are easier to support.

### Q: Does this work on ESP32 or ESP32-C3?
**A**: No, this is designed for ESP32-S3 specifically. Other chips have different peripherals.

### Q: How do I add the real Terminus font?
**A**: See `components/font/terminus_8x16.c` for instructions on converting BDF to C array.

---

## Getting Help

If you encounter other issues:

1. **Check logs**:
   ```bash
   idf.py monitor
   ```
   Look for ERROR or WARNING messages

2. **Enable verbose logging**:
   ```
   idf.py menuconfig
   → Component config → Log output
   → Default log verbosity: Debug
   ```

3. **Check memory**:
   ```bash
   idf.py size-components
   idf.py size-files
   ```

4. **Clean build**:
   ```bash
   idf.py fullclean
   idf.py build
   ```

5. **Erase flash**:
   ```bash
   idf.py erase-flash
   idf.py flash
   ```

6. **Report issue**:
   - Include ESP-IDF version (`idf.py --version`)
   - Include full error message
   - Include relevant code changes

---

## Debug Checklist

Before reporting issues, verify:

- [ ] ESP-IDF version is 5.1 or later
- [ ] All submodules are updated (`git submodule update --init --recursive`)
- [ ] Environment is sourced (`. $IDF_PATH/export.sh`)
- [ ] Board is connected and detected (`idf.py -p PORT monitor`)
- [ ] Power supply is adequate (5V/2A minimum)
- [ ] WiFi credentials are correct
- [ ] Terminal dimensions match display (100×30 for 800×480)

---

**Last Updated**: 2026-02-23

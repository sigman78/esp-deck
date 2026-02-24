/*
 * Display driver interface for Waveshare ESP32-S3-Touch-LCD-7
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_lcd_panel_ops.h"

// Display dimensions
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

// Bounce buffer configuration (one character row at a time)
#define BOUNCE_BUFFER_HEIGHT  16  // One character row (8x16 font)
#define BOUNCE_BUFFER_SIZE    (DISPLAY_WIDTH * BOUNCE_BUFFER_HEIGHT)

// Color format (RGB565)
typedef uint16_t color_t;

// RGB565 color macros
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_RED       RGB565(255, 0, 0)
#define COLOR_GREEN     RGB565(0, 255, 0)
#define COLOR_BLUE      RGB565(0, 0, 255)
#define COLOR_YELLOW    RGB565(255, 255, 0)
#define COLOR_CYAN      RGB565(0, 255, 255)
#define COLOR_MAGENTA   RGB565(255, 0, 255)

/*
 * Terminal cell — defined here (not in terminal.h) so the display ISR can
 * read cell data without creating a circular dependency.
 */
typedef struct {
    char    ch;         // Character codepoint (Latin-1 for now)
    uint8_t fg_color;   // Foreground ANSI-256 palette index
    uint8_t bg_color;   // Background ANSI-256 palette index
    uint8_t attrs;      // Attribute flags (see ATTR_* below)
} terminal_cell_t;

// Cell attribute flags
#define ATTR_BOLD       (1 << 0)
#define ATTR_UNDERLINE  (1 << 1)
#define ATTR_REVERSE    (1 << 2)
#define ATTR_BLINK      (1 << 3)

/**
 * Initialize display driver
 */
esp_err_t display_init(void);

/**
 * Get LCD panel handle
 */
esp_lcd_panel_handle_t display_get_panel(void);

/**
 * Register the terminal cell buffer so the display ISR can render from it.
 *
 * Call once after terminal_init(). The pointer must remain valid for the
 * lifetime of the display (never free the terminal buffer).
 *
 * @param buf   Pointer to cols*rows terminal_cell_t array (must be in DRAM)
 * @param cols  Number of character columns
 * @param rows  Number of character rows
 */
void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows);

/**
 * Set backlight brightness (0-100%)
 */
esp_err_t display_set_backlight(uint8_t brightness);


#endif // DISPLAY_H

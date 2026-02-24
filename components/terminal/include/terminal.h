/*
 * Terminal emulator interface
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "display.h"

// Terminal cell attributes
typedef struct {
    char ch;              // Character
    uint8_t fg_color;     // Foreground color (ANSI 256)
    uint8_t bg_color;     // Background color (ANSI 256)
    uint8_t attrs;        // Attributes (bold, underline, etc.)
} terminal_cell_t;

// Attribute flags
#define ATTR_BOLD       (1 << 0)
#define ATTR_UNDERLINE  (1 << 1)
#define ATTR_REVERSE    (1 << 2)
#define ATTR_BLINK      (1 << 3)

/**
 * Initialize terminal
 *
 * @param cols Number of columns
 * @param rows Number of rows
 */
esp_err_t terminal_init(int cols, int rows);

/**
 * Write data to terminal (process ANSI sequences)
 *
 * @param data Input data
 * @param len Data length
 */
void terminal_write(const char *data, size_t len);

/**
 * Print string to terminal (convenience function)
 *
 * @param str String to print
 */
void terminal_print(const char *str);

/**
 * Clear terminal screen
 */
void terminal_clear(void);

/**
 * Set cursor position
 *
 * @param x Column (0-based)
 * @param y Row (0-based)
 */
void terminal_set_cursor(int x, int y);

/**
 * Get cursor position
 *
 * @param x Pointer to store column
 * @param y Pointer to store row
 */
void terminal_get_cursor(int *x, int *y);

/**
 * Scroll terminal up by N lines
 *
 * @param lines Number of lines to scroll
 */
void terminal_scroll_up(int lines);

/**
 * Render terminal to display
 */
void terminal_render(void);

/**
 * Handle keyboard input
 *
 * @param key Key character or special code
 */
void terminal_input(char key);

#endif // TERMINAL_H

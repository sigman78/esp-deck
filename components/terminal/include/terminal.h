/*
 * Terminal emulator interface
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "display.h"

// terminal_cell_t and ATTR_* flags are defined in display.h (included above).

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
 * Handle keyboard input
 *
 * @param key Key character or special code
 */
void terminal_input(char key);

#endif // TERMINAL_H

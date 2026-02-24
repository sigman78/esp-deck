/*
 * Terminal emulator implementation
 */

#include "terminal.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "terminal";

// Terminal state
static struct {
    int cols;
    int rows;
    int cursor_x;
    int cursor_y;
    terminal_cell_t *buffer;
    uint8_t current_fg;
    uint8_t current_bg;
    uint8_t current_attrs;
    bool initialized;
} term;

/**
 * Initialize terminal
 */
esp_err_t terminal_init(int cols, int rows)
{
    ESP_LOGI(TAG, "Initializing terminal (%dx%d) with bounce buffer rendering", cols, rows);

    term.cols = cols;
    term.rows = rows;
    term.cursor_x = 0;
    term.cursor_y = 0;
    term.current_fg = 7;  // White
    term.current_bg = 0;  // Black
    term.current_attrs = 0;

    // Allocate terminal buffer in SRAM for fast access
    size_t buffer_size = cols * rows * sizeof(terminal_cell_t);
    term.buffer = malloc(buffer_size);

    if (!term.buffer) {
        ESP_LOGE(TAG, "Failed to allocate terminal buffer");
        return ESP_ERR_NO_MEM;
    }

    // Clear buffer
    for (int i = 0; i < cols * rows; i++) {
        term.buffer[i].ch = ' ';
        term.buffer[i].fg_color = term.current_fg;
        term.buffer[i].bg_color = term.current_bg;
        term.buffer[i].attrs = 0;
    }

    term.initialized = true;

    // Share the cell buffer with the display component so the ISR can render
    // from it directly during on_bounce_empty.
    display_set_text_buffer(term.buffer, cols, rows);

    ESP_LOGI(TAG, "Terminal initialized: %dx%d, buffer %d bytes", cols, rows, buffer_size);

    return ESP_OK;
}

/**
 * Put character at cursor position
 */
static void put_char(char ch)
{
    if (!term.initialized) return;

    if (ch == '\n') {
        term.cursor_x = 0;
        term.cursor_y++;
        if (term.cursor_y >= term.rows) {
            terminal_scroll_up(1);
            term.cursor_y = term.rows - 1;
        }
        return;
    }

    if (ch == '\r') {
        term.cursor_x = 0;
        return;
    }

    if (ch == '\b') {
        if (term.cursor_x > 0) {
            term.cursor_x--;
        }
        return;
    }

    // Regular character
    if (term.cursor_x >= term.cols) {
        term.cursor_x = 0;
        term.cursor_y++;
        if (term.cursor_y >= term.rows) {
            terminal_scroll_up(1);
            term.cursor_y = term.rows - 1;
        }
    }

    int idx = term.cursor_y * term.cols + term.cursor_x;
    term.buffer[idx].ch = ch;
    term.buffer[idx].fg_color = term.current_fg;
    term.buffer[idx].bg_color = term.current_bg;
    term.buffer[idx].attrs = term.current_attrs;

    term.cursor_x++;
}

/**
 * Write data to terminal
 */
void terminal_write(const char *data, size_t len)
{
    if (!term.initialized) return;

    // TODO: Implement proper ANSI escape sequence parsing
    // For now, just handle plain text

    for (size_t i = 0; i < len; i++) {
        put_char(data[i]);
    }
}

/**
 * Print string to terminal
 */
void terminal_print(const char *str)
{
    terminal_write(str, strlen(str));
}

/**
 * Clear terminal screen
 */
void terminal_clear(void)
{
    if (!term.initialized) return;

    for (int i = 0; i < term.cols * term.rows; i++) {
        term.buffer[i].ch = ' ';
        term.buffer[i].fg_color = term.current_fg;
        term.buffer[i].bg_color = term.current_bg;
        term.buffer[i].attrs = 0;
    }

    term.cursor_x = 0;
    term.cursor_y = 0;
}

/**
 * Set cursor position
 */
void terminal_set_cursor(int x, int y)
{
    if (x >= 0 && x < term.cols) {
        term.cursor_x = x;
    }
    if (y >= 0 && y < term.rows) {
        term.cursor_y = y;
    }
}

/**
 * Get cursor position
 */
void terminal_get_cursor(int *x, int *y)
{
    if (x) *x = term.cursor_x;
    if (y) *y = term.cursor_y;
}

/**
 * Scroll terminal up
 */
void terminal_scroll_up(int lines)
{
    if (!term.initialized || lines <= 0) return;

    // Move lines up
    int move_lines = term.rows - lines;
    if (move_lines > 0) {
        memmove(term.buffer,
                term.buffer + (lines * term.cols),
                move_lines * term.cols * sizeof(terminal_cell_t));
    }

    // Clear bottom lines
    int start_idx = move_lines * term.cols;
    for (int i = start_idx; i < term.cols * term.rows; i++) {
        term.buffer[i].ch = ' ';
        term.buffer[i].fg_color = term.current_fg;
        term.buffer[i].bg_color = term.current_bg;
        term.buffer[i].attrs = 0;
    }
}

/**
 * Handle keyboard input
 */
void terminal_input(char key)
{
    // TODO: Send to SSH client
    ESP_LOGI(TAG, "Key input: 0x%02X ('%c')", key, key);
}

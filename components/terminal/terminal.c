/*
 * Terminal emulator implementation
 */

#include "terminal.h"
#include "font.h"
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
    bool *dirty_rows;          // Track which rows need redrawing
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

    // Allocate dirty row tracking (one bool per row)
    term.dirty_rows = calloc(rows, sizeof(bool));
    if (!term.dirty_rows) {
        ESP_LOGE(TAG, "Failed to allocate dirty row buffer");
        free(term.buffer);
        return ESP_ERR_NO_MEM;
    }

    // Clear buffer
    for (int i = 0; i < cols * rows; i++) {
        term.buffer[i].ch = ' ';
        term.buffer[i].fg_color = term.current_fg;
        term.buffer[i].bg_color = term.current_bg;
        term.buffer[i].attrs = 0;
    }

    // Mark all rows as dirty for initial render
    for (int i = 0; i < rows; i++) {
        term.dirty_rows[i] = true;
    }

    term.initialized = true;

    ESP_LOGI(TAG, "Terminal initialized:");
    ESP_LOGI(TAG, "  Buffer: %d bytes (SRAM)", buffer_size);
    ESP_LOGI(TAG, "  Dirty tracking: %d bytes", rows);
    ESP_LOGI(TAG, "  Total memory: %d bytes", buffer_size + rows);

    terminal_render();

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

    // Mark row as dirty
    term.dirty_rows[term.cursor_y] = true;

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

    terminal_render();
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

    // Mark all rows as dirty
    for (int i = 0; i < term.rows; i++) {
        term.dirty_rows[i] = true;
    }

    term.cursor_x = 0;
    term.cursor_y = 0;

    terminal_render();
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

    // Mark all rows as dirty after scroll
    for (int i = 0; i < term.rows; i++) {
        term.dirty_rows[i] = true;
    }
}

/**
 * Render terminal to display using bounce buffer
 * Only renders dirty rows for efficiency
 */
void terminal_render(void)
{
    if (!term.initialized) return;

    color_t *bounce_buf = display_get_bounce_buffer();
    if (!bounce_buf) return;

    int dirty_count = 0;

    // Render each dirty row
    for (int row = 0; row < term.rows; row++) {
        if (!term.dirty_rows[row]) {
            continue;  // Skip clean rows
        }

        dirty_count++;

        // Clear bounce buffer for this row
        memset(bounce_buf, 0, BOUNCE_BUFFER_SIZE * sizeof(color_t));

        // Render all characters in this row to bounce buffer
        for (int col = 0; col < term.cols; col++) {
            int idx = row * term.cols + col;
            terminal_cell_t *cell = &term.buffer[idx];

            int x = col * FONT_WIDTH;
            int y = 0;  // Always 0 in bounce buffer (relative position)

            color_t fg = ansi_to_rgb565(cell->fg_color);
            color_t bg = ansi_to_rgb565(cell->bg_color);

            // Handle reverse attribute
            if (cell->attrs & ATTR_REVERSE) {
                color_t tmp = fg;
                fg = bg;
                bg = tmp;
            }

            font_draw_char(x, y, cell->ch, fg, bg);
        }

        // Flush bounce buffer to this row on LCD
        int lcd_row = row * FONT_HEIGHT;
        display_flush_row(lcd_row, FONT_HEIGHT);

        // Mark row as clean
        term.dirty_rows[row] = false;
    }

    if (dirty_count > 0) {
        ESP_LOGD(TAG, "Rendered %d dirty rows", dirty_count);
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

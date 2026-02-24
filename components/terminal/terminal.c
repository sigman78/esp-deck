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
    // UTF-8 decode state — persists across terminal_write calls
    struct {
        uint16_t acc;      // codepoint accumulator
        uint8_t  left;     // continuation bytes still expected
        uint8_t  discard;  // 1 = consuming a >BMP sequence, emit nothing
    } utf8;
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
    term.current_fg    = 7;  // White
    term.current_bg    = 0;  // Black
    term.current_attrs = 0;
    term.utf8.acc      = 0;
    term.utf8.left     = 0;
    term.utf8.discard  = 0;

    // Allocate terminal buffer in SRAM for fast access
    size_t buffer_size = cols * rows * sizeof(terminal_cell_t);
    term.buffer = malloc(buffer_size);

    if (!term.buffer) {
        ESP_LOGE(TAG, "Failed to allocate terminal buffer");
        return ESP_ERR_NO_MEM;
    }

    // Clear buffer
    for (int i = 0; i < cols * rows; i++) {
        term.buffer[i].cp = 0x0020;
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
 * Place one Unicode codepoint at the cursor position.
 */
static void put_char(uint16_t cp)
{
    if (!term.initialized) return;

    if (cp == '\n') {
        term.cursor_x = 0;
        term.cursor_y++;
        if (term.cursor_y >= term.rows) {
            terminal_scroll_up(1);
            term.cursor_y = term.rows - 1;
        }
        return;
    }

    if (cp == '\r') { term.cursor_x = 0; return; }
    if (cp == '\b') { if (term.cursor_x > 0) term.cursor_x--; return; }

    if (term.cursor_x >= term.cols) {
        term.cursor_x = 0;
        term.cursor_y++;
        if (term.cursor_y >= term.rows) {
            terminal_scroll_up(1);
            term.cursor_y = term.rows - 1;
        }
    }

    int idx = term.cursor_y * term.cols + term.cursor_x;
    term.buffer[idx].cp       = cp;
    term.buffer[idx].fg_color = term.current_fg;
    term.buffer[idx].bg_color = term.current_bg;
    term.buffer[idx].attrs    = term.current_attrs;

    term.cursor_x++;
}

/**
 * Feed one raw byte through the UTF-8 → Unicode decoder.
 *
 * Sequences above U+FFFF (4-byte) are replaced with U+FFFD; the
 * continuation bytes are consumed but not accumulated.  Invalid bytes
 * (unexpected continuations, 0xFE/0xFF) are silently skipped.
 */
static void utf8_feed(uint8_t byte)
{
    if (term.utf8.left > 0) {
        if ((byte & 0xC0u) == 0x80u) {
            // Valid continuation byte
            if (!term.utf8.discard) {
                term.utf8.acc = (uint16_t)((term.utf8.acc << 6) | (byte & 0x3Fu));
            }
            if (--term.utf8.left == 0) {
                if (!term.utf8.discard) put_char(term.utf8.acc);
                term.utf8.acc     = 0;
                term.utf8.discard = 0;
            }
        } else {
            // Sync error: not a continuation — abort sequence, reprocess byte
            term.utf8.left    = 0;
            term.utf8.acc     = 0;
            term.utf8.discard = 0;
            utf8_feed(byte);   // tail-call: at most one level of recursion
        }
        return;
    }

    // Leader byte
    if (byte < 0x80u) {
        put_char((uint16_t)byte);
    } else if ((byte & 0xE0u) == 0xC0u) {       // 2-byte  U+0080..U+07FF
        term.utf8.acc     = byte & 0x1Fu;
        term.utf8.left    = 1;
        term.utf8.discard = 0;
    } else if ((byte & 0xF0u) == 0xE0u) {        // 3-byte  U+0800..U+FFFF
        term.utf8.acc     = byte & 0x0Fu;
        term.utf8.left    = 2;
        term.utf8.discard = 0;
    } else if ((byte & 0xF8u) == 0xF0u) {        // 4-byte  >BMP → U+FFFD
        put_char(0xFFFDu);
        term.utf8.acc     = 0;
        term.utf8.left    = 3;
        term.utf8.discard = 1;
    }
    // else: 0xF8..0xFF or stray continuation — skip
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
        utf8_feed((uint8_t)data[i]);
    }
}

/**
 * Print string to terminal
 */
void terminal_print(const char *str)
{
    terminal_write(str, strlen(str));
}

void terminal_set_color(uint8_t fg, uint8_t bg)
{
    term.current_fg = fg;
    term.current_bg = bg;
}

void terminal_set_attrs(uint8_t attrs)
{
    term.current_attrs = attrs;
}

/**
 * Clear terminal screen
 */
void terminal_clear(void)
{
    if (!term.initialized) return;

    for (int i = 0; i < term.cols * term.rows; i++) {
        term.buffer[i].cp = 0x0020;
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
        term.buffer[i].cp = 0x0020;
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

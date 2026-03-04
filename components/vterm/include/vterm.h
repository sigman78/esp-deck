/*
 * vterm -- VT/ANSI terminal emulator.
 *
 * Parses VT100...xterm byte streams and drives the display cell buffer
 * directly.  Use vterm_write() to feed raw SSH/PTY data.
 * For plain text without escape sequences use the terminal component.
 */

#ifndef VTERM_H
#define VTERM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize the VT emulator.
 *
 * @param cols  Terminal width in character columns.
 * @param rows  Terminal height in character rows.
 */
esp_err_t vterm_init(int cols, int rows);

/**
 * Feed raw bytes from the remote (SSH/PTY) into the VT parser.
 * Bytes are accumulated in an internal buffer (VTERM_BUF_SIZE, default 256).
 * The display is refreshed automatically when a newline is encountered or
 * the buffer is full.  Call vterm_flush() to push any remaining bytes.
 *
 * @param data  Byte stream (may contain VT escape sequences).
 * @param len   Number of bytes.
 */
void vterm_write(const char *data, size_t len);

/**
 * Feed bytes directly into the VT parser without buffering.
 * Any pending buffered bytes are flushed first, then @p data is fed
 * immediately and the display is refreshed on return.
 *
 * @param data  Byte stream (may contain VT escape sequences).
 * @param len   Number of bytes.
 */
void vterm_write_dir(const char *data, size_t len);

/**
 * Flush any bytes held in the internal write buffer to the VT parser
 * and refresh the display.  No-op if the buffer is empty.
 */
void vterm_flush(void);

/**
 * Register a callback for bytes the terminal state machine needs to
 * send back to the remote (cursor-position reports, DA1 responses, ...).
 * Pass NULL to disable.
 *
 * @param cb    Callback function pointer.
 * @param user  Opaque pointer forwarded to the callback.
 */
typedef void (*vterm_response_cb_t)(const char *data, size_t len, void *user);
void vterm_set_response_cb(vterm_response_cb_t cb, void *user);

/**
 * Reset the VT state machine to its initial state.
 */
void vterm_reset(void);

/**
 * Returns true when the remote has enabled application cursor key mode
 * (DECCKM, ESC [ ? 1 h).  Use this to decide whether arrow keys should
 * be sent as ESC O A/B/C/D (application) or ESC [ A/B/C/D (normal).
 */
bool vterm_app_cursor_keys(void);

/**
 * Log a performance summary (flushes, bytes, tsm cycles, draw cycles).
 * No-op when CONFIG_VTERM_BENCH is disabled.
 */
void vterm_bench_report(void);

/**
 * Clear all performance accumulators.
 * No-op when CONFIG_VTERM_BENCH is disabled.
 */
void vterm_bench_reset(void);

#endif /* VTERM_H */

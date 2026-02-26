/*
 * vterm — VT/ANSI terminal emulator (libtsm backend).
 *
 * Parses VT100…xterm byte streams and drives the display cell buffer
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
 * Escape sequences are interpreted; the display cell buffer is
 * refreshed on return.
 *
 * @param data  Byte stream (may contain VT escape sequences).
 * @param len   Number of bytes.
 */
void vterm_write(const char *data, size_t len);

/**
 * Register a callback for bytes the terminal state machine needs to
 * send back to the remote (cursor-position reports, DA1 responses, …).
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

#endif /* VTERM_H */

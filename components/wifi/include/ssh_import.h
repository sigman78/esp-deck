/*
 * ssh_import — add SSH connection profiles (incl. private keys) over WiFi.
 *
 * Brings up a temporary WPA2 SoftAP and a tiny HTTP server that serves a
 * one-page form. The user joins the AP from a phone/laptop, opens the page,
 * fills in host/user/auth (password OR a pasted/uploaded private key) and
 * submits. The device validates it, stores any key blob, and appends the
 * profile to profiles.ini — no INI reflash.
 *
 * Security model (why plain HTTP is acceptable here):
 *   - The SoftAP is WPA2-protected; the passphrase IS the proof-of-possession
 *     shown on the device screen. You must read the on-glass code to join, so
 *     the air link is encrypted end-to-end and the pasted private key is not
 *     sniffable. The POST additionally carries the PoP as an app-layer gate.
 *   - The AP is single-purpose and torn down the moment the modal closes,
 *     reclaiming the internal RAM the httpd + AP consume.
 *
 * Like wifi_provision, SoftAP is used deliberately: no BLE, so the HID
 * keyboard's NimBLE stack is untouched.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* UI-facing state (poll from the shell). */
enum {
    SSH_IMPORT_ST_IDLE = 0,
    SSH_IMPORT_ST_ACTIVE,   /* SoftAP + httpd up, waiting for a submission */
    SSH_IMPORT_ST_ERROR,    /* failed to start (no RAM / driver error)     */
};

/** Begin the import AP + HTTP server. Parks wifi_manager for the duration. */
esp_err_t ssh_import_start(void);

/** Stop + free the HTTP server and AP, restore STA mode. */
void ssh_import_stop(void);

/** Current SSH_IMPORT_ST_* value. */
int ssh_import_state(void);

/** SoftAP SSID the client should join (e.g. "DECK-SETUP-1A2B"). */
const char *ssh_import_service_name(void);

/** WPA2 passphrase / proof-of-possession shown on the device. */
const char *ssh_import_pop(void);

/** URL to open once joined (e.g. "http://192.168.4.1"). */
const char *ssh_import_url(void);

/** Number of profiles imported since ssh_import_start(). */
int ssh_import_count(void);

/** Name of the most recently imported profile ("" until the first). */
const char *ssh_import_last(void);

/** Last rejection reason ("" if none / cleared by a later success). */
const char *ssh_import_err(void);

/** WiFi-join QR: side length in modules, or 0 if none. */
int ssh_import_qr_size(void);

/** True if the QR module at (x,y) is dark. Out-of-range returns false. */
bool ssh_import_qr_module(int x, int y);

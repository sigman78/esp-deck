/*
 * ssh_import — host/sim stub. No SoftAP/httpd; just enough state so the
 * shell's import screen can be exercised in the simulator.
 */

#include "ssh_import.h"
#include <string.h>

static int s_state = SSH_IMPORT_ST_IDLE;

esp_err_t ssh_import_start(void) { s_state = SSH_IMPORT_ST_ACTIVE; return ESP_OK; }
void      ssh_import_stop(void)  { s_state = SSH_IMPORT_ST_IDLE; }
int       ssh_import_state(void) { return s_state; }

const char *ssh_import_service_name(void) { return "DECK-SETUP-SIM"; }
const char *ssh_import_pop(void)          { return "1A2B3C4D"; }
const char *ssh_import_url(void)          { return "http://192.168.4.1"; }
const char *ssh_import_last(void)         { return ""; }
const char *ssh_import_err(void)          { return ""; }
int         ssh_import_count(void)        { return 0; }

int  ssh_import_qr_size(void)           { return 0; }
bool ssh_import_qr_module(int x, int y) { (void)x; (void)y; return false; }

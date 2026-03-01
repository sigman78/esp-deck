/*
 * sim/main.c — PC simulator: SDL keyboard → SSH → vterm → SDL display
 *
 * Usage: cyberdeck_sim.exe [host [port [user [password]]]]
 *
 * Defaults: localhost 22 user ""
 * Override at compile time with -DCONFIG_SSH_DEFAULT_HOST etc.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "display.h"
#include "font.h"
#include "vterm.h"
#include "ssh_client.h"
#include "wifi_manager.h"
#include "storage.h"

#define SIM_COLS 100
#define SIM_ROWS  30

#ifndef CONFIG_SSH_DEFAULT_HOST
#define CONFIG_SSH_DEFAULT_HOST "localhost"
#endif
#ifndef CONFIG_SSH_DEFAULT_PORT
#define CONFIG_SSH_DEFAULT_PORT 22
#endif
#ifndef CONFIG_SSH_DEFAULT_USER
#define CONFIG_SSH_DEFAULT_USER "user"
#endif
#ifndef CONFIG_SSH_DEFAULT_PASS
#define CONFIG_SSH_DEFAULT_PASS ""
#endif

/* ---------- key translation ---------- */

/*
 * Translate an SDL keydown event to a terminal escape sequence.
 * Returns NULL for printable characters (handled by SDL_TEXTINPUT).
 * Returns a static/literal string for control/special keys.
 */
static const char *translate_key(SDL_Keycode sym, SDL_Keymod mod)
{
    /* Ctrl+letter → \x01..\x1a */
    if ((mod & KMOD_CTRL) && sym >= SDLK_a && sym <= SDLK_z) {
        static char ctrl_buf[2];
        ctrl_buf[0] = (char)(sym - SDLK_a + 1);
        ctrl_buf[1] = '\0';
        return ctrl_buf;
    }

    switch (sym) {
    case SDLK_RETURN:    return "\r";
    case SDLK_KP_ENTER:  return "\r";
    case SDLK_BACKSPACE: return "\x7f";
    case SDLK_TAB:       return "\t";
    case SDLK_ESCAPE:    return "\x1b";

    case SDLK_UP:    return vterm_app_cursor_keys() ? "\x1bOA" : "\x1b[A";
    case SDLK_DOWN:  return vterm_app_cursor_keys() ? "\x1bOB" : "\x1b[B";
    case SDLK_LEFT:  return vterm_app_cursor_keys() ? "\x1bOD" : "\x1b[D";
    case SDLK_RIGHT: return vterm_app_cursor_keys() ? "\x1bOC" : "\x1b[C";

    case SDLK_HOME:      return "\x1b[H";
    case SDLK_END:       return "\x1b[F";
    case SDLK_PAGEUP:    return "\x1b[5~";
    case SDLK_PAGEDOWN:  return "\x1b[6~";
    case SDLK_DELETE:    return "\x1b[3~";
    case SDLK_INSERT:    return "\x1b[2~";

    case SDLK_F1:        return "\x1bOP";
    case SDLK_F2:        return "\x1bOQ";
    case SDLK_F3:        return "\x1bOR";
    case SDLK_F4:        return "\x1bOS";
    case SDLK_F5:        return "\x1b[15~";
    case SDLK_F6:        return "\x1b[17~";
    case SDLK_F7:        return "\x1b[18~";
    case SDLK_F8:        return "\x1b[19~";
    case SDLK_F9:        return "\x1b[20~";
    case SDLK_F10:       return "\x1b[21~";
    case SDLK_F11:       return "\x1b[23~";
    case SDLK_F12:       return "\x1b[24~";

    default:             return NULL;
    }
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    /* Load profiles; priority: argv > stored "default" > Kconfig macros */
    if (storage_init() != ESP_OK)
        fprintf(stderr, "storage_init() failed — using defaults\n");

    static conn_profile_t s_profiles[8];
    int profile_count = 0;
    storage_load_profiles(s_profiles, &profile_count, 8);
    const conn_profile_t *def =
        storage_find_profile(s_profiles, profile_count, "default");

    const char *host     = (argc > 1) ? argv[1]
                         : (def)      ? def->host
                         :               CONFIG_SSH_DEFAULT_HOST;
    int         port_i   = (argc > 2) ? atoi(argv[2])
                         : (def)      ? (int)def->port
                         :               CONFIG_SSH_DEFAULT_PORT;
    const char *user     = (argc > 3) ? argv[3]
                         : (def)      ? def->user
                         :               CONFIG_SSH_DEFAULT_USER;
    const char *password = (argc > 4) ? argv[4]
                         : (def && def->auth == STORAGE_AUTH_PASSWORD)
                                       ? def->password
                         :               CONFIG_SSH_DEFAULT_PASS;
    uint16_t    port     = (uint16_t)port_i;

    /* ── SDL + display stack ── */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    font_init();
    display_init();
    vterm_init(SIM_COLS, SIM_ROWS);

    /* ── announce connection attempt ── */
    char banner[128];
    snprintf(banner, sizeof(banner),
             "Connecting to %s:%d as %s ...\r\n", host, port, user);
    vterm_write(banner, strlen(banner));
    display_render_frame();

    /* ── wifi stub ── */
    wifi_manager_init();
    wifi_manager_connect();

    /* ── SSH ── */
    ssh_config_t cfg = {
        .host        = host,
        .port        = port,
        .username    = user,
        .password    = password,
        .private_key = NULL,
    };

    if (ssh_client_init() != ESP_OK) {
        const char *err = "ssh_client_init() failed\r\n";
        vterm_write(err, strlen(err));
        display_render_frame();
    } else if (ssh_client_connect(&cfg) != ESP_OK) {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg),
                 "SSH connect to %s:%d failed\r\n", host, port);
        vterm_write(errmsg, strlen(errmsg));
        display_render_frame();
        /* Fall through to event loop so user can read the error */
    }

    /* ── event loop ── */
    bool running = true;
    while (running) {
        bool gotInput = false;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_KEYDOWN: {
                /* Alt+Enter — toggle 1×/2× window scale, do not forward to SSH */
                if ((ev.key.keysym.mod & KMOD_ALT) &&
                    (ev.key.keysym.sym == SDLK_RETURN ||
                     ev.key.keysym.sym == SDLK_KP_ENTER)) {
                    display_toggle_scale();
                    break;
                }
                /* SDL_TEXTINPUT handles printable chars; only process special/ctrl here */
                const char *seq = translate_key(ev.key.keysym.sym,
                                                ev.key.keysym.mod);
                if (seq && ssh_client_is_connected()) {
                    ssh_client_send((const uint8_t *)seq, strlen(seq));
                }
                gotInput = true;
                break;
            }

            case SDL_TEXTINPUT:
                if (ssh_client_is_connected()) {
                    ssh_client_send((const uint8_t *)ev.text.text,
                                    strlen(ev.text.text));
                }
                gotInput = true;
                break;

            default:
                break;
            }
        }

        if (!ssh_client_is_connected()) {
            /* Short delay so the user can read any final output */
            SDL_Delay(2000);
            running = false;
        }

        display_render_frame();
        SDL_Delay(gotInput ? 1 : 16);
    }

    ssh_client_disconnect();
    SDL_Quit();
    return 0;
}

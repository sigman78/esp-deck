/*
 * sim/main.c — host composition root.
 *
 * SDL event pumping and host-specific key translation stay here. Shared boot,
 * Wi-Fi, SSH, and session orchestration live in cyberdeck_app.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyberdeck_app.h"
#include "display.h"
#include "font.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"
#include "wifi_manager.h"

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

static void status_line(const char *prefix, int color, const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "\x1b[38;5;%dm%s\x1b[0m %s\r\n", color, prefix, msg);
    vterm_write(buf, strlen(buf));
}

static void app_status_info(const char *msg, void *user)
{
    (void)user;
    status_line("[*]", 14, msg);
}

static void app_status_ok(const char *msg, void *user)
{
    (void)user;
    status_line("[+]", 10, msg);
}

static void app_status_fail(const char *msg, void *user)
{
    (void)user;
    status_line("[!]", 9, msg);
}

/*
 * Translate an SDL keydown event to a terminal escape sequence.
 * Returns NULL for printable characters (handled by SDL_TEXTINPUT).
 */
static const char *translate_key(SDL_Keycode sym, SDL_Keymod mod)
{
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

int main(int argc, char *argv[])
{
    static cyberdeck_app_t app;

    const bool explicit_connection = argc > 1;
    const char *host     = (argc > 1) ? argv[1] : CONFIG_SSH_DEFAULT_HOST;
    int         port_i   = (argc > 2) ? atoi(argv[2]) : CONFIG_SSH_DEFAULT_PORT;
    const char *user     = (argc > 3) ? argv[3] : CONFIG_SSH_DEFAULT_USER;
    const char *password = (argc > 4) ? argv[4] : CONFIG_SSH_DEFAULT_PASS;
    uint16_t    port     = (uint16_t)port_i;

    if (storage_init() != ESP_OK) {
        fprintf(stderr, "storage_init() failed — using defaults\n");
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    font_init();
    display_init();
    vterm_init(SIM_COLS, SIM_ROWS);
    vterm_write("\x1b[2J\x1b[H", 7);
    display_render_frame();

    if (wifi_manager_init() != ESP_OK) {
        fprintf(stderr, "wifi_manager_init() failed\n");
    }
    if (ssh_client_init() != ESP_OK) {
        fprintf(stderr, "ssh_client_init() failed\n");
    }

    cyberdeck_app_config_t app_cfg = {
        .boot_delay_ms = 250,
        .wifi_timeout_ms = 30000,
        .ssh_retry_delay_ms = 5000,
        .auto_reconnect = true,
        .prefer_explicit_connection = explicit_connection,
        .default_host = host,
        .default_port = port,
        .default_user = user,
        .default_password = password,
        .status_info = app_status_info,
        .status_ok = app_status_ok,
        .status_fail = app_status_fail,
        .request_pairing = NULL,
        .user = NULL,
    };

    if (cyberdeck_app_init(&app, &app_cfg, SDL_GetTicks64()) != ESP_OK) {
        fprintf(stderr, "cyberdeck_app_init() failed\n");
        SDL_Quit();
        return 1;
    }

    bool running = true;
    uint64_t shutdown_at_ms = 0;

    while (running) {
        bool got_input = false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_KEYDOWN: {
                if ((ev.key.keysym.mod & KMOD_ALT) &&
                    (ev.key.keysym.sym == SDLK_RETURN ||
                     ev.key.keysym.sym == SDLK_KP_ENTER)) {
                    display_toggle_scale();
                    break;
                }

                const char *seq = translate_key(ev.key.keysym.sym, ev.key.keysym.mod);
                if (seq) {
                    cyberdeck_app_send_bytes(&app, (const uint8_t *)seq, strlen(seq));
                }
                got_input = true;
                break;
            }

            case SDL_TEXTINPUT:
                cyberdeck_app_send_bytes(&app,
                                         (const uint8_t *)ev.text.text,
                                         strlen(ev.text.text));
                got_input = true;
                break;

            default:
                break;
            }
        }

        cyberdeck_app_tick(&app, SDL_GetTicks64());
        display_render_frame();

        if (!cyberdeck_app_is_running(&app)) {
            if (shutdown_at_ms == 0) {
                shutdown_at_ms = SDL_GetTicks64() + 2000;
            } else if (SDL_GetTicks64() >= shutdown_at_ms) {
                running = false;
            }
        }

        SDL_Delay(got_input ? 1 : 16);
    }

    ssh_client_disconnect();
    SDL_Quit();
    return 0;
}

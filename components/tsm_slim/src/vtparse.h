/*
 * vtparse — VT byte-stream parser
 *
 * Pure C state machine: consumes raw bytes, emits typed events.
 * No screen state, no allocations, no ESP-IDF dependency.
 *
 * Based on the Paul Williams DEC/ANSI state diagram:
 *   https://vt100.net/emu/dec_ansi_parser
 *
 * UTF-8 note: This parser operates in UTF-8 mode.  Multi-byte UTF-8
 * sequences are decoded only in the GROUND state (printable output).
 * All VT control sequences use 7-bit ASCII bytes exclusively.
 * 8-bit C1 shortcuts (0x80–0x9F) are not used; only 0x9C (ST) is
 * recognised in string states for termination.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum number of CSI/DCS parameters (sub-params via ':' share slots). */
#define VTP_PARAMS_MAX  16

/* Maximum bytes collected into the OSC string buffer (truncated beyond). */
#define VTP_OSC_MAX     256

/* ── Event types ──────────────────────────────────────────────────────────── */

typedef enum {
    VT_EV_PRINT,  /* printable character — .cp holds the codepoint           */
    VT_EV_C0,     /* C0 control byte    — .byte holds the raw byte           */
    VT_EV_ESC,    /* ESC sequence       — .intermediate, .final              */
    VT_EV_CSI,    /* CSI sequence       — .prefix, .params[], .nparams,
                   *                      .intermediate, .final              */
    VT_EV_OSC,    /* OSC string         — .osc, .osc_len                     */
    VT_EV_DCS,    /* DCS (stub)         — .final set on hook; passthrough
                   *                      data not forwarded in Phase 1      */
} vt_ev_type_t;

typedef struct {
    vt_ev_type_t   type;

    /* VT_EV_PRINT */
    uint32_t       cp;              /* Unicode codepoint (≤ U+FFFF)          */

    /* VT_EV_C0 */
    uint8_t        byte;            /* raw control byte                      */

    /* VT_EV_ESC, VT_EV_CSI */
    uint8_t        intermediate;    /* first intermediate byte (0x20–0x2F),
                                     * or 0 if none                          */
    uint8_t        final;           /* final byte                            */

    /* VT_EV_CSI only */
    uint8_t        prefix;          /* private-use marker: '<' '=' '>' '?'
                                     * or 0 if absent                        */
    int32_t        params[VTP_PARAMS_MAX]; /* -1 = omitted / use default     */
    uint8_t        nparams;         /* number of param slots populated       */

    /* VT_EV_OSC */
    const uint8_t *osc;             /* points into parser's internal buffer; */
    uint16_t       osc_len;         /* valid only during the callback        */
} vt_event_t;

/* Dispatch callback: called once per completed sequence. */
typedef void (*vt_dispatch_fn)(const vt_event_t *ev, void *user);

/* ── Parser state ─────────────────────────────────────────────────────────── */

typedef enum {
    VTP_ST_GROUND = 0,
    VTP_ST_ESC,
    VTP_ST_ESC_INT,
    VTP_ST_CSI_ENTRY,
    VTP_ST_CSI_PARAM,
    VTP_ST_CSI_INT,
    VTP_ST_CSI_IGNORE,
    VTP_ST_DCS_ENTRY,
    VTP_ST_DCS_PARAM,
    VTP_ST_DCS_INT,
    VTP_ST_DCS_PASS,
    VTP_ST_DCS_IGNORE,
    VTP_ST_OSC_STRING,
    VTP_ST_SOS_PM_APC,
} vtp_state_t;

/* prev_string: context saved when ESC interrupts a string-collecting state */
#define VTP_STR_NONE    0
#define VTP_STR_OSC     1
#define VTP_STR_DCS     2
#define VTP_STR_SOS     3   /* SOS / PM / APC — ESC \ terminates silently */

/* ── Parser struct ────────────────────────────────────────────────────────── */

typedef struct {
    vtp_state_t    state;
    uint8_t        prev_string;     /* VTP_STR_* saved context for ESC \     */

    /* UTF-8 decoder */
    uint32_t       utf8_cp;         /* accumulator                           */
    uint8_t        utf8_remain;     /* continuation bytes still expected     */

    /* Sequence parameters */
    uint8_t        intermediate;    /* first collected intermediate byte     */
    uint8_t        prefix;          /* first private-use parameter marker    */
    int32_t        params[VTP_PARAMS_MAX];
    uint8_t        nparams;         /* slots used so far                     */
    uint8_t        param_cur;       /* index of the currently open slot      */

    /* OSC string buffer (+1 for a null terminator written on emit) */
    uint8_t        osc_buf[VTP_OSC_MAX + 1];
    uint16_t       osc_len;

    /* Dispatch */
    vt_dispatch_fn dispatch;
    void          *user;
} vtparse_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Initialise (or re-initialise) a parser.  The struct need not be zeroed
 * beforehand; vtparse_init does a full memset internally. */
void vtparse_init(vtparse_t *p, vt_dispatch_fn dispatch, void *user);

/* Feed raw bytes.  May call dispatch zero or more times per byte. */
void vtparse_feed(vtparse_t *p, const uint8_t *data, size_t len);

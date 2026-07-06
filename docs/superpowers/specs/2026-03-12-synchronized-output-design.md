# Synchronized Output (DEC mode ?2026) — Design Spec

**Date:** 2026-03-12
**Status:** Approved

---

## Overview

Implement DEC private mode 2026 (Synchronized Output / BSU-ESU) as described in `SYNCOUT.md`.

When active, the display buffer is frozen at its last rendered state while the VT parser continues processing incoming bytes. On ESU (end synchronized update), all accumulated dirty rows are flushed to the display buffer in one atomic step, eliminating tearing.

---

## Sequences

| Sequence | Meaning |
|----------|---------|
| `CSI ? 2026 h` | BSU — begin synchronized update (freeze rendering) |
| `CSI ? 2026 l` | ESU — end synchronized update (flush + unfreeze) |
| `CSI ? 2026 $ p` | DECRQM — query mode state; response via `send_response()` |

DECRQM response format: `CSI ? 2026 ; N $ y`
- `N = 1` — mode currently set (sync active)
- `N = 2` — mode currently reset (sync inactive, but supported)

---

## Affected Files

```
components/tsm/src/termstate.h   — add sync_update flag to tsm_mode_t
components/tsm/src/termstate.c   — handle mode 2026 h/l and DECRQM $ p
components/tsm/include/tsm.h     — add tsm_sync_update() public API
tests/tsm/test_termstate.c       — add 6 new tests
components/vterm/vterm.c         — gate refresh_display() on sync state
```

---

## Layer 1 — termstate.h

Add one field to `tsm_mode_t`:

```c
bool sync_update;  /* ?2026 — synchronized output: freeze renderer */
```

No other struct changes needed.

**Initial state:** `tsm_new()` uses `heap_caps_calloc` for the `tsm_t` struct,
which zero-initialises all fields including `mode.sync_update`. No explicit
init is required. `test_sync_initial_state` verifies this.

**On reset:** `do_hard_reset()` calls `memset(&t->mode, 0, sizeof(t->mode))`,
which clears `sync_update`. `test_sync_reset_clears_mode` verifies this
explicitly.

---

## Layer 2 — termstate.c

### Mode set/reset (h / l)

In the `prefix == '?'` block of `do_csi()`, add to the switch:

```c
case 2026: t->mode.sync_update = set; break;
```

### DECRQM query ($ p)

Insert the following block **after** `mode_n` is computed but **before** the
`if (!set && !reset) return;` early-return guard. Placement is critical: with
`final == 'p'`, `set` and `reset` are both false, so the guard would discard
the sequence before the handler is reached if placed later.

Only mode 2026 is answered; all other DECRQM queries are left unanswered
(same behaviour as today, where DECRQM is not implemented at all).

```c
/* DECRQM — query mode state: CSI ? Pn $ p */
if (intermediate == '$' && final == 'p' && mode_n == 2026) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "\x1b[?2026;%d$y",
                     t->mode.sync_update ? 1 : 2);
    if (n > 0 && n < (int)sizeof(buf))
        send_response(t, buf, (size_t)n);
    return;
}
```

This reuses the existing `send_response()` helper and the `mode_n` local
already computed just above.

---

## Layer 3 — tsm.h (public API)

Add:

```c
/* Returns true when synchronized output mode (?2026) is active.
 * While true, vterm must not flush dirty rows to the display buffer. */
bool tsm_sync_update(const tsm_t *tsm);
```

Implementation in termstate.c (one-liner near other getters):

```c
bool tsm_sync_update(const tsm_t *t) { return t->mode.sync_update; }
```

---

## Layer 4 — vterm.c

### Two guard sites

`vterm_write_dir()` contains two potential `refresh_display()` calls:

1. `flush_buf()` at line 175 — clears the write buffer, then calls
   `refresh_display()` internally.
2. The direct `refresh_display()` at the end of `vterm_write_dir()` after the
   inline `tsm_feed()`.

Both must be guarded. Patch `flush_buf()`'s internal call, and `vterm_write_dir()`'s
direct call, to:

```c
if (!tsm_sync_update(s_tsm))
    refresh_display();
```

### Why this works for ESU mid-feed

`tsm_feed()` processes sequences synchronously. When ESU (`CSI ? 2026 l`) is
parsed, `mode.sync_update` becomes false before `tsm_feed()` returns. The
post-feed guard then evaluates false → `refresh_display()` is called, copying
all dirty rows that accumulated during the sync window in one atomic pass.

If a single feed buffer contains content **before** BSU, that content has
already dirtied rows. Those rows stay in tsm's dirty set until ESU clears them.
This is correct: the display should not update mid-frame regardless of when
in the buffer BSU arrived.

### vterm_reset()

`vterm_reset()` calls `tsm_reset()` (which zeroes `mode.sync_update` via
`memset`) and then `refresh_display()`. Because `sync_update` is false at the
point `refresh_display()` is called, no guard is needed there — it works
correctly as-is.

### Dirty tracking

tsm's dirty row array continues accumulating during sync — no changes needed.
The full accumulated dirty set is consumed by the single `refresh_display()`
call triggered on ESU.

---

## Layer 5 — Tests

All tests go in `tests/tsm/test_termstate.c`, using existing helpers
(`feed()`, `capture_response()`, `clear_response()`).

| Test name | Stimulus | Assertion |
|-----------|----------|-----------|
| `test_sync_initial_state` | (fresh terminal, no input) | `tsm_sync_update()` == false |
| `test_sync_mode_bsu` | `\x1b[?2026h` | `tsm_sync_update()` == true |
| `test_sync_mode_esu` | `\x1b[?2026l` | `tsm_sync_update()` == false |
| `test_sync_bsu_esu_roundtrip` | `h` then `l` | false after ESU |
| `test_sync_decrqm_inactive` | `\x1b[?2026$p` (mode off) | response == `\x1b[?2026;2$y` |
| `test_sync_decrqm_active` | set mode, then `\x1b[?2026$p` | response == `\x1b[?2026;1$y` |
| `test_sync_reset_clears_mode` | set mode, `tsm_reset()` | `tsm_sync_update()` == false |

---

## Out of Scope

- **Timeout:** Not implemented. Graceful degradation (no-sync behavior) if ESU
  is never received. Can be added as a follow-up in vterm with a timestamp
  comparison in `vterm_write_dir()`.
- **Mouse, bracketed paste:** Unrelated stubs already present.
- **Scrollback:** Not affected.

---

## Acceptance Criteria

1. All 7 new tests pass alongside existing 125 tests (no regressions).
2. `CSI ? 2026 $ p` with mode off returns `\x1b[?2026;2$y`.
3. `CSI ? 2026 $ p` with mode on returns `\x1b[?2026;1$y`.
4. `tsm_reset()` leaves `tsm_sync_update()` returning false.
5. `vterm.c` compiles cleanly with the new `tsm_sync_update()` call.

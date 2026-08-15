#!/usr/bin/env python3
"""
gen_terminus.py -- generate components/font/terminusWxH[b].c in the
compressed glyph format ("v1", see components/font/terminus_font.h).

Per face (regular / bold) of each size the pool holds deduplicated,
row-cropped glyph records (header + PackBits row stream); ranges[] carries
one uint16_t byte-offset per covered codepoint into that pool. Regular faces
merge adjacent ranges whose gap is <= 3 codepoints (gap cps point at the '?'
record). Bold faces store only the codepoints whose real bold bitmap differs
from "smear the regular glyph one pixel"; identical ones use the sentinel
0xFFFF instead of a stored record. 10x20/12x24 (uint16_t rows) additionally
share a <=255-entry row palette between the regular and bold pools of that
size, with index 0xFF as an inline 2-byte escape for the long tail.

Two independent glyph sources feed the same pipeline:

  * BDF   -- the original input format (kept working; no BDFs currently
             live in the repo, so this path is untested against real data).
  * "convert" -- reads a pair of CURRENT-format (pre-v1, uncompressed)
             terminusWxH[b].c files, via regex on the
             `font_(range|bold)_XXXX_YYYY[]` arrays. This is how the repo's
             six tables were regenerated: from snapshots of the previous
             generator's output.

Usage:
    python gen_terminus.py bdf WxH normal.bdf bold.bdf out.c out_bold.c
    python gen_terminus.py convert WxH ref.c ref_bold.c out.c out_bold.c [--verify]
    python gen_terminus.py verify WxH ref.c ref_bold.c out.c out_bold.c

`convert`/`bdf` write the new-format files and print byte-size totals.
`verify` (or `convert ... --verify`) re-parses the EMITTED .c files (not the
in-memory encoder state) with a Python reimplementation of the decoder,
decodes every codepoint the reference covers plus the bold subset under the
three-way bold semantics, and byte-compares against the reference. Any
mismatch exits 1.
"""

import re
import sys
from collections import Counter
from pathlib import Path


# ---------------------------------------------------------------------------
# BDF parsing (unchanged from the pre-v1 generator)
# ---------------------------------------------------------------------------

def parse_bdf(path):
    """Return (width, height, {codepoint: [row_int; height]})."""
    fbb = None                      # (w, h, xoff, yoff)
    glyphs = {}
    cp = None
    bbx = None
    rows = None
    in_bitmap = False

    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("FONTBOUNDINGBOX"):
            _, w, h, xo, yo = line.split()
            fbb = (int(w), int(h), int(xo), int(yo))
        elif line.startswith("STARTCHAR"):
            cp, bbx, rows, in_bitmap = None, None, [], False
        elif line.startswith("ENCODING"):
            cp = int(line.split()[1])
        elif line.startswith("BBX"):
            _, bw, bh, bx, by = line.split()
            bbx = (int(bw), int(bh), int(bx), int(by))
        elif line.startswith("BITMAP"):
            in_bitmap = True
        elif line.startswith("ENDCHAR"):
            if cp is not None and 0 <= cp <= 0xFFFF:
                glyphs[cp] = compose_cell(fbb, bbx, rows, cp)
            in_bitmap = False
        elif in_bitmap:
            rows.append(line.strip())

    if fbb is None:
        sys.exit("no FONTBOUNDINGBOX in BDF")
    return fbb[0], fbb[1], glyphs


def compose_cell(fbb, bbx, hexrows, cp):
    """Place a BBX-cropped bitmap into the full font cell, right-aligned."""
    fw, fh, fxo, fyo = fbb
    bw, bh, bx, by = bbx
    start_row = (fh + fyo) - (bh + by)
    x_off = bx - fxo
    cell = [0] * fh
    mask = (1 << fw) - 1
    for i, hx in enumerate(hexrows):
        raw = int(hx, 16)
        bits = raw >> (len(hx) * 4 - bw)          # right-align bw bits
        shift = fw - x_off - bw
        val = bits << shift if shift >= 0 else bits >> -shift
        r = start_row + i
        if not 0 <= r < fh:
            if bits:
                print(f"warn: U+{cp:04X} row {r} clipped (set bits outside cell)")
            continue
        if val & ~mask:
            print(f"warn: U+{cp:04X} bits clipped horizontally")
        cell[r] = val & mask
    return cell


# Bold coverage — subset "A": the scripts a terminal actually bolds.
# Codepoints outside this fall back to the normal glyph, unsmeared.
BOLD_SUBSET = [
    (0x0020, 0x007E),   # ASCII
    (0x00A0, 0x017F),   # Latin-1 supplement + Extended-A
    (0x0400, 0x045F),   # Cyrillic (basic Russian + extensions)
]

# gap*2 < 8 (one FontRange directory entry) <=> gap <= 3
MAX_MERGE_GAP = 3

EXPECTED_BOLD_EXCEPTIONS = {8: 150, 10: 67, 12: 56}


def contiguous_ranges(cps):
    cps = sorted(cps)
    out = []
    run = [cps[0], cps[0]]
    for c in cps[1:]:
        if c == run[1] + 1:
            run[1] = c
        else:
            out.append(tuple(run))
            run = [c, c]
    out.append(tuple(run))
    return out


def merge_ranges(ranges, max_gap=MAX_MERGE_GAP):
    ranges = sorted(ranges)
    merged = [list(ranges[0])]
    for lo, hi in ranges[1:]:
        gap = lo - merged[-1][1] - 1
        if gap <= max_gap:
            merged[-1][1] = hi
        else:
            merged.append([lo, hi])
    return [tuple(r) for r in merged]


def bold_ranges_for(font_cps):
    """BOLD_SUBSET clipped to the codepoints the regular font covers."""
    out = []
    for lo, hi in BOLD_SUBSET:
        covered = [c for c in range(lo, hi + 1) if c in font_cps]
        if covered:
            out.extend(contiguous_ranges(covered))
    return out


# ---------------------------------------------------------------------------
# Old-format (pre-v1) .c parsing -- the "convert" source and the --verify
# reference. Reused for both: parse_old_regular / parse_old_bold.
# ---------------------------------------------------------------------------

def _parse_old_arrays(path, prefix):
    text = Path(path).read_text(encoding="utf-8")
    pattern = re.compile(prefix + r"_([0-9A-F]{4})_([0-9A-F]{4})\[\]\s*=\s*\{(.*?)\};", re.S)
    arrays = {}
    for m in pattern.finditer(text):
        lo, hi = int(m.group(1), 16), int(m.group(2), 16)
        vals = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", m.group(3))]
        arrays[(lo, hi)] = vals
    return arrays


def parse_old_regular(path, height):
    """Return ({cp: [row_int; height]}, [contiguous (lo, hi) ranges])."""
    arrays = _parse_old_arrays(path, "font_range")
    glyphs = {}
    for (lo, hi), vals in arrays.items():
        for i, cp in enumerate(range(lo, hi + 1)):
            glyphs[cp] = vals[i * height:(i + 1) * height]
    ranges = sorted(arrays.keys())
    return glyphs, ranges


def parse_old_bold(path, height):
    """Return sparse {cp: [row_int; height]} -- only cps present in the
    old differ-only bold table (absence there means bold == normal)."""
    arrays = _parse_old_arrays(path, "font_bold")
    glyphs = {}
    for (lo, hi), vals in arrays.items():
        for i, cp in enumerate(range(lo, hi + 1)):
            glyphs[cp] = vals[i * height:(i + 1) * height]
    return glyphs


# ---------------------------------------------------------------------------
# Core v1 encoder
# ---------------------------------------------------------------------------

def crop(rows, height):
    """(start, len, cropped_row_tuple). All-blank: start=0.
    h==16 forces len=1 with a single encoded 0x00 row (the 4-bit len field
    can't express 0); h>16 uses len=0 (no body) for a true blank record."""
    idxs = [i for i, v in enumerate(rows) if v != 0]
    if not idxs:
        return (0, 1, (0,)) if height == 16 else (0, 0, ())
    s, e = idxs[0], idxs[-1]
    return (s, e - s + 1, tuple(rows[s:e + 1]))


def packbits_encode(rows, scost, emit_symbol):
    """Optimal literal/repeat PackBits partition via O(n^2) DP.
    Returns the encoded body bytes (bytearray)."""
    n = len(rows)
    if n == 0:
        return bytearray()
    INF = float("inf")
    dp = [INF] * (n + 1)
    dp[0] = 0
    choice = [None] * (n + 1)
    for i in range(n):
        if dp[i] == INF:
            continue
        base = dp[i] + 1
        # literal runs: length 1..128
        run_cost = 0
        for L in range(1, min(128, n - i) + 1):
            run_cost += scost(rows[i + L - 1])
            total = base + run_cost
            j = i + L
            if total < dp[j]:
                dp[j] = total
                choice[j] = ("lit", i, L)
        # repeat runs: length 2..129, constant symbol
        max_run = 1
        while max_run < min(129, n - i) and rows[i + max_run] == rows[i]:
            max_run += 1
        if max_run >= 2:
            rep_total = base + scost(rows[i])
            for RL in range(2, max_run + 1):
                j = i + RL
                if rep_total < dp[j]:
                    dp[j] = rep_total
                    choice[j] = ("rep", i, RL)
    # reconstruct
    segs = []
    j = n
    while j > 0:
        typ, i, L = choice[j]
        segs.append((typ, i, L))
        j = i
    segs.reverse()
    out = bytearray()
    for typ, i, L in segs:
        if typ == "lit":
            out.append(L - 1)
            for k in range(L):
                emit_symbol(out, rows[i + k])
        else:
            out.append(0x80 | (L - 2))
            emit_symbol(out, rows[i])
    return out


def build_font_data(width, height, normal, get_real_bold):
    """normal: {cp: [row_int; height]} full regular coverage.
    get_real_bold(cp): the TRUE bold bitmap for cp (a [row_int; height]
    list) for every cp in bold_ranges_for(normal). Source-agnostic: BDF
    callers pass the bold BDF dict directly; the .c converter reconstructs
    it as bold_sparse.get(cp, normal[cp]) (old sparse-table semantics)."""
    rb = 1 if width <= 8 else 2
    font_cps = set(normal)
    reg_ranges = contiguous_ranges(font_cps)
    reg_crop = {cp: crop(rows, height) for cp, rows in normal.items()}

    bold_ranges = bold_ranges_for(font_cps)
    assert len(bold_ranges) == 3, f"expected 3 bold ranges, got {bold_ranges}"
    bold_cps = [cp for lo, hi in bold_ranges for cp in range(lo, hi + 1)]

    def smear_row(r):
        return (r | ((r << 1) & 0xFF)) if width == 8 else (r | (r >> 1))

    exc_crop = {}
    for cp in bold_cps:
        real_bold = list(get_real_bold(cp))
        smeared = [smear_row(r) for r in normal[cp]]
        if real_bold != smeared:
            exc_crop[cp] = crop(real_bold, height)

    # --- shared row palette (rb==2 sizes only) ---
    palette = []
    row_to_idx = {}
    if rb == 2:
        freq = Counter()
        for rec in set(reg_crop.values()):
            for row in rec[2]:
                freq[row] += 1
        for rec in set(exc_crop.values()):
            for row in rec[2]:
                freq[row] += 1
        ordered = sorted(freq, key=lambda r: (-freq[r], r))
        palette = ordered[:255]
        row_to_idx = {r: i for i, r in enumerate(palette)}

    def scost(sym):
        if rb == 1:
            return 1
        return 1 if sym in row_to_idx else 3

    def emit_symbol(out, sym):
        if rb == 1:
            out.append(sym & 0xFF)
            return
        idx = row_to_idx.get(sym, 0xFF)
        out.append(idx)
        if idx == 0xFF:
            out.append(sym & 0xFF)
            out.append((sym >> 8) & 0xFF)

    def encode_record(rec):
        start, ln, rows = rec
        if ln == 0:
            return bytearray([0, 0])
        if height == 16:
            header = bytearray([((start & 0xF) << 4) | ((ln - 1) & 0xF)])
        else:
            header = bytearray([start, ln])
        return header + packbits_encode(list(rows), scost, emit_symbol)

    def build_pool(crop_map, cp_order):
        pool = bytearray()
        offsets = {}
        records = []             # (offset, rec, bytes) in first-seen order
        cps_by_rec = {}
        for cp in cp_order:
            rec = crop_map[cp]
            cps_by_rec.setdefault(rec, []).append(cp)
            if rec not in offsets:
                b = encode_record(rec)
                offsets[rec] = len(pool)
                records.append((len(pool), rec, b))
                pool += b
        assert len(pool) <= 65534, f"pool too large: {len(pool)} bytes"
        cps_by_offset = {offsets[rec]: cps for rec, cps in cps_by_rec.items()}
        return bytes(pool), offsets, records, cps_by_offset

    reg_pool, reg_offsets, reg_records, reg_cps_by_off = build_pool(reg_crop, sorted(reg_crop))
    if exc_crop:
        bold_pool, bold_offsets, bold_records, bold_cps_by_off = build_pool(
            exc_crop, sorted(exc_crop))
    else:
        bold_pool, bold_offsets, bold_records, bold_cps_by_off = b"", {}, [], {}

    assert 0x3F in reg_crop, "'?' (U+003F) missing from regular coverage"
    q_offset = reg_offsets[reg_crop[0x3F]]

    merged_reg_ranges = merge_ranges(reg_ranges)
    reg_range_data = []
    for lo, hi in merged_reg_ranges:
        idx = []
        for cp in range(lo, hi + 1):
            off = reg_offsets[reg_crop[cp]] if cp in reg_crop else q_offset
            assert off < len(reg_pool)
            idx.append(off)
        reg_range_data.append((lo, hi, idx))

    bold_range_data = []
    for lo, hi in bold_ranges:
        idx = []
        for cp in range(lo, hi + 1):
            if cp in exc_crop:
                off = bold_offsets[exc_crop[cp]]
                assert off < len(bold_pool)
                idx.append(off)
            else:
                idx.append(0xFFFF)
        bold_range_data.append((lo, hi, idx))

    return {
        "width": width, "height": height, "rb": rb,
        "reg_pool": reg_pool, "reg_records": reg_records,
        "reg_cps_by_off": reg_cps_by_off, "reg_range_data": reg_range_data,
        "bold_pool": bold_pool, "bold_records": bold_records,
        "bold_cps_by_off": bold_cps_by_off, "bold_range_data": bold_range_data,
        "palette": palette, "exception_count": len(exc_crop),
        "smear_left": 1 if width == 8 else 0,
    }


def check_exception_count(width, count):
    want = EXPECTED_BOLD_EXCEPTIONS[width]
    if count != want:
        sys.exit(f"bold exception count mismatch for width={width}: "
                  f"got {count}, expected {want}")


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

FORMAT_NOTE = """\
 * Compressed glyph pool ("format v1" -- see tools/gen_terminus.py and
 * components/font/terminus_font.h for the record/PackBits layout). Each
 * pool line is one deduplicated, row-cropped glyph record: header byte(s)
 * followed by a PackBits stream. idx[] holds one uint16_t byte-offset per
 * codepoint into the pool ({bold_note}).
"""

HEADER = """\
/*
 * {w}x{h} bitmap glyph data derived from the Terminus Font ({src}).
 *
{format_note} *
 * Copyright (c) 2020 Dimitar Zhekov <dimitar.zhekov@gmail.com>,
 * with Reserved Font Name "Terminus Font".
 *
 * Licensed under the SIL Open Font License, Version 1.1 -- see the full text
 * in components/font/LICENSE. This file is NOT covered by the repository's
 * MIT license.
 *
 * Generated by tools/gen_terminus.py -- do not edit by hand.
 */
#include "terminus_font.h"
#include <stddef.h>

#if FONT_RT_{w}X{h}

"""

BOLD_HEADER = """\
/*
 * {w}x{h} BOLD glyph data derived from the Terminus Font ({src}).
 * Sparse exception pool: only codepoints (within subset A -- ASCII,
 * Latin-1/Extended-A, Cyrillic) whose real bold bitmap is NOT exactly the
 * regular glyph smeared one pixel are stored here; idx == 0xFFFF means
 * "smear the regular glyph" (see terminusN_bold_smear_left).
 *
{format_note} *
 * Copyright (c) 2020 Dimitar Zhekov <dimitar.zhekov@gmail.com>,
 * with Reserved Font Name "Terminus Font".
 *
 * Licensed under the SIL Open Font License, Version 1.1 -- see the full text
 * in components/font/LICENSE. This file is NOT covered by the repository's
 * MIT license.
 *
 * Generated by tools/gen_terminus.py -- do not edit by hand.
 */
#include "terminus_font.h"
#include <stddef.h>

#if FONT_RT_{w}X{h} && FONT_BOLD_ENABLED

"""


def _fmt_pool_lines(records, cps_by_off, prefix_indent="    "):
    """One pool record per source line, hex bytes + a trailing comment
    with its byte offset, header (start/len) and referencing codepoints."""
    lines = []
    for off, rec, b in records:
        start, ln, _ = rec
        cps = cps_by_off[off]
        shown = ", ".join(f"U+{c:04X}" for c in cps[:3])
        more = f" +{len(cps) - 3} more" if len(cps) > 3 else ""
        vals = ", ".join(f"0x{x:02X}" for x in b)
        lines.append(
            f"{prefix_indent}{vals}, "
            f"// off=0x{off:04X} start={start} len={ln} "
            f"({len(cps)} cp{'s' if len(cps) != 1 else ''}: {shown}{more})\n")
    return lines


def _fmt_idx_array(name, lo, hi, idx, ctype="uint16_t", sentinel_note=None):
    lines = [f"static const {ctype} {name}[] = {{\n"]
    for i, cp in enumerate(range(lo, hi + 1)):
        v = idx[i]
        note = ""
        if sentinel_note and v == 0xFFFF:
            note = "  (smear)"
        lines.append(f"    0x{v:04X}, // U+{cp:04X}{note}\n")
    lines.append("};\n\n")
    return lines


def emit_regular_file(out_path, width, height, data, src_name):
    rb = data["rb"]
    bold_note = ("rb==2: index into terminusN_palette[]; 0xFF escapes to "
                 "2 raw LE bytes" if rb == 2 else "rb==1: raw row byte")
    format_note = FORMAT_NOTE.format(bold_note=bold_note)
    format_note = "".join(f" {l}" if not l.startswith(" *") else l
                           for l in format_note.splitlines(keepends=True))

    parts = [HEADER.format(w=width, h=height, src=src_name, format_note=format_note)]

    # pool[] is always a byte stream (header bytes, PackBits control bytes,
    # palette-index/escape bytes) regardless of row width -- terminus_font.h
    # declares it uint8_t for every size.
    parts.append(f"const uint8_t terminus{width}x{height}_pool[] = {{\n")
    parts.extend(_fmt_pool_lines(data["reg_records"], data["reg_cps_by_off"]))
    parts.append("};\n\n")
    parts.append(f"const uint16_t terminus{width}x{height}_pool_bytes = "
                 f"{len(data['reg_pool'])};\n\n")

    if rb == 2:
        pal = data["palette"]
        parts.append(f"const uint16_t terminus{width}x{height}_palette[] = {{\n")
        for i in range(0, len(pal), 8):
            chunk = pal[i:i + 8]
            vals = ", ".join(f"0x{v:04X}" for v in chunk)
            parts.append(f"    {vals}, // idx 0x{i:02X}\n")
        parts.append("};\n\n")
        parts.append(f"const uint16_t terminus{width}x{height}_palette_len = {len(pal)};\n\n")

    for lo, hi, idx in data["reg_range_data"]:
        name = f"font_idx_{lo:04X}_{hi:04X}"
        parts.extend(_fmt_idx_array(name, lo, hi, idx))

    parts.append(f"const FontRange terminus{width}x{height}_ranges[] = {{\n")
    for lo, hi, _ in data["reg_range_data"]:
        parts.append(f"    {{0x{lo:04X}, 0x{hi:04X}, font_idx_{lo:04X}_{hi:04X}}},\n")
    parts.append("};\n\n")
    parts.append(f"const int terminus{width}x{height}_num_ranges = "
                 f"{len(data['reg_range_data'])};\n\n")
    parts.append(f"#endif /* FONT_RT_{width}X{height} */\n")

    Path(out_path).write_text("".join(parts), encoding="utf-8", newline="\n")


def emit_bold_file(out_path, width, height, data, src_name):
    rb = data["rb"]
    bold_note = ("rb==2: index into terminusN_palette[] (defined in the "
                 "regular file)" if rb == 2 else "rb==1: raw row byte")
    format_note = FORMAT_NOTE.format(bold_note=bold_note)
    format_note = "".join(f" {l}" if not l.startswith(" *") else l
                           for l in format_note.splitlines(keepends=True))

    parts = [BOLD_HEADER.format(w=width, h=height, src=src_name, format_note=format_note)]

    # See emit_regular_file: pool[] is always uint8_t.
    parts.append(f"const uint8_t terminus{width}x{height}_bold_pool[] = {{\n")
    parts.extend(_fmt_pool_lines(data["bold_records"], data["bold_cps_by_off"]))
    parts.append("};\n\n")
    parts.append(f"const uint16_t terminus{width}x{height}_bold_pool_bytes = "
                 f"{len(data['bold_pool'])};\n\n")

    for lo, hi, idx in data["bold_range_data"]:
        name = f"font_bidx_{lo:04X}_{hi:04X}"
        parts.extend(_fmt_idx_array(name, lo, hi, idx, sentinel_note=True))

    parts.append(f"const FontRange terminus{width}x{height}_bold_ranges[] = {{\n")
    for lo, hi, _ in data["bold_range_data"]:
        parts.append(f"    {{0x{lo:04X}, 0x{hi:04X}, font_bidx_{lo:04X}_{hi:04X}}},\n")
    parts.append("};\n\n")
    parts.append(f"const int terminus{width}x{height}_num_bold_ranges = "
                 f"{len(data['bold_range_data'])};\n\n")
    parts.append(f"const uint8_t terminus{width}x{height}_bold_smear_left = "
                 f"{data['smear_left']};\n\n")
    parts.append(f"#endif /* FONT_RT_{width}X{height} && FONT_BOLD_ENABLED */\n")

    Path(out_path).write_text("".join(parts), encoding="utf-8", newline="\n")


def report_sizes(width, height, data):
    rb = data["rb"]
    reg_pool_b = len(data["reg_pool"])
    reg_idx_b = sum((hi - lo + 1) * 2 for lo, hi, _ in data["reg_range_data"])
    reg_ranges_b = len(data["reg_range_data"]) * 8   # sizeof(FontRange), 4B ptr
    pal_b = len(data["palette"]) * 2 if rb == 2 else 0
    bold_pool_b = len(data["bold_pool"])
    bold_idx_b = sum((hi - lo + 1) * 2 for lo, hi, _ in data["bold_range_data"])
    bold_ranges_b = len(data["bold_range_data"]) * 8

    reg_total = reg_pool_b + reg_idx_b + reg_ranges_b
    bold_total = bold_pool_b + bold_idx_b + bold_ranges_b
    grand_total = reg_total + bold_total + pal_b

    print(f"{width}x{height}: regular pool={reg_pool_b}B idx={reg_idx_b}B "
          f"ranges={reg_ranges_b}B ({len(data['reg_range_data'])}) "
          f"-> {reg_total}B")
    if rb == 2:
        print(f"{width}x{height}: palette={pal_b}B ({len(data['palette'])} entries)")
    print(f"{width}x{height}: bold    pool={bold_pool_b}B idx={bold_idx_b}B "
          f"ranges={bold_ranges_b}B ({len(data['bold_range_data'])}) "
          f"-> {bold_total}B  [{data['exception_count']} exceptions]")
    print(f"{width}x{height}: TOTAL = {grand_total}B "
          f"(ranges sized at 8B/entry, 4B pointer)")


# ---------------------------------------------------------------------------
# --verify: reimplement the decoder against the EMITTED .c files
# ---------------------------------------------------------------------------

def _strip_line_comments(s):
    return re.sub(r"//[^\n]*", "", s)


def _extract_array(text, name):
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        return None
    body = _strip_line_comments(m.group(1))
    return [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", body)]


def _extract_scalar(text, name):
    m = re.search(re.escape(name) + r"\s*=\s*(\d+);", text)
    return int(m.group(1)) if m else None


def parse_emitted_regular(path, width, height):
    text = Path(path).read_text(encoding="utf-8")
    pool = _extract_array(text, f"terminus{width}x{height}_pool")
    pool_bytes = _extract_scalar(text, f"terminus{width}x{height}_pool_bytes")
    palette = _extract_array(text, f"terminus{width}x{height}_palette")
    idx_arrays = {}
    for m in re.finditer(r"font_idx_([0-9A-F]{4})_([0-9A-F]{4})\[\]\s*=\s*\{(.*?)\};",
                          text, re.S):
        lo, hi = int(m.group(1), 16), int(m.group(2), 16)
        vals = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", m.group(3))]
        idx_arrays[(lo, hi)] = vals
    ranges = sorted(idx_arrays.keys())
    return pool, pool_bytes, palette, idx_arrays, ranges


def parse_emitted_bold(path, width, height):
    text = Path(path).read_text(encoding="utf-8")
    pool = _extract_array(text, f"terminus{width}x{height}_bold_pool")
    pool_bytes = _extract_scalar(text, f"terminus{width}x{height}_bold_pool_bytes")
    smear_left = _extract_scalar(text, f"terminus{width}x{height}_bold_smear_left")
    idx_arrays = {}
    for m in re.finditer(r"font_bidx_([0-9A-F]{4})_([0-9A-F]{4})\[\]\s*=\s*\{(.*?)\};",
                          text, re.S):
        lo, hi = int(m.group(1), 16), int(m.group(2), 16)
        vals = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", m.group(3))]
        idx_arrays[(lo, hi)] = vals
    ranges = sorted(idx_arrays.keys())
    return pool, pool_bytes, smear_left, idx_arrays, ranges


def decode_record(pool, offset, height, rb, palette):
    """Reimplementation of the v1 decoder for one glyph record."""
    if height == 16:
        b0 = pool[offset]
        start = (b0 >> 4) & 0xF
        ln = (b0 & 0xF) + 1
        pos = offset + 1
    else:
        start = pool[offset]
        ln = pool[offset + 1]
        pos = offset + 2
        if ln == 0:
            return [0] * height

    body = []
    produced = 0
    while produced < ln:
        c = pool[pos]
        pos += 1
        if c < 0x80:
            n = c + 1
            for _ in range(n):
                if rb == 1:
                    sym = pool[pos]
                    pos += 1
                else:
                    pidx = pool[pos]
                    pos += 1
                    if pidx == 0xFF:
                        sym = pool[pos] | (pool[pos + 1] << 8)
                        pos += 2
                    else:
                        sym = palette[pidx]
                body.append(sym)
            produced += n
        else:
            n = (c & 0x7F) + 2
            if rb == 1:
                sym = pool[pos]
                pos += 1
            else:
                pidx = pool[pos]
                pos += 1
                if pidx == 0xFF:
                    sym = pool[pos] | (pool[pos + 1] << 8)
                    pos += 2
                else:
                    sym = palette[pidx]
            body.extend([sym] * n)
            produced += n
    assert produced == ln, f"packbits overrun at offset {offset}"

    rows = [0] * height
    for i, v in enumerate(body):
        rows[start + i] = v
    return rows


def verify(width, height, ref_reg_path, ref_bold_path, emitted_reg_path, emitted_bold_path):
    rb = 1 if width <= 8 else 2
    ref_normal, _ = parse_old_regular(ref_reg_path, height)
    ref_bold_sparse = parse_old_bold(ref_bold_path, height)

    pool, pool_bytes, palette, idx_arrays, ranges = parse_emitted_regular(
        emitted_reg_path, width, height)
    assert pool is not None, "regular pool not found in emitted file"
    assert pool_bytes == len(pool), f"pool_bytes {pool_bytes} != actual {len(pool)}"
    assert pool_bytes <= 65534

    cp_offset = {}
    for lo, hi in ranges:
        arr = idx_arrays[(lo, hi)]
        assert len(arr) == hi - lo + 1
        for i, cp in enumerate(range(lo, hi + 1)):
            assert arr[i] < pool_bytes, f"U+{cp:04X} idx {arr[i]} >= pool_bytes"
            cp_offset[cp] = arr[i]

    errors = []
    checked = 0
    for cp, want in ref_normal.items():
        off = cp_offset.get(cp)
        if off is None:
            errors.append(f"U+{cp:04X} missing from emitted regular idx")
            continue
        got = decode_record(pool, off, height, rb, palette)
        if got != want:
            errors.append(f"U+{cp:04X} regular mismatch: got {got} want {want}")
        checked += 1

    q_off = cp_offset.get(0x3F)
    if q_off is None:
        errors.append("U+003F ('?') missing from emitted regular idx")

    gap_cp = None
    for lo, hi in ranges:
        for cp in range(lo, hi + 1):
            if cp not in ref_normal:
                gap_cp = cp
                break
        if gap_cp is not None:
            break
    if gap_cp is not None and q_off is not None:
        got = decode_record(pool, cp_offset[gap_cp], height, rb, palette)
        want_q = decode_record(pool, q_off, height, rb, palette)
        if got != want_q:
            errors.append(f"gap U+{gap_cp:04X} does not decode to '?' record "
                           f"(got {got} want {want_q})")

    bpool, bpool_bytes, smear_left, bidx_arrays, branges = parse_emitted_bold(
        emitted_bold_path, width, height)
    assert bpool is not None, "bold pool not found in emitted file"
    assert bpool_bytes == len(bpool), f"bold pool_bytes {bpool_bytes} != actual {len(bpool)}"
    assert bpool_bytes <= 65534
    want_smear_left = 1 if width == 8 else 0
    if smear_left != want_smear_left:
        errors.append(f"bold_smear_left {smear_left} != expected {want_smear_left}")

    def smear_row(r):
        return (r | ((r << 1) & 0xFF)) if width == 8 else (r | (r >> 1))

    bcp_offset = {}
    for lo, hi in branges:
        arr = bidx_arrays[(lo, hi)]
        assert len(arr) == hi - lo + 1
        for i, cp in enumerate(range(lo, hi + 1)):
            if arr[i] != 0xFFFF:
                assert arr[i] < bpool_bytes, f"U+{cp:04X} bold idx {arr[i]} >= bold pool_bytes"
            bcp_offset[cp] = arr[i]

    exception_count = 0
    bold_checked = 0
    for lo, hi in branges:
        for cp in range(lo, hi + 1):
            val = bcp_offset[cp]
            true_bold = ref_bold_sparse.get(cp, ref_normal[cp])
            if val == 0xFFFF:
                reg_off = cp_offset.get(cp)
                got = [smear_row(r) for r in decode_record(pool, reg_off, height, rb, palette)]
            else:
                exception_count += 1
                got = decode_record(bpool, val, height, rb, palette)
            if got != true_bold:
                errors.append(f"U+{cp:04X} bold mismatch: got {got} want {true_bold}")
            bold_checked += 1

    want_exc = EXPECTED_BOLD_EXCEPTIONS[width]
    if exception_count != want_exc:
        errors.append(f"bold exception count {exception_count} != expected {want_exc}")

    if errors:
        for e in errors[:30]:
            print("MISMATCH:", e)
        if len(errors) > 30:
            print(f"... and {len(errors) - 30} more")
        sys.exit(f"FAIL {width}x{height}: {len(errors)} error(s)")

    print(f"PASS {width}x{height}: {checked} regular glyphs OK"
          + (f", gap U+{gap_cp:04X} -> '?' OK" if gap_cp is not None else ", no gap cps")
          + f"; {bold_checked} bold cps OK ({exception_count} exceptions); "
          f"pool={len(pool)}B bold_pool={len(bpool)}B")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_wh(spec):
    w, h = spec.lower().split("x")
    return int(w), int(h)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    mode = sys.argv[1]

    if mode == "convert" and len(sys.argv) >= 7:
        width, height = parse_wh(sys.argv[2])
        ref_reg, ref_bold, out_reg, out_bold = sys.argv[3:7]
        do_verify = "--verify" in sys.argv[7:]

        normal, _ = parse_old_regular(ref_reg, height)
        bold_sparse = parse_old_bold(ref_bold, height)
        get_real_bold = lambda cp: bold_sparse.get(cp, normal[cp])  # noqa: E731

        data = build_font_data(width, height, normal, get_real_bold)
        check_exception_count(width, data["exception_count"])
        emit_regular_file(out_reg, width, height, data, Path(ref_reg).name)
        emit_bold_file(out_bold, width, height, data, Path(ref_bold).name)
        report_sizes(width, height, data)

        if do_verify:
            verify(width, height, ref_reg, ref_bold, out_reg, out_bold)
        return

    if mode == "verify" and len(sys.argv) >= 7:
        width, height = parse_wh(sys.argv[2])
        ref_reg, ref_bold, out_reg, out_bold = sys.argv[3:7]
        verify(width, height, ref_reg, ref_bold, out_reg, out_bold)
        return

    if mode == "bdf" and len(sys.argv) >= 7:
        width, height = parse_wh(sys.argv[2])
        nbdf, bbdf, out_reg, out_bold = sys.argv[3:7]
        nw, nh, normal = parse_bdf(nbdf)
        bw, bh, bold = parse_bdf(bbdf)
        if (nw, nh) != (width, height) or (bw, bh) != (width, height):
            sys.exit(f"bdf: size mismatch (normal {nw}x{nh}, bold {bw}x{bh}, "
                      f"expected {width}x{height})")
        get_real_bold = lambda cp: bold.get(cp, normal[cp])  # noqa: E731
        data = build_font_data(width, height, normal, get_real_bold)
        check_exception_count(width, data["exception_count"])
        emit_regular_file(out_reg, width, height, data, Path(nbdf).name)
        emit_bold_file(out_bold, width, height, data, Path(bbdf).name)
        report_sizes(width, height, data)
        return

    sys.exit(__doc__)


if __name__ == "__main__":
    main()

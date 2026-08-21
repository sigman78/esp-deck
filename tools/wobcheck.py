"""Validate wob_shift_line's word-level index math against the per-pixel
reference it replaces, for every displacement the wobble can produce.

Models RGB565 little-endian packing: word j holds [px(2j) low, px(2j+1) high].
"""
W = 800                 # pixels per scanline
NW = W // 2             # 32-bit words per scanline


def reference(px, d):
    """The original: lp[i] = lp[i-d], vacated edge black. Not in place, so the
    overlap direction cannot hide a bug."""
    out = [0] * W
    for i in range(W):
        s = i - d
        out[i] = px[s] if 0 <= s < W else 0
    return out


def pack(px):
    return [(px[2 * j] & 0xFFFF) | ((px[2 * j + 1] & 0xFFFF) << 16) for j in range(NW)]


def unpack(w):
    px = []
    for v in w:
        px.append(v & 0xFFFF)
        px.append((v >> 16) & 0xFFFF)
    return px


def M(x):
    return x & 0xFFFFFFFF


def optimized(px, d):
    """Mirror of wob_shift_line, including its in-place iteration order."""
    w = pack(px)
    if (d & 1) == 0:
        k = d >> 1
        if k > 0:
            for i in range(NW - 1, k - 1, -1):
                w[i] = w[i - k]
            for i in range(k - 1, -1, -1):
                w[i] = 0
        else:
            kk = -k
            for i in range(0, NW - kk):
                w[i] = w[i + kk]
            for i in range(NW - kk, NW):
                w[i] = 0
    else:
        k = (d - 1) >> 1            # Python >> floors, matching arithmetic shift
        if d > 0:
            for i in range(NW - 1, k, -1):
                w[i] = M((w[i - k - 1] >> 16) | M(w[i - k] << 16))
            w[k] = M(w[0] << 16)
            for i in range(k - 1, -1, -1):
                w[i] = 0
        else:
            last = NW - 1 + k
            for i in range(0, last + 1):
                w[i] = M((w[i - k - 1] >> 16) | M(w[i - k] << 16))
            w[last + 1] = w[NW - 1] >> 16
            for i in range(last + 2, NW):
                w[i] = 0
    return unpack(w)


# distinctive, non-repeating pattern so any misplacement shows
src = [(i * 7919 + 1) & 0xFFFF or 1 for i in range(W)]

bad = 0
for d in range(-8, 9):
    if d == 0:
        continue
    exp = reference(src, d)
    got = optimized(src, d)
    if exp != got:
        bad += 1
        diffs = [i for i in range(W) if exp[i] != got[i]]
        print('d=%+d  MISMATCH at %d px, first indices %s' % (d, len(diffs), diffs[:8]))
        for i in diffs[:4]:
            print('     px[%d] expected %04x got %04x' % (i, exp[i], got[i]))
    else:
        print('d=%+d  ok' % d)

print()
print('FAILED' if bad else 'all %d displacements match the reference' % 16)

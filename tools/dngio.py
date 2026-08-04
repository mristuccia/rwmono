"""Minimal uncompressed-TIFF/DNG writer and reader for the rwmono test harness.

Writes a Bayer CFA DNG that LibRaw can open, and reads back the uncompressed
monochrome DNGs rwmono produces with --uncompressed.
"""
import struct
import numpy as np

BYTE, ASCII, SHORT, LONG, RATIONAL, SBYTE, UNDEF, SSHORT, SLONG, SRATIONAL = range(1, 11)
TSIZE = {BYTE: 1, ASCII: 1, SHORT: 2, LONG: 4, RATIONAL: 8, SBYTE: 1,
         UNDEF: 1, SSHORT: 2, SLONG: 4, SRATIONAL: 8}


def _pack_vals(typ, vals):
    if typ == ASCII:
        return vals.encode() + b"\0"
    fmt = {BYTE: "B", SHORT: "H", LONG: "I", SBYTE: "b", UNDEF: "B",
           SSHORT: "h", SLONG: "i"}.get(typ)
    if fmt:
        return b"".join(struct.pack("<" + fmt, v) for v in vals)
    out = b""
    for v in vals:                       # (num, den) pairs
        f = "<ii" if typ == SRATIONAL else "<II"
        out += struct.pack(f, v[0], v[1])
    return out


def write_cfa_dng(path, cfa, black, white, cfa_pattern=(0, 1, 1, 2),
                  as_shot=(1 / 2.63, 1.0, 1 / 1.62), model="SynthBayer"):
    """cfa: uint16 HxW mosaic."""
    h, w = cfa.shape
    data = cfa.astype("<u2").tobytes()
    ident = np.array([1, 0, 0, 0, 1, 0, 0, 0, 1])

    tags = [
        (254, LONG, [0]), (256, LONG, [w]), (257, LONG, [h]),
        (258, SHORT, [16]), (259, SHORT, [1]), (262, SHORT, [32803]),
        (271, ASCII, "Synth"), (272, ASCII, model),
        (273, LONG, None),                                  # StripOffsets
        (277, SHORT, [1]), (278, LONG, [h]),
        (279, LONG, [len(data)]), (284, SHORT, [1]),
        (33421, SHORT, [2, 2]),
        (33422, BYTE, list(cfa_pattern)),
        (50706, BYTE, [1, 4, 0, 0]), (50707, BYTE, [1, 1, 0, 0]),
        (50708, ASCII, model),
        (50713, SHORT, [1, 1]),
        (50714, RATIONAL, [(int(black), 1)]),
        (50717, LONG, [int(white)]),
        (50721, SRATIONAL, [(int(round(v * 10000)), 10000) for v in ident]),
        (50728, RATIONAL, [(int(round(v * 100000)), 100000) for v in as_shot]),
        (50778, SHORT, [21]),
    ]
    tags.sort(key=lambda t: t[0])

    hdr = 8
    ifd_size = 2 + 12 * len(tags) + 4
    heap_off = hdr + ifd_size
    heap, entries = b"", []
    strip_off = heap_off                      # patched below, after heap sized
    blobs = {}
    for tag, typ, vals in tags:
        if vals is None:
            blobs[tag] = None
            continue
        payload = _pack_vals(typ, vals)
        n = len(vals) if typ != ASCII else len(payload)
        blobs[tag] = (typ, n, payload)
    heap = b""
    for tag, typ, vals in tags:
        b = blobs[tag]
        if b is None:
            continue
        typ_, n, payload = b
        if len(payload) > 4:
            blobs[tag] = (typ_, n, None, heap_off + len(heap))
            heap += payload + (b"\0" if len(payload) % 2 else b"")
        else:
            blobs[tag] = (typ_, n, payload.ljust(4, b"\0"), None)
    strip_off = heap_off + len(heap)

    ifd = struct.pack("<H", len(tags))
    for tag, typ, vals in tags:
        if tag == 273:
            ifd += struct.pack("<HHI4s", 273, LONG, 1, struct.pack("<I", strip_off))
            continue
        typ_, n, inline, off = blobs[tag]
        ifd += struct.pack("<HHI", tag, typ_, n)
        ifd += inline if inline is not None else struct.pack("<I", off)
    ifd += struct.pack("<I", 0)

    with open(path, "wb") as f:
        f.write(struct.pack("<2sHI", b"II", 42, hdr))
        f.write(ifd)
        f.write(heap)
        f.write(data)


def read_mono_dng(path):
    """Read an uncompressed single-strip/tile 16-bit mono TIFF/DNG."""
    buf = open(path, "rb").read()
    assert buf[:2] == b"II"
    off = struct.unpack_from("<I", buf, 4)[0]
    n = struct.unpack_from("<H", buf, off)[0]
    t = {}
    for i in range(n):
        p = off + 2 + 12 * i
        tag, typ, cnt = struct.unpack_from("<HHI", buf, p)
        size = TSIZE.get(typ, 1) * cnt
        vp = struct.unpack_from("<I", buf, p + 8)[0] if size > 4 else p + 8
        t[tag] = (typ, cnt, vp)

    def val(tag, default=None):
        if tag not in t:
            return default
        typ, cnt, vp = t[tag]
        fmt = {SHORT: "H", LONG: "I", BYTE: "B"}.get(typ)
        if fmt:
            return list(struct.unpack_from("<" + str(cnt) + fmt, buf, vp))
        if typ == RATIONAL:
            a, b = struct.unpack_from("<II", buf, vp)
            return [a / b]
        return None

    w, h = val(256)[0], val(257)[0]
    offs = val(273) or val(324)
    cnts = val(279) or val(325)
    raw = b"".join(buf[o:o + c] for o, c in zip(offs, cnts))
    img = np.frombuffer(raw, dtype="<u2")[:w * h].reshape(h, w).astype(np.float64)
    black = (val(50714) or [0])[0]
    white = (val(50717) or [65535])[0]
    return img, float(black), float(white)

"""SPKL <-> 16-bit RGB PNG codec.

The FrameMatter .spkleak container is a 24-byte header followed by
w*h*3 little-endian uint16 RGB samples.  A 16-bit RGB PNG carries the
exact same samples (big-endian on the wire), so the conversion is
bit-exact and the Vulkan kernel's tex[] SSBO layout is unchanged.

Header-only metadata that PNG cannot express (local, front) is kept in
the per-category manifest.json.
"""
import hashlib
import struct
import zlib

import numpy as np

_SIG = b"\x89PNG\r\n\x1a\n"


def _chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _paeth(a, b, c):
    a = a.astype(np.int32)
    b = b.astype(np.int32)
    c = c.astype(np.int32)
    p = a + b - c
    pa = np.abs(p - a)
    pb = np.abs(p - b)
    pc = np.abs(p - c)
    return np.where((pa <= pb) & (pa <= pc), a, np.where(pb <= pc, b, c)).astype(np.uint8)


def _encode_png(width, height, raw, bpp, depth, meta):
    out = bytearray()
    prev = np.zeros(raw.shape[1], dtype=np.uint8)
    for y in range(height):
        cur = raw[y]
        left = np.concatenate((np.zeros(bpp, dtype=np.uint8), cur[:-bpp]))
        upleft = np.concatenate((np.zeros(bpp, dtype=np.uint8), prev[:-bpp]))
        candidates = []
        for ft in (0, 1, 2, 3, 4):
            if ft == 0:
                f = cur
            elif ft == 1:
                f = (cur.astype(np.uint16) - left) & 0xFF
            elif ft == 2:
                f = (cur.astype(np.uint16) - prev) & 0xFF
            elif ft == 3:
                f = (cur.astype(np.uint16) -
                     ((left.astype(np.uint16) + prev) >> 1)) & 0xFF
            else:
                pred = _paeth(left, prev, upleft)
                f = (cur.astype(np.uint16) - pred) & 0xFF
            f = f.astype(np.uint8)
            resid = np.minimum(f.astype(np.int32), 256 - f.astype(np.int32)).sum()
            candidates.append((int(resid), ft, f))
        _, ft, f = min(candidates, key=lambda t: t[0])
        out.append(ft)
        out += f.tobytes()
        prev = cur
    ihdr = struct.pack(">IIBBBBB", width, height, depth, 2, 0, 0, 0)
    text = b""
    for k, v in sorted((meta or {}).items()):
        text += _chunk(b"tEXt", k.encode("ascii") + b"\0" + str(v).encode("ascii"))
    return (_SIG + _chunk(b"IHDR", ihdr) + text +
            _chunk(b"IDAT", zlib.compress(bytes(out), 9)) +
            _chunk(b"IEND", b""))


def encode_png16(width, height, rgb16_le, meta=None):
    """rgb16_le: bytes of w*h*3 uint16 LE. Returns PNG bytes (lossless)."""
    arr = np.frombuffer(rgb16_le, dtype="<u2").astype(">u2")
    raw = arr.view(np.uint8).reshape(height, width * 3 * 2)
    return _encode_png(width, height, raw, 6, 16, meta)


def encode_png8(width, height, rgb8, meta=None):
    """rgb8: bytes of w*h*3 uint8. Host expands v -> v*257 on load."""
    raw = np.frombuffer(rgb8, dtype=np.uint8).reshape(height, width * 3)
    return _encode_png(width, height, raw, 3, 8, meta)


def decode_png16(data):
    """Inverse of encode_png16. Returns (w, h, rgb16_le_bytes, meta)."""
    assert data[:8] == _SIG, "not a PNG"
    pos = 8
    ihdr = None
    idat = []
    meta = {}
    while pos < len(data):
        (n,) = struct.unpack_from(">I", data, pos)
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if tag == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat.append(body)
        elif tag == b"tEXt":
            k, _, v = body.partition(b"\0")
            meta[k.decode("ascii")] = v.decode("ascii")
        elif tag == b"IEND":
            break
    w, h, depth, ctype, comp, filt, interlace = ihdr
    assert (depth, ctype, comp, filt, interlace) == (16, 2, 0, 0, 0), ihdr
    raw = zlib.decompress(b"".join(idat))
    stride = w * 6
    bpp = 6
    rows = []
    prev = np.zeros(stride, dtype=np.uint8)
    off = 0
    for _ in range(h):
        ft = raw[off]
        f = np.frombuffer(raw, dtype=np.uint8, count=stride, offset=off + 1)
        off += 1 + stride
        cur = np.empty(stride, dtype=np.uint8)
        for i in range(stride):
            a = int(cur[i - bpp]) if i >= bpp else 0
            b = int(prev[i])
            c = int(prev[i - bpp]) if i >= bpp else 0
            if ft == 0:
                pred = 0
            elif ft == 1:
                pred = a
            elif ft == 2:
                pred = b
            elif ft == 3:
                pred = (a + b) >> 1
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            else:
                raise ValueError("bad PNG filter %d" % ft)
            cur[i] = (int(f[i]) + pred) & 0xFF
        rows.append(cur)
        prev = cur
    be = np.concatenate(rows).tobytes()
    le = np.frombuffer(be, dtype=">u2").astype("<u2").tobytes()
    return w, h, le, meta


def read_spkleak(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, w, h, local, front = struct.unpack_from("<4sIIIIf", data, 0)
    assert magic == b"SPKL" and version == 1, path
    assert len(data) == 24 + w * h * 6, path
    return dict(w=w, h=h, local=local, front=front,
                pixels=data[24:], sha256=hashlib.sha256(data).hexdigest())

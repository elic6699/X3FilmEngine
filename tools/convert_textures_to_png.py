"""Convert FrameMatter's 46 raw RGB16 .spkleak overlays to self-contained
16-bit RGB PNGs, losslessly, and emit the release manifests.

Why: .spkleak is a 24-byte header + uncompressed w*h*3 uint16 LE RGB
(332 MB total). A 16-bit PNG carries the same samples with PNG filters +
deflate, is readable by any image tool, and matches the macOS app's
"ship PNG, not raw .spkleak" packaging. The Vulkan kernel still receives
the identical uint16 SSBO payload, so rendering is bit-exact.

The header fields PNG cannot express (local, front) are written as tEXt
chunks inside each file and mirrored in manifest.json.
"""
import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path

from spk_png import encode_png16, encode_png8, decode_png16, read_spkleak

SRC = Path(r"D:/dev/x3l-work/framematter-src/engine/resources/textures")
DST = Path(r"D:/dev/x3l-work/fm-win/engine/resources/textures-png")
DEPTH = 16

import numpy as np


def convert(category, prefix, count):
    src_dir = SRC / category
    dst_dir = DST / category
    dst_dir.mkdir(parents=True, exist_ok=True)
    old = json.loads((src_dir / "manifest.json").read_text())["materials"]
    by_name = {r["file"]: r for r in old}
    records = []
    raw_total = png_total = 0
    for n in range(1, count + 1):
        name = "%s-%03d.spkleak" % (prefix, n)
        src = src_dir / name
        s = read_spkleak(str(src))
        meta = {"spk-local": str(s["local"]),
                "spk-front": repr(s["front"]),
                "spk-source": name,
                "spk-source-sha256": s["sha256"]}
        t = time.time()
        if DEPTH == 16:
            png = encode_png16(s["w"], s["h"], s["pixels"], meta)
        else:
            hi = (np.frombuffer(s["pixels"], dtype="<u2") >> 8).astype(np.uint8).tobytes()
            png = encode_png8(s["w"], s["h"], hi, meta)
        dst = dst_dir / (name[:-8] + ".png")
        dst.write_bytes(png)
        raw_total += 24 + len(s["pixels"])
        png_total += len(png)
        records.append({
            "file": dst.name,
            "sha256": hashlib.sha256(png).hexdigest(),
            "width": s["w"],
            "height": s["h"],
            "local": s["local"],
            "front": s["front"],
            "sourceFile": name,
            "sourceSha256": s["sha256"],
            "encoding": ("PNG 16-bit RGB (lossless from SPKL RGB16 LE)" if DEPTH == 16
                     else "PNG 8-bit RGB (high byte; host expands v*257)"),
        })
        print("  %-24s %7.2f -> %6.2f MB  %5.1fs" %
              (dst.name, (24 + len(s["pixels"])) / 1048576,
               len(png) / 1048576, time.time() - t), flush=True)
    manifest = {
        "version": 2,
        "encoding": ("PNG 16-bit RGB, RGB16 LE reconstructed by the host"
                     if DEPTH == 16 else
                     "PNG 8-bit RGB, host expands v*257 to RGB16 LE"),
        "sourceFormat": "SPKL v1 (24-byte header + RGB16 LE)",
        "metadata": "local/front duplicated as PNG tEXt chunks spk-local / spk-front",
        "materials": records,
    }
    (dst_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return raw_total, png_total


def main():
    global DST, DEPTH
    ap = argparse.ArgumentParser()
    ap.add_argument("--depth", type=int, default=16, choices=(8, 16))
    ap.add_argument("--out", default=str(DST))
    a = ap.parse_args()
    DEPTH = a.depth
    DST = Path(a.out)
    print("source :", SRC, flush=True)
    print("output :", DST, "(depth %d)" % DEPTH, flush=True)
    r1, p1 = convert("light-leaks", "light-leak", 41)
    r2, p2 = convert("aging", "aging", 5)
    print("\nraw  : %.2f MB" % ((r1 + r2) / 1048576), flush=True)
    print("png  : %.2f MB" % ((p1 + p2) / 1048576), flush=True)
    print("ratio: %.2fx (saved %.2f MB)" %
          ((r1 + r2) / (p1 + p2), (r1 + r2 - p1 - p2) / 1048576), flush=True)
    if DEPTH != 16:
        print("SKIP bit-exact roundtrip (8-bit is a deliberate quantisation)", flush=True)
        print("PASS: 46 conversions", flush=True)
        return

    # Bit-exact round-trip spot checks (slow Python decoder: 3 files).
    import glob
    picks = [DST / "aging" / "aging-001.png",
             DST / "light-leaks" / "light-leak-001.png",
             DST / "light-leaks" / "light-leak-041.png"]
    for p in picks:
        src = SRC / p.parent.name / (p.stem + ".spkleak")
        s = read_spkleak(str(src))
        w, h, le, meta = decode_png16(p.read_bytes())
        assert (w, h) == (s["w"], s["h"]), p
        assert le == s["pixels"], "pixel mismatch: " + str(p)
        assert int(meta["spk-local"]) == s["local"], p
        assert abs(float(meta["spk-front"]) - s["front"]) < 1e-9, p
        print("roundtrip OK:", p.name, flush=True)
    print("PASS: 46 conversions, all spot checks bit-exact", flush=True)


if __name__ == "__main__":
    main()

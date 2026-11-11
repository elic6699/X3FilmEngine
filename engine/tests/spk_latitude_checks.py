#!/usr/bin/env python3
"""Regression checks for the X3Lumina scene-linear latitude sideband.

X3Lumina feeds the film engine a 16-bit display-coded wire whose LOOK LUTs
were calibrated on. Values above 1.0 and negative excursions never reach the
engine that way. Two opt-in sidebands restore the missing information without
touching the LOOK path:

  * tier 1 -- two scalars in ``open_pixels.params`` (P99.5 and frame max),
    consumed by the meter and ``node_headroom``'s clip;
  * tier 2 -- a full float32 scene-linear frame via ``set_scene_frame``,
    used to build a per-pixel headroom gain map.

Both default to off, so the unchanged wire must stay byte-identical. This
harness starts one host process per case and checks that contract.

Usage::

    python3 engine/tests/spk_latitude_checks.py \
        [--host engine/build/spektralab-host] \
        [--resources engine/resources]

Exit status 0 = all checks passed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from pathlib import Path

ENGINE = Path(__file__).resolve().parents[1]

W, H = 96, 32
MIDGRAY = 0.184
HIGH_VALUE = 4.0


class Host:
    def __init__(self, binary: Path, resources: Path):
        self.proc = subprocess.Popen(
            [str(binary), "--resources", str(resources)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )

    @staticmethod
    def _read_exact(stream, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = stream.read(n - len(buf))
            if not chunk:
                raise RuntimeError(f"host closed stdout after {len(buf)}/{n} bytes")
            buf += chunk
        return buf

    def call(self, method: str, params: dict | None = None, payload: bytes = b"") -> tuple[dict, bytes]:
        header = json.dumps(
            {"id": 1, "method": method, "params": params or {}},
            separators=(",", ":"),
        ).encode()
        self.proc.stdin.write(struct.pack("<II", len(header), len(payload)) + header + payload)
        self.proc.stdin.flush()
        hl, pl = struct.unpack("<II", self._read_exact(self.proc.stdout, 8))
        reply = json.loads(self._read_exact(self.proc.stdout, hl))
        data = self._read_exact(self.proc.stdout, pl) if pl else b""
        if not reply.get("ok"):
            raise RuntimeError(f"{method} failed: {reply}")
        return reply, data

    def close(self) -> None:
        try:
            self.call("shutdown")
        except Exception:
            pass
        self.proc.kill()
        self.proc.wait(timeout=5)


def wire_frame() -> bytes:
    """16-bit wire: a clipped highlight block on midgray."""
    bg = round(MIDGRAY * 65535)
    out = bytearray()
    for y in range(H):
        for x in range(W):
            c = 65535 if (x > W * 0.75 and y < H * 0.5) else bg
            out += struct.pack("<HHH", c, c, c)
    return bytes(out)


def scene_frame() -> bytes:
    """True scene-linear twin: the same block really is at 4.0, not 1.0."""
    out = bytearray()
    for y in range(H):
        for x in range(W):
            v = HIGH_VALUE if (x > W * 0.75 and y < H * 0.5) else MIDGRAY
            out += struct.pack("<fff", v, v, v)
    return bytes(out)


def run_case(host_path: Path, resources: Path, tier: int) -> dict:
    h = Host(host_path, resources)
    try:
        hello, _ = h.call("hello")
        methods = hello["result"]["methods"]
        if tier == 2 and "set_scene_frame" not in methods:
            raise RuntimeError("host does not advertise set_scene_frame")

        open_params = {
            "width": W, "height": H,
            "format": "rgb16le", "space": "linear_prophoto",
        }
        if tier >= 1:
            # Well above the protect_highlights threshold (1.041) and the
            # wire's clipped 1.0, so the meter and headroom clip both move.
            open_params["params"] = {
                "scene_linear_p995": 4.0,
                "scene_linear_frame_max": 12.0,
            }
        opened, _ = h.call("open_pixels", open_params, wire_frame())
        sid = opened["result"]["session"]

        # Keep the metering path identical across all three cases: the only
        # difference between them is then the sideband itself. `solve` forces
        # a re-meter, which is where the tier-1 scalars are consumed.
        h.call("set_params", {"session": sid, "delta": {"auto_exposure_method": "protect_highlights"}})
        h.call("solve", {"session": sid, "target": "exposure"})

        if tier == 2:
            h.call("set_scene_frame", {"session": sid, "width": W, "height": H}, scene_frame())

        _, pixels = h.call("render", {
            "session": sid, "tier": "full", "format": "rgba16", "display": "prophoto",
        })
    finally:
        h.close()

    values = struct.unpack(f"<{W * H * 4}H", pixels[: W * H * 8])
    r = [values[i * 4] for i in range(W * H)]
    def mean_at(pred) -> float:
        sel = [r[y * W + x] for y in range(H) for x in range(W) if pred(x, y)]
        return sum(sel) / max(len(sel), 1)
    hi = lambda x, y: x > W * 0.75 and y < H * 0.5
    return {
        "md5": hashlib.md5(pixels).hexdigest(),
        "highlight_mean": mean_at(hi),
        "rest_mean": mean_at(lambda x, y: not hi(x, y)),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", type=Path, default=ENGINE / "build" / "spektralab-host")
    ap.add_argument("--resources", type=Path, default=ENGINE / "resources")
    args = ap.parse_args()

    if not args.host.is_file():
        print(f"FAIL host not found: {args.host}", file=sys.stderr)
        return 2

    print(f"host      : {args.host}")
    print(f"resources : {args.resources}")
    a = run_case(args.host, args.resources, 0)
    a_repeat = run_case(args.host, args.resources, 0)
    b = run_case(args.host, args.resources, 1)
    c = run_case(args.host, args.resources, 2)

    print()
    for name, r in (("default", a), ("default(repeat)", a_repeat), ("tier1", b), ("tier2", c)):
        print(f"  {name:<16} md5={r['md5']}  highlight={r['highlight_mean']:.0f}  rest={r['rest_mean']:.0f}")

    failures = []
    if a["md5"] != a_repeat["md5"]:
        failures.append("default render is not deterministic")
    if b["md5"] == a["md5"]:
        failures.append("tier 1 sideband had no effect")
    if c["md5"] == a["md5"]:
        failures.append("tier 2 sideband had no effect")
    if c["md5"] == b["md5"]:
        failures.append("tier 2 is indistinguishable from tier 1")
    # Compare tier 2 against tier 1, not against the default: both share the
    # same scalars and metering, so the only difference is the full float
    # frame. Tier 1's protect_highlights decision is a global exposure change
    # and would otherwise swamp the local headroom gain.
    hi_gain = c["highlight_mean"] - b["highlight_mean"]
    rest_gain = c["rest_mean"] - b["rest_mean"]
    if hi_gain < 1000:
        failures.append(f"tier 2 highlight gain too small ({hi_gain:.0f})")
    if abs(rest_gain) > hi_gain / 2:
        failures.append(f"tier 2 is a global shift, not local highlight ({rest_gain:.0f})")

    print()
    print(f"tier2 - tier1: highlight {hi_gain:+.0f} LSB, rest {rest_gain:+.0f} LSB")
    if failures:
        for f in failures:
            print(f"FAIL {f}", file=sys.stderr)
        return 1
    print("PASS default deterministic, tier1/tier2 effective, gain localised to clipped highlights")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

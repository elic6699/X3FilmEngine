# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Compare shipping grain-stage output to unmodified Spektrafilm apply_grain."""
import argparse
import json
import sys
import tempfile
import subprocess
from pathlib import Path
import numpy as np
root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference', type=Path, required=True, help='Unmodified Spektrafilm checkout')
parser.add_argument('--probe', type=Path, default=root / 'engine/build/grain_probe')
args = parser.parse_args()
sys.path.insert(0, str(args.reference.resolve() / 'src'))
from spektrafilm.model.grain import apply_grain
from spektrafilm.runtime.params_schema import GrainParams
from spk_ctypes import Engine, EngineError

# Exercise the same wire and cached-negative path the editor uses.
with Engine() as engine:
    image = np.full((192, 192, 3), 0.18, dtype=np.float32)
    with engine.open(image, {'auto_exposure': False, 'glare_active': False}) as session:
        session.render('full')
        for name, values in [('grain_particle_area_um2', [0.01, 0.2, 0.4, 2]),
                             ('grain_blur', [0, 0.4, 0.65, 0.9, 2])]:
            for value in values:
                reply = session.set_params({name: value})
                got, progress = session.render('full')
                assert reply['invalidated'] == 'shoot', reply
                assert not progress.negative_was_cached
                assert np.isfinite(got).all()
                assert session.get_params()[name] == value
        for name, value in [('grain_particle_area_um2', 0), ('grain_particle_area_um2', -1),
                            ('grain_particle_area_um2', 2.01), ('grain_blur', -0.01), ('grain_blur', 2.01)]:
            try:
                session.set_params({name: value})
            except EngineError:
                pass
            else:
                raise AssertionError((name, value))
print('PASS live-session parameter edits, negative invalidation and invalid values')
temporary = tempfile.TemporaryDirectory(prefix='spektralab-grain-')
work = Path(temporary.name)
profile = json.loads((root / 'engine/resources/profiles/kodak_portra_400.json').read_text())
curves = np.array(profile['data']['density_curves'], dtype=float)
layers = np.array(profile['data']['density_curves_layers'], dtype=float)

def stats(image):
    # Exclude filter boundaries and compare each channel independently.
    a = image[16:-16, 16:-16].astype(float)
    mean = a.mean(axis=(0, 1))
    centered = a - mean
    std = centered.std(axis=(0, 1))
    skew = ((centered / std) ** 3).mean(axis=(0, 1))
    corr = (centered[:, 1:] * centered[:, :-1]).mean(axis=(0, 1)) / std**2
    # 16×16 blocks, each 30×30 pixels: estimate sampling error with blur correlation.
    # The old whole-print 0.004 absolute bar alone is unsuitable at large
    # particle areas in density units. Hold means within six measured SEs.
    block_means = a.reshape(16, 30, 16, 30, 3).mean(axis=(1, 3))
    mean_se = block_means.std(axis=(0, 1), ddof=1) / 16
    return mean, std, skew, corr, mean_se

failures = 0
for layered in (False, True):
    for area, blur in ((0.2, 0.65), (0.8, 0.65), (0.2, 1.2), (0.01, 0), (2, 2)):
        for density in (0.25, 1, 2):
            output = work / 'probe.f32'
            subprocess.run([str(args.probe.resolve()), str(root / 'engine/resources'),
                            str(output), str(area), str(blur), str(int(layered)), str(density)], check=True)
            native = np.fromfile(output, dtype=np.float32).reshape(512, 512, 3)
            params = GrainParams(particle_area_um2=area, blur=blur, sublayers_active=layered)
            reference = apply_grain(np.full((512, 512, 3), density, dtype=float), 1.75,
                                    params, curves, layers, profile['info']['type'], use_fast_stats=False)
            m, s, k, c, se = stats(native)
            rm, rs, rk, rc, rse = stats(reference)
            mean_bar = np.maximum(0.004, 6 * np.sqrt(se**2 + rse**2))
            errors = [float(np.max(np.abs(m-rm))), float(np.max(np.abs(s/rs-1))),
                      float(np.max(np.abs(k-rk))), float(np.max(np.abs(c-rc)))]
            ok = bool(np.all(np.abs(m-rm) <= mean_bar)) and all(v <= bar for v, bar in zip(errors[1:], (0.08, 0.25, 0.05)))
            failures += not ok
            print('PASS' if ok else 'FAIL', 'layered', layered, 'area', area, 'blur', blur,
                  'density', density, 'mean/std-ratio/skew/lag1 errors', errors,
                  'mean bars', mean_bar.tolist(), flush=True)
temporary.cleanup()
print('Failures:', failures)
raise SystemExit(bool(failures))

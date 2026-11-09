# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Actual Metal output: the broad halo follows all four sides of a hole."""
import numpy as np
from spk_ctypes import Engine


def runs(indices):
    return [r for r in np.split(indices, np.flatnonzero(np.diff(indices) > 1) + 1) if len(r)]


with Engine() as engine:
    options = dict(auto_exposure=False, grain_active=False, glare_active=False,
                   halation_active=False, overscan_active=True, overscan_format='135_full_67',
                   overscan_camera_seed=19, overscan_frame_seed=5, overscan_fog=0.,
                   overscan_leaks=0., output_color_space='sRGB', scratch_active=True,
                   scratch_strength=0., scratch_long_lines=0., scratch_edge_marks=0.,
                   scratch_leak_strength=.6, scratch_seed=17)
    source = np.full((1000, 2000, 3), .02, np.float32)

    def render(**extra):
        with engine.open(source, dict(options, **extra)) as session:
            pixels, _ = session.render('full')
            return pixels, session.overscan_geometry()

    white, g = render(overscan_holes='white', scratch_leak_strength=0.)
    holes = np.all(white[..., :3] > 65000, axis=2)
    clean_rim, _ = render(overscan_holes='black', scratch_leak_strength=0.)
    thin, _ = render(overscan_holes='black', scratch_leak_spread=0.)
    broad, _ = render(overscan_holes='black', scratch_leak_spread=.5)
    halo_only, _ = render(overscan_holes='black', scratch_leak_style='halo', scratch_leak_spread=.5)
    halo_delta = np.max(abs(halo_only[..., :3].astype(float) - clean_rim[..., :3]), axis=2)
    delta = np.max(abs(broad[..., :3].astype(float) - thin[..., :3]), axis=2)
    offset = max(3, round(.13 / g['mm_per_px']))
    rim_delta = np.max(abs(thin[..., :3].astype(float) - clean_rim[..., :3]), axis=2)
    samples = []
    rim_samples = []
    halo_samples = []
    rim_widths = []
    row_directions = []
    for row in runs(np.flatnonzero(holes.any(axis=1))):
        y0, y1 = row[0], row[-1]
        if y0 < max(9, offset + 2) or y1 >= holes.shape[0] - max(9, offset + 2):
            continue
        first = len(rim_samples)
        for col in runs(np.flatnonzero(holes[y0:y1 + 1].any(axis=0))):
            x0, x1 = col[0], col[-1]
            if x0 < max(9, offset + 2) or x1 >= holes.shape[1] - max(9, offset + 2):
                continue
            cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
            points = [(cx, y0-offset), (cx, y1+offset),
                      (x0-offset, cy), (x1+offset, cy)]
            samples.append([np.median(delta[y-1:y+2, x-1:x+2]) for x, y in points])
            halo_samples.append([np.median(halo_delta[y-1:y+2, x-1:x+2]) for x, y in points])
            # Actual output, not a duplicated noise formula: compare lit arcs on each side.
            sides = [rim_delta[max(0,y0-2):y0+3, x0:x1+1],
                     rim_delta[y1-2:y1+3, x0:x1+1],
                     rim_delta[y0:y1+1, max(0,x0-2):x0+3],
                     rim_delta[y0:y1+1, x1-2:x1+3]]
            rim_samples.append([s.max() for s in sides])
            # Integrated width / peak, on resolved straight edges away from corners.
            xa, xb = x0+(x1-x0)//4, x1-(x1-x0)//4
            ya, yb = y0+(y1-y0)//4, y1-(y1-y0)//4
            for horizontal, edge, lo, hi in ((True,y0,xa,xb), (True,y1,xa,xb),
                                               (False,x0,ya,yb), (False,x1,ya,yb)):
                strip = rim_delta[edge-2:edge+3,lo:hi] if horizontal else rim_delta[lo:hi,edge-2:edge+3]
                iy, ix = np.unravel_index(strip.argmax(), strip.shape)
                along = lo + (ix if horizontal else iy)
                profile = rim_delta[edge-8:edge+9,along] if horizontal else rim_delta[along,edge-8:edge+9]
                if profile.max() > 1028:
                    rim_widths.append(profile.sum()/profile.max())
        totals = np.sum(rim_samples[first:], axis=0)
        vector = np.array([totals[3]-totals[2], totals[1]-totals[0]])
        row_directions.append(vector / max(np.linalg.norm(vector), 1.))
    assert len(samples) > 8, 'no independently located holes to measure'
    agreement = np.dot(row_directions[0], row_directions[-1])
    print('upper/lower row lighting agreement:', agreement, flush=True)
    assert agreement > .8, 'the two rows do not share a main light direction'
    rim_samples = np.array(rim_samples)
    for name, readings in [('rim', rim_samples), ('halo', np.array(halo_samples))]:
        peaks = readings.max(axis=1)
        alignment = np.bincount(readings.argmax(axis=1), minlength=4).max() / len(readings)
        print(name, 'weakest hole:', peaks.min(), 'peak max/min:', peaks.max()/max(peaks.min(),1.),
              'same-side fraction:', alignment, flush=True)
        assert peaks.min() > 1000, (name, 'some holes have almost no light')
        assert peaks.max()/peaks.min() < 2., (name, 'hole strengths vary too much')
        assert alignment >= .9, (name, 'individual holes point in different directions')
    contrast = (rim_samples.max(axis=1) - rim_samples.min(axis=1)) / np.maximum(rim_samples.max(axis=1), 1.)
    print('median within-hole rim contrast:', np.median(contrast), flush=True)
    assert np.median(contrast) > .7, 'rim is still a nearly complete uniform outline'
    assert len(rim_widths) > 8, 'too few visible arcs to measure thickness'
    widths = np.percentile(rim_widths, [10, 90])
    print('visible arc widths, p10/p90 (pixels):', widths, flush=True)
    # A shared base size now varies only ±20% per hole; direction still tapers each arc.
    assert widths[1]/widths[0] > 1.05, 'visible arcs lost all thickness variation'
    sides = np.median(samples, axis=0)
    print('four-side broad halo (16-bit output):', sides, flush=True)
    # Local lights intentionally leave some sides dark; each side must still be reachable.
    assert np.max(samples, axis=0).min() > 257, 'broad halo cannot reach all four sides'
    correlation = np.corrcoef(rim_samples.max(axis=1), np.max(samples, axis=1))[0, 1]
    print('rim/halo per-hole correlation:', correlation, flush=True)
    # With bounded hole strengths, correlation is dominated by pixel alignment/nonlinear response.
    agreement = np.mean(rim_samples.argmax(axis=1) == np.array(halo_samples).argmax(axis=1))
    assert agreement >= .9, 'rim and halo do not share their main lit side'
    inside = holes.copy()
    for axis in (0, 1):
        for shift in (-8, 8):
            inside &= np.roll(holes, shift, axis=axis)
    assert delta[inside].max() <= 2, 'broad halo filled a hole interior'
    print('PASS full-contour halo and clean hole interiors', flush=True)

    # Compare the actual live/full path, including nonlinear film/print response.
    source = np.full((1800, 3600, 3), .02, np.float32)
    clean = {}
    with engine.open(source, dict(options, overscan_holes='black', scratch_leak_strength=0.)) as session:
        for tier in ('live', 'full'):
            clean[tier] = session.render(tier)[0]
    for spread, tolerance in ((0., .25), (.5, .15)):
        gains = []
        with engine.open(source, dict(options, overscan_holes='black', scratch_leak_spread=spread)) as session:
            for tier in ('live', 'full'):
                image, _ = session.render(tier)
                gains.append(np.abs(image[..., :3].astype(float) - clean[tier][..., :3]).mean())
            exported, _ = session.render('full', reprint=True)
            assert np.array_equal(image, exported), 'reprint changed the halo'
        # A subpixel line has a looser visual bar than its resolved broad halo.
        ratio = gains[0] / gains[1]
        assert abs(ratio - 1.) < tolerance, ('preview/full halo strength drift', spread, ratio)
        print('PASS live/full gain and reprint:', spread, round(ratio, 3), flush=True)

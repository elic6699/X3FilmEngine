# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Six appearances through the shipping C ABI/Metal path, not noise reimplementations."""
import numpy as np
from spk_ctypes import Engine, EngineError

STYLES = ('rim', 'halo', 'shadow', 'band', 'streak', 'burn')
OPTIONS = dict(auto_exposure=False, grain_active=False, glare_active=False,
               halation_active=False, overscan_active=True, overscan_format='135_full_67',
               overscan_camera_seed=19, overscan_frame_seed=5, overscan_fog=0.,
               overscan_leaks=0., output_color_space='sRGB', scratch_active=True,
               scratch_strength=0., scratch_long_lines=0., scratch_edge_marks=0.,
               scratch_dust_amount=0., scratch_seed=17, scratch_leak_spread=.7)
with Engine() as engine:
    source = np.full((900, 1800, 3), .035, np.float32)

    def render(**extra):
        with engine.open(source, dict(OPTIONS, **extra)) as session:
            return session.render('full')[0]

    schema = engine.params_schema()
    fields = {f['name']: f for f in schema['fields']}
    assert fields['scratch_leak_style']['default'] == 'rim'
    assert fields['scratch_leak_style']['layer'] == 'shoot'
    for invalid in ('unknown', '', 1, True):
        try:
            render(scratch_leak_style=invalid)
        except EngineError:
            pass
        else:
            raise AssertionError(('invalid style accepted', invalid))
    clean = render(scratch_leak_strength=0.)
    white = render(scratch_leak_strength=0., overscan_holes='white')
    interior = np.all(white[..., :3] > 65000, axis=2)
    for axis in (0, 1):
        for shift in (-5, 5):
            interior &= np.roll(np.all(white[..., :3] > 65000, axis=2), shift, axis=axis)
    assert interior.any()
    signatures = []
    for style in STYLES:
        assert np.array_equal(clean, render(scratch_leak_style=style, scratch_leak_strength=0.)), style
        lo = render(scratch_leak_style=style, scratch_leak_strength=.25)
        hi = render(scratch_leak_style=style, scratch_leak_strength=1.)
        repeat = render(scratch_leak_style=style, scratch_leak_strength=1.)
        assert np.array_equal(hi, repeat), ('unstable saved seed', style)
        assert not np.array_equal(hi, render(scratch_leak_style=style, scratch_leak_strength=1., scratch_seed=18)), style
        assert np.array_equal(hi[interior], clean[interior]), ('filled hole interior', style)
        delta = hi[..., :3].astype(float) - clean[..., :3]
        gain = np.abs(delta).mean()
        assert gain > np.abs(lo[..., :3].astype(float)-clean[..., :3]).mean()*1.1, ('strength has too little range', style)
        assert (delta.mean() < -10 if style == 'shadow' else delta.mean() > 10), ('wrong light/shadow sign', style)
        changed = np.max(np.abs(delta), axis=2) > 514
        if style != 'rim':
            assert changed.mean() > .004, ('new maximum is barely visible', style)
        assert all(not np.array_equal(hi, previous) for previous in signatures), ('duplicate style', style)
        signatures.append(hi)
        with engine.open(source, dict(OPTIONS, scratch_leak_strength=1.)) as session:
            session.render('full')
            reply = session.set_params({'scratch_leak_style': style})
            switched, _ = session.render('full')
            assert np.array_equal(hi, switched), ('style switch kept a stale negative', style)
            if style != 'rim':
                assert reply['invalidated'] == 'shoot', ('wrong invalidation layer', style)
        print('PASS', style, 'mean 16-bit change', round(gain, 1), 'changed area', round(changed.mean(), 3), flush=True)

    # Portrait/paired strips exercise the same film-coordinate effects and hole mask.
    for fmt, pair, shape in [('135', False, (1200, 800)), ('135_half', True, (800, 1233))]:
        source = np.full((*shape, 3), .035, np.float32)
        for style in STYLES:
            with engine.open(source, dict(OPTIONS, overscan_format=fmt, overscan_pair=pair,
                                          scratch_leak_style=style, scratch_leak_strength=.75)) as session:
                full, _ = session.render('full')
                exported, _ = session.render('full', reprint=True)
                assert np.array_equal(full, exported), ('reprint mismatch', fmt, style)
        print('PASS portrait/pair and reprint', fmt, pair, flush=True)

    # Spatial effects must survive preview resizing with comparable total strength.
    source = np.full((1800, 3600, 3), .035, np.float32)
    clean = {}
    with engine.open(source, dict(OPTIONS, scratch_leak_strength=0.)) as session:
        for tier in ('live', 'full'):
            clean[tier] = session.render(tier)[0]
    for style in STYLES[1:]:
        gains = []
        with engine.open(source, dict(OPTIONS, scratch_leak_style=style, scratch_leak_strength=.75)) as session:
            for tier in ('live', 'full'):
                frame, _ = session.render(tier)
                gains.append(np.abs(frame[..., :3].astype(float)-clean[tier][..., :3]).mean())
        ratio = gains[0]/gains[1]
        assert abs(ratio-1.) < .15, ('live/full drift', style, ratio)
        print('PASS live/full', style, round(ratio, 3), flush=True)

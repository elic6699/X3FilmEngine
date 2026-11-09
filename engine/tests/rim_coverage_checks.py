# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Actual Metal fine-rim coverage: default arc through a four-sided rim."""
import numpy as np
from spk_ctypes import Engine, EngineError


def runs(indices):
    return [r for r in np.split(indices,np.flatnonzero(np.diff(indices)>1)+1) if len(r)]


BASE = dict(auto_exposure=False,grain_active=False,glare_active=False,halation_active=False,
            overscan_active=True,overscan_format='135_full_67',overscan_camera_seed=19,
            overscan_frame_seed=5,overscan_fog=0.,overscan_leaks=0.,output_color_space='sRGB',
            scratch_active=True,scratch_strength=0.,scratch_long_lines=0.,scratch_edge_marks=0.,
            scratch_dust_amount=0.,scratch_seed=17,scratch_leak_style='rim',
            scratch_leak_strength=1.,scratch_leak_spread=0.,overscan_holes='black')
with Engine() as engine:
    schema = {f['name']: f for f in engine.params_schema()['fields']}
    assert schema['scratch_leak_coverage']['default'] == 0.
    assert schema['scratch_leak_coverage']['range'] == [0,1]
    source = np.full((1000,2000,3),.02,np.float32)

    def render(**extra):
        with engine.open(source,dict(BASE,**extra)) as session:
            return session.render('full')[0]

    for value in [-.1,1.1,True,'1']:
        try:render(scratch_leak_coverage=value)
        except EngineError:pass
        else:raise AssertionError(('invalid coverage accepted',value))
    clean = render(scratch_leak_strength=0.)
    assert np.array_equal(clean,render(scratch_leak_strength=0.,scratch_leak_coverage=1.))
    arc = render()
    assert np.array_equal(arc,render(scratch_leak_coverage=0.)), 'default changed the one-sided arc'
    frames = [arc] + [render(scratch_leak_coverage=v) for v in [.25,.5,.75,1.]]
    white = render(scratch_leak_strength=0.,overscan_holes='white')
    holes = np.all(white[...,:3]>65000,axis=2)
    interior = holes.copy()
    for axis in [0,1]:
        for shift in [-5,5]:interior &= np.roll(holes,shift,axis=axis)
    metrics = []
    for image in frames:
        assert np.array_equal(image[interior],clean[interior]),'rim filled hole interiors'
        delta = np.max(abs(image[...,:3].astype(float)-clean[...,:3]),axis=2)
        sides = []
        for row in runs(np.flatnonzero(holes.any(axis=1))):
            y0,y1=row[0],row[-1]
            if y0<8 or y1>=holes.shape[0]-8:continue
            for col in runs(np.flatnonzero(holes[y0:y1+1].any(axis=0))):
                x0,x1=col[0],col[-1]
                if x0<8 or x1>=holes.shape[1]-8:continue
                xa,xb=x0+(x1-x0)//4,x1-(x1-x0)//4
                ya,yb=y0+(y1-y0)//4,y1-(y1-y0)//4
                sides.append([delta[y0-3:y0+4,xa:xb].max(),delta[y1-3:y1+4,xa:xb].max(),
                              delta[ya:yb,x0-3:x0+4].max(),delta[ya:yb,x1-3:x1+4].max()])
        metrics.append(np.array(sides))
    # Count independently located straight sides, not a shader formula copied into the test.
    fractions = [(m>2000).mean() for m in metrics]
    print('visible hole-side fractions at 0/25/50/75/100%:',fractions,flush=True)
    assert fractions[0]<.65,'one-sided preset already covers almost every side'
    assert fractions[-1]==1.,'100% coverage leaves a side dark'
    assert np.all(np.diff(fractions)>=0),'coverage does not increase progressively'
    assert np.min(metrics[-1])>2000,'some hole lacks a complete rim'
    assert np.median(metrics[-1].max(axis=1)/metrics[-1].min(axis=1))>1.1,'full rim lost all directional bias'
    assert np.array_equal(frames[-1],render(scratch_leak_coverage=1.)), 'coverage is not repeatable'
    assert not np.array_equal(frames[-1],render(scratch_leak_coverage=1.,scratch_seed=18))
    with engine.open(source,BASE) as session:
        session.render('full')
        reply=session.set_params({'scratch_leak_coverage':1.})
        assert reply['invalidated']=='shoot'
        image,_=session.render('full')
        exported,_=session.render('full',reprint=True)
        assert np.array_equal(image,frames[-1]) and np.array_equal(image,exported),'stale coverage after switch/export'
    for style in ['halo','shadow','band','streak','burn']:
        a=render(scratch_leak_style=style,scratch_leak_coverage=0.)
        b=render(scratch_leak_style=style,scratch_leak_coverage=1.)
        assert np.array_equal(a,b),('coverage affected another style',style)
    print('PASS defaults, ranges, all sides, seed, clear holes, shoot invalidation, reprint and other styles',flush=True)
    source=np.full((1800,3600,3),.02,np.float32)
    base={}
    with engine.open(source,dict(BASE,scratch_leak_strength=0.)) as session:
        for tier in ['live','full']:base[tier]=session.render(tier)[0]
    gains=[]
    with engine.open(source,dict(BASE,scratch_leak_coverage=1.)) as session:
        for tier in ['live','full']:
            image,_=session.render(tier)
            gains.append(np.abs(image[...,:3].astype(float)-base[tier][...,:3]).mean())
    ratio=gains[0]/gains[1]
    assert abs(ratio-1)<.25,('live/full full-rim drift',ratio)
    print('PASS live/full full-rim strength',round(ratio,3),flush=True)

# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Actual Metal rim softening with no outer halo: wider light, retained core."""
import numpy as np
from spk_ctypes import Engine, EngineError

BASE=dict(auto_exposure=False,grain_active=False,glare_active=False,halation_active=False,
          overscan_active=True,overscan_format='135_full_67',overscan_camera_seed=19,
          overscan_frame_seed=5,overscan_fog=0.,overscan_leaks=0.,output_color_space='sRGB',
          scratch_active=True,scratch_strength=0.,scratch_edge_marks=0.,scratch_dust_amount=0.,
          scratch_seed=17,scratch_leak_style='rim',scratch_leak_strength=.75,
          scratch_leak_coverage=1.,scratch_leak_spread=0.,overscan_holes='black')
with Engine() as engine:
    fields={f['name']:f for f in engine.params_schema()['fields']}
    assert fields['scratch_leak_softness']['default']==0.
    assert fields['scratch_leak_softness']['range']==[0,1]
    source=np.full((1000,2000,3),.02,np.float32)
    def render(**extra):
        with engine.open(source,dict(BASE,**extra)) as session:return session.render('full')[0]
    for value in [-.1,1.1,True,'1']:
        try:render(scratch_leak_softness=value)
        except EngineError:pass
        else:raise AssertionError(('invalid softness accepted',value))
    clean=render(scratch_leak_strength=0.)
    hard=render()
    assert np.array_equal(hard,render(scratch_leak_softness=0.))
    assert np.array_equal(clean,render(scratch_leak_strength=0.,scratch_leak_softness=1.))
    white=render(scratch_leak_strength=0.,overscan_holes='white')
    holes=np.all(white[...,:3]>65000,axis=2)
    interior=holes.copy()
    for axis in [0,1]:
        for shift in [-5,5]:interior &= np.roll(holes,shift,axis=axis)
    # Locate actual straight hole edges independently from the white-backing render.
    def runs(a):return [r for r in np.split(a,np.flatnonzero(np.diff(a)>1)+1) if len(r)]
    points=[]
    for row in runs(np.flatnonzero(holes.any(axis=1))):
        y0,y1=row[0],row[-1]
        if y0<12 or y1>=holes.shape[0]-12:continue
        for col in runs(np.flatnonzero(holes[y0:y1+1].any(axis=0))):
            x0,x1=col[0],col[-1]
            if x0<12 or x1>=holes.shape[1]-12:continue
            points.extend([((x0+x1)//2,y0-6),((x0+x1)//2,y1+6),(x0-6,(y0+y1)//2),(x1+6,(y0+y1)//2)])
    assert len(points)>20
    gains=[];outside=[]
    for softness in [0.,.25,.5,.75,1.]:
        image=render(scratch_leak_softness=softness)
        delta=np.max(abs(image[...,:3].astype(float)-clean[...,:3]),axis=2)
        gains.append(delta.mean());outside.append(np.median([delta[y,x] for x,y in points]))
        assert np.array_equal(image[interior],clean[interior]),'softening filled a hole'
    print('total change / outer-side light at 0/25/50/75/100:',gains,outside,flush=True)
    assert np.all(np.diff(gains)>0),'softening dimmed the light'
    assert outside[-1]>outside[0]+2000,'blur did not extend beyond the narrow rim'
    assert np.max(delta)>.7*np.max(np.abs(hard[...,:3].astype(float)-clean[...,:3])),'bright core was lost'
    assert np.array_equal(image,render(scratch_leak_softness=1.))
    assert not np.array_equal(image,render(scratch_leak_softness=1.,scratch_seed=18))
    with engine.open(source,BASE) as session:
        session.render('full')
        reply=session.set_params({'scratch_leak_softness':1.});assert reply['invalidated']=='shoot'
        full,_=session.render('full');exported,_=session.render('full',reprint=True)
        assert np.array_equal(image,full) and np.array_equal(full,exported)
    for style in ['halo','shadow','band','streak','burn']:
        assert np.array_equal(render(scratch_leak_style=style,scratch_leak_softness=0.),render(scratch_leak_style=style,scratch_leak_softness=1.)),style
    print('PASS core, independent spread=0 blur, defaults, seed, holes, validation, style gating and export',flush=True)
    for fmt,pair,shape in [('135',False,(1200,800)),('135_half',True,(800,1233))]:
        source=np.full((*shape,3),.02,np.float32)
        options=dict(BASE,overscan_format=fmt,overscan_pair=pair,scratch_leak_softness=1.)
        with engine.open(source,options) as session:
            full,_=session.render('full');exported,_=session.render('full',reprint=True)
            assert np.array_equal(full,exported),('portrait/pair softened export drift',fmt)
        with engine.open(source,dict(options,scratch_leak_softness=0.)) as session:hard,_=session.render('full')
        assert not np.array_equal(full,hard),('softness inactive in orientation/format',fmt)
        print('PASS softened portrait/pair',fmt,pair,flush=True)
    source=np.full((1800,3600,3),.02,np.float32)
    clean={}
    with engine.open(source,dict(BASE,scratch_leak_strength=0.)) as session:
        for tier in ['live','full']:clean[tier]=session.render(tier)[0]
    gains=[]
    with engine.open(source,dict(BASE,scratch_leak_softness=1.)) as session:
        for tier in ['live','full']:
            image,_=session.render(tier);gains.append(np.abs(image[...,:3].astype(float)-clean[tier][...,:3]).mean())
    ratio=gains[0]/gains[1];assert abs(ratio-1)<.15,ratio
    print('PASS softened live/full strength',round(ratio,3),flush=True)

# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Negative dust: actual Metal output, saved seed, scope and zero bypass."""
import numpy as np
from spk_ctypes import Engine, EngineError

BASE = dict(auto_exposure=False, grain_active=False, glare_active=False, halation_active=False,
            overscan_active=True, overscan_format='135', overscan_camera_seed=19,
            overscan_frame_seed=5, overscan_fog=0., overscan_leaks=0., overscan_holes='white',
            scratch_active=True, scratch_strength=0., scratch_edge_marks=0., scratch_long_lines=0.,
            scratch_dust_amount=0., scratch_dust_strength=.8, scratch_dust_size=.7,
            scratch_seed=17, output_color_space='sRGB')
with Engine() as engine:
    def render(w=1200,h=800,**extra):
        with engine.open(np.full((h,w,3),.18,np.float32),dict(BASE,**extra)) as session:
            image,_=session.render('full');g=session.overscan_geometry()
        return image,g
    for w,h,fmt,pair in [(1200,800,'135',False),(800,1200,'135',False),
                          (900,1200,'135_half',False),(1850,1200,'135_half',True),
                          (1390,700,'135_full_67',False)]:
        opt=dict(overscan_format=fmt,overscan_pair=pair)
        bare,g=render(w,h,**opt)
        def dust(**kw):return render(w,h,**dict(opt,scratch_dust_amount=1.,**kw))[0]
        zero=dust(scratch_dust_strength=0.)
        assert np.array_equal(bare,zero),'zero strength changed output'
        disabled=dust(scratch_active=False)
        assert np.array_equal(bare,disabled),'inactive scratch card changed output'
        border=dust()
        whole=dust(scratch_dust_full_frame=True)
        assert np.array_equal(whole,dust(scratch_dust_full_frame=True)),'pattern rerolled'
        assert not np.array_equal(whole,dust(scratch_dust_full_frame=True,scratch_seed=18)),'seed ignored'
        assert not np.array_equal(whole,dust(scratch_dust_full_frame=True,scratch_dust_size=.1)),'size ignored'
        diff=whole[...,:3].astype(int)-bare[...,:3].astype(int)
        assert (diff>100).any(),'dust invisible'
        assert np.percentile(diff.max(axis=2)[diff.max(axis=2)>100],90)>500,'dust too faint'
        # The existing scanner unsharp mask can undershoot around pale dust.
        pale=diff.max(axis=2)>200
        near=pale.copy()
        for dy in range(-5,6):
            for dx in range(-5,6): near |= np.roll(pale,(dy,dx),(0,1))
        assert not ((diff.min(axis=2)<-200)&~near).any(),'isolated dark dust core'
        changed=np.max(abs(border[...,:3].astype(int)-bare[...,:3].astype(int)),axis=2)>20
        assert changed.any(),'border-only dust invisible'
        holes=np.all(bare[...,:3]>65000,axis=2)
        inside=holes.copy()
        for axis in (0,1):
            for shift in (-3,3):inside &= np.roll(holes,shift,axis=axis)
        assert inside.any()
        assert not (np.max(abs(diff),axis=2)[inside]>2).any(),'dust entered empty holes'
        if fmt!='135_full_67':
            ys,xs=np.indices(bare.shape[:2]);px=np.stack([(xs+.5)/bare.shape[1],(ys+.5)/bare.shape[0]],axis=-1)
            for gate in g['gates']:
                q=np.array(gate).reshape(4,2);cr=[]
                for i in range(4):
                    e=q[(i+1)%4]-q[i]
                    cr.append(((px[...,0]-q[i,0])*e[1]-(px[...,1]-q[i,1])*e[0])/np.linalg.norm(e))
                cr=np.stack(cr);picture=np.all(cr>.015,axis=0)|np.all(cr<-.015,axis=0)
                assert not changed[picture].any(),'border-only dust entered picture'
                assert (np.max(abs(diff),axis=2)[picture]>100).any(),'whole-film dust omitted picture'
        print('PASS dust scope/zeros/seed/size/negative appearance/empty holes:',fmt,w,h,pair,flush=True)
    sparse,_=render(scratch_dust_amount=.4,scratch_dust_full_frame=True)
    dense,_=render(scratch_dust_amount=1.,scratch_dust_full_frame=True)
    bare,_=render()
    low=np.max(abs(sparse.astype(int)-bare.astype(int)),axis=2)>100
    high=np.max(abs(dense.astype(int)-bare.astype(int)),axis=2)>100
    assert high.sum()>low.sum() and not (low & ~high).any(),'amount moved existing dust'
    with engine.open(np.full((800,1200,3),.18,np.float32),dict(BASE,scratch_dust_amount=1.,scratch_dust_full_frame=True)) as s:
        full,_=s.render('full');again,_=s.render('full',reprint=True)
        assert np.array_equal(full,again),'reprint changed dust pattern'
        s.set_params({'scratch_dust_amount':0.})
        cleared,_=s.render('full')
        assert np.array_equal(cleared,bare),'turning dust off left a cached pattern'
        s.set_params({'scratch_dust_amount':1.})
        restored,_=s.render('full')
        assert np.array_equal(restored,full),'editing amount rerolled the seed'
    # Compare physical locations at different raster sizes with 2px AA tolerance.
    small0,_=render(600,400)
    small1,_=render(600,400,scratch_dust_amount=1.,scratch_dust_full_frame=True)
    low=(small1[...,:3].astype(int)-small0[...,:3].astype(int)).max(axis=2)>400
    high=(dense[...,:3].astype(int)-bare[...,:3].astype(int)).max(axis=2)>400
    near=high.copy()
    for dy in range(-3,4):
        for dx in range(-3,4):near |= np.roll(high,(dy,dx),(0,1))
    ys=np.minimum(((np.arange(low.shape[0])+.5)*high.shape[0]/low.shape[0]).astype(int),high.shape[0]-1)
    xs=np.minimum(((np.arange(low.shape[1])+.5)*high.shape[1]/low.shape[1]).astype(int),high.shape[1]-1)
    assert low.sum()>20 and near[np.ix_(ys,xs)][low].mean()>.95,'resizing moved dust locations'
    particles,_=render(scratch_dust_amount=1.,scratch_dust_full_frame=True,scratch_dust_fiber_ratio=0.)
    fibers,_=render(scratch_dust_amount=1.,scratch_dust_full_frame=True,scratch_dust_fiber_ratio=1.)
    assert not np.array_equal(particles,fibers),'fiber ratio has no effect'
    assert np.array_equal(fibers,render(scratch_dust_amount=1.,scratch_dust_full_frame=True,scratch_dust_fiber_ratio=1.)[0]),'fiber selection is unstable'
    pale=(fibers[...,:3].astype(int)-bare[...,:3].astype(int)).max(axis=2)>500
    # A connected visible line must be longer than any single compact dust core.
    seen=np.zeros_like(pale);longest=0
    for y,x in zip(*np.nonzero(pale)):
        if seen[y,x]:continue
        stack=[(int(y),int(x))];seen[y,x]=True;points=[]
        while stack:
            yy,xx=stack.pop();points.append((yy,xx))
            for dy,dx in ((1,0),(-1,0),(0,1),(0,-1),(1,1),(-1,-1),(1,-1),(-1,1)):
                ny,nx=yy+dy,xx+dx
                if 0<=ny<pale.shape[0] and 0<=nx<pale.shape[1] and pale[ny,nx] and not seen[ny,nx]:
                    seen[ny,nx]=True;stack.append((ny,nx))
        spans=np.ptp(np.array(points),axis=0)
        longest=max(longest,int(spans.max()))
    assert longest>15,('no elongated fiber visible',longest)
    try:render(scratch_dust_fiber_ratio=1.1)
    except EngineError:pass
    else:raise AssertionError('out-of-range fiber ratio accepted')
    print('PASS grayscale particles/fibers, visible long lines and ratio validation:',longest,flush=True)
    try:render(scratch_dust_amount=1.1)
    except EngineError:pass
    else:raise AssertionError('out-of-range dust accepted')
    print('PASS density nesting, reprint equality and validation',flush=True)

# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Full-width 135: source exposure reaches the hole rows; holes stay empty."""
import numpy as np
from spk_ctypes import Engine

BASE=dict(auto_exposure=False,grain_active=False,glare_active=False,halation_active=False,
          overscan_active=True,overscan_format='135_full_67',overscan_camera_seed=19,overscan_frame_seed=5,
          overscan_fog=0.,overscan_leaks=0.,overscan_holes='black',output_color_space='sRGB')
with Engine() as engine:
    def render(image,**extra):
        with engine.open(image,dict(BASE,**extra)) as session:
            out,_=session.render('full');geometry=session.overscan_geometry()
        return out,geometry

    for portrait in (False,True):
        w,h=(1390,700) if not portrait else (700,1390)
        source=np.full((h,w,3),.18,np.float32)
        altered=source.copy()
        # Leave the outermost source pixels unchanged: border-pixel repetition must fail this test.
        across=w if portrait else h
        for start,end in ((.25,.95),(2.6,4.1)):
            lo_source,hi_source=round(start/35*across),round(end/35*across)
            if portrait: altered[:,lo_source:hi_source,:]=(.4,.06,.02)
            else: altered[lo_source:hi_source,:,:]=(.4,.06,.02)
        bare,g=render(source)
        changed,g1=render(altered)
        assert g['format']=='135_full_67' and g['vertical']==portrait
        assert abs(min(bare.shape[:2])*g['mm_per_px']-35)<.12
        assert g==g1,'source content moved the film geometry'
        assert bare.shape[:2]==(h,w), 'full-width mode added frame-gap padding'
        lum=bare[...,:3].astype(float).mean(axis=2)
        middle=lum[h//3:2*h//3,w//3:2*w//3]
        edges=(lum[h//3:2*h//3,:2],lum[h//3:2*h//3,-2:],
               lum[:2,w//3:2*w//3],lum[-2:,w//3:2*w//3])
        assert all(np.median(edge)>0.75*np.median(middle) for edge in edges), 'full-width mode left a dark frame edge'
        diff=np.max(abs(changed[:,:,:3].astype(int)-bare[:,:,:3].astype(int)),axis=2)
        # A half-millimetre outer band lies beyond the entire perforation row.
        lo,hi=round(.35/g['mm_per_px']),round(.85/g['mm_per_px'])
        band=diff[:,lo:hi] if portrait else diff[lo:hi,:]
        assert np.median(band)>3000,'the source did not expose the film outside the perforations'
        # The bright white-backing control locates real hole interiors without copying geometry math.
        white,_=render(source,overscan_holes='white')
        holes=np.all(white[...,:3]>65000,axis=2)
        assert holes.sum()>1000
        between=np.zeros_like(holes)
        p0,p1=round(2.8/g['mm_per_px']),round(3.9/g['mm_per_px'])
        if portrait: between[:,p0:p1]=True
        else: between[p0:p1,:]=True
        between &= ~holes
        assert np.median(diff[between])>3000, 'scene content did not reach the film between the holes'
        inside=holes.copy()
        for axis in (0,1):
            for shift in (-6,6): inside &= np.roll(holes,shift,axis=axis)
        assert not (diff[inside]>2).any(),'image content filled actual hole interiors'
        np.testing.assert_allclose(np.median(bare[:,:,:3][inside],axis=0),np.array([4,10,13])*257,atol=6)
        # The ordinary XPan still masks the source above/below its 24 mm gate.
        xp_w,xp_h=(1300,480) if not portrait else (480,1300)
        xpan,_=render(np.full((xp_h,xp_w,3),.18,np.float32),overscan_format='135_xpan')
        full_band=bare[:,lo:hi,:3] if portrait else bare[lo:hi,:,:3]
        xp_band=xpan[:,lo:hi,:3] if portrait else xpan[lo:hi,:,:3]
        assert np.median(full_band)>3*np.median(xp_band),'full-width exposure behaves like a normal masked panorama'
        filed,gf=render(source,overscan_mode='filed')
        assert filed.shape[:2]==(h,w),'filed full-width mode added frame-gap padding'
        print('PASS full-strip source exposure, empty holes, XPan separation and filed width:',portrait,flush=True)

    source=np.full((700,1390,3),.18,np.float32)
    base,_=render(source)
    named,_=render(source,overscan_edge_text='CUSTOM TEST EDGE')
    assert not np.array_equal(base,named),'edge print disappeared from the exposed film'
    # All existing damage controls remain connected, even though the rebate carries scene content.
    for params in (dict(scratch_strength=1.,scratch_long_lines=1.,scratch_edge_marks=0.),
                   dict(scratch_strength=0.,scratch_long_lines=0.,scratch_edge_marks=1.),
                   dict(scratch_strength=0.,scratch_long_lines=0.,scratch_edge_marks=0.,scratch_leak_strength=1.)):
        worn,_=render(source,**dict(params,scratch_active=True,scratch_seed=17))
        assert not np.array_equal(base,worn),('full-width damage control is disconnected',params)
    with engine.open(source,BASE) as session:
        full,_=session.render('full');exported,_=session.render('full',reprint=True)
        assert np.array_equal(full,exported),'reprint/export used a different full-width layout'
    # Source transitions must reach the gate's outer film region without border pixel extension.
    print('PASS edge print, scratch/edge/leak controls and reprint consistency',flush=True)

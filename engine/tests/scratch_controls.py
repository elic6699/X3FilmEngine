# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Seeded film-border scratches: actual Metal output and geometry exclusion."""
import numpy as np
from spk_ctypes import Engine, EngineError

BASE = dict(auto_exposure=False, grain_active=False, glare_active=False, halation_active=False,
            overscan_active=True, overscan_format='135', overscan_fog=0., overscan_leaks=0.,
            film_format_mm=36., scratch_edge_marks=0.)
with Engine() as engine:
    def render(width=1800, height=1200, **extra):
        with engine.open(np.full((height,width,3),.18,np.float32),dict(BASE,**extra)) as s:
            image,_=s.render('full')
            geometry=s.overscan_geometry()
            return image,geometry

    for width,height,fmt,pair in [(1800,1200,'135',False),(1200,1800,'135',False),
                                  (1200,1600,'135_half',False),(1850,1200,'135_half',True)]:
        options=dict(overscan_format=fmt,overscan_pair=pair)
        bare,g=render(width,height,**options)
        zero,_=render(width,height,**options,scratch_active=True,scratch_strength=0.)
        assert np.array_equal(bare,zero),'zero strength altered the output'
        none,_=render(width,height,**options,scratch_active=True,scratch_long_lines=0.)
        assert np.array_equal(bare,none),'zero density altered the output'
        ys,xs=np.indices(bare.shape[:2]);px=np.stack([(xs+.5)/bare.shape[1],(ys+.5)/bare.shape[0]],axis=-1)
        params=dict(options,scratch_active=True,scratch_strength=1.,scratch_long_lines=1.,scratch_seed=17)
        worn,_=render(width,height,**params)
        again,_=render(width,height,**params)
        assert np.array_equal(worn,again),'seeded pattern moved between identical renders'
        assert not np.array_equal(worn,bare),('no visible scratches',fmt)
        changed=np.max(np.abs(worn.astype(int)-bare.astype(int)),axis=2)>2
        for gate in g['gates']:
            q=np.array(gate).reshape(4,2)
            crosses=[]
            for i in range(4):
                edge=q[(i+1)%4]-q[i]
                cross=(px[...,0]-q[i,0])*edge[1]-(px[...,1]-q[i,1])*edge[0]
                crosses.append(cross/np.linalg.norm(edge))
            cr=np.stack(crosses)
            inside=(np.all(cr>.01,axis=0)|np.all(cr<-.01,axis=0))
            assert not changed[inside].any(),('scratch entered picture',fmt)
        hole=np.all(bare[...,:3]>65000,axis=2)
        assert not changed[hole].any(),('scratch entered white perforation',fmt)
        other,_=render(width,height,**dict(params,scratch_seed=18))
        assert not np.array_equal(worn,other),'another seed had no effect'
        print('PASS border-only friction scratches, rotation/pair, zero bypass and repeatability:',fmt,width,height,pair,flush=True)

    white,_=render(overscan_holes='white')
    black,_=render(overscan_holes='black')
    holes=np.all(white[...,:3]>65000,axis=2)
    assert holes.any()
    assert np.max(black[...,:3][holes])<6000, 'near-black backing is too bright'
    srgb_black,_=render(overscan_holes='black',output_color_space='sRGB')
    np.testing.assert_allclose(np.median(srgb_black[...,:3][holes],axis=0), np.array([4,10,13])*257, atol=6)
    print('PASS white/black perforation backgrounds',flush=True)

    for background in ('white','black'):
        options=dict(overscan_holes=background,scratch_active=True,scratch_strength=0.,scratch_long_lines=0.)
        rim0,_=render(**dict(options,scratch_edge_marks=0.))
        rim1,_=render(**dict(options,scratch_edge_marks=1.,scratch_seed=17))
        same,_=render(**dict(options,scratch_edge_marks=1.,scratch_seed=17))
        another,_=render(**dict(options,scratch_edge_marks=1.,scratch_seed=18))
        assert np.array_equal(rim1,same), 'edge marks are not repeatable'
        assert not np.array_equal(rim1,another), 'edge marks did not change with Another'
        changed=np.max(abs(rim1.astype(int)-rim0.astype(int)),axis=2)>2
        assert changed.any(), 'edge marks are invisible with friction off'
        assert np.array_equal(rim0, white if background=='white' else black), 'zero marks changed the baseline'
        if background=='white':
            outward=np.all(rim0[...,:3]<30000,axis=2)&np.all(rim1[...,:3]>45000,axis=2)
            inward=np.all(rim0[...,:3]>60000,axis=2)&np.any(rim1[...,:3]<45000,axis=2)
            assert outward.any(), 'pull wear did not extend the hole outward'
            assert not inward.any(), 'pull wear pushed film material inward into the hole'
            # Hole bands and bounds come from the baseline render, independent of the shader model.
            def runs(indices):
                return [r for r in np.split(indices,np.flatnonzero(np.diff(indices)>1)+1) if len(r)]
            contact=np.zeros_like(holes)
            for row in runs(np.flatnonzero(holes.any(axis=1))):
                y0,y1=int(row[0]),int(row[-1])+1
                for col in runs(np.flatnonzero(holes[y0:y1].any(axis=0))):
                    x0=int(col[0]); cy=(y0+y1)//2
                    contact[max(0,cy-10):cy+11,max(0,x0-10):x0+11]=True
            assert not outward[~contact].any(), 'pull deformation moved away from the left short-edge centre'
            delta=rim1[...,:3].astype(int)-rim0[...,:3].astype(int)
            assert not (delta[~contact]>2).any(), 'edge colour added light outside the pull contact'
            colour=np.max(delta,axis=2)-np.min(delta,axis=2)
            assert (colour[~contact]>20).any(), 'edge colour is only a neutral brightness change'
        # Enclose the local outline changes; picture pixels stay outside.
        around=holes.copy()
        for axis in (0,1):
            expanded=around.copy()
            for shift in range(-24,25): expanded|=np.roll(around,shift,axis=axis)
            around=expanded
        assert not changed[~around].any(), 'edge marks spread beyond the hole neighbourhood'
        center=holes.copy()
        for axis in (0,1):
            for shift in (-8,8): center&=np.roll(holes,shift,axis=axis)
        assert not changed[center].any(), 'edge marks filled a hole interior'
        with engine.open(np.full((1200,1800,3),.18,np.float32),dict(BASE,**dict(options,scratch_edge_marks=1.,scratch_seed=17))) as session:
            full,_=session.render('full'); exported,_=session.render('full',reprint=True)
            assert np.array_equal(full,exported), 'export changed edge marks'
        print('PASS local seeded edge marks, hole interiors, friction independence and export:',background,flush=True)

    for width,height,fmt,pair in [(1400,900,'135',False),(900,1400,'135',False),
                                  (900,1200,'135_half',False),(1400,900,'135_half',True)]:
        opts=dict(overscan_format=fmt,overscan_pair=pair,scratch_active=True,
                  scratch_long_lines=0.,scratch_strength=0.,scratch_edge_marks=0.)
        base,g=render(width,height,**dict(opts,scratch_leak_strength=0.))
        zero,_=render(width,height,**dict(opts,scratch_leak_strength=0.,scratch_leak_spread=1.,scratch_seed=18))
        assert np.array_equal(base,zero), 'zero leak strength changed pixels'
        full,g1=render(width,height,**dict(opts,scratch_leak_strength=1.,scratch_leak_spread=1.,scratch_seed=17))
        again,_=render(width,height,**dict(opts,scratch_leak_strength=1.,scratch_leak_spread=1.,scratch_seed=17))
        another,_=render(width,height,**dict(opts,scratch_leak_strength=1.,scratch_leak_spread=1.,scratch_seed=18))
        narrow,_=render(width,height,**dict(opts,scratch_leak_strength=1.,scratch_leak_spread=0.,scratch_seed=17))
        assert g==g1, 'leak altered gate/film geometry'
        assert np.array_equal(full,again), 'leak seed is not repeatable'
        assert not np.array_equal(full,base), 'leak produced no exposure change'
        assert not np.array_equal(full,another), 'Another did not alter leak exposure'
        wide_changed=np.max(abs(full.astype(int)-base.astype(int)),axis=2)>8
        narrow_changed=np.max(abs(narrow.astype(int)-base.astype(int)),axis=2)>8
        assert wide_changed.sum()>narrow_changed.sum(), 'spread did not expand leak coverage'
        assert not np.array_equal(full,narrow), 'spread had no effect'
        print('PASS seeded perforation leak exposure, spread, zero bypass and geometry:',fmt,width,height,pair,flush=True)
    # Separate rows must both render the shared leak pattern, including formerly lopsided seeds.
    side_opts=dict(scratch_active=True,scratch_long_lines=0.,scratch_strength=0.,scratch_edge_marks=0.,scratch_leak_spread=1.)
    clean,g=render(1200,800,**dict(side_opts,scratch_leak_strength=0.))
    gate=np.array(g['gates'][0]).reshape(4,2)*[clean.shape[1],clean.shape[0]]
    y0,y1=int(gate[:,1].min()),int(gate[:,1].max())
    for seed in (1,2,3,17,18,19):
        leaked,_=render(1200,800,**dict(side_opts,scratch_leak_strength=1.,scratch_seed=seed))
        diff=np.max(abs(leaked[...,:3].astype(int)-clean[...,:3].astype(int)),axis=2)
        upper=diff[max(0,y0-60):y0+40].max()
        lower=diff[y1-40:min(diff.shape[0],y1+60)].max()
        assert min(upper,lower)>257, ('a whole perforation row lost its leak',seed,upper,lower)
        # Local light directions may favour one row; both must still remain reachable.
        print('PASS shared upper/lower leak pattern:',seed,round(upper/257,2),round(lower/257,2),flush=True)

    # With minimum broad spread, the far/outer hole edges cannot be reached by the broad leak.
    rim_off,_=render(1800,1200,overscan_holes='black',scratch_active=True,scratch_long_lines=0.,scratch_strength=0.,
                     scratch_edge_marks=0.,scratch_leak_strength=0.,scratch_leak_spread=0.)
    rim_on,_=render(1800,1200,overscan_holes='black',scratch_active=True,scratch_long_lines=0.,scratch_strength=0.,
                    scratch_edge_marks=0.,scratch_leak_strength=1.,scratch_leak_spread=0.,scratch_seed=17)
    rim_diff=np.max(abs(rim_on[...,:3].astype(int)-rim_off[...,:3].astype(int)),axis=2)
    rows=np.flatnonzero(holes.any(axis=1))
    bands=np.split(rows,np.flatnonzero(np.diff(rows)>1)+1)
    for index,band in enumerate(bands):
        if not len(band): continue
        outer=int(band[0] if index==0 else band[-1])
        assert rim_diff[max(0,outer-4):outer+5].max()>257, ('missing narrow outer-edge highlight',index)
    inside=holes.copy()
    for axis in (0,1):
        for shift in (-8,8): inside&=np.roll(holes,shift,axis=axis)
    assert not (rim_diff[inside]>2).any(), 'narrow highlight filled hole interiors'
    print('PASS narrow exposure highlights on both outer hole edges, minimum spread, clear interiors',flush=True)

    leak_opts=dict(BASE,scratch_active=True,scratch_long_lines=0.,scratch_strength=0.,scratch_edge_marks=0.,
                   scratch_leak_strength=1.,scratch_leak_spread=1.,scratch_seed=17)
    with engine.open(np.full((900,1400,3),.18,np.float32),leak_opts) as session:
        full,_=session.render('full'); exported,_=session.render('full',reprint=True)
        assert np.array_equal(full,exported), 'export changed perforation leak exposure'
    for disabled in (dict(overscan_active=False),dict(overscan_format='120_645'),dict(scratch_active=False)):
        height=667 if disabled.get('overscan_format')=='120_645' else 600
        bare,_=render(900,height,**dict(disabled,scratch_leak_strength=0.))
        leak,_=render(900,height,**dict(dict(scratch_active=True) | disabled,scratch_leak_strength=1.))
        assert np.array_equal(bare,leak), 'leak rendered in a disabled/unsupported mode'
    print('PASS perforation leak export and format gating',flush=True)

    bare,_=render(1200,900,overscan_format='120_645')
    worn,_=render(1200,900,overscan_format='120_645',scratch_active=True,scratch_strength=1.)
    assert np.array_equal(bare,worn),'unsupported 120 format gained scratches'
    bare,_=render(1200,800,overscan_active=False)
    worn,_=render(1200,800,overscan_active=False,scratch_active=True,scratch_strength=1.)
    assert np.array_equal(bare,worn),'scratch rendered without film edge'
    with engine.open(np.full((1200,1800,3),.18,np.float32),dict(BASE,scratch_active=True,scratch_seed=17)) as s:
        full,_=s.render('full'); reprint,_=s.render('full',reprint=True)
        assert np.array_equal(full,reprint),'export changed the scratch pattern'
    for name,values in [('scratch_strength',(-.01,1.01)),('scratch_long_lines',(-.01,1.01)),
                        ('scratch_edge_marks',(-.01,1.01)),('scratch_leak_strength',(-.01,1.01)),
                        ('scratch_leak_spread',(-.01,1.01)),('scratch_seed',(0,2147483648))]:
        for value in values:
            try: render(600,400,**{name:value})
            except EngineError: pass
            else: raise AssertionError(('invalid scratch parameter accepted',name,value))
    print('PASS format gating, export and parameter validation',flush=True)

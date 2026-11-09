# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Screen and geometry checks through the native Metal C ABI; no private assets required."""
from pathlib import Path
import tempfile, struct
import numpy as np
from spk_ctypes import Engine, EngineError

BASE = dict(auto_exposure=False, grain_active=False, glare_active=False, halation_active=False,
            output_color_space='sRGB')
with tempfile.TemporaryDirectory() as directory, Engine() as engine:
    assert 'aging_active' in {f['name'] for f in engine.params_schema()['fields']}
    folder = Path(directory)
    def texture(name, pixels):
        path = folder/name
        h,w,_ = pixels.shape
        path.write_bytes(struct.pack('<4sIIIIf', b'SPKL',1,w,h,1,0.) + pixels.astype('<u2').tobytes())
        return str(path)
    solid = np.full((32,48,3), [30000,18000,8000], np.uint16)
    path = texture('solid.spkleak', solid)
    def render(w=180,h=120,**extra):
        source = np.full((h,w,3), .035, np.float32)
        with engine.open(source, {**BASE, "aging_texture": path, **extra}) as session:
            return session.render('full')[0][...,:3]
    clean=render()
    assert np.array_equal(clean,render(aging_active=True,aging_strength=0,aging_texture='/missing'))
    base=clean.astype(float)/65535
    for strength in [.1,.5,.7,1.]:
        actual=render(aging_active=True,aging_strength=strength).astype(float)/65535
        expected=base+(1-base)*solid[0,0]/65535*strength
        np.testing.assert_allclose(actual,expected,atol=4/65535)
    # Asymmetric smooth coordinates avoid relying on a private texture or a photographic baseline.
    y,x=np.mgrid[:96,:144]
    grid=np.stack([x/143*.8,y/95*.6,(x/143+y/95)*.2],axis=-1)
    path=texture('coordinates.spkleak',np.rint(grid*65535).astype(np.uint16))
    for w,h in [(180,120),(120,180),(120,120),(160,120),(192,108)]:
        normal=render(w,h,aging_active=True,aging_strength=1.)
        # Mirror axes remain intuitive in both landscape and portrait.
        for flip in [1,2,3]:
            screen_flip = flip
            view=normal[:,::-1] if screen_flip==1 else normal[::-1] if screen_flip==2 else normal[::-1,::-1]
            np.testing.assert_allclose(render(w,h,aging_active=True,aging_strength=1.,aging_flip=flip),view,atol=4)
        rotated=render(w,h,aging_active=True,aging_strength=1.,aging_rotation=2)
        np.testing.assert_allclose(rotated,normal[::-1,::-1],atol=4)
    for turn in [1,3]:
        normal=render(120,120,aging_active=True,aging_strength=1.)
        np.testing.assert_allclose(render(120,120,aging_active=True,aging_strength=1.,aging_rotation=turn),
                                   np.rot90(normal,-turn),atol=4)
    for key in ['aging_rotation','aging_flip']:
        for value in [-1,4,.5]:
            try: render(aging_active=True,**{key:value})
            except EngineError: pass
            else: raise AssertionError((key,value))
    for options in [dict(digital_intermediate=True),dict(striped=True)]:
        try: render(aging_active=True,**options)
        except EngineError: pass
        else: raise AssertionError('unsupported mode accepted')
    # A print-only edit must retain the negative cache; export DI must ignore the appearance.
    source=np.full((120,180,3),.035,np.float32)
    with engine.open(source,{**BASE,'aging_texture':path}) as session:
        before=session.render('full')[0]
        di=session.export_di('kodak_portra_endura')[0]
        session.set_params(dict(aging_active=True,aging_strength=.8,aging_rotation=1,aging_flip=2))
        aged,meta=session.render('full',reprint=True)
        assert meta.negative_was_cached and not np.array_equal(aged,before)
        assert np.array_equal(di,session.export_di('kodak_portra_endura')[0])
        assert np.array_equal(aged,session.render('full',reprint=True)[0])
        session.set_params(dict(aging_active=False))
        assert np.array_equal(before,session.render('full',reprint=True)[0])
    for content in [b'bad',struct.pack('<4sIIIIf',b'SPKL',1,2000,1333,1,0.)]:
        bad=folder/'bad.spkleak';bad.write_bytes(content)
        try: render(aging_active=True,aging_texture=str(bad))
        except EngineError: pass
        else: raise AssertionError('malformed texture accepted')
    print('PASS native Screen curve, zero bypass, five aspects, mirrors/rotations, invalid controls, modes and textures')

# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Wide texture looks through the shipping C ABI/Metal pipeline."""
from pathlib import Path
import struct,tempfile,ctypes,json
import numpy as np
from spk_ctypes import Engine,EngineError,SpkResult

ROOT=Path(__file__).resolve().parents[1]
BASE=dict(auto_exposure=False,grain_active=False,glare_active=False,halation_active=False,
          output_color_space='sRGB',wide_leak_depth=.28,wide_leak_scale=1.2,wide_leak_seed=1)
with tempfile.TemporaryDirectory() as directory,Engine() as engine:
    folder=Path(directory)
    # A tiny original test texture keeps CI independent of the private source material.
    texture=np.zeros((64,128,3),np.uint16)
    ramp=np.clip((.65-np.arange(128)/127)/.2,0,1)
    texture[:]=np.rint(ramp[None,:,None]*65535).astype(np.uint16)
    path=folder/'test.spkleak';path.write_bytes(struct.pack('<4sIIIIf',b'SPKL',1,128,64,0,.5)+texture.astype('<u2').tobytes())
    source=np.full((800,1600,3),.035,np.float32)
    def render(**extra):
        with engine.open(source,dict(BASE,wide_leak_texture=str(path),**extra)) as session:return session.render('full')[0]
    schema={f['name']:f for f in engine.params_schema()['fields']}
    assert schema['wide_leak_active']['layer']=='print'
    clean=render(wide_leak_active=False)
    # Inactive/zero strength never read a missing file.
    with engine.open(source,dict(BASE,wide_leak_active=True,wide_leak_strength=0.,wide_leak_texture='/not/a/texture')) as session:
        assert np.array_equal(clean,session.render('full')[0])
    # A constant colour measures the actual Screen response without UV ambiguity.
    solid=np.zeros((32,64,3),np.uint16)
    solid[:]=np.rint(np.array([.48,.22,.07])*65535).astype(np.uint16)
    solid_path=folder/'solid.spkleak'
    solid_path.write_bytes(struct.pack('<4sIIIIf',b'SPKL',1,64,32,1,0.)+solid.astype('<u2').tobytes())
    base=clean[...,:3].astype(np.float64)/65535
    light=solid[0,0].astype(np.float64)/65535
    previous=None
    for strength in [.0,.25,.35,.5,.6999,.7,.7001,.85,1.]:
        with engine.open(source,dict(BASE,wide_leak_active=True,wide_leak_texture=str(solid_path),wide_leak_strength=strength)) as session:
            image=session.render('full')[0][...,:3]
        t=np.clip((strength-.7)/.3,0,1)
        gain=min(strength/.7,1)+3*t*t
        expected=base+(1-base)*np.clip(light*gain,0,1)
        mask=np.clip((light.max()-.12)/(.65-.12),0,1);mask=mask*mask*(3-2*mask)
        expected += (1-expected)*(t*t*(3-2*t)*mask)
        np.testing.assert_allclose(image/65535,expected,atol=5/65535,err_msg=f'70% Screen knee at {strength}')
        if previous is not None:assert np.all(image.astype(int)>=previous.astype(int)-2)
        previous=image
    print('PASS natural Screen through 70%, continuous knee and enhanced white tail',flush=True)
    gains=[];white=[]
    for strength in [0.,.25,.5,.75,1.]:
        image=render(wide_leak_active=True,wide_leak_strength=strength)
        if strength==0:assert np.array_equal(clean,image)
        delta=np.abs(image[...,:3].astype(float)-clean[...,:3])
        gains.append(delta.mean());white.append(np.all(image[...,:3]>65000,axis=2).mean())
        assert np.array_equal(image[:,int(image.shape[1]*.8):],clean[:,int(clean.shape[1]*.8):]),'dark texture tinted the unaffected region'
    print('mean change / white fraction by strength:',gains,white,flush=True)
    assert np.all(np.diff(gains)>0)
    assert white[-1]>.1 and white[-1]>white[-2]+.02,'100% is not substantially white'
    full=render(wide_leak_active=True,wide_leak_strength=1.)
    assert np.array_equal(full,render(wide_leak_active=True,wide_leak_strength=1.))
    assert not np.array_equal(full,render(wide_leak_active=True,wide_leak_strength=1.,wide_leak_seed=29))
    mirrored=render(wide_leak_active=True,wide_leak_strength=1.,wide_leak_direction=1)
    np.testing.assert_allclose(full[:,::-1],mirrored,atol=2)
    for direction in [2,3]:assert not np.array_equal(full,render(wide_leak_active=True,wide_leak_strength=1.,wide_leak_direction=direction))
    assert not np.array_equal(full,render(wide_leak_active=True,wide_leak_strength=1.,wide_leak_softness=1.))
    for depth in [.1,.5]:assert not np.array_equal(full,render(wide_leak_active=True,wide_leak_strength=1.,wide_leak_depth=depth))
    for space in ['Display P3','ProPhoto RGB']:
        image=render(wide_leak_active=True,wide_leak_strength=1.,output_color_space=space)
        assert np.all(image[...,:3]>65000,axis=2).mean()>.1,('white core not neutral/white',space)
    def direct_di(session):
        fn=engine._lib.spk_render_digital_intermediate
        fn.restype=ctypes.c_int32
        fn.argtypes=[ctypes.c_void_p,ctypes.POINTER(SpkResult),ctypes.POINTER(ctypes.c_char_p)]
        result,metadata=SpkResult(),ctypes.c_char_p()
        status=fn(session._handle,ctypes.byref(result),ctypes.byref(metadata))
        if status:raise EngineError(engine._last_error())
        image=session._take_result(result);engine._take_json(metadata)
        return image
    with engine.open(source,dict(BASE,wide_leak_active=False,wide_leak_texture=str(path))) as session:
        session.render('full')
        di_before=direct_di(session)
        lut_di_before=session.export_di('kodak_portra_endura')[0]
        reply=session.set_params({'wide_leak_active':True,'wide_leak_strength':1.})
        assert reply['invalidated']=='print'
        image,result=session.render('full',reprint=True)
        assert result.negative_was_cached,'texture edit re-exposed the film'
        exported,_=session.render('full',reprint=True)
        assert np.array_equal(full,image) and np.array_equal(full,exported)
        assert np.array_equal(di_before,direct_di(session)), 'scan overlay altered the direct DI'
        assert np.array_equal(lut_di_before,session.export_di('kodak_portra_endura')[0]), 'scan overlay altered the LUT DI'
    # Asymmetric RGB ramps expose crop-window/flip errors without private assets.
    ramp_texture=np.zeros((64,128,3),np.uint16)
    ramp_texture[:,:,0]=np.rint(np.linspace(.05,.85,128)*65535).astype(np.uint16)[None,:]
    ramp_texture[:,:,1]=np.rint(np.linspace(.1,.7,64)*65535).astype(np.uint16)[:,None]
    ramp_texture[:,:,2]=7000
    ramp_path=folder/'uv.spkleak'
    ramp_path.write_bytes(struct.pack('<4sIIIIf',b'SPKL',1,128,64,1,0.)+ramp_texture.astype('<u2').tobytes())
    for h,w in [(320,480),(320,320),(480,320),(640,360),(240,650)]:
        uv_source=np.full((h,w,3),.035,np.float32)
        opts=dict(BASE,wide_leak_active=True,wide_leak_texture=str(ramp_path),wide_leak_anchored=True,wide_leak_strength=.7,
                  wide_leak_anchor_x=.27,wide_leak_anchor_y=1.)
        def uv_render(**extra):
            with engine.open(uv_source,dict(opts,**extra)) as session:return session.render('full')[0]
        normal=uv_render()
        for flip,view in [(1,normal[:,::-1]),(2,normal[::-1]),(3,normal[::-1,::-1])]:
            np.testing.assert_allclose(uv_render(wide_leak_flip=flip),view,atol=3,err_msg='flip changed selected crop')
        assert np.array_equal(normal,uv_render(wide_leak_seed=29,wide_leak_direction=3,wide_leak_scale=.5,wide_leak_depth=.9)), 'anchored fit inherited legacy jitter/placement'
        if w/h<2:
            left=uv_render(wide_leak_anchor_x=0.)
            right=uv_render(wide_leak_anchor_x=1.)
            assert right[:,:,0].mean()>left[:,:,0].mean(), 'horizontal anchor ignored'
        else:
            top=uv_render(wide_leak_anchor_y=0.)
            bottom=uv_render(wide_leak_anchor_y=1.)
            assert bottom[:,:,1].mean()>top[:,:,1].mean(), 'vertical anchor ignored'
    print('PASS 5 aspect ratios, saved crop-window flips and anchor controls',flush=True)
    # Do not colour empty perforations or the carrier when the film edge is present.
    options=dict(wide_leak_active=False,overscan_active=True,overscan_format='135_full_67',overscan_holes='white',overscan_fog=0.,overscan_leaks=0.)
    white_back=render(**options);holes=np.all(white_back[...,:3]>65000,axis=2)
    inside=holes.copy()
    for axis in [0,1]:
        for shift in [-8,8]:inside &= np.roll(holes,shift,axis=axis)
    black=render(**dict(options,overscan_holes='black'))
    lit=render(**dict(options,overscan_holes='black',wide_leak_active=True,wide_leak_strength=1.))
    assert inside.any() and np.max(np.abs(lit[inside].astype(int)-black[inside].astype(int)))<=2,'wide light filled the holes'
    for bad in [{'wide_leak_strength':1.1},{'wide_leak_direction':1.5},{'wide_leak_seed':0},{'wide_leak_flip':1.5},{'wide_leak_anchor_x':1.1},{'wide_leak_flip':4}]:
        try:render(**bad)
        except EngineError:pass
        else:raise AssertionError(('bad parameter accepted',bad))
    for unsupported in [dict(digital_intermediate=True),dict(striped=True)]:
        try:
            with engine.open(source,dict(BASE,wide_leak_active=True,wide_leak_strength=1.,wide_leak_texture=str(path),**unsupported)) as session:session.render('full')
        except EngineError as error:assert 'wide light leaks' in str(error),str(error)
        else:raise AssertionError(('unsupported mode silently accepted',unsupported))
    for bad_bytes in [b'',struct.pack('<4sIIIIf',b'SPKL',1,0,3,0,0.),struct.pack('<4sIIIIf',b'SPKL',1,4096,4096,0,.5)]:
        broken=folder/'broken.spkleak';broken.write_bytes(bad_bytes)
        try:
            with engine.open(source,dict(BASE,wide_leak_active=True,wide_leak_strength=1.,wide_leak_texture=str(broken))) as session:session.render('full')
        except EngineError:pass
        else:raise AssertionError('malformed texture accepted')
    print('PASS directions, bounds, seeds, white output spaces, print caching, holes, export and malformed files',flush=True)
    materials=ROOT/'resources/textures/light-leaks'
    manifest=materials/'manifest.json'
    if manifest.exists():
        entries=json.loads(manifest.read_text())['materials']
        assert len(entries)==41, 'release catalog is incomplete'
        for entry in entries:
            material=materials/entry['file']
            options=dict(BASE,wide_leak_active=True,wide_leak_strength=1.,wide_leak_texture=str(material))
            if 'anchor' in entry:
                options.update(wide_leak_anchored=True,wide_leak_anchor_x=entry['anchor'][0],wide_leak_anchor_y=entry['anchor'][1])
            with engine.open(source,options) as session:image,_=session.render('full')
            white_fraction=np.all(image[...,:3]>65000,axis=2).mean()
            assert np.abs(image[...,:3].astype(float)-clean[...,:3]).mean()>0,(entry['file'],'material made no visible change')
            options['wide_leak_strength']=.7
            with engine.open(source,options) as session:natural,_=session.render('full')
            assert np.all(image[...,:3].astype(int)>=natural[...,:3].astype(int)-2),(entry['file'],'white tail darkened the image')
        print('PASS all 41 release materials render; maximum is never darker than natural 70%',flush=True)
    else:raise AssertionError('release material manifest absent')
    source=np.full((1800,3600,3),.035,np.float32)
    baseline={}
    with engine.open(source,BASE) as session:
        for tier in ['live','full']:baseline[tier]=session.render(tier)[0]
    gains=[]
    with engine.open(source,dict(BASE,wide_leak_active=True,wide_leak_strength=.75,wide_leak_texture=str(path))) as session:
        for tier in ['live','full']:
            image,_=session.render(tier);gains.append(np.abs(image[...,:3].astype(float)-baseline[tier][...,:3]).mean())
    ratio=gains[0]/gains[1];assert abs(ratio-1)<.05,ratio
    print('PASS live/full intensity',round(ratio,3),flush=True)

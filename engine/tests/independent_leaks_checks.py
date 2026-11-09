# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""The actual C ABI/Metal path separates light switches/seeds from wear."""
from pathlib import Path
import numpy as np
from spk_ctypes import Engine, EngineError

BASE=dict(auto_exposure=False,grain_active=False,glare_active=False,halation_active=False,
          overscan_active=True,overscan_format='135_full_67',overscan_camera_seed=19,
          overscan_frame_seed=5,overscan_fog=0.,overscan_leaks=0.,output_color_space='sRGB',
          overscan_holes='black',scratch_strength=0.,scratch_long_lines=0.,scratch_edge_marks=0.,
          scratch_dust_amount=0.,scratch_seed=17,scratch_leak_strength=.65,scratch_leak_spread=.6,
          scratch_leak_softness=.5,scratch_leak_coverage=.75)
source=np.full((900,1800,3),.035,np.float32)
with Engine() as engine:
    fields={f['name']:f for f in engine.params_schema()['fields']}
    assert fields['scratch_leak_enabled']['default']==-1
    assert fields['scratch_leak_seed']['default']==0
    def render(**extra):
        with engine.open(source,dict(BASE,**extra)) as session:return session.render('full')[0]
    for params in [dict(scratch_leak_enabled=2),dict(scratch_leak_enabled=.5),dict(scratch_leak_seed=-1),dict(scratch_leak_seed=1.5)]:
        try:render(**params)
        except EngineError:pass
        else:raise AssertionError(('invalid independent settings accepted',params))
    for style in ['rim','halo']:
        clean=render(scratch_active=False,scratch_leak_enabled=0,scratch_leak_style=style)
        independent=render(scratch_active=False,scratch_leak_enabled=1,scratch_leak_seed=17,scratch_leak_style=style)
        assert not np.array_equal(clean,independent),('wear-off also disabled light',style)
        legacy=render(scratch_active=True,scratch_leak_style=style)
        assert np.array_equal(legacy,independent),('independence changed accepted light pattern',style)
        assert np.array_equal(independent,render(scratch_active=False,scratch_leak_enabled=1,scratch_leak_seed=17,scratch_seed=29,scratch_leak_style=style)),('wear seed changed light pattern',style)
        assert not np.array_equal(independent,render(scratch_active=False,scratch_leak_enabled=1,scratch_leak_seed=29,scratch_leak_style=style)),style
        assert np.array_equal(clean,render(scratch_active=True,scratch_leak_enabled=0,scratch_leak_style=style)),('independent off ignored while wear on',style)
        with engine.open(source,dict(BASE,scratch_active=False,scratch_leak_enabled=1,scratch_leak_seed=17,scratch_leak_style=style)) as session:
            image,_=session.render('full');exported,_=session.render('full',reprint=True)
            assert np.array_equal(image,exported)
            reply=session.set_params({'scratch_leak_enabled':0});assert reply['invalidated']=='shoot'
            image,_=session.render('full');assert np.array_equal(image,clean),'off retained a stale lit negative'
        print('PASS independent wear-off/light-on, exact legacy match, seeds, off and reprint:',style,flush=True)
    # With light off, changing its pattern must not change actual friction/dust pixels.
    wear=dict(scratch_active=True,scratch_strength=.7,scratch_long_lines=.6,scratch_edge_marks=.5,
              scratch_dust_amount=.3,scratch_leak_enabled=0)
    assert np.array_equal(render(**dict(wear,scratch_leak_seed=17)),render(**dict(wear,scratch_leak_seed=29)))
    print('PASS light seed leaves scratches/dust unchanged',flush=True)

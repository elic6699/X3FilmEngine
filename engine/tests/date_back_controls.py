# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Date-back render checks with synthetic pixels; no user photographs needed."""
import numpy as np
from spk_ctypes import Engine, EngineError

BASE = {'auto_exposure': False, 'grain_active': False, 'glare_active': False,
        'halation_active': False, 'film_format_mm': 36.0,
        'date_imprint_size': 1.0, 'date_imprint_style': 'dseg', 'date_imprint_text': '15 12 : 37'}

DSEG_STYLES = ('dseg', 'dseg_mini_italic', 'dseg_mini_regular', 'dseg_classic_regular')

with Engine() as engine:
    def render(w=1200, h=800, **extra):
        frame = np.full((h, w, 3), 0.18, dtype=np.float32)
        with engine.open(frame, dict(BASE, **extra)) as session:
            image, _ = session.render('full')
            return image

    def box(image, baseline):
        delta = np.max(np.abs(image[..., :3].astype(float) - baseline[..., :3]), axis=2)
        ys, xs = np.where(delta > 512)
        assert len(xs) > 100, 'date did not render'
        return (xs.min(), ys.min(), xs.max(), ys.max())

    # Real raster checks: two separate baseline marks, each centered on its field.
    bare = render()
    for style in ('lcd', 'dots', *DSEG_STYLES):
        params = dict(date_imprint_active=True, date_imprint_style=style,
                      date_imprint_text='16 17 50', date_imprint_time_underline=True,
                      date_imprint_margin=100)
        image = render(**params)
        x0, y0, x1, y1 = box(image, bare)
        assert abs((x0+x1)/2-600) < 3 and abs((y0+y1)/2-400) < 3, (style, (x0,y0,x1,y1))
        delta = np.max(np.abs(image[..., :3].astype(float)-bare[..., :3]), axis=2)
        # Near the bottom of the imprint, only the two short bars remain.
        columns = np.flatnonzero(np.any(delta[y1-2:y1+1] > 512, axis=0))
        groups = np.split(columns, np.flatnonzero(np.diff(columns)>1)+1)
        assert len(groups) == 2, ('expected two field underlines', style, len(groups))
        assert groups[0][0] > x0 + (x1-x0)*.3, ('day was underlined', style)
        for margin in (0, 100):
            for corner in ('br', 'bl', 'tr', 'tl'):
                box(render(**dict(params, date_imprint_margin=margin, date_imprint_corner=corner)), bare)
    for text in ('16 17 : 50', 'bad', '16 1750'):
        try:
            render(date_imprint_active=True, date_imprint_time_underline=True, date_imprint_text=text)
        except EngineError:
            pass
        else:
            raise AssertionError(('malformed underline text accepted', text))
    for text in ('17 : 50 : 08', '26 09 16 17 : 50 : 08'):
        with engine.open(np.full((800,1200,3), .18, dtype=np.float32),
                         dict(BASE, date_imprint_active=True, date_imprint_text=text,
                              overscan_active=True, date_imprint_placement='rebate')) as session:
            full, _ = session.render('full')
            reprint, _ = session.render('full', reprint=True)
            assert np.array_equal(full, reprint), 'multiple-colon export changed'
    assert {f['name']:f for f in engine.params_schema()['fields']}['date_imprint_time_underline']['default'] is False
    print('PASS two hour/minute underlines, placement, validation and multiple-colon export')

    for w, h, fmt in ((1200, 800, '135'), (800, 1200, '135'), (800, 1200, '135_half')):
        bare = render(w, h, overscan_format=fmt)
        for style in ('lcd', *DSEG_STYLES):
            for corner in ('br', 'bl', 'tr', 'tl'):
                a = render(w, h, overscan_format=fmt, date_imprint_active=True,
                           date_imprint_style=style, date_imprint_margin=100, date_imprint_corner=corner)
                x0, y0, x1, y1 = box(a, bare)
                assert abs((x0+x1)/2-w/2) < 3, (style, corner, fmt, (x0,y0,x1,y1))
                assert abs((y0+y1)/2-h/2) < 3, (style, corner, fmt, (x0,y0,x1,y1))
                a = render(w, h, overscan_format=fmt, date_imprint_active=True,
                           date_imprint_style=style, date_imprint_margin=0, date_imprint_corner=corner)
                x0, y0, x1, y1 = box(a, bare)
                assert 0 <= x0 < x1 < w and 0 <= y0 < y1 < h
        print('PASS full/half frame, portrait/landscape, all LCD styles, all corners:', fmt, w, h)

    # Each choice must reach its actual bundled font, rather than share a fallback.
    bare = render()
    faces = []
    for style in DSEG_STYLES:
        a = render(date_imprint_active=True, date_imprint_style=style,
                   date_imprint_text='88 : 88', date_imprint_margin=100, date_imprint_field_gap=.2)
        b = render(date_imprint_active=True, date_imprint_style=style,
                   date_imprint_text='88 : 88', date_imprint_margin=100, date_imprint_field_gap=3)
        assert box(b, bare)[2]-box(b, bare)[0] > box(a, bare)[2]-box(a, bare)[0], style
        # At the minimum gap, optical softness can join the colon to a digit.
        # Measure centering where the visible glyphs remain separate.
        middle = render(date_imprint_active=True, date_imprint_style=style,
                        date_imprint_text='88 : 88', date_imprint_margin=100, date_imprint_field_gap=1.2)
        for image in (middle, b):
            columns = np.flatnonzero(np.any(np.max(np.abs(image[..., :3].astype(float)-bare[..., :3]), axis=2) > 512, axis=0))
            groups = np.split(columns, np.flatnonzero(np.diff(columns) > 1)+1)
            assert len(groups) == 5, ('digits/colon merged', style)
            assert abs((groups[2][0]+groups[2][-1]-groups[1][-1]-groups[3][0])/2) < 2, ('colon off-center', style)
        assert all(not np.array_equal(a, previous) for previous in faces), ('font selection has no effect', style)
        faces.append(a)
        with engine.open(np.full((800, 1200, 3), .18, dtype=np.float32),
                         dict(BASE, date_imprint_active=True, date_imprint_style=style)) as session:
            full, _ = session.render('full')
            reprint, _ = session.render('full', reprint=True)
            assert np.array_equal(full, reprint), ('font changed in export reprint', style)
    print('PASS four distinct bundled DSEG faces, shared spacing/centered colon and export reprint')

    # 120 places dates in the film margin: the 135-only control must do nothing.
    params = dict(overscan_active=True, overscan_format='120_645', date_imprint_active=True)
    a = render(1120, 830, **params, date_imprint_margin=0)
    b = render(1120, 830, **params, date_imprint_margin=100)
    assert np.array_equal(a, b), '135 margin moved a 120 imprint'
    params = dict(overscan_active=True, overscan_format='135', date_imprint_active=True,
                  date_imprint_placement='rebate')
    assert np.array_equal(render(**params, date_imprint_margin=0), render(**params, date_imprint_margin=100))
    print('PASS 120 and between-frame placement unchanged')

    # The 135 photo gate, not the scan's outer border, determines the center.
    frame = np.full((800, 1200, 3), 0.18, dtype=np.float32)
    settings = dict(BASE, overscan_active=True, overscan_format='135')
    with engine.open(frame, settings) as session:
        bare, _ = session.render('full')
        geometry = session.overscan_geometry()
        session.set_params({'date_imprint_active': True, 'date_imprint_margin': 100})
        image, _ = session.render('full')
    x0, y0, x1, y1 = box(image, bare)
    gate = np.array(geometry['gates'][0]).reshape(4, 2)
    center = gate.mean(axis=0) * [image.shape[1], image.shape[0]]
    assert np.max(np.abs(np.array([(x0+x1)/2, (y0+y1)/2])-center)) < 3
    print('PASS margin centers inside the content gate, excluding the border')

    # Export uses full-tier reprint; keep that cached-negative path equal too.
    frame = np.full((1600, 2400, 3), 0.18, dtype=np.float32)
    with engine.open(frame, dict(BASE, date_imprint_active=True, date_imprint_margin=100)) as session:
        live, _ = session.render('live')
        full, _ = session.render('full')
        exported, _ = session.render('full', reprint=True)
        assert np.array_equal(full, exported), 'export reprint changed the date'
    for image in (live, full):
        # The flat background is unaffected outside the center's date.
        baseline = np.broadcast_to(image[0, 0], image.shape)
        x0, y0, x1, y1 = box(image, baseline)
        assert abs((x0+x1)/2/image.shape[1] - .5) < .003
        assert abs((y0+y1)/2/image.shape[0] - .5) < .003
    print('PASS preview/full center agreement and full-tier export reprint')

    # Font v0.46 advances a digit by 0.816H. Tracking adds exactly 0.20H.
    bare = render()
    def date_width(text, gap, style='dseg'):
        image = render(date_imprint_active=True, date_imprint_text=text,
                       date_imprint_style=style, date_imprint_margin=100,
                       date_imprint_field_gap=gap)
        x0, y0, x1, y1 = box(image, bare)
        assert abs((x0+x1)/2-600) < 3
        return x1-x0
    h_pixels = 1.3 * 1200 / 36
    assert abs((date_width('88', .60)-date_width('8', .60))/h_pixels - 1.016) < .07
    narrow = date_width('26 09 15', .20)
    wide = date_width('26 09 15', 1.20)
    assert abs((wide-narrow)/h_pixels - 2.0) < .07
    assert date_width('26', .20) == date_width('26', 1.20)
    for style in ('lcd',):
        a = render(date_imprint_active=True, date_imprint_style=style, date_imprint_field_gap=.20)
        b = render(date_imprint_active=True, date_imprint_style=style, date_imprint_field_gap=1.20)
        assert np.array_equal(a, b), ('spacing changed another style', style)
    for text in ('26 09 16', '16 17 : 50', '17 : 50 : 08'):
        widths = []
        for gap in (.2, 1.2, 3):
            image = render(date_imprint_active=True, date_imprint_style='dots',
                           date_imprint_text=text, date_imprint_field_gap=gap, date_imprint_margin=100)
            x0, y0, x1, y1 = box(image, bare)
            widths.append(x1-x0)
        assert widths[0] < widths[1] < widths[2], ('dot spacing did not move fields', text)
    assert date_width('26', .2, 'dots') == date_width('26', 3, 'dots'), 'dot digit pitch changed'
    print('PASS dot-matrix field spacing, clock separators and unchanged digit pitch')

    for value in (.19, 3.01):
        try:
            render(date_imprint_field_gap=value)
        except EngineError:
            pass
        else:
            raise AssertionError(('invalid field spacing accepted', value))
    separations = []
    for gap in (.20, 1.20, 3.00):
        image = render(date_imprint_active=True, date_imprint_text='88 : 88',
                       date_imprint_margin=100, date_imprint_field_gap=gap)
        delta = np.max(np.abs(image[..., :3].astype(float)-bare[..., :3]), axis=2)
        columns = np.flatnonzero(np.any(delta > 512, axis=0))
        groups = np.split(columns, np.flatnonzero(np.diff(columns) > 1)+1)
        assert len(groups) == 5, ('digits/colon merged', gap, len(groups))
        colon_center = (groups[2][0]+groups[2][-1])/2
        time_center = (groups[1][-1]+groups[3][0])/2
        assert abs(colon_center-time_center) < 2, ('colon off-center', gap)
        separations.append(groups[3][0]-groups[1][-1])
    assert separations[0] < separations[1] < separations[2]
    assert abs((separations[2]-separations[0])/h_pixels - 2.80) < .08
    print('PASS hour/minute spacing tracks the full 0.20–3.00H range with centered colon')
    print('PASS DSEG field spacing, +0.20H tracking, centering and other styles unchanged')

    for value in (.19, 1.01):
        try:
            render(date_imprint_size=value)
        except EngineError:
            pass
        else:
            raise AssertionError(("invalid date size accepted", value))

    for value in (-2, -0.5, 101):
        try:
            render(date_imprint_margin=value)
        except EngineError:
            pass
        else:
            raise AssertionError(('invalid margin accepted', value))
    try:
        render(date_imprint_active=True, date_imprint_text='unsupported')
    except EngineError:
        pass
    else:
        raise AssertionError('DSEG silently substituted unsupported glyphs')
    fields = {f['name']: f for f in engine.params_schema()['fields']}
    assert fields['date_imprint_size']['default'] == .4
    assert fields['date_imprint_size']['range'] == [.2, 1]
    assert fields['date_imprint_field_gap']['default'] == 1.2
    assert fields['date_imprint_field_gap']['range'] == [.2, 3]
    for style in ('lcd', 'dseg', 'dots'):
        small = render(date_imprint_active=True, date_imprint_style=style,
                       date_imprint_size=.2, date_imprint_margin=100)
        larger = render(date_imprint_active=True, date_imprint_style=style,
                        date_imprint_size=.5, date_imprint_margin=100)
        def height(image):
            difference = np.max(np.abs(image[..., :3].astype(float)-bare[..., :3]), axis=2)
            ys, xs = np.where(difference > 512)
            assert len(xs), ('small date vanished', style)
            return ys.max()-ys.min()
        assert height(small) < height(larger), ('minimum size was clamped', style)
    print('PASS shared size default 0.4 and minimum 0.2 across date faces')
    print('PASS margin validation and no silent glyph substitution')

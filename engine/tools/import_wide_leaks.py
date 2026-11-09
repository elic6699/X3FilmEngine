# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Import reviewed local derivatives and four original JPEGs; never downloads assets."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import zipfile
import numpy as np
from PIL import Image

POOL_IDS = ('01','02','03','04','05','06','07','09','10','12','13','16','18','19','20','22','24','26','27','29','31','32','38','40-pair','40-warm','40-cool','41')
ONETAKE = (
    ('020','leak_020_g05_jpg_50_left.jpg',(.27,.5)),
    ('035','leak_035_g69_jpg_50_left.jpg',(.38,.5)),
    ('040','leak_040_g55_jpg_50_right.jpg',(1.,.5)),
    ('060','leak_060_g09_jpg_50_bottom.jpg',(.5,1.)),
)
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def import_pool(directory, destination):
    recipe = json.loads((directory/'recipe.json').read_text())
    sources = {r['number']:r for r in recipe['sources']}
    variants = {r['key']:r for r in recipe['variants']}
    records = []
    for identifier in POOL_IDS:
        key = str(int(identifier)) if identifier.isdecimal() else identifier
        record = variants[key]
        source = sources[record['number']]
        if sha(Path(source['source'])) != source['source_sha256']:
            raise ValueError(f'source changed since review: {identifier}')
        path = directory/'textures'/f'{key}.spkleak'
        if sha(path) != record['sha256']:
            raise ValueError(f'derivative changed since review: {identifier}')
        output = destination/f'{identifier}.spkleak'
        shutil.copy2(path, output)
        records.append(dict(id=identifier,source=Path(source['source']).name,source_sha256=source['source_sha256'],
                            file=output.name,sha256=sha(output),recipe=record['recipe'],local=record['local']))
    if len({variants[str(int(i)) if i.isdecimal() else i]['number'] for i in POOL_IDS}) != 25:
        raise ValueError('reviewed pool must contain 25 source materials')
    return records

def import_onetake(directory, destination):
    provenance = json.loads((directory/'provenance.json').read_text())
    apk = directory/'Onetake-3.3.9-130.apk'
    if sha(apk) != provenance['sha256']:
        raise ValueError('Onetake APK differs from provenance')
    records = []
    with zipfile.ZipFile(apk) as archive:
        for code, name, anchor in ONETAKE:
            source = directory/'recovered/textures-and-glsl'/name
            member = 'res/drawable-nodpi-v4/'+name
            if source.read_bytes() != archive.read(member):
                raise ValueError(f'JPEG differs from APK member: {name}')
            with Image.open(source) as image:
                if image.format != 'JPEG' or image.mode != 'RGB' or image.size != (1565,1038):
                    raise ValueError(f'unexpected JPEG format: {name}')
                pixels = np.asarray(image).astype('<u2')*257
            output = destination/f'onetake_{code}_v1.spkleak'
            output.write_bytes(struct.pack('<4sIIIIf',b'SPKL',1,pixels.shape[1],pixels.shape[0],1,0.)+pixels.tobytes())
            records.append(dict(id=f'onetake_{code}_v1',source=name,source_sha256=sha(source),apk_member=member,
                                apk_sha256=provenance['sha256'],file=output.name,sha256=sha(output),anchor=anchor,
                                recipe='unaltered JPEG RGB8 decode expanded exactly to RGB16; no floor/normalisation/blur',local=True))
    return records

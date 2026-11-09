# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Import five local research JPEGs at native size; no downloads or redistribution grant."""
from pathlib import Path
import hashlib
import json
import struct
import zipfile
import numpy as np
from PIL import Image


def import_textures(source, destination):
    provenance = json.loads((source / 'provenance.json').read_text())
    apk = source / 'Onetake-3.3.9-130.apk'
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    if digest(apk) != provenance['sha256']:
        raise ValueError('APK differs from source provenance')
    destination.mkdir(parents=True, exist_ok=True)
    records = []
    with zipfile.ZipFile(apk) as archive:
        for n in range(1, 6):
            identifier = f'dust{n:02d}'
            path = source / 'recovered/textures-and-glsl' / f'{identifier}.jpg'
            member = f'res/drawable-nodpi-v4/{identifier}.jpg'
            if path.read_bytes() != archive.read(member):
                raise ValueError(f'JPEG differs from APK: {identifier}')
            with Image.open(path) as image:
                if image.format != 'JPEG' or image.mode != 'RGB' or image.size != (2000, 1333):
                    raise ValueError(f'unexpected texture: {identifier}')
                pixels = np.asarray(image).astype('<u2') * 257
            output = destination / f'{identifier}.spkleak'
            output.write_bytes(struct.pack('<4sIIIIf', b'SPKL', 1, 2000, 1333, 1, 0.) + pixels.tobytes())
            assert len(output.read_bytes()) == 24 + 2000 * 1333 * 6
            records.append(dict(id=identifier, source=path.name, source_sha256=digest(path),
                                apk_member=member, apk_sha256=provenance['sha256'],
                                file=output.name, sha256=digest(output)))
    (destination / 'manifest.json').write_text(json.dumps(dict(local_only=True,
        recipe='Native JPEG RGB8 expanded exactly to RGB16; no resize, floor, blur or normalisation',
        rights='Local research only; redistribution and commercial permission not established',
        materials=records), indent=2) + '\n')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    import_textures(args.source, args.destination)

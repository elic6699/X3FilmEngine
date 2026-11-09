# SPDX-FileCopyrightText: 2026 LittleSixNine
# SPDX-License-Identifier: GPL-3.0-or-later
# Applies only to FrameMatter contributions; upstream notices are retained. See COPYRIGHT.txt.

"""Prepare user-owned TIFF overlays for local builds; not a runtime dependency."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import numpy as np
from PIL import Image
import tifffile

SELECTED = (11, 14, 28, 36, 42, 17, 34, 39, 15, 23)

def prepare_additional(path, number):
    from scipy.ndimage import gaussian_filter, median_filter
    n = number
    def smooth(lo, hi, x):
        q = np.clip((x-lo)/(hi-lo), 0, 1)
        return q*q*(3-2*q)
    def resize(a, w):
        h = round(a.shape[0]*w/a.shape[1])
        return np.stack([np.asarray(Image.fromarray(a[...,c]).resize((w,h), Image.Resampling.LANCZOS)) for c in range(3)], axis=2)
    raw=tifffile.imread(path)
    if raw.dtype != np.uint16:
        raise ValueError(f'{path.name}: expected 16-bit TIFF')
    if raw.ndim==2:raw=np.repeat(raw[...,None],3,axis=2)
    if raw.ndim != 3 or raw.shape[2] != 3:
        raise ValueError(f'{path.name}: expected grayscale or RGB TIFF')
    a=np.clip(resize(raw.astype(np.float32)/65535.,1200),0,1);del raw
    # Suppress scan grain and narrow scratches, retaining the photographed light field.
    a=median_filter(a,size=(5,5,1),mode='nearest')
    if n==15:a=median_filter(a,size=(25,1,1),mode='nearest')
    a=gaussian_filter(a,sigma=(9 if n in [15,23,39] else 2,)*2+(0,),mode='nearest')
    local=1;front=0.;floor=None
    if n==34:
        # Estimate blue backing per row from regions away from the orange band.
        background=np.median(np.concatenate([a[:,:180],a[:,600:]],axis=1),axis=1)
        background=gaussian_filter(background,sigma=(18,0))
        residual=np.maximum(a-background[:,None,:],0)
        warm=np.maximum(a[...,0]-.6*a[...,2]-.015,0)
        tex=residual*smooth(.025,.14,warm)[...,None]
        tex=np.clip(tex/max(float(np.quantile(tex[...,0],.999)),.1)*.9,0,1)
        recipe='按行估计光带两侧蓝底；扣底后以红蓝差抑制蓝色噪声；保留原始橙色带和软尾，归一化亮芯至0.9'
    elif n==39:
        # Remove the scanned black carrier, then estimate grey backing inside the film.
        a=a[:,:round(a.shape[1]*.94)].copy()
        # Extend each row's clipped bright rim across the black carrier before cropping.
        start=round(a.shape[1]*.8)
        peaks=np.argmax(a[:,start:,0],axis=1)+start
        for row,peak in enumerate(peaks):a[row,peak:]=a[row,peak]
        a=a[:,:round(a.shape[1]*.99)].copy()
        floor=np.median(a[:,:round(a.shape[1]*.45)],axis=(0,1))
        tex=np.maximum(a-floor,0)/np.maximum(1-floor,1e-6)
        tex*=smooth(.025,.12,np.max(tex,axis=2))[...,None]
        tex*=smooth(0.,.08,np.linspace(0,1,tex.shape[1]))[None,:,None]
        tex=tex[:,::-1].copy();local=0
        profile=np.median(tex.mean(axis=2),axis=0)
        active=np.flatnonzero(profile>max(.025,float(profile.max())*.13))
        assert len(active)>0;front=float(active[-1]/(tex.shape[1]-1))
        recipe='按行将饱和亮边向扫描黑片边外延，消除黑边断口；用左侧45%估计灰底；清除远侧扫描边缘，保留无色软晕并翻至左侧'
    else:
        lum=a@np.array([.2126,.7152,.0722],np.float32)
        floor=np.median(a[lum<=np.quantile(lum,.12)],axis=0)
        residual=np.maximum(a-floor,0)
        tex=np.clip(residual/np.maximum(1-floor,1e-6),0,1)
        tex*=smooth(.015,.09,np.max(residual,axis=2))[...,None]
        tex=tex[:,::-1].copy()
        if n==23:tex=np.clip(tex*1.35,0,1)
        recipe='5像素中值与9像素高斯抑制划痕颗粒；扣最暗12%区域的通道中位数底色；翻转至左亮右暗'+('；蓝紫残余增益1.35' if n==23 else '；25像素纵向中值抑制水平长划痕')
    tex=np.clip(tex,0,1).astype(np.float32)
    assert np.isfinite(tex).all() and tex.min()>=0 and tex.max()<=1
    if n==39:assert np.max(np.ptp(tex,axis=2))<1e-6
    return tex, floor, front, local, recipe


def prepare(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    records = []
    for number in SELECTED:
        path = source / f'Chemlooks-Film-Overlay-{number}.tif'
        if number in (34, 39, 15, 23):
            texture, floor, front, local, recipe = prepare_additional(path, number)
            flipped = number != 34
        else:
            raw = tifffile.imread(path)
            if raw.dtype != np.uint16 or raw.ndim != 3 or raw.shape[2] != 3:
                raise ValueError(f'{path.name}: expected 16-bit RGB')
            data = raw.astype(np.float32) / 65535.
            factor = min(1., 1536 / max(data.shape[:2]))
            size = (round(data.shape[1]*factor), round(data.shape[0]*factor))
            data = np.stack([np.asarray(Image.fromarray(data[...,c]).resize(size, Image.Resampling.LANCZOS)) for c in range(3)], axis=2)
            data = np.clip(data, 0, 1)
            luminance = data @ np.array([.2126,.7152,.0722], np.float32)
            floor = np.median(data[luminance <= np.quantile(luminance,.15)], axis=0)
            residual = np.maximum(data-floor,0)
            mask = np.clip((np.max(residual,axis=2)-.015)/(.09-.015),0,1)
            texture = np.clip(residual/(1-floor),0,1) * (mask*mask*(3-2*mask))[...,None]
            front = 0.
            flipped = False
            if number != 17:
                edge = max(2, texture.shape[1]//20)
                flipped = bool(texture[:,-edge:].mean() > texture[:,:edge].mean())
                if flipped: texture = texture[:,::-1].copy()
                profile = np.median(texture @ np.array([.2126,.7152,.0722],np.float32),axis=0)
                active = np.flatnonzero(profile > max(.035,float(profile.max())*.2))
                if not len(active): raise ValueError(f'{path.name}: no light front')
                front = float(active[-1]/(texture.shape[1]-1))
            local = int(number == 17)
            recipe = 'darkest-15% median subtraction; residual gate .015-.09'
        output = destination / f'{number:02d}.spkleak'
        pixels = np.rint(texture*65535).astype('<u2')
        header = struct.pack('<4sIIIIf', b'SPKL',1,pixels.shape[1],pixels.shape[0],local,front)
        output.write_bytes(header+pixels.tobytes())
        records.append(dict(number=number,source=path.name,source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                            file=output.name,sha256=hashlib.sha256(output.read_bytes()).hexdigest(),width=pixels.shape[1],height=pixels.shape[0],
                            dark_floor_rgb=None if floor is None else floor.tolist(),front_u=front,flipped=flipped,local=bool(local),recipe=recipe))
    (destination/'manifest.json').write_text(json.dumps(dict(version=1,encoding='assumed sRGB',distribution='local development only; redistribution permission not established',materials=records),ensure_ascii=False,indent=2))
    print(f'Prepared {len(records)} local textures at {destination}')

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir',type=Path,required=True)
    parser.add_argument('--output-dir',type=Path,default=Path(__file__).resolve().parents[1]/'resources/local_light_leaks')
    parser.add_argument('--reviewed-pool-dir',type=Path)
    parser.add_argument('--onetake-dir',type=Path)
    args=parser.parse_args()
    prepare(args.source_dir,args.output_dir)

    if args.reviewed_pool_dir or args.onetake_dir:
        from import_wide_leaks import import_pool, import_onetake
        manifest_path = args.output_dir/'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        if args.reviewed_pool_dir:
            manifest['materials'] += import_pool(args.reviewed_pool_dir,args.output_dir)
        if args.onetake_dir:
            manifest['materials'] += import_onetake(args.onetake_dir,args.output_dir)
        manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2))
        print(f"Total local textures: {len(manifest['materials'])}")

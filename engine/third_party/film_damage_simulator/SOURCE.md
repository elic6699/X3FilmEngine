# Film border scratch masks

Shape reference: Daniela Ivanova, John Williamson and Paul Henderson,
FilmDamageSimulator (Eurographics 2023), MIT, copyright Daniela Ivanova 2022.
https://github.com/daniela997/FilmDamageSimulator
https://github.com/daniela997/FilmDamageSimulator/blob/main/damage_generator/generate_masks.py

The native shader adapts line_scratch's thin-line times low-frequency
noise envelope, Gaussian softness and random length. It uses SpektraLab's
existing seeded value noise instead of the Python Perlin/image buffers.
The placement adapts the source's nonuniform artifact placement into
small groups of parallel friction scratches along a film edge, constrained by the existing gate and hole
geometry. No reference images or external datasets are bundled.

The neutral optical-density offset is an explicit first-version approximation
of light obstructed at a scratch. It reuses the existing CMY-density to
print/scan path, not a physically calibrated emulsion-loss or refractive model.
It does not copy the unlicensed ComfyUI-Darkroom implementation.
The user's two scan crops (2026-11-06) guide fine grouped lines and sparse
scuffs, not their orange negative colour or the central coloured stripes.

Perforation-edge marks reuse SpektraLab's existing `perf_sdf` roughness and
Gaussian burr/chip geometry. They do not use FilmDamageSimulator to claim
that a scan colour fringe is wear, and they do not amplify the light kernel.
Additional wear dimensions are visual estimates from the owner's crops,
not measured calibration; the hole centres and pitch stay unchanged.

Pull-contact burrs are restricted to the left short-edge centre per the
owner's fixed film-leader direction. Additional edge colour uses the existing
spectral base transmittance as absorption, with no glow or invented RGB fringe.

Pull deformation expands the hole towards the leader (negative signed-distance
displacement); it does not push a film-coloured spike into the hole. Both the
baseline tiny deformation and extra seeded wear follow this convention.

## Negative-scan dust and fibers

112 approved grayscale masks are derived from Daniela Ivanova's **Scans of
film damage**, https://doi.org/10.6084/m9.figshare.21803292 (CC BY 4.0).
Data license: DATA-LICENSE-CC-BY-4.0.txt. Credits/changes: DUST-ATTRIBUTION.txt.
The MIT license above covers the algorithm reference code, not this dataset.

`dust_masks/` contains the cropped, background-separated grayscale masks;
`dust_assets.json` preserves source scan/annotation ids and extraction geometry.
There are 64 dust, 32 short-fiber and 16 long-fiber masks from Scan (1), (4),
(8), (9) and (10). Masks P01–P16 are the first approved set; D01–D48, S01–S32
and L01–L16 are the later approved set. No photographic scene content is used.

Extraction follows upstream scans.py's coordinate scaling (1.5 for scans
8/9/10). Local median background and MAD noise floor are removed (2 sigma for
dust, 1.5 for fibers), P99 gray normalization is applied, with .65 source-pixel
contour feather. Peak normalization is artistic, not calibrated optical density.

Run `python3 engine/tools/bake_dust_atlas.py` (Pillow, offline only) to rebuild
`engine/src/pipeline/dust_atlas.h`: 112 unique 64x64 padded grayscale tiles.
Only the cropped masks and generated atlas are needed; full source scans stay
in the developer's ignored reference folder. The app needs no Python or loose
mask files to render. Source aspect and actual internal gray values survive;
the former filled annotation-polygon renderer has been removed.

Seeded selection/placement uses physical film coordinates. Fiber ratio .25
mixes 80% short and 20% long among selected fibers. Size ranges are visual
estimates. Neutral transmission loss follows the existing negative/print/
scanner path, not a calibrated scanner-only spectral or defocus model.

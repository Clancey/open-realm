# Warcraft III visionOS asset export

The C asset layer exports trusted CPU-side Warcraft III data to native
renderers without exposing engine or desktop renderer internals.

## ABI contract

`platform/bridge/bz_tabletop_assets.h` defines ABI version 1. Public values are
fixed-width C PODs or opaque retained `bzTTAsset_t`/`bzTTTerrain_t` handles.
All payloads are immutable after publication.

Asset identity comes from a retained tabletop snapshot and a configstring slot:

```c
asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                    snapshot, model_index,
                                    BZ_TTA_ASSET_MODEL, &metadata);
texture = BZ_TTA_RegisterModelTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                      asset, texture_index);
```

The Warcraft source validates that the configstring is a confined relative
path, then calls `FS_ReadFile`. This preserves the established loose-file and
MPQ override order, including TFT archives overriding ROC assets. Absolute
paths, drive prefixes, empty components, `.`/`..`, and control characters are
rejected before filesystem lookup. Model textures can only be registered from
their parsed MDX `TEXS` record; the ABI does not expose a guessed-path
registration entry point.

The cache key is asset kind, canonical configstring identity, and the POD
metadata (class/category, team color, tint, and footprint). A cache entry owns
one reference and each registration/`LatestTerrain` call returns another.
Shutdown removes publication references; outstanding readers remain valid
until they release their own references. Each initialization advances a
generation, so a filesystem load or terrain copy started before shutdown cannot
publish into a restarted lifecycle.

Failures produce explicit status-bearing placeholders rather than `NULL`:
images are a top-left 1x1 opaque magenta RGBA8 pixel and models are empty
version-800 descriptors. Allocation failure and invalid ABI/lifecycle arguments
are the only registration cases that can return `NULL`.

## Terrain

`BZ_TTA_PublishTerrainFromGame()` deep-copies `world.map`. Terrain dimensions
are corner-grid dimensions; tile dimensions are each one smaller. Mesh
partitioning is deterministic at 32x32 tiles and edge chunks are clipped to
the remaining tile count.

Each corner exports:

- corrected ground height, including cliff level and the W3E height correction;
- water height with the format's 80-unit water bias removed, matching the
  desktop terrain renderer;
- ground/cliff table indices and FourCC identities;
- ground/cliff variations and cliff level;
- ramp, water, blight, boundary, and map-edge flags.

The publication token includes the map identity, vertex storage, dimensions,
and type-table storage. Repeated snapshots of an unchanged map reuse the
published immutable terrain.

## BLP decoding

`games/warcraft-3/visionos/wc3_blp_decode.c` decodes BLP1 paletted/JPEG and
BLP2 paletted, raw BGRA, DXT1, DXT3, and DXT5 mip level zero into immutable
RGBA8. The descriptor always declares `BZ_TTA_ORIGIN_TOP_LEFT`; no renderer
upload or implicit vertical flip remains in the decode path. The asymmetric
2x2 fixture proves row orientation rather than relying on symmetric colors.

Every file offset, mip size, alpha plane, JPEG shared-header concatenation, and
DXT block access is bounds checked. Dimensions are capped at 8192 pixels per
axis and checked for multiplication overflow before allocation. stb_image uses
the same dimension cap, and BLP1 JPEG
dimensions are preflighted against the BLP header before full decode.

## MDX decoding

`games/warcraft-3/visionos/wc3_mdx_decode.c` accepts classic binary MDX version
800 and flattens it into one file-shaped allocation:

- model/sequence/geoset bounds;
- geoset positions, normals, UVs, and validated 16-bit triangle indices;
- materials and static layers, including blend/filter mode and texture index;
- texture references, replaceable IDs, and wrapping flags;
- sequences;
- nodes, pivots, and the first translation/rotation/scaling track values.

MDX texture records contain a 260-byte path field. The ABI identity buffer is
therefore 260 bytes; treating it as a conventional 256-byte path truncates
valid `TEXS` records. Material, layer, geoset, and node record sizes include
their own size DWORD; top-level chunk sizes do not. Inclusive records and
counted arrays are checked against their containing span before trailing data
is read.

Known model gaps are MDX 1000/1500, full animation curves and skinning
matrices, geoset animations, particle/ribbon emitters, and event/camera data.
The metadata fields carry authoritative category/team/tint/footprint values
when supplied by the game-facing registration call; automatic classification
of every doodad/destructable/resource identity is not yet wired.

## Validation

```sh
make test-bz-tabletop-assets
make test
make openwarcraft3 opensc2
make xrsimulator xros
```

Focused fixtures cover ROC/TFT resolution, asymmetric BLP orientation, MDX
geometry/material/bounds and repeated inclusive records, terrain
dimensions/corners/water/cliffs, concurrent placeholder log-once/cache behavior,
path confinement, malformed spans and zero arrays, lifecycle-crossing loads,
retained concurrent readers, and cleanup/publication races. The fixtures are
generated data; retail MPQs and decoded proprietary outputs must never enter
the repository.

`make run-sc2` currently hardcodes `-data data/StarCraft2` and exposes no
external-data variable. Validation with a legal install outside the repository
therefore stops at `make opensc2`; do not symlink or copy that install into the
tree merely to exercise the convenience target.

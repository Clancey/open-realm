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
count = BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND);
BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND, 0, &terrain_texture);
ground = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                       BZ_TTA_TERRAIN_TEXTURE_GROUND, terrain_texture.type_index);
status = BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                      &entity_input, &metadata);
```

The Warcraft source validates that the configstring is a confined relative
path, then calls `FS_ReadFile`. This preserves the established loose-file and
MPQ override order, including TFT archives overriding ROC assets. Absolute
paths, drive prefixes, empty components, `.`/`..`, and control characters are
rejected before filesystem lookup. Model textures can only be registered from
their parsed MDX `TEXS` record. Empty nonzero replaceable records are resolved
from the model's authoritative class metadata; the ABI exposes no guessed-path
registration entry point.

Spawn registration uses Doodads/Destructable `numVar`: single-variation rows
register the authored file stem, while multi-variation rows append the map's
variation index. A shared archive-probed resolver then applies the desktop
fallback order after an exact miss: case-insensitive `.mdl` to `.mdx`, then
removal of one trailing variation digit before `.mdx`. Server configstrings,
desktop loading, and export therefore cannot use divergent path walkers. Exact,
fallback, and generated spawn identities that exceed their destination bound are
rejected before archive probing rather than truncated.
Retail `War3.mpq` checks confirm the resulting archive identities
`Doodads\LordaeronSummer\Plants\Wheat\wheat.mdx`,
`Doodads\Ashenvale\Plants\AshenBush0\AshenBush0.mdx`, and
`Doodads\LordaeronSummer\Props\Cage\Cage.mdx` and
`Doodads\LordaeronSummer\Props\TorchHuman\TorchHuman.mdx`;
their generated `Wheat0.mdx`, `AshenBush00.mdx`, `Cage0.mdx`, and
`TorchHuman0.mdx` identities do not exist.

The asset cache key is asset kind, authoritative registration identity, and the POD
metadata (class/category, team color, tint, and footprint). A cache entry owns
one reference and each registration/`LatestTerrain` call returns another.
Entity metadata requires an active published map because custom unit fields
are map-local. `G_SpawnEntities` publishes an immutable retained custom-class
alias snapshot; worker readers never dereference mutable `world.info`. Metadata
is cached by class ID and snapshot generation, with the old cache discarded
when that generation changes. Runtime team-color and tint override bits are
applied after the immutable table result is read from cache. Generic bridge
serialization keeps source callbacks from concurrently entering the
process-global filesystem/archive readers. Shutdown drains active source
callbacks before runtime filesystem teardown.
Shutdown removes publication references; outstanding readers remain valid
until they release their own references. Each initialization advances a
generation, so a filesystem load or terrain copy started before shutdown cannot
publish into a restarted lifecycle.

Referenced asset failures produce explicit status-bearing placeholders rather than `NULL`:
images are a top-left 1x1 opaque magenta RGBA8 pixel and models are empty
version-800 descriptors. Allocation failure and invalid ABI/lifecycle arguments
return `NULL`. A zero-reference terrain table entry is not a registration:
`BZ_TTA_RegisterTerrainTexture()` returns `NULL` before SLK/MPQ lookup, without
logging or caching a placeholder.

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

The W3E cliff nibble reserves value 15 as a no-cliff sentinel rather than a
cliff-table index. `BZ_TTA_TERRAIN_NO_CLIFF` preserves that state and
`cliff_id` is zero for those corners. A bounded retail Human02 inspection found
2,349 sentinel corners; the first is corner 2732 (`x=23`, `y=21`) in its
129x129 corner grid. Other out-of-range ground/cliff indices remain malformed.

W3E ground/cliff counts describe the complete type tables, which may include
unreferenced editor data. The dense `BZ_TTTerrain_ReferencedTextureCount()` /
`BZ_TTTerrain_ReferencedTexture()` list contains only types referenced by
exported non-sentinel corners. Each `bzTTTerrainTextureInfo_t` preserves the
original `type_index`, FourCC `type_id`, and authoritative `corner_count`, so
corner indices and registration indices cannot diverge. Consumers register only
this list. Human02 contains cliff types `[CLdi, CLgr, CLno]`, but its corner
counts are `[2796, 11496, 0]`; retail ROC/TFT `CliffTypes.slk` contains no
`CLno` row because the map never references it. A referenced missing type still
returns the normal explicit status-bearing placeholder.

The publication token includes the map identity, vertex storage, dimensions,
and type-table storage. Repeated snapshots of an unchanged map reuse the
published immutable terrain.

Terrain image paths are resolved only inside the Warcraft source callback.
Ground FourCCs use `TerrainArt\Terrain.slk` fields `dir` and `file`, forming
`<dir>\<file>.blp`. Cliff FourCCs use `CliffTypes.slk` fields `texDir` and
`texFile`: the source first checks `<texDir>\<map tileset>_<texFile>.blp`, then
the generic `<texDir>\<texFile>.blp`. The returned retained image therefore
uses the same tileset fallback and MPQ override order as the desktop renderer.

## Entity metadata

Class IDs are resolved with the game spawn-table precedence: Doodad,
Destructable, then Unit. No network struct is widened and no server edict is
read. Unit `buffType=resource` identifies gold-mine resources without hardcoded
unit IDs; destructable `targType=tree` identifies lumber resources. Buildings,
resources, destructables, and pathing doodads derive footprint dimensions from
bounded TGA width/height multiplied by the 32-world-unit routing cell size.
Mobile units use twice the authentic `ucol` collision radius as their
world-unit footprint diameter. Missing required
path/collision data returns an explicit cached/log-once error instead of
guessing from selection radius or model bounds.

`Doodads.slk` uses the literal `pathTex=none` for authored non-pathing rows.
This is an absent optional footprint, not an archive identity. ROC and TFT
Human02 rows `LPwh`, `LOfl`, `LOth`, `LPrs`, `LPlp`, `LOtz`, `LOsm`, `AWfs`,
`LPcw`, and `AOsr` therefore resolve as valid doodad metadata with a zero
footprint. A non-sentinel path identity that is missing or malformed remains an
explicit cached/log-once metadata error.

Table tint and custom team-color values are defaults. Callers can replace team
color and/or runtime tint with the corresponding `override_mask` bits;
`BZ_TTA_TEAM_COLOR_NONE` means the table defines no custom team color.

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

`BZ_TTA_RegisterModelTexture()` resolves ordinary `TEXS` paths directly. For an
empty nonzero replaceable record, the WC3 source maps the model metadata
`class_id` through the immutable map alias snapshot (including custom
destructables from `war3map.w3b`), requires a matching
DestructableData `texID`, and registers `<texFile>.blp` through normal FS/MPQ
override order. Missing, mismatched, malformed, or unconfined authored mappings
remain explicit cached placeholders. Retail ROC and TFT both declare `LTlt`
replaceable ID 31 as
`ReplaceableTextures\LordaeronTree\LordaeronSummerTree`; the BLP is inherited
from `War3.mpq` when TFT archives are mounted.

Known model gaps are MDX 1000/1500, full animation curves and skinning
matrices, geoset animations, particle/ribbon emitters, and event/camera data.
Classic MDX may omit a geoset `UVBS` stream entirely; the descriptor then
authoritatively reports `uv_count=0`. Consumers may synthesize zero UVs for that
fully absent stream, but a partial nonzero stream remains malformed.

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

Retail ROC/TFT checks use the existing diagnostic tool without extracting data:

```sh
build/bin/mpqtool -data "/Users/clancey/Downloads/Warcraft III" grep Ldrt TerrainArt
build/bin/mpqtool -data "/Users/clancey/Downloads/Warcraft III" grep pathTex Doodads
build/bin/mpqtool -data "/Users/clancey/Downloads/Warcraft III" grep ngol Units
build/bin/mpqtool -mpq "/Users/clancey/Downloads/Warcraft III/War3.mpq" \
  info "Maps/Campaign/Human02.w3m"
build/bin/mpqtool -mpq "/Users/clancey/Downloads/Warcraft III/War3.mpq" \
  info "ReplaceableTextures/LordaeronTree/LordaeronSummerTree.blp"
```

The retail rows confirm ROC `Ldrt` as
`TerrainArt\LordaeronSummer\Lords_Dirt.blp`, `CLdi` as
`ReplaceableTextures\Cliff` + `Cliff0`, and both ROC/TFT `ngol` as
`PathTextures\16x16Goldmine.tga` with `buffType=resource`. The final command
confirms the campaign-map archive identity without copying the map from
`War3.mpq`.

`make run-sc2` currently hardcodes `-data data/StarCraft2` and exposes no
external-data variable. Validation with a legal install outside the repository
therefore stops at `make opensc2`; do not symlink or copy that install into the
tree merely to exercise the convenience target.

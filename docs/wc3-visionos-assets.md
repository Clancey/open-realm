# Warcraft III visionOS asset export

The C asset layer exports trusted CPU-side Warcraft III data to native
renderers without exposing engine or desktop renderer internals.

## ABI contract

`platform/bridge/bz_tabletop_assets.h` defines ABI version 2. Public values are
fixed-width C PODs or opaque retained `bzTTAsset_t`/`bzTTTerrain_t` handles.
All payloads are immutable after publication. Version 2 appends
`BZ_TTA_CATEGORY_ITEM`; the metadata POD remains 36 bytes.

Asset identity comes from a retained tabletop snapshot and a configstring slot:

```c
asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                    snapshot, CS_MODELS + model_index,
                                    BZ_TTA_ASSET_MODEL, &metadata);
texture = BZ_TTA_RegisterModelTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                      asset, texture_index);
team_count = BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                     BZ_TTA_TEAM_TEXTURE_GLOW);
team_glow = BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                      BZ_TTA_TEAM_TEXTURE_GLOW, team_color);
count = BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND);
BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND, 0, &terrain_texture);
ground = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                       BZ_TTA_TERRAIN_TEXTURE_GROUND, terrain_texture.type_index);
water = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                      BZ_TTA_TERRAIN_TEXTURE_WATER, 0);
status = BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                      &entity_input, &metadata);
```

The Warcraft source validates that the configstring is a confined relative
path, then calls `FS_ReadFile`. This preserves the established loose-file and
MPQ override order, including TFT archives overriding ROC assets. Absolute
paths, drive prefixes, empty components, `.`/`..`, and control characters are
rejected before filesystem lookup. Model textures can only be registered from
their parsed MDX `TEXS` record; the ABI does not expose a guessed-path
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

The Swift adapter mirrors the current public configstring layout's
`CS_MODELS == 32` when converting the entity's relative model index to the
absolute slot required by `BZ_TTA_RegisterConfigString`. It performs this call
before releasing the retained transport snapshot. No archive path is inferred.

## Team textures

Classic MDX replaceable IDs 1 and 2 are per-entity team color and team glow,
respectively. The public `bzTTTeamTextureKind_t` values intentionally match
those IDs. `BZ_TTA_TeamTextureCount()` reports the active provider's authored
range; Warcraft returns 16 for both kinds. `BZ_TTA_RegisterTeamTexture()` accepts
only indices `[0, count)` and resolves the identity entirely in C:

- color: `ReplaceableTextures\TeamColor\TeamColor%02u.blp`;
- glow: `ReplaceableTextures\TeamGlow\TeamGlow%02u.blp`.

The caller passes the entity's team-color index, never a path or model
metadata. This preserves shared model-template caching: the same MDX can be
drawn for multiple teams without first-entity cache poisoning. Existing
`BZ_TTA_RegisterModelTexture()` remains model-only, and `entity.image` remains
the independent per-entity destructable skin override.

Registration returns a retained immutable image with `OK`; a valid authored
index whose file is missing returns the normal cached/log-once `NOT_FOUND`
placeholder. Unsupported kinds, indices outside the provider count, invalid
ABI, and inactive lifecycle return `NULL` without lookup or cache activity.
Concurrent registration shares one decoded cache entry.

Desktop `MDLX_GetTexture()` selects team color/glow before the generic
entity-skin override and binds the selected image using the MDX layer's authored
blend mode and flags. Retail `War3.mpq` contains `TeamColor00..15.blp` and
`TeamGlow00..15.blp`; `War3x.mpq` contains no replacements, so TFT inherits the
ROC images through normal archive order. Team color 00 is an opaque uniform 8x8
image, while team glow 00 is an authored opaque 32x32 gradient; substituting a
solid palette color for glow loses required spatial image data.

Swift deep-copies each retained image once per `(kind, team_color)` while the
asset lifecycle is valid, then keys per-entity material variants by copied image
kind, team index, and content. Team images never mutate or key shared geometry templates. Textureless
fixture roles may use the deterministic Swift palette; production team roles
always consume the C image and preserve the exported MDX blend/flags/alpha.

```sh
build/bin/mpqtool -mpq "$HOME/Downloads/Warcraft III/War3.mpq" \
  ls ReplaceableTextures/TeamColor
build/bin/mpqtool -mpq "$HOME/Downloads/Warcraft III/War3.mpq" \
  ls ReplaceableTextures/TeamGlow
```

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

These values drive desktop-equivalent water placement. Swift emits water only
when at least one corner is wet and no corner is a map edge, uses the four
corrected water heights, repeats the registered singleton image every three
tiles, and clamps each corner's opacity to
`max(0, min(0.5, (water_height - ground_height) / 50))`. RealityKit carries the
four opacity values as vertex color into a water-only shader graph; it does not
replace the exported image with a color material.

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
this list and preserve its original indices when building layers; unreferenced
gaps in the complete W3E table do not require registration, and the lowest
published index becomes the base when index zero is unused. Human02 contains
cliff types `[CLdi, CLgr, CLno]`, but its corner
counts are `[2796, 11496, 0]`; retail ROC/TFT `CliffTypes.slk` contains no
`CLno` row because the map never references it. A referenced missing type still
returns the normal explicit status-bearing placeholder.

Water is one semantic terrain image rather than a W3E FourCC table. The
referenced-texture APIs return one record only when at least one tile matches
desktop `IsTileWater`: any of its four corners is water-flagged and none is
map-edge suppressed. Its record has `type_index=0`, `type_id=0`, and
`corner_count` equal to the total authoritative water-flagged corner count.
The renderer registers index zero and still determines individual cell
visibility, UVs, and depth opacity from exported corners. The Warcraft provider
resolves it to `ReplaceableTextures\Water\Water12.blp`. A no-water map, or one
whose water is entirely map-edge suppressed, returns zero references and
registration returns `NULL` before path resolution, archive lookup, cache
insertion, or logging. A referenced but missing Water12 file uses the normal
retained `NOT_FOUND` placeholder, log-once, and cache semantics. Success returns
a retained `OK` image; each registration owns one caller reference that must be
released.

The publication token includes the map identity, vertex storage, dimensions,
and type-table storage. Repeated snapshots of an unchanged map reuse the
published immutable terrain.

Terrain image paths are resolved only inside the Warcraft source callback.
Ground FourCCs use `TerrainArt\Terrain.slk` fields `dir` and `file`, forming
`<dir>\<file>.blp`. Cliff FourCCs use `CliffTypes.slk` fields `texDir` and
`texFile`: the source first checks `<texDir>\<map tileset>_<texFile>.blp`, then
the generic `<texDir>\<texFile>.blp`. The returned retained image therefore
uses the same tileset fallback and MPQ override order as the desktop renderer.
The global water identity matches desktop `R_GameLoadAssets()`. Verify the
archive placement without extracting proprietary data:

```sh
build/bin/mpqtool -mpq "$HOME/Downloads/Warcraft III/War3.mpq" \
  info ReplaceableTextures/Water/Water12.blp
build/bin/mpqtool -mpq "$HOME/Downloads/Warcraft III/War3x.mpq" \
  info ReplaceableTextures/Water/Water12.blp
```

Retail ROC reports a 14,963-byte uncompressed Water12 entry; `War3x.mpq` has no
replacement, so TFT correctly inherits the ROC image through archive override
order.

## Entity metadata

Class IDs are resolved with the game spawn-table precedence: Doodad,
Destructable, Unit, then Item. No network struct is widened and no server edict is
read. Unit `buffType=resource` identifies gold-mine resources without hardcoded
unit IDs; destructable `targType=tree` identifies lumber resources. Buildings,
resources, destructables, and pathing doodads derive footprint dimensions from
bounded TGA width/height multiplied by the 32-world-unit routing cell size.
Mobile units use twice the authentic `ucol` collision radius as their
world-unit footprint diameter. Missing required
path/collision data returns an explicit cached/log-once error instead of
guessing from selection radius or model bounds.

Items resolve from the active whole-file `Units\ItemData.slk` replacement.
ROC names the classification column `itemClass` and has no scale/tint columns;
TFT names it `class` and adds `scale` plus `colorR/G/B`. Initialization detects
that table schema before binding `ItemsMetaData`, so toggling TFT never combines
rows or column names from ROC. The retail IDs `rde4`, `ratf`, `rlif`, `rwiz`,
`prvt`, and `ckng` are the little-endian values `0x34656472`, `0x66746172`,
`0x66696c72`, `0x7a697772`, `0x74767270`, and `0x676e6b63`.

ItemData authors no pathing texture or collision field. Items therefore export
the distinct item category with a zero footprint; selection size is never used
as a collision proxy. ROC preserves the placed/JASS entity scale because it has
no table scale. TFT item spawn applies its authored scale (all 273 retail rows
use `1`), and the transport already carries that server-authored entity scale.
Missing rows and malformed model identities remain cached/log-once metadata
errors; a confined identity whose model file is absent uses the normal cached
model placeholder.

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
- geoset positions, normals, optional UVs, and validated 16-bit triangle indices; MDX 800 permits
  an absent `UVBS` array, which Swift expands to deterministic `(0,0)` coordinates for RealityKit,
  while any nonzero UV count must match the vertex count;
- materials and static layers, including blend/filter mode and texture index;
- texture references, replaceable IDs, and wrapping flags;
- sequences;
- nodes, pivots, and the first translation/rotation/scaling track values.

MDX replaceable IDs 1 and 2 are team color and glow. Other IDs require
the server-authored per-entity image override rather than a model-name
inference. `SP_SpawnDestructable()` resolves `DestructableData.slk` `texFile`
through `ImageIndex`; transport ABI v1 already copies that relative
`CS_IMAGES` index. Swift registers the configstring while its retained snapshot
is alive and applies the copied image only to non-team replaceable layers.
Model-template cache keys include the override identity and copied-content
hash so two entities may share MDX geometry without sharing different skins.
Human02's replaceable-ID-31 Lordaeron trees exercise this path.

RealityKit lacks destination-color multiply blending. Human02's filter-mode-5
SmokeSmudge textures are grayscale JPEGs (bounded retail probe: maximum RGB
channel spread 5), so Swift converts them exactly to unlit black alpha masks.
Colored modulate textures remain explicit unsupported materials; mode 6 applies
the documented 2x luminance before conversion.

A bounded TFT `HumanX01` simulator probe found 60 explicit colored-modulate
placeholder materials, primarily on `ArcaneObservatory.mdx` and `Ziggurat.mdx`.
Use `NightElfX01` for zero-placeholder TFT acceptance until RealityKit gains an
authoritative colored destination-modulate equivalent.

MDX texture records contain a 260-byte path field. The ABI identity buffer is
therefore 260 bytes; treating it as a conventional 256-byte path truncates
valid `TEXS` records. Material, layer, geoset, and node record sizes include
their own size DWORD; top-level chunk sizes do not. Inclusive records and
counted arrays are checked against their containing span before trailing data
is read.

Known model gaps are MDX 1000/1500, full animation curves and skinning
matrices, geoset animations, particle/ribbon emitters, and event/camera data.
Classic MDX may omit a geoset `UVBS` stream entirely; the descriptor then
authoritatively reports `uv_count=0`. Consumers may synthesize zero UVs for that
fully absent stream, but a partial nonzero stream remains malformed.
Retail MDX 800 geosets place `UVAS`/`UVBS` after the fixed `MATS` metadata block.
The bounded record parser must continue after `MATS`; treating it as an end marker
silently discards every authored model UV while leaving positions and indices valid.

## Swift value adapter

`WarcraftAssetAdapter.swift` contains framework-free exported-value records and
validation/lowering. `LiveTabletopTransport.swift` is the only Swift file that
touches retained C handles: it copies model bounds, geosets, materials/layers,
textures, sequences, nodes, terrain corners/type tables/images, resolved entity
metadata, and cache counters, then releases each handle before actor/main-thread
handoff. Terrain textures are registered only while their retained terrain
snapshot is alive. One copied terrain and at most 256 copied models are retained
for the live transport lifecycle, avoiding repeated corner/geoset/pixel copies
while C registration continues to validate cache-hit behavior. MDX z-up
positions and normals become RealityKit y-up values
and triangle winding is reversed to preserve front faces. BLP orientation
remains explicit and is normalized to top-left before texture creation.

Swift iterates the dense referenced-texture list, validates each original table
index/FourCC/corner count, then passes `type_index` to
`BZ_TTA_RegisterTerrainTexture`; the game exporter owns all
Terrain.slk/CliffTypes.slk identity, zero-reference filtering, and fallback
rules. The pure terrain adapter retains the complete type tables for corner
layer semantics and applies the desktop 64x64 atlas/layer-mask rules, including
base variation selection and the 5% center inset. It preserves no-cliff
sentinels, uses per-cell cliff materials, and never accepts placeholder
ground/cliff images as production terrain.

Entity classes pass through `BZ_TTA_ResolveEntityMetadata`. The transport
player is an explicit team-color override; no runtime tint override is supplied
because the transport has no authoritative tint field. Successful results
drive mobile/building/resource/doodad/destructable/item categories and WC3 table
footprints. Failed results remain explicit placeholders rather than Swift
radius/category guesses.

## Validation

```sh
make test-bz-tabletop-assets
make test
make openwarcraft3 opensc2
make xrsimulator xros
```

Focused fixtures cover ROC/TFT terrain and entity metadata resolution,
asymmetric BLP orientation, MDX geometry/material/bounds and repeated inclusive
records, terrain dimensions/corners/water/cliffs/no-cliff sentinels and atlas
layer UVs, concurrent placeholder log-once/cache behavior,
path confinement, malformed spans and zero arrays, lifecycle-crossing loads,
retained concurrent readers, and cleanup/publication races. The fixtures are
generated data; retail MPQs and decoded proprietary outputs must never enter
the repository.

Retail ROC/TFT checks use the existing diagnostic tool without extracting data:

```sh
build/bin/mpqtool -data "/Users/clancey/Downloads/Warcraft III" grep Ldrt TerrainArt
build/bin/mpqtool -data "/Users/clancey/Downloads/Warcraft III" grep pathTex Doodads
build/bin/mpqtool -data "/Users/clancey/Downloads/Warcraft III" grep ngol Units
build/bin/mpqtool -data "/Users/clancey/Downloads/Warcraft III" grep rde4 Units
build/bin/mpqtool -mpq "/Users/clancey/Downloads/Warcraft III/War3.mpq" \
  info "Maps/Campaign/Human02.w3m"
```

The retail rows confirm ROC `Ldrt` as
`TerrainArt\LordaeronSummer\Lords_Dirt.blp`, `CLdi` as
`ReplaceableTextures\Cliff` + `Cliff0`, and both ROC/TFT `ngol` as
`PathTextures\16x16Goldmine.tga` with `buffType=resource`. The final command
confirms the campaign-map archive identity without copying the map from
`War3.mpq`.

Simulator acceptance keeps the disposable Human02 process alive for 240 seconds
after the first stable summary. This is required because the six campaign items
enter the capped visible-entity publication later; the gate rejects any late
metadata error, explicit metadata placeholder, or missing production descriptor
rather than trusting the earlier zero-placeholder summary.

`make run-sc2` currently hardcodes `-data data/StarCraft2` and exposes no
external-data variable. Validation with a legal install outside the repository
therefore stops at `make opensc2`; do not symlink or copy that install into the
tree merely to exercise the convenience target.

# StarCraft II visionOS terrain, DDS, and M3 assets

Layer 2A publishes immutable SC2 terrain and encoded DDS descriptors for a later native renderer. The public C99 ABI is
`games/starcraft-2/visionos/sc2_tabletop_assets.h`; it is independently versioned at
`BZ_SC2_TABLETOP_ASSETS_ABI_VERSION == 1` and does not reuse Warcraft's W3E/MDX asset ABI.

Layer 2B1 adds the separately versioned `sc2_tabletop_models.h`
(`BZ_SC2_TABLETOP_MODELS_ABI_VERSION == 1`). It publishes static M3 geometry plus raw authored
material/layer descriptors. Layer 2A remains ABI v1 and adds only the append-only
`BZ_SC2A_RegisterImage()` entry point, so terrain and model layers share one confined DDS cache.

## Ownership and lifecycle

- `BZ_SC2A_LatestTerrain()` and `BZ_SC2A_RegisterTerrainImage()` return retained opaque handles or typed placeholders.
- Terrain publication deep-copies the current authoritative `sc2Map_t` into one immutable allocation with trailing arrays.
- Image registration validates and copies encoded DDS mip payloads; no OpenGL or renderer-private pointer crosses the ABI.
- Callers release terrain and image handles explicitly. Retained handles remain readable across map replacement and shutdown.
- A mutex protects publication, refcounts, cache state, and counters. One provider mutex shared by
  the image and model modules serializes every `FS_ReadFile()` call and provider shutdown.
- Image paths are confined relative archive identities and normalized to backslashes before cache lookup.
- Missing, malformed, unsupported, and oversized images remain exact cached placeholder statuses and log once per identity.
- Overlong invalid identities use one bounded prefix-and-hash cache/log key, so repeat failures
  remain cache hits and cannot allocate or log indefinitely.
- Model handles use the same retained/cache-generation contract. One immutable allocation contains
  canonical vertices, U16 indices, divisions, regions, batches, bone lookups, material references,
  standard materials, composite sections, and flattened layers.
- Model descriptors never contain parser allocations, archive pointers, SDL/OpenGL objects, or
  renderer-private pointers. Retained handles remain readable after registration reload or terminal
  shutdown.
- Every typed trailing-array segment is aligned independently with `_Alignof(TYPE)`; byte-size and
  offset arithmetic is checked before allocation. Terrain image registration carries its originally
  captured source and generation through cache publication instead of recapturing lifecycle state.

The cell grid is capped at 1024 per dimension. HMAP may be one sample larger. MASK is authored at eight times cell
resolution, so it has a separate 8192 dimension cap and a 256 MiB decoded payload cap. DDS dimensions are capped at
16384 and encoded payloads at 256 MiB.

## Exported terrain data

The terrain descriptor includes map generation, availability/malformed/unsupported flags, cell/HMAP/MASK dimensions,
resolved diffuse and normal identities, cliff sets and cells, decoded visible heights, LFCT/CLIF cells, decoded 0..15
MASK layers, fog, and lighting. Known-but-unparsed water, synchronized pathing, fluff doodad, hard tile, vertex color,
and painted pathing data are surfaced as unsupported flags rather than silently discarded.

DDS descriptors preserve DXT1, DXT3, DXT5, RGB8, BGR8, RGBA8, or BGRA8 encoded mip data. Cubemap, volume,
luminance, BC4, BC5/ATI2, DX10, packed YUV, unknown masks, truncation, overflow, and excessive sizes are rejected
explicitly. Uncompressed rows follow Microsoft's byte-tight DDS file-layout formula; non-tight declared runtime
pitch is rejected because the header cannot describe lower-mip strides, and desktop GL upload uses unpack alignment
1. Layer 2A does not transcode DDS to RGBA.

## TRaynor01 retail evidence

Measure the read-only retail fileset without extracting or modifying it:

```sh
make sc2map
build/bin/sc2map -data "$HOME/Downloads/Starcraft II/StarCraft2" \
  --asset-inventory Maps/Campaign/TRaynor01.SC2Map
```

The frozen Layer 2A inventory is:

| Field | Exact value |
| --- | --- |
| Cells / HMAP / MASK | `136x160` / `137x161` / `1088x1280x8` |
| Terrain textures / cliff sets / cliff cells | `8` / `2` / `1576` |
| Availability / malformed / unsupported | `0x0000003f` / `0x00000000` / `0x0000003d` |
| Fog / lighting | `1` / `0` |
| Referenced terrain images | `16`, all present |
| Every terrain image | DXT5, `1024x1024`, 11 mips, 1,398,128 payload bytes |

The live catalog reports 1,024 retained objects (the parser cap), 3 units, 7 actors, 6 models,
0 footprints, 261 unresolved models, and 763 resolved object instances.

## Layer 2B1 M3 contract and retail evidence

Layer 2B1 intentionally stops before decoded rendering policy. It exports packed normals/tangents,
all authored UV sets and vertex color, four bone influences, U16 indices, region-local bone lookup
offsets, material references, raw standard-material fields, all 14 layer kinds, static initial
values, animated bits, and texture identities. `BZ_SC2M_RegisterLayerImage()` resolves a layer
through the Layer 2A encoded-DDS handle.

Nonstandard material kinds, recursive composite evaluation, animated layer evaluation, and
unverified material flag meanings remain explicit `BZ_SC2M_UNSUPPORTED_*` bits. No record is
demoted or silently dropped. Skeleton hierarchy/evaluation, animations, attachments, entity
resolution, renderer policy, Swift, RealityKit, and simulator work are Layer 2C or later.

The diagnostic milestone used the exact desktop archive order and read-only retail files. All 178
unique M3 identities exercised by TRaynor01 parse headlessly: 49 resolved catalog-object models and
129 cliff variants. All are MODL version 23. A strict declaration-validation sweep after parser
hardening accepts all 178 with zero malformed or unsupported-version failures.

M3 decoding is cumulatively bounded at 256 MiB of input/owned decoded bytes and 16 Mi parse-work
operations. These limits preserve legal shared references without shared ownership, while rejecting
reference aliasing that would otherwise amplify a roughly 1 MiB file into gigabytes of deep copies.
The strict TRaynor01 set remains 178/178. An extended 179-model rerun adds ThorCE's 4,437-reference
model and peaks at 3,277,644 decoded bytes and 219,705 work operations. Run `m3tool --info` to inspect
the exact counters for any retail identity.

| Inventory | Object models | Cliff models |
| --- | ---: | ---: |
| Unique identities | 49 | 129 |
| Vertices / U16 indices | 127,249 / 303,687 | 13,399 / 38,379 |
| Divisions / regions / batches | 49 / 86 / 86 | 129 / 129 / 129 |
| Bones / bone lookups | 714 / 390 | 129 / 129 |
| Standard materials / material references | 107 / 110 | 129 / 129 |
| Flattened material layers | 1,391 | 1,677 |
| Vertex declarations | 3 `0x0180007d`, 29 `0x0182007d`, 17 `0x0186007d` | 129 `0x0182007d` |
| Material-reference kinds | 107 standard, 2 displacement, 1 composite | 129 standard |
| Blend modes | 76 mode 0, 19 mode 1, 10 mode 2, 2 mode 3 | 129 mode 1 |
| Layer UV sources | 1,372 source 0, 5 source 1, 10 source 6, 4 source 7 | 1,677 source 0 |
| DDS layer results | 304 ok, 6 not found, 4 unsupported, 1,077 empty | 623 ok, 1,054 empty |

`BloodSplats_03.m3`, `Decal_03.m3`, and `FireMedium.m3` are valid zero-geometry material/effect
containers. `MengskHologramBillboard.m3` contains composite and displacement references;
`TRaynor01RadioTower.m3` contains displacement. These records drive the explicit 2B1 unsupported
status instead of being omitted. The one unsupported non-empty DDS identity is
`Assets\Textures\MengskStatue_Environment.dds`; the six missing identities remain visible in
`m3tool --dump-all`.

Marine freezes the representative skinned/two-UV public-ABI proof:

| Marine field | Exact value |
| --- | --- |
| Identity / MODL / declaration | `Assets\Units\Terran\Marine\Marine.m3` / 23 / `0x0186007d` |
| Vertices / U16 indices | 1,678 / 3,216 |
| Divisions / regions / batches | 1 / 2 / 2 |
| Bones / bone lookups | 38 / 20 |
| Region 0 | vertices 0..1551, 2,880 indices, bone lookup 0, four weights |
| Region 1 | vertices 1552..1677, 336 indices, bone lookup 19, one weight |
| Materials | one standard `marine`, flags `0x4`, additional flags `0x200`, blend 0 |
| Non-empty layers | diffuse, decal (UV1), specular, normal; all valid DXT5 |

## Tests

```sh
make test-sc2 test-sc2-tabletop-assets test-sc2-tabletop-models test-sc2-tabletop-runtime
SC2DATA="$HOME/Downloads/Starcraft II/StarCraft2" make test-sc2-live test-sc2-tabletop-live
make visionos-sc2
```

`test-sc2-tabletop-assets` covers the shared generic image cache, stable overlong keys, and terrain
registration across shutdown/re-init. `test-sc2-tabletop-models` covers ABI layouts, exact root and
section declarations, legacy MODL/REGN/DIV_ layouts, version-zero animation tables, geometry ranges
and face values, the desktop 128-bone capability, typed-array alignment, cross-image/model provider
serialization, malformed/missing/confined paths, cache reuse, reload, retained lifetime, and
terminal behavior. `test-sc2-renderer-model-dispatch` covers 0-3 byte inputs and valid four-byte M3
dispatch. `test-client-model-lifecycle` proves map-clear and final shutdown release every owned
desktop model/portrait slot exactly once. Runtime live proof freezes Marine through the public ABI.

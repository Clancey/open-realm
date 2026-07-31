# StarCraft II visionOS terrain and DDS assets

Layer 2A publishes immutable SC2 terrain and encoded DDS descriptors for a later native renderer. The public C99 ABI is
`games/starcraft-2/visionos/sc2_tabletop_assets.h`; it is independently versioned at
`BZ_SC2_TABLETOP_ASSETS_ABI_VERSION == 1` and does not reuse Warcraft's W3E/MDX asset ABI.

## Ownership and lifecycle

- `BZ_SC2A_LatestTerrain()` and `BZ_SC2A_RegisterTerrainImage()` return retained opaque handles or typed placeholders.
- Terrain publication deep-copies the current authoritative `sc2Map_t` into one immutable allocation with trailing arrays.
- Image registration validates and copies encoded DDS mip payloads; no OpenGL or renderer-private pointer crosses the ABI.
- Callers release terrain and image handles explicitly. Retained handles remain readable across map replacement and shutdown.
- A mutex protects publication, refcounts, cache state, and counters. Slow archive reads use a separate source mutex.
- Image paths are confined relative archive identities and normalized to backslashes before cache lookup.
- Missing, malformed, unsupported, and oversized images remain exact cached placeholder statuses and log once per identity.

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

The live catalog currently reports 1,024 retained objects (the parser cap), 3 units, 7 actors, 6 models, 0 footprints,
and 261 unresolved models. Those model/catalog limitations belong to later M3/entity work and are not hidden by this ABI.

## Tests

```sh
make test-sc2 test-sc2-tabletop-assets test-sc2-tabletop-runtime
SC2DATA="$HOME/Downloads/Starcraft II/StarCraft2" make test-sc2-live test-sc2-tabletop-live
make visionos-sc2
```

`test-sc2-tabletop-assets` covers ABI layouts, typed failures, path confinement, cache reuse, reload/shutdown lifetime,
and concurrent publication/registration/shutdown. The runtime tests freeze the fixture and TRaynor01 terrain/DDS
counters through the public ABI. Layer 2B owns M3 geometry and material export; Layer 2C owns skeleton, animation, and
entity visual resolution.

# StarCraft II

This is an alternate game target for StarCraft II data experiments. It exists mainly to keep the engine honest across more than one Blizzard RTS asset family and to exercise the M3 renderer path behind the same selected-game module boundary.

The code here owns a small game module and the StarCraft II M3 renderer hooks.

## Status

Desktop runtime plus a headless native visionOS snapshot foundation.

`opensc2` loads retail Wings of Liberty maps including TRaynor01, parses SC2
catalog/layout/terrain data, renders M3 models and terrain on desktop, publishes
an SC2 HUD, and supports minimal authoritative selection and movement. It is not
a complete StarCraft II implementation.

## Working

- Separate `opensc2` executable and game/renderer libraries.
- Selected-game module integration for StarCraft II via the shared engine build.
- M3 model loader entry point for StarCraft II model data.
- M3 material, reference table, sequence, keyframe interpolation, and shader scaffolding.
- Basic skeletal/skinned model render path through the compound renderer.
- Minimal game module that can initialize, load map collision through the shared map interface, and provide required game exports.
- Build integration through `make opensc2`.
- SC2-specific layout/HUD parsing and server-authored HUD delivery.
- Catalog-driven unit/actor/model resolution and parsed map terrain/objects.
- Minimal authoritative selection and point/entity movement.
- arm64 `xrsimulator`/`xros` headless engine and bridge archives with immutable
  snapshot ABI v3 and typed commands.

## Partial

- M3 support is actively shaped around the renderer path and is not full StarCraft II asset parity.
- The game module implements only the minimal unit lifecycle and movement needed
  to exercise the authoritative runtime.
- The desktop SC2 HUD is functional but not a complete menu or gameplay UI.
- The visionOS layer has no visible native app or asset renderer yet.

## Not There Yet

- Playable StarCraft II gameplay.
- StarCraft II data table, trigger, ability, race, or campaign systems.
- Complete SC2 map format support.
- Complete M3 material, animation, particle, attachment, and lighting fidelity.
- Complete StarCraft II menus, editor-like behavior, or multiplayer flow.
- RealityKit SC2 terrain/M3 rendering and native visionOS HUD/product flow.

## Build And Run

Build:

```bash
make opensc2
```

Run with the Makefile's sample StarCraft II data path and first Terran campaign map:

```bash
make run-sc2
```

Or run directly through map resolution:

```bash
build/bin/opensc2 -data data/StarCraft2 +map TRaynor01
```

## Notes

This target expects locally supplied StarCraft II data for real asset experiments. Original assets, names, and game data belong to Blizzard Entertainment. The directory is here so the engine can grow beyond one asset format without pretending the SC2 game is already built.

## Documentation

Public reverse-engineering and modding references for how StarCraft II maps are stored, opened, and rendered. Not Blizzard documentation and not a complete implementation spec — a map for loader and renderer work.

### Documents

- [Map Storage And Loading](docs/map-storage-and-loading.md) — container format, component folders, dependency/XML loading behavior, and cache/download context.
- [Embedded Map Files](docs/embedded-map-files.md) — full binary specs for all known files inside `.SC2Map` archives.
- [Map, Model, And Unit Data](docs/map-model-unit-data.md) — practical path from placed objects through catalog XML to M3 models.
- [Parser Notes](docs/parser-notes.md) — practical loading order and implementation guidance.
- [HUD Layout Pipeline](docs/hud-layout-pipeline.md) — `.SC2Layout` → `sc2BaseFrame_t` → `uiFrame_t` → `svc_layout` pipeline; UI texture resolution via Assets.txt.
- [Retail Installer Extraction](docs/installer-extraction.md) — read-only ISO/MPQE provenance, MPQ v2 repack rules, exact local manifest, and validation.
- [visionOS Foundation](docs/visionos-foundation.md) — headless archives, selected-game transport seam, staging, tests, and next-layer boundary.
- [References](docs/references.md) — all public sources, tools, and GitHub repos used.
- [Sounds](docs/sounds.md)

### File Format Details

- [MapInfo](docs/file-formats/mapinfo.md) — complete `MapInfo` binary struct with all fields and player slot layout.
- [PlacedObjects](docs/file-formats/objects.md) — complete `<PlacedObjects>` XML schema.
- [Actors And Models](docs/file-formats/actors-and-models.md) — actor system, `CActorUnit` key fields, `CModel` catalog fields.
- [M3 Model Format](docs/file-formats/m3.md) — M3 binary model format: reference table, geometry, skeleton, animations, materials.
- [Assets.txt (UI Texture Catalog)](docs/file-formats/assets-txt.md) — `GameData/Assets.txt` skin format (`UI/Key=path`), archive priority, and how `hud.c` loads it.

### Short Version

`.SC2Map`, `.SC2Mod`, `.SC2Archive`, and `.s2ma` files are MPQ archives. A map contains metadata, terrain/pathing binary layers, placed-object XML, minimap/loading assets, localized strings, trigger/Galaxy code, and map-local game-data XML.

### Implementation Status

| Area | Status |
| --- | --- |
| MPQ loading | done |
| `MapInfo` | done |
| `Objects` (units, doodads) | done |
| `t3Terrain.xml` (cliff sets, cells, textures) | done |
| `t3HeightMap` | done |
| `t3SyncHeightMap` (fine height detail) | done |
| `t3SyncCliffLevel` | done |
| `t3CellFlags` | done |
| `t3TextureMasks` | done |
| Terrain rendering (ground + cliff walls) | done |
| Cliff config canonicalization | done |
| `t3SyncPathingInfo` (pathing) | **not started** |
| `t3Water` | **not started** |
| `t3FluffDoodad` | **not started** |
| Catalog-driven unit → model resolution | done |
| `.m3a` animation supplements | **not started** |
| Team-color texture swapping | **not started** |

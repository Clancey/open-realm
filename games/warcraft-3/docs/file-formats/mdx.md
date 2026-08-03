# Warcraft III MDX Model Format

MDX (also written as MDLX) is Warcraft III's binary 3D model format. It stores geometry, materials, bones, animations, particle emitters, and more in a chunked binary layout.

## File Layout

An MDX file is a sequence of tagged chunks. Each chunk begins with a 4-byte FourCC identifier followed by a 4-byte chunk size in bytes. The top-level structure is:

```
MDLX                       ← magic / version header
VERS  <size>  <version>    ← format version (800 for WC3, 1000 / 1500 for Reforged)
MODL  <size>  <modelInfo>  ← global model info (name, bounds)
SEQS  <size>  [sequence]…  ← named animation sequences
GLBS  <size>  [globalSeq]… ← global sequence durations
TEXS  <size>  [texture]…   ← texture path list
MTLS  <size>  [material]…  ← materials (layer stacks)
GEOS  <size>  [geoset]…    ← geometry (vertices + faces)
BONE  <size>  [bone]…      ← skeleton bones
HELP  <size>  [helper]…    ← helper nodes (invisible pivot points)
ATCH  <size>  [attach]…    ← attachment points (e.g. "Overhead")
PIVT  <size>  [pivot]…     ← per-node pivot positions
PREM  <size>  [emitter]…   ← particle emitter v1
PRE2  <size>  [emitter2]…  ← particle emitter v2
RIBB  <size>  [ribbon]…    ← ribbon emitters
EVTS  <size>  [event]…     ← event objects (sounds, etc.)
CLID  <size>  [collision]… ← collision shapes
LITE  <size>  [light]…     ← light nodes
```

Not all chunks are present in every model. The Warcraft III loader (`games/warcraft-3/renderer/mdx/r_mdx_load.c`) dispatches on each FourCC tag.

## Node Hierarchy

All nodes (bones, helpers, attachments, emitters, event objects, collision shapes, lights) share a common **node header**:

| Field | Type | Description |
|-------|------|-------------|
| `size` | `DWORD` | Total size of this node record in bytes |
| `name` | `char[80]` | Human-readable name |
| `objectId` | `DWORD` | Unique node index (0-based) |
| `parentId` | `DWORD` | Parent node index (`0xFFFFFFFF` = root) |
| `flags` | `DWORD` | Node type and billboarding flags |

The `flags` field uses the following bitmasks:

| Flag | Value | Meaning |
|------|-------|---------|
| `Helper` | `0` | Plain helper (pivot only) |
| `DontInheritTranslation` | `1` | Ignore parent translation |
| `DontInheritRotation` | `2` | Ignore parent rotation |
| `DontInheritScaling` | `4` | Ignore parent scale |
| `Billboarded` | `8` | Always faces the camera |
| `BillboardedLockX` | `16` | Billboarded on X axis only |
| `BillboardedLockY` | `32` | Billboarded on Y axis only |
| `BillboardedLockZ` | `64` | Billboarded on Z axis only |
| `CameraAnchored` | `128` | Positioned relative to the camera |
| `Bone` | `256` | This node is a skeleton bone |
| `Light` | `512` | This node is a light source |
| `EventObject` | `1024` | Triggers events (sounds, effects) |
| `Attachment` | `2048` | Named attachment point |
| `ParticleEmitter` | `4096` | Particle emitter |
| `CollisionShape` | `8192` | Physics collision volume |
| `RibbonEmitter` | `16384` | Ribbon/trail emitter |

## Keyframe Tracks (Animated Values)

Animation data is stored as **keyframe tracks**. Each track is identified by a 4-byte tag that encodes the node type and the animated property. For example:

| Tag | Target | Property |
|-----|--------|----------|
| `KGTR` | Node | Translation |
| `KGRT` | Node | Rotation (quaternion) |
| `KGSC` | Node | Scale |
| `KMTA` | Material layer | Alpha |
| `KMTE` | Material layer | Emissive gain |
| `KP2V` | Particle emitter v2 | Visibility |
| `KP2E` | Particle emitter v2 | Emission rate |
| `KLAV` | Light | Ambient intensity |

A keyframe track record starts with:

```
DWORD  numKeys
DWORD  interpolationType   // 0=none, 1=linear, 2=hermite, 3=bezier
DWORD  globalSeqId         // 0xFFFFFFFF if not driven by a global seq
[key × numKeys]
```

Each key is `{ DWORD frame; <value>; [<inTan>; <outTan>] }` where the tangent pair is only present for hermite/bezier interpolation.

## Geoset (Geometry)

A geoset is one draw call: a set of vertices sharing the same material. Its sub-chunks are:

| Sub-chunk | Description |
|-----------|-------------|
| `VRTX` | Vertex positions — `float[3]` each |
| `NRMS` | Vertex normals — `float[3]` each |
| `UVBS` | UV sets — `float[2]` each |
| `PTYP` | Primitive types (always `4` = triangles) |
| `PCNT` | Primitive counts |
| `PVTX` | Vertex index list (triangle list) |
| `GNDX` | Bone group index per vertex |
| `MTGC` | Vertex count per bone group |
| `MATS` | Bone indices for each bone group |
| `UVAS` | Number of UV channels |
| `UVBS` | UV coordinates for a channel |

The renderer builds a static VBO from `VRTX`/`NRMS`/`UVBS` and a matrix palette from the `MATS`/`GNDX`/`MTGC` tables for GPU skinning.
Retail MDX 800 writes the fixed `MATS` metadata first, followed by `UVAS` and
its `UVBS` stream. A geoset record is bounded by its inclusive size DWORD, not
by `MATS`; readers must continue through the remaining tagged data.

## Geoset Flags

The `flags` field of a geoset (`mdxGeoFlags_t`) controls render state:

| Flag | Value | Meaning |
|------|-------|---------|
| `Unshaded` | `0x01` | No lighting — use vertex colour directly |
| `TwoSided` | `0x10` | Disable back-face culling |
| `Unfogged` | `0x20` | Not affected by distance fog |
| `NoDepthTest` | `0x40` | Always drawn on top |
| `NoDepthSet` | `0x80` | Do not write to the depth buffer |

## Material Layers

Each material is a stack of one or more layers rendered in order (additive blending for particle effects, etc.). A layer record includes:

- filter mode (none, transparent, blend, additive, add-alpha, modulate, modulate2×)
- texture index (into the `TEXS` table)
- texture animation index
- flag bits (unshaded, sphere env map, two-sided, unfogged, no-depth-test, no-depth-set)
- animated alpha (`KMTA` track)

## Particle Emitters (PRE2)

**Authoritative scope audit** (traced, not assumed - see
`platform/android/quest/app/src/main/cpp/bz_quest_wc3_particles.h`'s header comment for the
consumer-side citation): of every MDX chunk that could plausibly be called a "particle/spell/
transient effect", **PRE2 (particle emitter v2) is the only one this codebase's authoritative
renderer parses, simulates, AND draws end to end** —
`games/warcraft-3/renderer/mdx/r_mdx_load.c`'s `ReadParticleEmitter` →
`r_mdx_geoset.c`'s `MDLX_RenderEmitter` → `renderer/r_particles.c`'s
`R_SpawnParticle`/`R_UpdateParticles`/`R_DrawParticles`. Specifically:

| Chunk | Parsed? | Simulated? | Drawn? | In scope? |
|-------|---------|------------|--------|-----------|
| `PRE2` (particle emitter v2) | Yes | Yes | Yes | **Yes** |
| `PREM` (particle emitter v1) | No parser anywhere in this codebase | - | - | No |
| `RIBB` (ribbon emitter) | No parser anywhere in this codebase | - | - | No |
| `EVTS` (event object) | Yes (`r_mdx_load.c`) | No - consumed by nothing; sound uses a wholly separate `entityState_t.event`/sound mechanism (see `doc/architecture/sound.md`) | No | No - would require hardcoding external asset-name conventions, forbidden by AGENTS.md |

### Chunk/record layout

`PRE2` is a top-level chunk (size DWORD + tagged records, like `GEOS`/`GEOA`). Each emitter record
has a genuine **nested double inclusive-size field** — confirmed empirically via a byte-exact
synthetic fixture round-trip against the real host parser (`r_mdx_load.c`), not guessed, and
cross-checked against the community-standard `mdx-m3-viewer` TS parser:

1. An **outer** inclusive size (consumed by the emitter-list iteration, like `parse_nodes()`'s own
   per-record `record_size`) wraps the *entire* emitter: generic node header + KGTR/KGRT/KGSC
   tracks + all PRE2-specific fields + PRE2's own `KP2x` tracks.
2. An **inner** inclusive size immediately follows and wraps *only* the generic node header + its
   own KGTR/KGRT/KGSC tracks (an emitter *is* a node — `mdxParticleEmitter_t` embeds `mdxNode_t`
   exactly like a bone/helper/light/attachment does).

This exact two-size-field shape is shared by `LITE` (Light) and `ATCH` (Attachment) records too
(`r_mdx_load.c`'s `ReadLight`/`ReadAttachment`) — it looks like a redundant re-read at first glance
but is not; plain nodes (`HELP`) or nodes whose trailing fields use `MSG_ReadOverflow` (`BONE`,
`CLID`, `EVTS`) have only one size field. See
`games/warcraft-3/visionos/wc3_mdx_decode.c`'s `parse_particle_emitters()` for the implementation.

After the inner-size-bounded node header, the PRE2-specific fields follow in this exact order
(`wc3_mdx_decode.c`, cross-checked field-for-field against `r_mdx.h`'s `mdxParticleEmitter_t`):

| Field | Type | Notes |
|-------|------|-------|
| Speed, Variation, Latitude, Gravity, LifeSpan, EmissionRate, Length, Width | `float` × 8 | static defaults; see "Animatable channels" below |
| FilterMode | `DWORD` | raw on-disk enum, see mapping table below |
| Rows, Columns | `DWORD` × 2 | atlas grid; a raw `0` means "1×1" (normalized at decode time, matching `r_particles.c`'s own `FX_GetFrame` per-frame `? 1` fallback) |
| FrameFlags | `DWORD` | raw Head(0)/Tail(1)/Both(2), see mapping table below |
| TailLength, Time | `float` × 2 | `Time` is the 3-segment color/alpha/scale blend midpoint fraction `[0,1]` |
| SegmentColor | `float[9]` | 3 RGB stops (start/mid/end) |
| SegmentAlpha | `BYTE[3]` | 3 alpha stops |
| ParticleScaling | `float[3]` | 3 size-multiplier stops |
| LifeSpanUVAnim, DecayUVAnim, TailUVAnim, TailDecayUVAnim | `{start,end,repeat}` × 4 (12 `DWORD`s) | parsed to stay byte-aligned but **not exported** - `r_particles.c`'s `FX_GetFrame` never reads them either, deriving the atlas frame purely from the particle's own lifetime fraction |
| TextureID | `DWORD` | index into `TEXS` |
| Squirt | `DWORD` | parsed, discarded - no consumer anywhere in this codebase |
| PriorityPlane | `DWORD` | parsed, discarded - no consumer anywhere in this codebase |
| ReplaceableId | `DWORD` | `TEXREPL_NONE`/`TEAMCOLOR`/`TEAMGLOW`, same convention as material layers |

### FilterMode → `bzTTBlendMode_t` mapping

PRE2's `FilterMode` uses its **own** distinct raw numbering (NOT the same numeric convention as a
material layer's `blend_mode`, which already matches `bzTTBlendMode_t` directly with no
translation needed) — translated once at decode time
(`wc3_mdx_decode.c`'s `kFilterModeToBlend[5]`):

| Raw FilterMode | Meaning | `bzTTBlendMode_t` |
|---------------:|---------|--------------------|
| 0 | Blend | `BZ_TTA_BLEND_ALPHA` |
| 1 | Additive | `BZ_TTA_BLEND_ADDITIVE` |
| 2 | Modulate | `BZ_TTA_BLEND_MODULATE` |
| 3 | Modulate2x | `BZ_TTA_BLEND_MODULATE_2X` |
| 4 | AlphaKey | `BZ_TTA_BLEND_TRANSPARENT` |

Any other raw value is malformed input and rejected (`goto fail`), never silently clamped.

### FrameFlags → `bzTTParticleHeadTail_t` mapping

Raw `FrameFlags` (0/1/2) maps directly to `bzTTParticleHeadTail_t`
(`BZ_TTA_PARTICLE_HEAD`/`_TAIL`/`_BOTH`) with no translation table needed (same numbering). Head
spawns a single billboard per particle; Tail spawns a trail/streak using `TailLength`; Both draws
one particle as both. **This project's authoritative CPU particle simulation
(`renderer/r_particles.c`) does not consume tail rendering at all** - `cparticle_t` has no
head/tail distinction and `R_DrawParticles()` always emits one billboard quad regardless of the
source emitter's `FrameFlags`. `head_or_tail`/`headOrTail` is carried through the bridge ABI and
into the Quest particle module so a future tail-rendering slice has the data already plumbed, but
is not yet acted on anywhere - a documented, honest scope cut, not an oversight.

### Animatable channels and a traced authoritative quirk

PRE2 has 8 animatable float channels, one keyframe track tag each (`bzTTEmitterChannel_t` in the
bridge ABI):

| Tag | Channel | `bzTTEmitterChannel_t` |
|-----|---------|------------------------|
| `KP2V` | Visibility | `BZ_TTA_EMITTER_VISIBILITY` |
| `KP2E` | EmissionRate | `BZ_TTA_EMITTER_EMISSION_RATE` |
| `KP2W` | Width | `BZ_TTA_EMITTER_WIDTH` |
| `KP2N` | Length | `BZ_TTA_EMITTER_LENGTH` |
| `KP2S` | Speed | `BZ_TTA_EMITTER_SPEED` |
| `KP2L` | Latitude | `BZ_TTA_EMITTER_LATITUDE` |
| `KP2G` | Gravity | `BZ_TTA_EMITTER_GRAVITY` |
| `KP2R` | Variation | `BZ_TTA_EMITTER_VARIATION` |

`platform/bridge/bz_tabletop_assets.h`'s `bzTTParticleEmitterInfo_t`/`BZ_TTAsset_EmitterTrackInfo`/
`BZ_TTAsset_CopyEmitterFloatKeys` expose all 8 faithfully and completely, since the bridge ABI is a
general, versioned mirror of the on-disk format, not scoped to any one consumer's rendering
behavior.

**However**, a careful re-reading of `games/warcraft-3/renderer/mdx/r_mdx_geoset.c`'s
`MDLX_RenderEmitter` (verified via `git blame` - the whole function was introduced in a single
atomic commit, `2bfbd4e0`, and never revisited since) shows it does **not** actually use all 7
sampled physical-channel locals its own `GET_PARTICLE_ANIM_PARAM(model, emitter, X)` macro
computes:

```c
GET_PARTICLE_ANIM_PARAM(model, emitter, EmissionRate);  /* sampled local IS used (line 98) */
GET_PARTICLE_ANIM_PARAM(model, emitter, Width);         /* sampled local is NEVER used */
GET_PARTICLE_ANIM_PARAM(model, emitter, Length);        /* sampled local is NEVER used */
GET_PARTICLE_ANIM_PARAM(model, emitter, Speed);         /* sampled local IS used (line 114) */
GET_PARTICLE_ANIM_PARAM(model, emitter, Latitude);      /* sampled local is NEVER used */
GET_PARTICLE_ANIM_PARAM(model, emitter, Gravity);       /* sampled local IS used (line 115) */
GET_PARTICLE_ANIM_PARAM(model, emitter, Variation);     /* sampled local is NEVER used */
...
VECTOR3 origin = FX_GenerateRandomOrigin(emitter->Length, emitter->Width);      /* static fields */
VECTOR3 direction = FX_GenerateRandomDirection(emitter->Latitude * M_PI / 180); /* static field */
p->vel = Vector3_scale(&direction, Speed);                                     /* sampled local */
p->accel = Vector3_scale(&(VECTOR3){0,0,-1}, Gravity);                         /* sampled local */
```

`FX_GenerateRandomOrigin`/`FX_GenerateRandomDirection` read `emitter->Length`/`emitter->Width`/
`emitter->Latitude` — the **static** struct fields — directly, bypassing their own already-computed
sampled locals of the identical name. The sampled `Variation` local is never read by anything at
all; an exhaustive `grep` of every "Variation" occurrence in this codebase confirms it has no
other use anywhere (parsed, stored, freed - never consumed). Only `EmissionRate`, `Speed`, and
`Gravity` actually use their track-sampled values; `Width`/`Length`/`Latitude` are always static;
`Variation` is always dead.

This is the authoritative renderer's real, observable behavior — whether originally intentional or
an incomplete refactor, it is what every retail/custom PRE2 emitter in this codebase's reference
implementation actually does, and this project's mandate is to reproduce traced authoritative
behavior, not an idealized version of it. Accordingly,
`platform/android/quest/app/src/main/cpp/bz_quest_wc3_render.h`'s `bzQuestWc3StoredEmitter_t` only
stores tracks for Visibility/EmissionRate/Speed/Gravity; Width/Length/Latitude are stored as plain
static scalars (never track-sampled by `bz_quest_vk_wc3.c`); there is no Variation field anywhere
in the Quest particle pipeline. This is a deliberate, evidence-traced design choice, not a missed
channel — "fixing" it to sample all 7 would diverge from traced behavior.

## Animation Sequences

Each entry in `SEQS` describes one named clip:

| Field | Type | Description |
|-------|------|-------------|
| `name` | `char[80]` | Sequence name (e.g. `"Stand"`, `"Walk"`, `"Attack"`) |
| `interval` | `DWORD[2]` | [startFrame, endFrame] in milliseconds |
| `moveSpeed` | `float` | Ground speed during the animation |
| `flags` | `DWORD` | `1` = non-looping |
| `rarity` | `float` | Random weight for stand variations |
| `syncPoint` | `DWORD` | Sync reference frame |
| `extent` | `Extent` | Bounding box + sphere for this clip |

Standard sequence names are: `Stand`, `Walk`, `Attack`, `Attack Slam`, `Attack 2`, `Decay Flesh`, `Decay Bone`, `Death`, `Dissipate`, `Portrait`, `Spell`, `Stand Channel`, `Stand Ready`, `Stand Work`.

## Runtime ownership

`R_LoadModelMDLX()` owns the root model, fixed arrays, every linked record family, every nested
keytrack/array, geoset GL objects, and texture registry entries created for `TEXS`. `MDLX_Release()`
releases that complete graph exactly once. `model->nodes[]` and `geoset->geosetAnim` are borrowed
indexes into owned records and are not separate allocations. Texture entries are unlinked from the
renderer registry before release; shared built-in missing-asset placeholders remain renderer-owned.
Renderer-global `tr.model[]` assets are duplicate-safe owners and are released before
`MDLX_Shutdown()` while the GL context is current; borrowed texture-inspection cache state is cleared
with them.

## Related Source Files

| Source | Purpose |
|--------|---------|
| `games/warcraft-3/renderer/mdx/r_mdx.h` | All MDX struct definitions |
| `games/warcraft-3/renderer/mdx/r_mdx_load.c` | Chunk parser and model loader |
| `games/warcraft-3/renderer/mdx/r_mdx_render.c` | Per-frame skinning and draw calls |
| `games/warcraft-3/renderer/mdx/r_mdx_interpolation.c` | Keyframe track evaluation |
| `games/warcraft-3/renderer/mdx/r_mdx_geoset.c` | `MDLX_RenderEmitter`/`MDLX_RenderParticleEmitters` (particle spawn) |
| `renderer/r_particles.c` | `R_SpawnParticle`/`R_UpdateParticles`/`R_DrawParticles` (particle simulation + draw) |
| `games/warcraft-3/visionos/wc3_mdx_decode.c` | Shared PRE2 decoder feeding the bridge asset ABI |
| `platform/bridge/bz_tabletop_assets.h` | `bzTTParticleEmitterInfo_t`/`bzTTEmitterChannel_t` bridge ABI (v4+) |
| `platform/android/quest/app/src/main/cpp/bz_quest_wc3_particles.h` | Quest-side pure particle simulation (pool/spawn/age/pack) |

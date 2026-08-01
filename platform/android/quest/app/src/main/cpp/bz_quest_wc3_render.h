/*
 * bz_quest_wc3_render.h - layer 5A: platform-independent Warcraft III
 * static-model coordinate math, render-item descriptors, and render-list
 * construction.
 *
 * Every function/struct here takes and returns plain float/int/char arrays
 * only - never a bzTTAsset_t, bzTTSnapshot_t, VkBuffer, or VkImage. This lets
 * platform/android/quest/tests/test_bz_quest_wc3_render.c build and check
 * these exact coordinate/scale/list-construction decisions with a plain host
 * C compiler, no NDK/Android SDK/OpenXR loader/Vulkan headers/engine link
 * required - mirrors bz_quest_pure.h's and bz_quest_frame.h's rationale (see
 * their header comments).
 *
 * bz_quest_wc3_capture.c (the one impure translation unit that actually
 * calls BZ_TT_Latest()/BZ_TTSnapshot_EntityAt()/BZ_TTA_RegisterConfigString()/
 * BZ_TTA_ResolveEntityMetadata()/BZ_TTA_RegisterTeamTexture()/BZ_TTAsset_*())
 * copies ABI data into the bzQuestWc3Model_t/bzQuestWc3EntityInput_t structs
 * declared here, then calls bz_quest_wc3_build_render_list() to turn them
 * into the final bzQuestWc3RenderItem_t list bz_quest_vk_wc3.c consumes.
 *
 * -- Coordinate conversion evidence (do not change without re-deriving) --
 *
 * The Warcraft III entity/MDX coordinate system is Z-up (evidence:
 * games/warcraft-3/renderer/mdx/r_mdx_render.c:43 uses {0,0,1} as the "up"
 * vector for billboarded nodes). OpenXR/Vulkan clip space here, like the
 * reviewed visionOS/RealityKit target, is Y-up right-handed. The already-
 * shipped, reviewed visionOS renderer performs an exact Y<->Z axis swap plus
 * a heading negation plus a triangle-winding fix to compensate - see:
 *
 *   - Position swap: TabletopVector3(x: raw.origin_x, y: raw.origin_z,
 *     z: raw.origin_y) - LiveTabletopTransport.swift:56.
 *   - Vertex/normal swap: WarcraftVector3(x: $0.x, y: $0.z, z: $0.y) -
 *     WarcraftAssetAdapter.swift:379-380.
 *   - Heading negation: TabletopCoordinateConversion.heading() negates the
 *     engine angle - TabletopAdapter.swift:54.
 *   - Heading applied as a rotation about the (target) Y axis:
 *     simd_quatf(angle: item.descriptor.heading, axis: [0,1,0]) -
 *     RealityTabletopView.swift:268.
 *   - Winding fix: after the axis swap, indices are reordered
 *     [i0, i2, i1] instead of [i0, i1, i2] to restore correct front-face
 *     winding (the axis swap alone flips handedness) -
 *     WarcraftAssetAdapter.swift:384-389.
 *
 * This is the same Z-up -> Y-up-right-handed conversion Quest/OpenXR needs
 * (also Y-up right-handed), not something visionOS-specific, so this module
 * replicates it exactly rather than re-deriving new axis/winding rules.
 *
 * -- Scale evidence (do not change without re-deriving) --
 *
 * Entity/world scale is deliberately NOT a raw MDX-unit passthrough. The
 * reviewed renderer applies a stylized "diorama" scale in two stages, both
 * required for the live/production ".world" coordinate space every
 * snapshot-driven entity uses (LiveTabletopTransport.swift's
 * `coordinateSpace: .world(bounds)`):
 *
 *   1. WarcraftCategoryScale.scale(category, footprint) - a per-category
 *      multiplier (unit/item 0.72, building 1.0, resource 0.9, doodad 0.8,
 *      destructable 0.86, unknown 0.72) applied INDEPENDENTLY per axis:
 *      max(footprint.width, 0.25) for X and max(footprint.depth, 0.25) for
 *      Z (never max(width, depth) shared across both axes - a rectangular
 *      footprint must stay rectangular, not be forced square) - the bare
 *      category multiplier alone for Y -
 *      WarcraftRenderDescriptors.swift:375-391. `footprint.width`/`.depth`
 *      are bzTTAssetMetadata_t.footprint_x/footprint_y respectively,
 *      verbatim, each independently floor-clamped
 *      (LiveTabletopTransport.swift:686: `WarcraftFootprint(width:
 *      raw.footprint_x, depth: raw.footprint_y)`).
 *   2. A further ".world" space scale-down: (min(x,2)*0.06, y*0.08,
 *      min(z,2)*0.06) - WarcraftRenderMath.swift:498-514.
 *
 * bzTTEntity_t.scale (the WC3 runtime unit-scale field, e.g. SetUnitScale)
 * is never folded into this world matrix by the reviewed renderer (grepped:
 * no reference to entity.metadata.scale anywhere in WarcraftRenderMath.swift
 * or WarcraftRenderDescriptors.swift's world-transform code), so this
 * module deliberately does not apply it either - replicating the reviewed
 * behavior exactly rather than guessing a use for an unused field.
 *
 * -- Shared world/tabletop position transform (fixes layer 5D's inherited
 * terrain/entity coordinate mismatch - do not change without re-deriving) --
 *
 * The scale/center evidence above only covers each entity's OWN mesh size.
 * Positions are a separate concern: layer 5B's terrain
 * (bz_quest_wc3_terrain.c) already places every vertex inside a bounded
 * "diorama" box via scale = 1.08 / max(spanX, spanZ) centered on the map
 * bounds' midpoint (bz_quest_wc3_terrain_measure(), mirroring
 * WarcraftAssetAdapter.swift:560-719's terrain adapter). The SAME
 * WarcraftWorldTransform(centerX, centerZ, scale) is independently applied
 * to entity positions by the actual production render provider -
 * `ProductionWarcraftRenderProvider.scene()` in
 * FixtureWarcraftRenderProvider.swift:161-224 (this is the provider
 * OpenRealmTabletopApp.swift:84 actually wires up for real snapshots; the
 * "Fixture" in that file's name refers to an unrelated preview-only sibling
 * provider in the same file, not this one) - via
 * `transform.point(entity.position)`, where `point()` is defined at
 * WarcraftAssetAdapter.swift:147-154 as:
 *   point(x, y, z) = ((x - centerX) * scale, y * scale, (z - centerZ) * scale)
 * i.e. height (target Y) is scaled but never re-centered, exactly matching
 * terrain's own per-corner `height * scale` with no separate height offset.
 *
 * Before this fix, layer 5D placed entities/fog/selection markers in RAW
 * unscaled engine coordinates while terrain already lived inside that
 * bounded box - a real spatial mismatch (not a cosmetic one) that this
 * struct/function pair resolves by giving every position consumer
 * (bz_quest_wc3_build_world_matrix() below, bz_quest_wc3_terrain_measure(),
 * and bz_quest_wc3_fog.c's fog-quad/selection-marker placement) ONE shared
 * implementation, applied exactly once per position - never compounded, and
 * never folded into the model-local footprint/category scale above (that
 * scale intentionally stays map-bounds-independent, matching the reviewed
 * 5A/5C behavior; only WORLD POSITION is bounds-relative).
 */
#ifndef BZ_QUEST_WC3_RENDER_H
#define BZ_QUEST_WC3_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_wc3_anim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float scale;
    float centerX;
    float centerZ;
} bzQuestWc3WorldTransform_t;

/* 1.08 - the diorama-box target size bz_quest_wc3_terrain.c's
 * bz_quest_wc3_terrain_measure() has always used (WarcraftAssetAdapter.swift:
 * 560-719's terrain adapter target), and bz_quest_wc3_world_transform_measure()
 * below derives `scale` from. Exposed here (not private to the .c) so the
 * HUD panel placement math (bz_quest_wc3_hud.c) can anchor itself just
 * outside the diorama's fixed maximum half-extent (this constant / 2)
 * without duplicating the literal - see AGENTS.md's DRY rule. */
#define BZ_QUEST_WC3_WORLD_TARGET_SPAN_F 1.08f

/*
 * Derives the shared world->diorama transform from raw map bounds (engine X
 * span as X, engine "north" span as Z - the same two horizontal axes
 * bz_quest_wc3_terrain.h's bzQuestWc3TerrainBounds_t already names
 * minX/maxX/minZ/maxZ, and platform/bridge/bz_tabletop_transport.h's
 * bzTTBox2_t names min_x/max_x/min_y/max_y). Returns false (leaving *out
 * untouched) for degenerate bounds (non-finite, or zero/negative span on
 * either axis) - callers must fall back to a raw/unscaled passthrough
 * rather than divide by zero or propagate NaN, matching
 * bz_quest_wc3_terrain_measure()'s own bounds validation (which now calls
 * this function instead of repeating the `1.08f` literal - see this file's
 * header comment).
 */
bool bz_quest_wc3_world_transform_measure(float minX, float minZ, float maxX, float maxZ,
                                          bzQuestWc3WorldTransform_t *out);

/*
 * Applies `transform` to one already Y-up-swapped point (x = engine X,
 * y = engine up/height, z = engine "north"): ((x-centerX)*scale, y*scale,
 * (z-centerZ)*scale) - WarcraftAssetAdapter.swift:152-154's `point()`
 * verbatim (see this file's header comment). `transform` NULL means "no
 * valid map bounds this frame" and passes `x,y,z` through unscaled/
 * uncentered (raw passthrough) rather than crash or fabricate a scale -
 * matching this file's pre-fix behavior for a defensive no-map-bounds edge
 * case (docs/quest-tabletop.md's "no map is ever loaded in this dev
 * environment" note).
 */
void bz_quest_wc3_world_transform_point(const bzQuestWc3WorldTransform_t *transform, float x, float y,
                                        float z, float outXYZ[3]);

/*
 * Exact inverse of bz_quest_wc3_world_transform_point(): maps a diorama/
 * target-space point (tx = X axis, ty = up/height axis, tz = "north" axis)
 * back to the same (x = engine X, y = engine up/height, z = engine "north")
 * triple that function's forward `x,y,z` arguments accept, so
 * point(inverse(p)) == p and inverse(point(p)) == p within float rounding:
 *   x = tx/scale + centerX, y = ty/scale, z = tz/scale + centerZ.
 * Added for the layer 6 controller input hit-test path, which converts a
 * terrain ray/plane hit (found in target space) back into the authoritative
 * engine world coordinates BZ_TT_PostSmartPoint()/PostTargetPoint() expect -
 * run exactly once per hit, never re-derived with a separate literal. As
 * with the forward function, `transform` NULL means "no valid map bounds"
 * and passes tx,ty,tz through unchanged (raw passthrough); a scale of 0 is
 * impossible for a transform produced by bz_quest_wc3_world_transform_measure()
 * (it rejects zero/negative span), so no divide-by-zero guard is needed for
 * a measured transform, and the NULL branch never divides at all.
 */
void bz_quest_wc3_world_transform_point_inverse(const bzQuestWc3WorldTransform_t *transform, float tx,
                                                float ty, float tz, float outXYZ[3]);

enum {
    /* MDX texture records/model config-string identities carry a fixed
     * 260-byte path (mirrors platform/bridge/bz_tabletop_assets.h's
     * BZ_TTA_MAX_IDENTITY) - kept as its own literal (not an #include of
     * that header) so this file stays free of any bridge/ABI dependency,
     * per this file's header comment. */
    BZ_QUEST_WC3_MAX_IDENTITY = 260,
    /* Bounded per-model/per-frame counts. A real MDX model has at most a
     * few dozen geosets/materials/layers - these caps are generous multiples
     * of what any shipped Warcraft III asset uses; a model that exceeds one
     * is rejected with a logged reason (see bz_quest_wc3_capture.c), never
     * silently truncated. */
    BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL = 32,
    BZ_QUEST_WC3_MAX_LAYERS_PER_GEOSET = 4,
    BZ_QUEST_WC3_MAX_VERTS_PER_MODEL = 65536,
    BZ_QUEST_WC3_MAX_INDICES_PER_MODEL = 196608, /* 3 * MAX_VERTS_PER_MODEL, generous */
    /* Shipped Warcraft III model/team textures are never larger than
     * 512x512 (occasionally 1024 for a handful of UI-adjacent images) -
     * games/warcraft-3/visionos/wc3_blp_decode.c's own BLP_MAX_DIMENSION
     * (8192) is the *file format's* theoretical ceiling, not a realistic
     * asset size, and would require a 256MB RGBA8 staging buffer per
     * texture. 2048 is a generous multiple of any real shipped asset while
     * keeping the bounded staging buffer (see bz_quest_wc3_capture.c) a
     * fixed, small (16MB) static allocation; a texture that exceeds it is
     * logged once and its layer skipped, never silently cropped/resized. */
    BZ_QUEST_WC3_MAX_TEXTURE_DIM = 2048,
    BZ_QUEST_WC3_MAX_TEXTURE_BYTES = BZ_QUEST_WC3_MAX_TEXTURE_DIM * BZ_QUEST_WC3_MAX_TEXTURE_DIM * 4,
    /* Per-frame render-list caps. Overflow is reported (see
     * bzQuestWc3RenderList_t::overflowCount below), never silently dropped -
     * mirrors platform/bridge/bz_tabletop_transport.h's
     * BZ_TTSnapshot_EntitiesOverflowCount() contract for the same reason. */
    BZ_QUEST_WC3_MAX_RENDER_ITEMS = 1024, /* matches BZ_TT_MAX_ENTITIES */
    BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME = 128,
    /* Real MDX sequence/global-sequence counts are single digits to a few
     * dozen (a handful of shipped units exceed ~24 sequences); 64 is a
     * generous multiple - a model exceeding it is truncated with a logged
     * reason (see bz_quest_wc3_capture.c), never silently mis-indexed. */
    BZ_QUEST_WC3_MAX_SEQUENCES_PER_MODEL = 64,
    BZ_QUEST_WC3_MAX_GLOBAL_SEQUENCES_PER_MODEL = 64,
    /* Per-frame cap on the number of distinct (render item, geoset) pairs
     * that get a freshly-computed pose/bone-palette this frame - see
     * bz_quest_vk_wc3.h's BZ_QUEST_VK_WC3_MAX_SKINNED_DRAWS_PER_FRAME (this
     * constant lives here, not there, because bz_quest_wc3_capture.c's
     * pure-adjacent bounds-checking code also needs it and must not #include
     * the Vulkan-aware bz_quest_vk_wc3.h). A scene exceeding this many
     * simultaneously-animated geosets in one frame is a pathological case
     * far beyond any real Warcraft III map on Quest hardware - the excess
     * draws are logged once and fall back to an identity bone palette
     * (still drawn, just not skinned that frame - never a dropped/frozen
     * draw, per this slice's "never silently demote" rule). */
    BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME = 256,
};

/* One material layer, already resolved to a concrete texture identity by the
 * impure capture step (see bz_quest_wc3_capture.h) - this module never talks
 * to the asset ABI itself. Exactly one of three states holds: (1) a direct
 * (replaceable_id 0) texture - `textureIdentity` set, `teamColor`/`teamGlow`
 * false; (2) a team-color/glow (replaceable_id 1/2) texture - `teamColor` or
 * `teamGlow` true, `textureIdentity` left empty because the concrete texture
 * is per-ENTITY (each entity's own team_color), not per-model - the renderer
 * must instead use the draw's own bzQuestWc3RenderItem_t::teamColorTexture
 * Identity/teamGlowTextureIdentity (see that struct's comment); (3)
 * `unsupported` true for any other replaceable_id (a per-entity image
 * override - out of scope for this slice, see bz_quest_wc3_capture.h) -
 * callers must log once per unique (model identity, geoset, layer) and skip
 * drawing that layer, never substitute another texture. */
typedef struct {
    uint32_t blendMode;   /* bzTTBlendMode_t - see platform/bridge/bz_tabletop_assets.h */
    uint32_t flags;       /* raw MDX layer shading bits - see games/warcraft-3/renderer/mdx/r_mdx.h:27-35 */
    float alpha;
    bool unsupported;     /* true: replaceable_id was not 0/1/2 (per-entity image override, deferred) */
    bool teamColor;       /* true: replaceable_id 1 - resolve via the render item's team color texture */
    bool teamGlow;        /* true: replaceable_id 2 - resolve via the render item's team glow texture */
    char textureIdentity[BZ_QUEST_WC3_MAX_IDENTITY]; /* empty iff unsupported/teamColor/teamGlow */
} bzQuestWc3LayerDesc_t;

/* One geoset's vertex/index range within its model's combined vertex/index
 * arrays (see bzQuestWc3Model_t below), plus its resolved material layers.
 * Indices are already rebased to the model-combined vertex array (i.e. the
 * geoset's own 0-based MDX indices plus vertexOffset) - see
 * bz_quest_wc3_capture.c. */
typedef struct {
    uint32_t vertexOffset, vertexCount;
    uint32_t indexOffset, indexCount;
    uint32_t layerCount;
    bzQuestWc3LayerDesc_t layers[BZ_QUEST_WC3_MAX_LAYERS_PER_GEOSET];
} bzQuestWc3Geoset_t;

/* One vertex: position/normal already axis-swapped (Z-up -> Y-up) by the
 * capture step - see this file's header comment. UV is passed through
 * unmodified (MDX UVs are already the target 2D texture-space convention;
 * no evidence any conversion is needed - WarcraftAssetAdapter.swift copies
 * geoset UVs verbatim with no transform). */
/* Bone skin: up to 4 (node-index, weight) pairs into THIS GEOSET's own
 * matrix palette (bzQuestWc3StoredGeosetAnim_t::paletteNodeIndex below) -
 * mirrors platform/bridge/bz_tabletop_assets.h's bzTTVertexSkin_t exactly
 * (BZ_TTA_MAX_VERTEX_BONES == 4), copied through unresolved (boneIndex is a
 * *palette-local* slot 0..matrixPaletteCount-1, never a global node index -
 * see bz_quest_wc3_capture.c). boneWeight is 0..255 (matches the ABI's own
 * byte-weight convention - r_mdx_geoset.c's vertex-skin weights are already
 * byte-normalized, no float conversion needed until the GPU skin sums them
 * as weight/255.0). Every geoset always has >=1 valid bone (per
 * bzTTGeosetInfo_t's own "always has a resolved skin + matrix palette"
 * guarantee - see bz_quest_wc3_capture.c) - so this field is populated for
 * EVERY vertex of EVERY geoset, animated or not (boneIndex[0]=0,
 * boneWeight[0]=255, rest zero, for a geoset with no real BONE hierarchy),
 * letting one GPU-skinning vertex shader handle both cases uniformly rather
 * than needing a separate static-model pipeline variant. */
typedef struct {
    float position[3];
    float normal[3];
    float uv[2];
    uint8_t boneIndex[4];
    uint8_t boneWeight[4];
} bzQuestWc3Vertex_t;

/* ---------------------------------------------------------------------- */
/* Persistent per-model animation data (arena-owned, heap-allocated once per */
/* unique animated model - see bz_quest_wc3_capture.c and this file's       */
/* bzQuestWc3ModelMeta_t::anim comment below).                             */
/* ---------------------------------------------------------------------- */

/* One node channel's keyframe data, sized to the model's REAL key count
 * (never BZ_QUEST_WC3_MAX_KEYS_PER_TRACK) and pointing into the owning
 * model's single arena allocation - unlike bz_quest_wc3_anim.h's
 * bzQuestWc3Track_t (a fixed-size, stack/static-friendly struct meant for
 * one entity's transient per-frame pose build), this is the space-efficient
 * form actually retained in the Vulkan model cache across frames (see this
 * file's header comment on why a fixed-size-array-per-model design would
 * cost ~2.3MB * BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME here). Exactly one
 * of vec3Keys/quatKeys/floatKeys is non-NULL, matching which channel this
 * track belongs to (translation/scale -> vec3Keys, rotation -> quatKeys,
 * geoset alpha -> floatKeys). keyCount==0 means "no track" (all three
 * pointers NULL) - callers fall back to the pure module's own identity
 * defaults, exactly as bz_quest_wc3_anim.h's Track_t does. */
typedef struct {
    uint32_t keyCount;
    bzQuestWc3Interp_t interp;
    uint32_t globalSequence; /* BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE, or a globalSeqDurations[] index */
    const bzQuestWc3Vec3Key_t *vec3Keys;
    const bzQuestWc3QuatKey_t *quatKeys;
    const bzQuestWc3FloatKey_t *floatKeys;
} bzQuestWc3StoredTrack_t;

/* One MDX node - parentIndex already resolved from the ABI's raw object_id/
 * parent_id to this array's own 0-based index (mirroring platform/bridge/
 * bz_tabletop_assets.c's node_index_for_object_id() convention - see
 * bz_quest_wc3_capture.c), or BZ_QUEST_WC3_NO_PARENT for a root node. */
typedef struct {
    uint32_t parentIndex;
    bzQuestWc3Vec3_t pivot;
    bzQuestWc3StoredTrack_t translation, rotation, scale;
} bzQuestWc3StoredNode_t;

/* One sequence's [startMsec,endMsec) interval - the entity's authoritative
 * bzTTEntity_t.frame is matched against these to find the active sequence,
 * mirroring R_FindSequenceAtTime (r_mdx_anim.c) exactly - see
 * bz_quest_wc3_capture.c. */
typedef struct {
    uint32_t startMsec, endMsec;
} bzQuestWc3StoredSeqRange_t;

/* One geoset's dynamic-material animation state: alpha (GEOA/KGAO - a
 * scalar multiplied into every layer's own resolved alpha, kept separate
 * from and multiplicative with layer alpha per games/warcraft-3/renderer/
 * mdx/r_mdx_geoset.c's MDLX_EvaluateGeosetColor()/MDLX_EvaluateLayerAlpha()
 * split - see bz_quest_wc3_capture.c) and this geoset's own resolved
 * (already node-index-remapped by the ABI - see platform/bridge/
 * bz_tabletop_assets.h's BZ_TTAsset_CopyGeosetMatrixPalette doc comment)
 * bone-palette node-index list, consumed by
 * bz_quest_wc3_build_bone_palette(). `paletteNodeIndexCount` is always >= 1
 * (the ABI guarantees every geoset has a resolved palette - see this file's
 * bzQuestWc3Vertex_t comment). */
typedef struct {
    bool hasAlphaTrack;
    float staticAlpha; /* used verbatim when !hasAlphaTrack, matches bzTTGeosetAnimInfo_t::static_alpha */
    bzQuestWc3StoredTrack_t alphaTrack; /* meaningful iff hasAlphaTrack; floatKeys populated */
    uint32_t paletteNodeIndexCount;
    const uint32_t *paletteNodeIndices;
} bzQuestWc3StoredGeosetAnim_t;

/* Top-level persistent per-model animation data: one heap arena backs every
 * pointer below (nodes/sequences/globalSeqDurations/geosetAnims and every
 * key array they point into), allocated by a first ABI-Info()-only sizing
 * pass and filled by a second pass of real ABI Copy*() calls straight into
 * arena-relative addresses (see bz_quest_wc3_capture.c) - one malloc(), one
 * free(), sized to the model's REAL data rather than any fixed worst-case
 * cap. `geosetAnims`/`geosetAnimCount` is index-aligned with the owning
 * bzQuestWc3ModelMeta_t::geosets[] array (same index means same geoset). */
typedef struct {
    void *arena;
    uint32_t nodeCount;
    const bzQuestWc3StoredNode_t *nodes;
    uint32_t sequenceCount;
    const bzQuestWc3StoredSeqRange_t *sequences;
    uint32_t globalSeqDurationCount;
    const uint32_t *globalSeqDurations;
    uint32_t geosetAnimCount;
    const bzQuestWc3StoredGeosetAnim_t *geosetAnims;
} bzQuestWc3ModelAnim_t;

/*
 * Frees `anim->arena` and `anim` itself (a single two-step release for the
 * single-allocation arena design above) - safe to call with anim == NULL
 * (no-op), matching this project's other release-function conventions
 * (e.g. BZ_TTAsset_Release's NULL-safety).
 */
void bz_quest_wc3_model_anim_free(bzQuestWc3ModelAnim_t *anim);

/* Small, persistent per-model metadata - what the Vulkan GPU cache actually
 * retains across frames (see bz_quest_wc3_cache.h): geoset vertex/index
 * ranges and resolved material layers, WITHOUT the raw vertex/index arrays
 * themselves (those live only in the transient bzQuestWc3Model_t below,
 * for exactly as long as an upload-in-progress needs them). Keeping this
 * struct small (no MAX_VERTS_PER_MODEL-sized arrays) is what makes it safe
 * to retain BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME worth of these
 * persistently as normal cache-entry state. */
typedef struct {
    char identity[BZ_QUEST_WC3_MAX_IDENTITY];
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t geosetCount;
    bzQuestWc3Geoset_t geosets[BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL];
    /* NULL for a model with no animation data at all (e.g. decode found zero
     * nodes, or ABI info calls reported nothing to copy) - such a model is
     * still drawn via the same GPU-skinning pipeline using each geoset's own
     * trivial (>=1 entry, node-0-bound) palette (see bz_quest_wc3_capture.c
     * and this file's bzQuestWc3Vertex_t comment); `anim` only carries the
     * REAL keyframe/hierarchy data needed to move that palette away from
     * identity. Owned by the model's own Vulkan cache entry (see
     * bz_quest_vk_wc3.c's model_cache_create()/model_cache_destroy()) - one
     * heap arena per unique model, freed exactly once on cache eviction. */
    bzQuestWc3ModelAnim_t *anim;
} bzQuestWc3ModelMeta_t;

/* A fully-resolved, renderer-owned copy of one Warcraft model's static
 * geometry/material data - the Vulkan GPU cache's *transient* upload unit,
 * used only for exactly as long as a cache-miss upload takes (see
 * bz_quest_wc3_capture.c/bz_quest_vk_wc3.c). Deliberately holds the model's
 * *combined* geoset vertex/index arrays (all geosets concatenated) rather
 * than one buffer per geoset: this project's asset ABI already gives one
 * asset handle per model with N geosets, and one shared vertex/index buffer
 * per model (with per-geoset offset/count ranges) needs one allocation
 * instead of N tiny ones, without cropping/redecoding the ABI's own
 * geoset/vertex/index shape - see this file's task-scope comment on
 * consuming the asset ABI's native descriptors directly.
 *
 * At ~2.5MB (BZ_QUEST_WC3_MAX_VERTS_PER_MODEL * sizeof(bzQuestWc3Vertex_t) +
 * BZ_QUEST_WC3_MAX_INDICES_PER_MODEL * 4 bytes), this struct is deliberately
 * NEVER a stack-local variable (Android native-thread stacks are as small
 * as ~1MB) - callers must place it in static/file-scope storage or heap-
 * allocate it. bz_quest_wc3_capture.c owns exactly one such scratch buffer,
 * reused sequentially across cache-miss decodes within one frame (capture
 * is single-threaded and processes one model at a time). */
typedef struct {
    bzQuestWc3ModelMeta_t meta;
    bzQuestWc3Vertex_t vertices[BZ_QUEST_WC3_MAX_VERTS_PER_MODEL];
    uint32_t indices[BZ_QUEST_WC3_MAX_INDICES_PER_MODEL];
} bzQuestWc3Model_t;

/* Per-entity input to render-list construction - already-copied POD values
 * (engine-space, unconverted) from one bzTTEntity_t plus its resolved model
 * identity (see bz_quest_wc3_capture.c). `modelIdentity` empty means "no
 * model resolved for this entity this frame" (e.g. model config string 0,
 * or asset registration failed) - such entities are skipped, not drawn with
 * a placeholder geometry. */
typedef struct {
    float originX, originY, originZ; /* engine space: X, Y (north), Z (up) */
    float angle;                     /* engine-space yaw, radians */
    float footprintX, footprintY;    /* bzTTAssetMetadata_t.footprint_x/y */
    float tintR, tintG, tintB, tintA; /* bzTTAssetMetadata_t.tint_* - authoritative team-color RGBA */
    uint32_t category;               /* bzTTAssetCategory_t */
    uint32_t frame;                  /* bzTTEntity_t.frame (msec) - authoritative animation time, see bz_quest_wc3_anim.h */
    bool selected;                   /* bzTTEntity_t.selected - authoritative selection flag for marker overlays */
    /* This entity's own resolved team-color/glow textures (empty iff
     * registration failed or the provider has no team texture for this
     * team_color) - resolved PER ENTITY, not per model, because the same
     * model shared by two different-team entities needs two different
     * concrete textures for its replaceable_id 1/2 layers (see
     * bz_quest_wc3_capture.c and bzQuestWc3LayerDesc_t::teamColor/teamGlow).
     * Left as plain identity strings (not a raw team_color index) so this
     * file and bz_quest_vk_wc3.c never need their own copy of the ABI's
     * team_color->texture resolution rule - capture.c (the one file that
     * legitimately calls BZ_TTA_RegisterTeamTexture) already did it. */
    char teamColorTextureIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    char teamGlowTextureIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    char modelIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
} bzQuestWc3EntityInput_t;

/* Final per-instance draw descriptor: a resolved model identity (looked up
 * in the Vulkan GPU model cache by the renderer - see bz_quest_vk_wc3.h) and
 * a fully-built column-major world matrix (engine space -> target Y-up
 * right-handed space, already through the shared world/tabletop transform -
 * see this file's header comment), matching bz_quest_pure.h's
 * bz_quest_mat4_multiply() layout so bz_quest_vk_wc3.c can multiply it
 * against the eye's view*projection matrix with the same helper. `frame`,
 * `selected`, `footprintScale*`, `tint*`, and team-texture identities are
 * carried through unconverted from bzQuestWc3EntityInput_t for
 * bz_quest_vk_wc3.c's per-frame pose/bone-palette build, fog/selection
 * overlay, and team-texture binding - this file never touches
 * animation/material state itself, only passes the entity's own already-
 * resolved values through untouched. */
typedef struct {
    char modelIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    float world[16];
    /* Per-axis selection-marker scale - the SAME footprint/category formula
     * bz_quest_wc3_build_world_matrix() uses for this entity's own mesh
     * scale (bz_quest_wc3_entity_footprint_scale() below), NOT the raw
     * bzTTEntity_t.radius transport field. Fixes a real-world scale bug:
     * radius is tens of raw WC3 units while the model itself is placed
     * with the compressed diorama footprint scale, so a marker sized
     * directly from radius would never match the selected model's own
     * footprint. See this file's header comment. */
    float footprintScaleX, footprintScaleY, footprintScaleZ;
    float tintR, tintG, tintB, tintA;
    uint32_t frame;
    bool selected;
    char teamColorTextureIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    char teamGlowTextureIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
} bzQuestWc3RenderItem_t;

typedef struct {
    uint32_t count;
    bzQuestWc3RenderItem_t items[BZ_QUEST_WC3_MAX_RENDER_ITEMS];
    /* Entities beyond BZ_QUEST_WC3_MAX_RENDER_ITEMS or
     * BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME that did not fit - reported,
     * never silently dropped, mirroring
     * BZ_TTSnapshot_EntitiesOverflowCount()'s contract. */
    uint32_t overflowCount;
} bzQuestWc3RenderList_t;

/*
 * Derives the per-axis footprint/category mesh scale
 * (bz_quest_wc3_build_world_matrix()'s sx/sy/sz) from `category` and
 * `footprintX`/`footprintY` alone - factored out of that function so
 * bz_quest_wc3_build_render_list() can independently populate each render
 * item's footprintScale* fields with the EXACT SAME numbers used for the
 * entity's own mesh, and bz_quest_wc3_fog.c's selection markers can size
 * themselves identically without duplicating this formula (DRY - see this
 * file's header comment on why radius must not be used instead). Always
 * succeeds (well-defined for any finite input).
 */
void bz_quest_wc3_entity_footprint_scale(uint32_t category, float footprintX, float footprintY,
                                         float *outScaleX, float *outScaleY, float *outScaleZ);

/*
 * Converts one entity's engine-space origin/angle plus its resolved
 * category/footprint into a column-major world matrix in target (Y-up
 * right-handed) space: world = T(transform.point(swapped origin)) *
 * R(-angle around Y) * S(category/footprint scale) - see this file's header
 * comment for the exact evidence each step replicates. `transform` applies
 * the shared world/tabletop position scale+center (NULL means "no valid map
 * bounds this frame", raw passthrough - see bz_quest_wc3_world_transform_point()).
 * Always succeeds (no degenerate input exists: angle is unconstrained,
 * footprint/category feed a scale formula that is well-defined for any
 * finite input).
 */
void bz_quest_wc3_build_world_matrix(const bzQuestWc3EntityInput_t *entity,
                                     const bzQuestWc3WorldTransform_t *transform, float outWorld[16]);

/*
 * Converts one MDX-space (Z-up) bone/node pose matrix - a
 * bz_quest_wc3_anim.c bone-palette entry - into this file's own target
 * (Y-up right-handed) space, so it can be safely multiplied against vertex
 * positions that have ALREADY been through the Y<->Z axis swap documented
 * in this file's header comment. `bz_quest_wc3_anim.c` deliberately stays
 * in raw MDX (Z-up) space end to end (its own header comment: formulas are
 * transcribed verbatim from the desktop renderer and must not be altered),
 * so this conversion is a separate, explicit step owned by this coordinate
 * module, applied once to each finished 4x4 matrix - not to individual
 * translation/rotation/scale track components, which the task that
 * introduced this function called out as the wrong approach.
 *
 * The general rule for converting an affine matrix M under a linear
 * coordinate map S is M' = S * M * S^-1. The Y<->Z swap S used everywhere
 * else in this file (position/normal swap above) is an involution as a
 * 4x4 matrix (S*S = Identity - swapping two axes twice returns the
 * original), so S^-1 = S and the formula reduces to M' = S * M * S. This
 * also means an already-identity matrix (a genuinely static model, or an
 * unused/identity-padded bone-palette slot - see
 * bz_quest_wc3_build_bone_palette()'s own fill convention) converts to the
 * identity again (S*I*S = S*S = I), so callers may run every palette slot
 * - used and identity-padded alike - through this same function
 * unconditionally, with no special-casing for "is this slot really
 * animated" required. `inZup`/`outYup` may safely be the same array (every
 * read of `inZup` happens before the first write to `outYup`).
 */
void bz_quest_wc3_convert_matrix_zup_to_yup(const float inZup[16], float outYup[16]);

/*
 * Builds `outList` from `entities`/`entityCount` (already-captured,
 * already-resolved POD entity inputs - see bz_quest_wc3_capture.h).
 * Entities with an empty modelIdentity are skipped (no model resolved this
 * frame). Every remaining entity becomes exactly one bzQuestWc3RenderItem_t
 * (this slice does not batch/instance at the render-list-construction
 * level - see bz_quest_vk_wc3.h for how repeated identities are instead
 * naturally batched by the GPU cache key/draw-sort step). Stops appending
 * once BZ_QUEST_WC3_MAX_RENDER_ITEMS is reached and increments
 * outList->overflowCount for the remainder instead of silently dropping
 * them without a count. `outList` is fully rewritten (not appended to) -
 * callers must not reuse stale state across frames. `transform` (NULL
 * meaning "no valid map bounds this frame") is forwarded verbatim to
 * bz_quest_wc3_build_world_matrix() for every entity - the same transform
 * for every item in one call, since it is derived once per snapshot from
 * that snapshot's own map bounds (see bz_quest_wc3_capture.c).
 */
void bz_quest_wc3_build_render_list(const bzQuestWc3EntityInput_t *entities, uint32_t entityCount,
                                    const bzQuestWc3WorldTransform_t *transform,
                                    bzQuestWc3RenderList_t *outList);

/*
 * Stable equality for two model/texture identity strings, used as the
 * Vulkan GPU cache's key comparison (see bz_quest_wc3_cache.h) - a thin,
 * separately-testable wrapper (not just strcmp() inlined at every call
 * site) so a future key shape change (e.g. adding a variant suffix) has one
 * place to change and one place to test.
 */
bool bz_quest_wc3_identity_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_RENDER_H */

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
 *      destructable 0.86, unknown 0.72) applied to max(footprint, 0.25) for
 *      X/Z and to the bare category multiplier for Y -
 *      WarcraftRenderDescriptors.swift:375-391. `footprint` is
 *      bzTTAssetMetadata_t.footprint_x/footprint_y verbatim
 *      (LiveTabletopTransport.swift:686).
 *   2. A further ".world" space scale-down: (min(x,2)*0.06, y*0.08,
 *      min(z,2)*0.06) - WarcraftRenderMath.swift:498-514.
 *
 * bzTTEntity_t.scale (the WC3 runtime unit-scale field, e.g. SetUnitScale)
 * is never folded into this world matrix by the reviewed renderer (grepped:
 * no reference to entity.metadata.scale anywhere in WarcraftRenderMath.swift
 * or WarcraftRenderDescriptors.swift's world-transform code), so this
 * module deliberately does not apply it either - replicating the reviewed
 * behavior exactly rather than guessing a use for an unused field.
 */
#ifndef BZ_QUEST_WC3_RENDER_H
#define BZ_QUEST_WC3_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
};

/* One material layer, already resolved to a concrete texture identity by the
 * impure capture step (see bz_quest_wc3_capture.h) - this module never talks
 * to the asset ABI itself. `textureIdentity` is empty and `unsupported` is
 * true when the layer's replaceable_id could not be resolved to a supported
 * texture role for this slice (see bz_quest_wc3_capture.h's replaceable-id
 * contract) - callers must log once per unique (model identity, geoset,
 * layer) and skip drawing that layer, never substitute another texture. */
typedef struct {
    uint32_t blendMode;   /* bzTTBlendMode_t - see platform/bridge/bz_tabletop_assets.h */
    uint32_t flags;       /* raw MDX layer shading bits - see games/warcraft-3/renderer/mdx/r_mdx.h:27-35 */
    float alpha;
    bool unsupported;     /* true: replaceable_id was not 0 (direct)/1 (team color)/2 (team glow) */
    char textureIdentity[BZ_QUEST_WC3_MAX_IDENTITY]; /* empty iff unsupported */
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
typedef struct {
    float position[3];
    float normal[3];
    float uv[2];
} bzQuestWc3Vertex_t;

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
    uint32_t category;               /* bzTTAssetCategory_t */
    char modelIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
} bzQuestWc3EntityInput_t;

/* Final per-instance draw descriptor: a resolved model identity (looked up
 * in the Vulkan GPU model cache by the renderer - see bz_quest_vk_wc3.h) and
 * a fully-built column-major world matrix (engine space -> target Y-up
 * right-handed space), matching bz_quest_pure.h's bz_quest_mat4_multiply()
 * layout so bz_quest_vk_wc3.c can multiply it against the eye's
 * view*projection matrix with the same helper. */
typedef struct {
    char modelIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    float world[16];
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
 * Converts one entity's engine-space origin/angle plus its resolved
 * category/footprint into a column-major world matrix in target (Y-up
 * right-handed) space: world = T(swapped origin) * R(-angle around Y) *
 * S(category/footprint scale) - see this file's header comment for the
 * exact evidence each step replicates. Always succeeds (no degenerate input
 * exists: angle is unconstrained, footprint/category feed a scale formula
 * that is well-defined for any finite input).
 */
void bz_quest_wc3_build_world_matrix(const bzQuestWc3EntityInput_t *entity, float outWorld[16]);

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
 * callers must not reuse stale state across frames.
 */
void bz_quest_wc3_build_render_list(const bzQuestWc3EntityInput_t *entities, uint32_t entityCount,
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

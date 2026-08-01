/*
 * bz_quest_wc3_anim.h - layer 5C: platform-independent Warcraft III MDX
 * skeletal pose math (keyframe interpolation, sequence/global-sequence time
 * resolution, pivot-relative node matrices, parent-chain hierarchy, and
 * per-geoset bone-palette resolution).
 *
 * Every function here takes and returns plain float/int/bool arrays and the
 * small POD structs declared below only - never a bzTTAsset_t or Vulkan
 * type. This lets platform/android/quest/tests/test_bz_quest_wc3_anim.c
 * build and check these exact interpolation/hierarchy/wraparound decisions
 * with a plain host C compiler - mirrors bz_quest_wc3_render.h's own
 * rationale (see that header's comment). bz_quest_wc3_capture.c is the one
 * impure translation unit that copies bzTTNodeInfo_t/bzTTTrackInfo_t/
 * bzTTVec3Key_t/bzTTQuatKey_t/bzTTFloatKey_t ABI data into the
 * bzQuestWc3Node_t/bzQuestWc3Track_t structs declared here, then calls
 * bz_quest_wc3_build_pose()/bz_quest_wc3_build_bone_palette().
 *
 * -- Pose math evidence (do not change without re-deriving) --
 *
 * All formulas below are transcribed verbatim from the reviewed desktop MDX
 * renderer (the authoritative reference this project's engine already
 * ships), not re-derived from a generic skeletal-animation reference:
 *
 *   - Track sampling (interval selection, keyframe scan, end-of-interval
 *     wraparound toward the first in-range key): MDLX_GetModelKeytrackValue
 *     - games/warcraft-3/renderer/mdx/r_mdx_anim.c:29-97. This project's
 *     transport ABI (platform/bridge/bz_tabletop_transport.h's
 *     bzTTEntity_t.frame) carries only ONE authoritative time value per
 *     entity (no separate "previous tick" frame), so this module always
 *     evaluates the frame0==frame1 branch of R_CalculateNodeMatrix
 *     (r_mdx_anim.c:104-142) - i.e. no bzQuestWc3Anim inter-tick lerp -
 *     which is the exact desktop behavior when frame0==frame1, not an
 *     invented simplification.
 *   - Global sequences sample the RENDER clock, not entity/sequence time -
 *     r_mdx_anim.c:34-41 (`tr.viewDef.time` / `SDL_GetTicks()` fallback),
 *     wrapping modulo (duration_msec + 1) - the "+1" is transcribed exactly
 *     from that file's `gs_len = ...value + 1`. This is a pre-existing,
 *     deliberate desktop convention (global sequences are a render-driven
 *     ambient loop, e.g. a banner flapping, not gameplay-authoritative
 *     state) - callers pass a Quest-owned monotonic render-clock value (see
 *     bz_quest_wc3_capture.h), not something this module invents.
 *   - Scalar interpolation (lerp/hermite/bezier): r_mdx_interpolation.c:
 *     12-34. Vec3: shared/source/vector3.c's Vector3_lerp/_hermite/_bezier
 *     (identical per-component formulas). Quaternion: shared/source/
 *     quaternion.c's Quaternion_slerp (LINEAR/NONE) and Quaternion_sqlerp
 *     (HERMITE/BEZIER - both interpolation kinds use the SAME sqlerp call
 *     per r_mdx_interpolation.c:86-88, not a quaternion hermite/bezier
 *     variant).
 *   - Node local matrix: R_CalculateNodeMatrix (r_mdx_anim.c:104-142) -
 *     identity when no channel has a track; pure translation when only
 *     translation is tracked; pivot-relative rotation (no translation
 *     track) via Matrix4_from_rotation_origin; else the fused
 *     rotation*scale-about-pivot-then-translate via
 *     Matrix4_from_rotation_translation_scale_origin (shared/source/
 *     matrix4.c:326-415, transcribed exactly including the column-major
 *     v[12..14] translation-column convention already used by this
 *     project's own bz_quest_wc3_render.h/bz_quest_pure.h).
 *   - Hierarchy composition: R_GetNodeGlobalMatrix (r_mdx_anim.c:157-168) -
 *     global = parent_global * local (bz_quest_mat4_multiply(a,b,out)=a*b
 *     matches Matrix4_multiply's own column-major a*b convention exactly).
 *     Billboarded nodes (r_mdx_anim.c:169-195) are camera-facing and need
 *     per-eye view-direction data this pure module deliberately does not
 *     take (would require re-deriving VR-stereo billboard math with no
 *     existing evidence) - MDLXNODE_Billboarded nodes are therefore treated
 *     as ordinary (non-billboarded) hierarchy nodes here and the capture
 *     layer logs this once per unique model identity - see
 *     bz_quest_wc3_capture.c's "billboard-unsupported" log site. This is a
 *     deliberate, documented scope cut (task instruction: never silently
 *     demote/hide unresolved data), not a silent approximation.
 *   - Per-geoset bone palette: r_mdx_geoset.c:348-361 - `matrixPalette[i] =
 *     node_matrices[geoset->matrixPalette[i]]` for i < num_matrixPalette,
 *     Matrix4_identity() for the remainder up to MDX_MATRIX_PALETTE (128) -
 *     transcribed exactly (unused palette slots are identity, matching a
 *     weight of 0 always multiplying an identity matrix harmlessly).
 */
#ifndef BZ_QUEST_WC3_ANIM_H
#define BZ_QUEST_WC3_ANIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Real shipped Warcraft III unit/building skeletons top out at a few
     * dozen bones (e.g. a fully-rigged hero is well under 64); 128 is a
     * generous multiple while matching platform/bridge/bz_tabletop_assets.h's
     * own BZ_TTA_MAX_MATRIX_PALETTE (128) for symmetry - a model with more
     * nodes is truncated with a logged reason (see bz_quest_wc3_capture.c),
     * never silently mis-indexed. */
    BZ_QUEST_WC3_MAX_NODES_PER_MODEL = 128,
    /* Real shipped sequence/global-sequence tracks rarely exceed a dozen
     * keys; 64 is a generous multiple. A track with more keys is truncated
     * (capture logs once) rather than overflowing this module's fixed
     * arrays - see bz_quest_wc3_capture.c. */
    BZ_QUEST_WC3_MAX_KEYS_PER_TRACK = 64,
    /* Mirrors platform/bridge/bz_tabletop_assets.h's BZ_TTA_MAX_MATRIX_PALETTE
     * exactly (classic MDX_MATRIX_PALETTE) - cross-checked by a
     * _Static_assert in bz_quest_wc3_capture.c (this file deliberately does
     * not #include the bridge header - see this file's header comment). */
    BZ_QUEST_WC3_MAX_MATRIX_PALETTE = 128,
};

typedef enum {
    BZ_QUEST_WC3_INTERP_NONE = 0,
    BZ_QUEST_WC3_INTERP_LINEAR = 1,
    BZ_QUEST_WC3_INTERP_HERMITE = 2,
    BZ_QUEST_WC3_INTERP_BEZIER = 3,
} bzQuestWc3Interp_t;

/* UINT32_MAX sentinel, mirroring platform/bridge/bz_tabletop_assets.h's
 * BZ_TTA_NO_GLOBAL_SEQUENCE exactly (see this file's header comment on why
 * this file has no #include of that header). */
enum { BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE = 0xFFFFFFFFu };

typedef struct { float x, y, z; } bzQuestWc3Vec3_t;
typedef struct { float x, y, z, w; } bzQuestWc3Quat_t;

typedef struct {
    uint32_t timeMsec;
    bzQuestWc3Vec3_t value, inTan, outTan;
} bzQuestWc3Vec3Key_t;

typedef struct {
    uint32_t timeMsec;
    bzQuestWc3Quat_t value, inTan, outTan;
} bzQuestWc3QuatKey_t;

typedef struct {
    uint32_t timeMsec;
    float value, inTan, outTan;
} bzQuestWc3FloatKey_t;

/* One node channel's already-copied keyframe data (or keyCount==0 for "no
 * track for this channel" - see bz_quest_wc3_capture.c's copy step). Exactly
 * one of the three key arrays is meaningful per channel; callers fill only
 * the matching one (translation/scale -> vec3Keys, rotation -> quatKeys). */
typedef struct {
    uint32_t keyCount;
    bzQuestWc3Interp_t interp;
    uint32_t globalSequence; /* BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE, or a global-sequence-durations[] index */
    bzQuestWc3Vec3Key_t vec3Keys[BZ_QUEST_WC3_MAX_KEYS_PER_TRACK];
    bzQuestWc3QuatKey_t quatKeys[BZ_QUEST_WC3_MAX_KEYS_PER_TRACK];
} bzQuestWc3Track_t;

/* One MDX node (bone/helper/attachment/etc, any object with a pivot).
 * `parentIndex` is this array's own index (NOT a raw MDX object_id -
 * already resolved by the capture layer, matching
 * platform/bridge/bz_tabletop_assets.c's own node_index_for_object_id()
 * convention), or BZ_QUEST_WC3_NO_PARENT for a root node. */
enum { BZ_QUEST_WC3_NO_PARENT = 0xFFFFFFFFu };
typedef struct {
    uint32_t parentIndex;
    bzQuestWc3Vec3_t pivot;
    bzQuestWc3Track_t translation; /* keyCount 0 -> identity translation */
    bzQuestWc3Track_t rotation;    /* keyCount 0 -> identity rotation */
    bzQuestWc3Track_t scale;       /* keyCount 0 -> identity (1,1,1) scale */
} bzQuestWc3Node_t;

/*
 * Samples one vec3 track at `sampleTimeMsec` within `intervalStartMsec`/
 * `intervalEndMsec` (the selected sequence's [start_msec,end_msec) - already
 * resolved to [0,duration] by the caller for a global-sequence track, see
 * bz_quest_wc3_resolve_track_interval() below). `track->keyCount == 0`
 * yields `outValue = {0,0,0}` (translation/scale-as-zero is the identity
 * ADDITIVE default - see this file's header comment; scale callers must
 * add 1.0 themselves or use bz_quest_wc3_sample_vec3_track_scale(), which
 * defaults to {1,1,1} instead). Matches MDLX_GetModelKeytrackValue exactly,
 * including the end-of-interval wrap-toward-first-key formula.
 */
void bz_quest_wc3_sample_vec3_track(const bzQuestWc3Track_t *track, uint32_t intervalStartMsec,
                                    uint32_t intervalEndMsec, uint32_t sampleTimeMsec,
                                    bzQuestWc3Vec3_t *outValue);
/* Same as bz_quest_wc3_sample_vec3_track() but keyCount==0 yields {1,1,1} -
 * for scale tracks, whose "no track" rest value is a unit scale, not zero. */
void bz_quest_wc3_sample_vec3_track_scale(const bzQuestWc3Track_t *track, uint32_t intervalStartMsec,
                                          uint32_t intervalEndMsec, uint32_t sampleTimeMsec,
                                          bzQuestWc3Vec3_t *outValue);
/*
 * Samples one quaternion (rotation) track. `track->keyCount == 0` yields the
 * identity quaternion {0,0,0,1}. Matches MDLX_GetModelKeytrackValue exactly
 * (BEZIER and HERMITE both evaluate via Quaternion_sqlerp - see this file's
 * header comment).
 */
void bz_quest_wc3_sample_quat_track(const bzQuestWc3Track_t *track, uint32_t intervalStartMsec,
                                    uint32_t intervalEndMsec, uint32_t sampleTimeMsec,
                                    bzQuestWc3Quat_t *outValue);

/*
 * Resolves the [intervalStart,intervalEnd] and sampleTime a track must be
 * evaluated at, given the entity's authoritative sequence-relative
 * `entityFrameMsec` (bzTTEntity_t.frame, already known to fall within
 * [seqStartMsec,seqEndMsec) - the caller looked up the sequence via the same
 * [start,end) contract as R_FindSequenceAtTime), the render-clock
 * `renderClockMsec` (see this file's header comment on global sequences),
 * and `globalSeqDurationMsec` (only meaningful when `track->globalSequence
 * != BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE` - the caller already resolved it via
 * the model's global-sequence-durations array). Always succeeds.
 */
void bz_quest_wc3_resolve_track_interval(const bzQuestWc3Track_t *track, uint32_t seqStartMsec,
                                         uint32_t seqEndMsec, uint32_t entityFrameMsec,
                                         uint32_t renderClockMsec, uint32_t globalSeqDurationMsec,
                                         uint32_t *outIntervalStart, uint32_t *outIntervalEnd,
                                         uint32_t *outSampleTime);

/*
 * Builds one node's local (pivot-relative, parent-independent) matrix from
 * its already-sampled translation/rotation/scale values - see
 * R_CalculateNodeMatrix's four-way branch (this file's header comment).
 * `outLocal` is a column-major 4x4 matrix (bz_quest_pure.h layout).
 */
void bz_quest_wc3_node_local_matrix(bool hasTranslation, bool hasRotation, bool hasScale,
                                    const bzQuestWc3Vec3_t *translation, const bzQuestWc3Quat_t *rotation,
                                    const bzQuestWc3Vec3_t *scale, const bzQuestWc3Vec3_t *pivot,
                                    float outLocal[16]);

/*
 * Builds every node's current-pose GLOBAL (model-space) matrix for one
 * model at the given authoritative time, writing `outNodeMatrices[i]` for
 * i in [0,nodeCount). `nodes`/`nodeCount` describe the model's full node
 * array (bounded to BZ_QUEST_WC3_MAX_NODES_PER_MODEL by the caller - see
 * bz_quest_wc3_capture.c); `seqStartMsec`/`seqEndMsec` is the entity's
 * currently-selected sequence interval (already resolved by the caller via
 * the same [start,end) contract as R_FindSequenceAtTime - a caller with no
 * matching sequence must not call this function, matching desktop's
 * `if (!seq) return` early-out, which this module has no data to detect on
 * its own). `globalSeqDurations`/`globalSeqDurationCount` is the model's
 * full global-sequence-duration array (indexed by `bzQuestWc3Track_t.
 * globalSequence`); a track referencing an out-of-range index is treated as
 * BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE (falls back to sequence-relative time)
 * rather than reading out of bounds. Parent references beyond `nodeCount`
 * or forming a cycle are treated as un-parented (root) - MDX node parent
 * chains are acyclic by format contract, but a corrupt/adversarial asset
 * must not be able to recurse this function unboundedly; this is enforced
 * via a fixed BZ_QUEST_WC3_MAX_NODES_PER_MODEL-deep parent-walk cap, not
 * recursion, so a corrupt cycle degrades to "treat as root" instead of a
 * stack overflow.
 */
void bz_quest_wc3_build_pose(const bzQuestWc3Node_t *nodes, uint32_t nodeCount, uint32_t seqStartMsec,
                            uint32_t seqEndMsec, uint32_t entityFrameMsec, uint32_t renderClockMsec,
                            const uint32_t *globalSeqDurations, uint32_t globalSeqDurationCount,
                            float outNodeMatrices[][16]);

/*
 * Builds one geoset's bone-matrix palette (up to BZ_QUEST_WC3_MAX_MATRIX_
 * PALETTE mat4s - see bz_quest_vk_wc3.c's per-draw skinning uniform) from
 * that geoset's own resolved node-index list (bzTTAsset_CopyGeosetMatrix
 * Palette - already-built node array indices, see platform/bridge/
 * bz_tabletop_assets.h's doc comment) and `nodeMatrices`/`nodeCount` (this
 * model's just-built global pose - see bz_quest_wc3_build_pose() above).
 * Palette slots beyond `paletteNodeIndexCount` (and any out-of-range node
 * index) are filled with the identity matrix - see r_mdx_geoset.c:348-361's
 * exact fill-then-overwrite convention transcribed in this file's header
 * comment. `paletteNodeIndexCount` beyond BZ_QUEST_WC3_MAX_MATRIX_PALETTE is
 * clamped (the ABI itself already bounds this to BZ_TTA_MAX_MATRIX_PALETTE,
 * so this is a defense-in-depth clamp, not an expected path).
 */
void bz_quest_wc3_build_bone_palette(const uint32_t *paletteNodeIndices, uint32_t paletteNodeIndexCount,
                                    const float nodeMatrices[][16], uint32_t nodeCount,
                                    float outPalette[][16]);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_ANIM_H */

/*
 * bz_quest_wc3_anim.c - see bz_quest_wc3_anim.h.
 */
#include "bz_quest_wc3_anim.h"

#include <math.h>
#include <string.h>

/* -- scalar/vec3/quat interpolation - transcribed from
 * games/warcraft-3/renderer/mdx/r_mdx_interpolation.c and
 * shared/source/vector3.c/quaternion.c exactly, see this file's header
 * comment for citations. -- */

static float lerpf(float a, float b, float t) { return a * (1.0f - t) + b * t; }

static float bezierf(float left, float outTan, float inTan, float right, float t) {
    float inv = 1.0f - t, inv2 = inv * inv, t2 = t * t;
    float f1 = inv2 * inv, f2 = 3.0f * t * inv2, f3 = 3.0f * t2 * inv, f4 = t2 * t;
    return left * f1 + outTan * f2 + inTan * f3 + right * f4;
}

static float hermitef(float left, float outTan, float inTan, float right, float t) {
    float t2 = t * t;
    float f1 = t2 * (2.0f * t - 3.0f) + 1.0f, f2 = t2 * (t - 2.0f) + t;
    float f3 = t2 * (t - 1.0f), f4 = t2 * (3.0f - 2.0f * t);
    return left * f1 + outTan * f2 + inTan * f3 + right * f4;
}

static bzQuestWc3Vec3_t vec3_lerp(const bzQuestWc3Vec3_t *a, const bzQuestWc3Vec3_t *b, float t) {
    return (bzQuestWc3Vec3_t){lerpf(a->x, b->x, t), lerpf(a->y, b->y, t), lerpf(a->z, b->z, t)};
}

static bzQuestWc3Vec3_t vec3_bezier(const bzQuestWc3Vec3_t *a, const bzQuestWc3Vec3_t *b,
                                    const bzQuestWc3Vec3_t *c, const bzQuestWc3Vec3_t *d, float t) {
    return (bzQuestWc3Vec3_t){bezierf(a->x, b->x, c->x, d->x, t), bezierf(a->y, b->y, c->y, d->y, t),
                              bezierf(a->z, b->z, c->z, d->z, t)};
}

static bzQuestWc3Vec3_t vec3_hermite(const bzQuestWc3Vec3_t *a, const bzQuestWc3Vec3_t *b,
                                     const bzQuestWc3Vec3_t *c, const bzQuestWc3Vec3_t *d, float t) {
    return (bzQuestWc3Vec3_t){hermitef(a->x, b->x, c->x, d->x, t), hermitef(a->y, b->y, c->y, d->y, t),
                              hermitef(a->z, b->z, c->z, d->z, t)};
}

/* shared/cmath3.h's EPSILON. */
#define BZ_QUEST_WC3_EPSILON 1e-6f

/* Quaternion_slerp - shared/source/quaternion.c:3-30, transcribed exactly. */
static bzQuestWc3Quat_t quat_slerp(const bzQuestWc3Quat_t *a, const bzQuestWc3Quat_t *b, float t) {
    float ax = a->x, ay = a->y, az = a->z, aw = a->w;
    float bx = b->x, by = b->y, bz = b->z, bw = b->w;
    float cosom = ax * bx + ay * by + az * bz + aw * bw;
    if (cosom < 0.0f) {
        cosom = -cosom;
        bx = -bx; by = -by; bz = -bz; bw = -bw;
    }
    float scale0, scale1;
    if (1.0f - cosom > BZ_QUEST_WC3_EPSILON) {
        float omega = acosf(cosom);
        float sinom = sinf(omega);
        scale0 = sinf((1.0f - t) * omega) / sinom;
        scale1 = sinf(t * omega) / sinom;
    } else {
        scale0 = 1.0f - t;
        scale1 = t;
    }
    return (bzQuestWc3Quat_t){scale0 * ax + scale1 * bx, scale0 * ay + scale1 * by,
                              scale0 * az + scale1 * bz, scale0 * aw + scale1 * bw};
}

/* Quaternion_sqlerp - shared/source/quaternion.c:32-36, transcribed exactly
 * (both HERMITE and BEZIER interpolation kinds use this same function - see
 * r_mdx_interpolation.c:86-88). */
static bzQuestWc3Quat_t quat_sqlerp(const bzQuestWc3Quat_t *a, const bzQuestWc3Quat_t *b,
                                   const bzQuestWc3Quat_t *c, const bzQuestWc3Quat_t *d, float t) {
    bzQuestWc3Quat_t temp1 = quat_slerp(a, d, t);
    bzQuestWc3Quat_t temp2 = quat_slerp(b, c, t);
    return quat_slerp(&temp1, &temp2, 2.0f * t * (1.0f - t));
}

/* -- track sampling - transcribed from MDLX_GetModelKeytrackValue
 * (r_mdx_anim.c:29-97), generalized over vec3/quat via the VALUE_T macro
 * pattern below so both channel kinds share one exact copy of the
 * interval-scan/wraparound control flow. -- */

/* Generic key-scan core: given a interp function pointer taking four
 * VALUE_T inputs + t, replicate MDLX_GetModelKeytrackValue's exact branch
 * structure. Implemented twice (vec3/quat) rather than via void* + memcpy
 * to keep this file's control flow directly comparable line-by-line against
 * r_mdx_anim.c for future re-verification, per this project's "never guess"
 * discipline - a future reader diffing this file against r_mdx_anim.c
 * should see the same shape twice, not a generic templated indirection. */

#define BZ_QUEST_WC3_DEFINE_SAMPLE(NAME, KEY_T, VAL_T, LERP, BEZIER, HERMITE)                             \
    static void NAME(const KEY_T *keys, uint32_t keyCount, bzQuestWc3Interp_t interp,                     \
                     uint32_t intervalStart, uint32_t intervalEnd, uint32_t time, VAL_T *out) {           \
        const KEY_T *firstKF = NULL, *lastKF = NULL;                                                      \
        for (uint32_t i = 0; i < keyCount; i++) {                                                         \
            const KEY_T *kf = &keys[i];                                                                   \
            if (kf->timeMsec < intervalStart || kf->timeMsec > intervalEnd) continue;                     \
            if (!firstKF || kf->timeMsec < firstKF->timeMsec) firstKF = kf;                                \
            if (!lastKF || kf->timeMsec > lastKF->timeMsec) lastKF = kf;                                  \
        }                                                                                                 \
        if (lastKF && firstKF && time >= lastKF->timeMsec && time <= intervalEnd) {                       \
            if (firstKF != lastKF) {                                                                      \
                float endToStart = (float)((intervalEnd - lastKF->timeMsec) +                              \
                                           (firstKF->timeMsec - intervalStart));                           \
                float wrapT = (float)(time - lastKF->timeMsec) / endToStart;                               \
                if (wrapT < 1.0f) {                                                                        \
                    if (interp == BZ_QUEST_WC3_INTERP_BEZIER)                                              \
                        *out = BEZIER(&lastKF->value, &lastKF->outTan, &firstKF->inTan, &firstKF->value,   \
                                      wrapT);                                                              \
                    else if (interp == BZ_QUEST_WC3_INTERP_HERMITE)                                        \
                        *out = HERMITE(&lastKF->value, &lastKF->outTan, &firstKF->inTan, &firstKF->value,  \
                                       wrapT);                                                              \
                    else if (interp == BZ_QUEST_WC3_INTERP_NONE)                                           \
                        *out = lastKF->value;                                                              \
                    else                                                                                  \
                        *out = LERP(&lastKF->value, &firstKF->value, wrapT);                                \
                    return;                                                                                \
                }                                                                                          \
            }                                                                                              \
            *out = lastKF->value;                                                                          \
            return;                                                                                        \
        }                                                                                                 \
        const KEY_T *prevKF = NULL;                                                                        \
        for (uint32_t i = 0; i < keyCount; i++) {                                                          \
            const KEY_T *kf = &keys[i];                                                                    \
            if (kf->timeMsec < intervalStart) continue;                                                    \
            if (kf->timeMsec > intervalEnd) {                                                              \
                if (prevKF) *out = prevKF->value;                                                           \
                return;                                                                                     \
            }                                                                                              \
            if (kf->timeMsec == time || (kf->timeMsec > time && !prevKF)) {                                \
                *out = kf->value;                                                                            \
                return;                                                                                     \
            }                                                                                              \
            if (kf->timeMsec > time) {                                                                     \
                if (kf->timeMsec == prevKF->timeMsec) {                                                     \
                    *out = prevKF->value;                                                                   \
                } else if (interp == BZ_QUEST_WC3_INTERP_NONE) {                                            \
                    *out = prevKF->value;                                                                   \
                } else {                                                                                    \
                    float t = (float)(time - prevKF->timeMsec) / (float)(kf->timeMsec - prevKF->timeMsec);  \
                    if (interp == BZ_QUEST_WC3_INTERP_BEZIER)                                               \
                        *out = BEZIER(&prevKF->value, &prevKF->outTan, &kf->inTan, &kf->value, t);          \
                    else if (interp == BZ_QUEST_WC3_INTERP_HERMITE)                                         \
                        *out = HERMITE(&prevKF->value, &prevKF->outTan, &kf->inTan, &kf->value, t);         \
                    else                                                                                    \
                        *out = LERP(&prevKF->value, &kf->value, t);                                         \
                }                                                                                            \
                return;                                                                                     \
            }                                                                                              \
            prevKF = kf;                                                                                   \
        }                                                                                                  \
        if (prevKF) *out = prevKF->value;                                                                  \
    }

static float float_lerp(const float *a, const float *b, float t) { return lerpf(*a, *b, t); }
static float float_bezier(const float *a, const float *b, const float *c, const float *d, float t) {
    return bezierf(*a, *b, *c, *d, t);
}
static float float_hermite(const float *a, const float *b, const float *c, const float *d, float t) {
    return hermitef(*a, *b, *c, *d, t);
}

BZ_QUEST_WC3_DEFINE_SAMPLE(sample_vec3, bzQuestWc3Vec3Key_t, bzQuestWc3Vec3_t, vec3_lerp, vec3_bezier,
                          vec3_hermite)
/* Both HERMITE and BEZIER map to quat_sqlerp - r_mdx_interpolation.c:86-88 (see this file's
 * header comment) - not a distinct quaternion bezier/hermite formula. */
BZ_QUEST_WC3_DEFINE_SAMPLE(sample_quat, bzQuestWc3QuatKey_t, bzQuestWc3Quat_t, quat_slerp, quat_sqlerp,
                          quat_sqlerp)
BZ_QUEST_WC3_DEFINE_SAMPLE(sample_float, bzQuestWc3FloatKey_t, float, float_lerp, float_bezier,
                          float_hermite)

void bz_quest_wc3_sample_vec3_track(const bzQuestWc3Track_t *track, uint32_t intervalStartMsec,
                                    uint32_t intervalEndMsec, uint32_t sampleTimeMsec,
                                    bzQuestWc3Vec3_t *outValue) {
    if (track->keyCount == 0) {
        *outValue = (bzQuestWc3Vec3_t){0, 0, 0};
        return;
    }
    sample_vec3(track->vec3Keys, track->keyCount, track->interp, intervalStartMsec, intervalEndMsec,
               sampleTimeMsec, outValue);
}

void bz_quest_wc3_sample_vec3_track_scale(const bzQuestWc3Track_t *track, uint32_t intervalStartMsec,
                                          uint32_t intervalEndMsec, uint32_t sampleTimeMsec,
                                          bzQuestWc3Vec3_t *outValue) {
    if (track->keyCount == 0) {
        *outValue = (bzQuestWc3Vec3_t){1, 1, 1};
        return;
    }
    sample_vec3(track->vec3Keys, track->keyCount, track->interp, intervalStartMsec, intervalEndMsec,
               sampleTimeMsec, outValue);
}

void bz_quest_wc3_sample_quat_track(const bzQuestWc3Track_t *track, uint32_t intervalStartMsec,
                                    uint32_t intervalEndMsec, uint32_t sampleTimeMsec,
                                    bzQuestWc3Quat_t *outValue) {
    if (track->keyCount == 0) {
        *outValue = (bzQuestWc3Quat_t){0, 0, 0, 1};
        return;
    }
    sample_quat(track->quatKeys, track->keyCount, track->interp, intervalStartMsec, intervalEndMsec,
               sampleTimeMsec, outValue);
}

void bz_quest_wc3_sample_float_track(const bzQuestWc3Track_t *track, uint32_t intervalStartMsec,
                                     uint32_t intervalEndMsec, uint32_t sampleTimeMsec,
                                     float defaultValue, float *outValue) {
    if (track->keyCount == 0) {
        *outValue = defaultValue;
        return;
    }
    sample_float(track->floatKeys, track->keyCount, track->interp, intervalStartMsec, intervalEndMsec,
                sampleTimeMsec, outValue);
}

void bz_quest_wc3_resolve_track_interval(const bzQuestWc3Track_t *track, uint32_t seqStartMsec,
                                         uint32_t seqEndMsec, uint32_t entityFrameMsec,
                                         uint32_t renderClockMsec, uint32_t globalSeqDurationMsec,
                                         uint32_t *outIntervalStart, uint32_t *outIntervalEnd,
                                         uint32_t *outSampleTime) {
    if (track->globalSequence != BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE) {
        /* r_mdx_anim.c:34-41 - global sequences ignore entity/sequence time
         * entirely and sample the render clock, wrapping modulo
         * (duration + 1) - transcribed exactly, see this file's header
         * comment. */
        *outIntervalStart = 0;
        *outIntervalEnd = globalSeqDurationMsec;
        uint32_t gsLen = globalSeqDurationMsec + 1;
        *outSampleTime = gsLen > 0 ? (renderClockMsec % gsLen) : 0;
        return;
    }
    *outIntervalStart = seqStartMsec;
    *outIntervalEnd = seqEndMsec;
    *outSampleTime = entityFrameMsec;
}

/* -- node local matrix - transcribed from R_CalculateNodeMatrix
 * (r_mdx_anim.c:104-142) and Matrix4_from_rotation_origin/
 * _rotation_translation_scale_origin/_translation (shared/source/matrix4.c),
 * see this file's header comment. Column-major, v[col*4+row] - matches
 * bz_quest_pure.h's bz_quest_mat4_multiply() indexing exactly. -- */

static void mat4_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_from_translation(float m[16], const bzQuestWc3Vec3_t *v) {
    mat4_identity(m);
    m[12] = v->x; m[13] = v->y; m[14] = v->z;
}

/* Matrix4_from_rotation_origin (shared/source/matrix4.c:326-363). */
static void mat4_from_rotation_origin(float out[16], const bzQuestWc3Quat_t *q, const bzQuestWc3Vec3_t *o) {
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    float ox = o->x, oy = o->y, oz = o->z;
    out[0] = 1 - (yy + zz); out[1] = xy + wz; out[2] = xz - wy; out[3] = 0;
    out[4] = xy - wz; out[5] = 1 - (xx + zz); out[6] = yz + wx; out[7] = 0;
    out[8] = xz + wy; out[9] = yz - wx; out[10] = 1 - (xx + yy); out[11] = 0;
    out[12] = ox - (out[0] * ox + out[4] * oy + out[8] * oz);
    out[13] = oy - (out[1] * ox + out[5] * oy + out[9] * oz);
    out[14] = oz - (out[2] * ox + out[6] * oy + out[10] * oz);
    out[15] = 1;
}

/* Matrix4_from_rotation_translation_scale_origin (shared/source/matrix4.c:366-415). */
static void mat4_from_rts_origin(float out[16], const bzQuestWc3Quat_t *q, const bzQuestWc3Vec3_t *v,
                                 const bzQuestWc3Vec3_t *s, const bzQuestWc3Vec3_t *o) {
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;
    float sx = s->x, sy = s->y, sz = s->z;
    float ox = o->x, oy = o->y, oz = o->z;
    float o0 = (1 - (yy + zz)) * sx, o1 = (xy + wz) * sx, o2 = (xz - wy) * sx;
    float o4 = (xy - wz) * sy, o5 = (1 - (xx + zz)) * sy, o6 = (yz + wx) * sy;
    float o8 = (xz + wy) * sz, o9 = (yz - wx) * sz, o10 = (1 - (xx + yy)) * sz;
    out[0] = o0; out[1] = o1; out[2] = o2; out[3] = 0;
    out[4] = o4; out[5] = o5; out[6] = o6; out[7] = 0;
    out[8] = o8; out[9] = o9; out[10] = o10; out[11] = 0;
    out[12] = v->x + ox - (o0 * ox + o4 * oy + o8 * oz);
    out[13] = v->y + oy - (o1 * ox + o5 * oy + o9 * oz);
    out[14] = v->z + oz - (o2 * ox + o6 * oy + o10 * oz);
    out[15] = 1;
}

static void mat4_multiply(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = sum;
        }
}

void bz_quest_wc3_node_local_matrix(bool hasTranslation, bool hasRotation, bool hasScale,
                                    const bzQuestWc3Vec3_t *translation, const bzQuestWc3Quat_t *rotation,
                                    const bzQuestWc3Vec3_t *scale, const bzQuestWc3Vec3_t *pivot,
                                    float outLocal[16]) {
    if (!hasTranslation && !hasRotation && !hasScale) {
        mat4_identity(outLocal);
    } else if (hasTranslation && !hasRotation && !hasScale) {
        mat4_from_translation(outLocal, translation);
    } else if (!hasTranslation && hasRotation && !hasScale) {
        mat4_from_rotation_origin(outLocal, rotation, pivot);
    } else {
        bzQuestWc3Vec3_t t = hasTranslation ? *translation : (bzQuestWc3Vec3_t){0, 0, 0};
        bzQuestWc3Quat_t r = hasRotation ? *rotation : (bzQuestWc3Quat_t){0, 0, 0, 1};
        bzQuestWc3Vec3_t s = hasScale ? *scale : (bzQuestWc3Vec3_t){1, 1, 1};
        mat4_from_rts_origin(outLocal, &r, &t, &s, pivot);
    }
}

void bz_quest_wc3_build_pose(const bzQuestWc3Node_t *nodes, uint32_t nodeCount, uint32_t seqStartMsec,
                            uint32_t seqEndMsec, uint32_t entityFrameMsec, uint32_t renderClockMsec,
                            const uint32_t *globalSeqDurations, uint32_t globalSeqDurationCount,
                            float outNodeMatrices[][16]) {
    if (nodeCount > BZ_QUEST_WC3_MAX_NODES_PER_MODEL) nodeCount = BZ_QUEST_WC3_MAX_NODES_PER_MODEL;
    float localMatrices[BZ_QUEST_WC3_MAX_NODES_PER_MODEL][16];
    bool computed[BZ_QUEST_WC3_MAX_NODES_PER_MODEL];
    memset(computed, 0, sizeof(computed));

    for (uint32_t i = 0; i < nodeCount; i++) {
        const bzQuestWc3Node_t *node = &nodes[i];

        uint32_t tStart, tEnd, tTime;
        bzQuestWc3Vec3_t translation = {0, 0, 0};
        if (node->translation.keyCount != 0) {
            uint32_t gsd = (node->translation.globalSequence < globalSeqDurationCount)
                              ? globalSeqDurations[node->translation.globalSequence]
                              : 0;
            bz_quest_wc3_resolve_track_interval(&node->translation, seqStartMsec, seqEndMsec,
                                                entityFrameMsec, renderClockMsec, gsd, &tStart, &tEnd,
                                                &tTime);
            bz_quest_wc3_sample_vec3_track(&node->translation, tStart, tEnd, tTime, &translation);
        }
        uint32_t rStart, rEnd, rTime;
        bzQuestWc3Quat_t rotation = {0, 0, 0, 1};
        if (node->rotation.keyCount != 0) {
            uint32_t gsd = (node->rotation.globalSequence < globalSeqDurationCount)
                              ? globalSeqDurations[node->rotation.globalSequence]
                              : 0;
            bz_quest_wc3_resolve_track_interval(&node->rotation, seqStartMsec, seqEndMsec, entityFrameMsec,
                                                renderClockMsec, gsd, &rStart, &rEnd, &rTime);
            bz_quest_wc3_sample_quat_track(&node->rotation, rStart, rEnd, rTime, &rotation);
        }
        uint32_t sStart, sEnd, sTime;
        bzQuestWc3Vec3_t scale = {1, 1, 1};
        if (node->scale.keyCount != 0) {
            uint32_t gsd = (node->scale.globalSequence < globalSeqDurationCount)
                              ? globalSeqDurations[node->scale.globalSequence]
                              : 0;
            bz_quest_wc3_resolve_track_interval(&node->scale, seqStartMsec, seqEndMsec, entityFrameMsec,
                                                renderClockMsec, gsd, &sStart, &sEnd, &sTime);
            bz_quest_wc3_sample_vec3_track_scale(&node->scale, sStart, sEnd, sTime, &scale);
        }

        bz_quest_wc3_node_local_matrix(node->translation.keyCount != 0, node->rotation.keyCount != 0,
                                       node->scale.keyCount != 0, &translation, &rotation, &scale,
                                       &node->pivot, localMatrices[i]);
    }

    /* Global = parent_global * local, walked iteratively (not recursively -
     * see this file's header comment on the fixed-depth cycle guard) so a
     * node's own global matrix is computed only after its parent's. Nodes
     * are processed in array order with a bounded number of passes: a
     * well-formed MDX parent chain always has parent_index < node's own
     * index... is NOT guaranteed by the format (BONE/HELP order in the file
     * is not required to be parent-before-child), so this does up to
     * nodeCount passes, each pass resolving any node whose parent is either
     * root or already resolved - a node whose parent never resolves within
     * nodeCount passes (a cycle) falls back to being treated as root on the
     * final pass, guaranteeing termination. */
    for (uint32_t pass = 0; pass < nodeCount; pass++) {
        bool progressed = false;
        for (uint32_t i = 0; i < nodeCount; i++) {
            if (computed[i]) continue;
            uint32_t parent = nodes[i].parentIndex;
            if (parent == BZ_QUEST_WC3_NO_PARENT || parent >= nodeCount) {
                memcpy(outNodeMatrices[i], localMatrices[i], sizeof(float) * 16);
                computed[i] = true;
                progressed = true;
            } else if (computed[parent]) {
                mat4_multiply(outNodeMatrices[parent], localMatrices[i], outNodeMatrices[i]);
                computed[i] = true;
                progressed = true;
            }
        }
        if (!progressed) break;
    }
    /* Any node still unresolved is part of a parent cycle - treat as root
     * (local-only) so every entry is always written, never left garbage. */
    for (uint32_t i = 0; i < nodeCount; i++) {
        if (!computed[i]) memcpy(outNodeMatrices[i], localMatrices[i], sizeof(float) * 16);
    }
}

void bz_quest_wc3_build_bone_palette(const uint32_t *paletteNodeIndices, uint32_t paletteNodeIndexCount,
                                    const float nodeMatrices[][16], uint32_t nodeCount,
                                    float outPalette[][16]) {
    if (paletteNodeIndexCount > BZ_QUEST_WC3_MAX_MATRIX_PALETTE)
        paletteNodeIndexCount = BZ_QUEST_WC3_MAX_MATRIX_PALETTE;
    for (uint32_t i = 0; i < BZ_QUEST_WC3_MAX_MATRIX_PALETTE; i++) {
        if (i < paletteNodeIndexCount && paletteNodeIndices[i] < nodeCount) {
            memcpy(outPalette[i], nodeMatrices[paletteNodeIndices[i]], sizeof(float) * 16);
        } else {
            mat4_identity(outPalette[i]);
        }
    }
}

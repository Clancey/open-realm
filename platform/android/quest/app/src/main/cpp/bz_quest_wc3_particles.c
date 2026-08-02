/*
 * bz_quest_wc3_particles.c - see bz_quest_wc3_particles.h.
 */
#include "bz_quest_wc3_particles.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- PRNG: SplitMix64 (Vigna, public domain), chosen only for its
 * simplicity/determinism - no relation to desktop's libc rand()/srand(),
 * which is process-global, not independently seedable, and therefore unfit
 * for per-pool host-test determinism (see this file's header comment on why
 * this project's own mdxtool.c --seed precedent applies here too). -- */
static uint64_t next_u64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* Uniform float in [0,1) from the top 24 bits (ample precision for particle
 * spawn placement; avoids the double-precision division a full 53-bit
 * extraction would need on every call). */
static float next_float01(uint64_t *state) {
    return (float)(next_u64(state) >> 40) * (1.0f / 16777216.0f); /* 2^24 */
}

/* -- Column-major 4x4 * point - matches bz_quest_pure.h's
 * bz_quest_mat4_multiply() indexing (v[col*4+row]) exactly. Only the spawn
 * ORIGIN needs a matrix transform (includes the translation column,
 * m[12..14]) - velocity/acceleration are never matrix-transformed on
 * desktop either, see this file's header comment, so no separate
 * vector-only (translation-excluded) transform is needed here. -- */
static void mat4_transform_point(const float m[16], float x, float y, float z, float *outX, float *outY,
                                 float *outZ) {
    *outX = m[0] * x + m[4] * y + m[8] * z + m[12];
    *outY = m[1] * x + m[5] * y + m[9] * z + m[13];
    *outZ = m[2] * x + m[6] * y + m[10] * z + m[14];
}

void bz_quest_wc3_particles_pool_reset(bzQuestWc3ParticlePool_t *pool, uint64_t seed) {
    memset(pool, 0, sizeof(*pool));
    pool->rngState = seed ? seed : 0x9E3779B97F4A7C15ULL; /* an all-zero seed locks splitmix64 forever */
    for (uint32_t i = 0; i < BZ_QUEST_WC3_MAX_PARTICLES; i++)
        pool->particles[i].next = (i + 1 < BZ_QUEST_WC3_MAX_PARTICLES) ? (uint16_t)(i + 1)
                                                                       : BZ_QUEST_WC3_PARTICLE_NONE;
    pool->freeHead = 0;
    pool->activeHead = BZ_QUEST_WC3_PARTICLE_NONE;
    pool->activeCount = 0;
}

/* Pops one slot off the free list and pushes it onto the active list -
 * mirrors R_SpawnParticle() exactly (renderer/r_particles.c), including its
 * NULL-return-on-exhaustion contract (here: BZ_QUEST_WC3_PARTICLE_NONE). */
static uint16_t pool_spawn_slot(bzQuestWc3ParticlePool_t *pool) {
    uint16_t slot = pool->freeHead;
    if (slot == BZ_QUEST_WC3_PARTICLE_NONE) return BZ_QUEST_WC3_PARTICLE_NONE;
    pool->freeHead = pool->particles[slot].next;
    pool->particles[slot].next = pool->activeHead;
    pool->activeHead = slot;
    pool->activeCount++;
    return slot;
}

void bz_quest_wc3_particles_age(bzQuestWc3ParticlePool_t *pool, float deltaSec) {
    uint16_t cur = pool->activeHead, newActiveHead = BZ_QUEST_WC3_PARTICLE_NONE;
    uint16_t newActiveTail = BZ_QUEST_WC3_PARTICLE_NONE;
    while (cur != BZ_QUEST_WC3_PARTICLE_NONE) {
        uint16_t next = pool->particles[cur].next;
        pool->particles[cur].ageSec += deltaSec;
        if (pool->particles[cur].ageSec > pool->particles[cur].lifespanSec) {
            /* Expired: return to the free list, do not carry into the new active list. */
            pool->particles[cur].next = pool->freeHead;
            pool->freeHead = cur;
            pool->activeCount--;
        } else {
            pool->particles[cur].next = BZ_QUEST_WC3_PARTICLE_NONE;
            if (newActiveTail == BZ_QUEST_WC3_PARTICLE_NONE) newActiveHead = cur;
            else pool->particles[newActiveTail].next = cur;
            newActiveTail = cur;
        }
        cur = next;
    }
    pool->activeHead = newActiveHead;
}

/* FX_GenerateRandomOrigin (renderer/r_particles.c), rewritten to emit its
 * output directly in this project's Y-up target convention rather than raw
 * MDX Z-up - see this file's header comment on why a freshly-synthesized
 * vector (not an existing MDX-authored track value) may do this: desktop's
 * `(randLength, randWidth, 0)` (X=length axis, Y=width axis, Z=0, i.e. flat
 * in the Z-up "ground" XY plane) becomes `(randLength, 0, randWidth)` under
 * the project's standard (x,y,z)->(x,z,y) axis swap (Y-up's "ground" plane
 * is XZ, height is Y) - X is untouched, the flat/height axis moves from Z
 * to Y (0), and the width axis moves from Y to Z. */
static void random_origin_yup(uint64_t *rng, float length, float width, float *outX, float *outY, float *outZ) {
    *outX = (next_float01(rng) - 0.5f) * length;
    *outY = 0.0f;
    *outZ = (next_float01(rng) - 0.5f) * width;
}

/* FX_GenerateRandomDirection (renderer/r_particles.c): a uniformly-sampled
 * point on a spherical cap of half-angle `latitudeDeg` around the ABSOLUTE
 * world +Z axis (Z-up) - NEVER the emitter's own rotated local axis (see
 * this file's header comment: desktop's own p->vel is never matrix-
 * transformed, confirmed by re-reading r_mdx_geoset.c's MDLX_RenderEmitter
 * directly). Under this project's standard axis swap, world +Z (Z-up)
 * becomes world +Y (Y-up), so this function's cone axis is world +Y -
 * `(sin(phi)cos(theta), cos(phi), sin(phi)sin(theta))` instead of desktop's
 * `(sin(phi)cos(theta), sin(phi)sin(theta), cos(phi))`. The caller
 * (bz_quest_wc3_particles_emit()) must NOT multiply this result by the
 * emitter's world matrix - only the spawn origin (random_origin_yup()'s
 * result) goes through that matrix, exactly matching desktop. */
static void random_direction_yup(uint64_t *rng, float latitudeDeg, float *outX, float *outY, float *outZ) {
    float latitudeRad = latitudeDeg * (float)M_PI / 180.0f;
    float theta = next_float01(rng) * 2.0f * (float)M_PI;
    float phi = next_float01(rng) * latitudeRad;
    float sinPhi = sinf(phi);
    *outX = sinPhi * cosf(theta);
    *outY = cosf(phi);
    *outZ = sinPhi * sinf(theta);
}

static uint8_t clamp_alpha_component(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)v;
}

uint32_t bz_quest_wc3_particles_emit(bzQuestWc3ParticlePool_t *pool,
                                     const bzQuestWc3ParticleEmitterFrame_t *emitter,
                                     uint32_t previousClockMsec, uint32_t currentClockMsec,
                                     bool *outOverflowed) {
    uint32_t spawned = 0;
    bool overflowed = false;
    /* MDLX_RenderEmitter's own early-out (r_mdx_geoset.c) - matches exactly. */
    if (emitter->emissionRate <= 0.0f) {
        if (outOverflowed) *outOverflowed = false;
        return 0;
    }
    /* MAX(1, EmissionRate) - transcribed exactly, including its floor (see this file's header
     * comment on why this is not "fixed" without evidence it is wrong). */
    float rate = emitter->emissionRate < 1.0f ? 1.0f : emitter->emissionRate;
    float stepMsec = 1000.0f / rate;
    /* Phase-locked to whole real-world seconds - transcribed exactly from
     * MDLX_RenderEmitter (r_mdx_geoset.c:91-129), see this file's header comment. */
    uint32_t lastFrameTime = previousClockMsec;
    uint32_t start = lastFrameTime - (lastFrameTime % 1000u);
    bool spawning = false;
    uint32_t iterations = 0;
    for (float time = (float)start; time < (float)currentClockMsec;
         time += stepMsec, iterations++) {
        if (iterations >= BZ_QUEST_WC3_MAX_PARTICLE_SPAWNS_PER_EMITTER_PER_FRAME * 4u) {
            /* Defense-in-depth only - see BZ_QUEST_WC3_MAX_PARTICLE_SPAWNS_PER_EMITTER_PER_FRAME's
             * doc comment; a real EmissionRate/frame-delta combination never reaches this. */
            overflowed = true;
            break;
        }
        if (time >= (float)lastFrameTime) spawning = true;
        if (!spawning) continue;
        if (spawned >= BZ_QUEST_WC3_MAX_PARTICLE_SPAWNS_PER_EMITTER_PER_FRAME) {
            overflowed = true;
            break;
        }
        uint16_t slot = pool_spawn_slot(pool);
        if (slot == BZ_QUEST_WC3_PARTICLE_NONE) {
            overflowed = true;
            break;
        }
        bzQuestWc3Particle_t *p = &pool->particles[slot];
        float localX, localY, localZ, dirX, dirY, dirZ;
        random_origin_yup(&pool->rngState, emitter->length, emitter->width, &localX, &localY, &localZ);
        random_direction_yup(&pool->rngState, emitter->latitude, &dirX, &dirY, &dirZ);
        /* Only the spawn ORIGIN goes through the emitter's world matrix - re-read directly from
         * r_mdx_geoset.c's MDLX_RenderEmitter (not assumed): `p->vel`/`p->accel` are NEVER
         * matrix-transformed on desktop, computed directly in absolute engine space (see this
         * file's header comment). Velocity/acceleration below are therefore used exactly as
         * random_direction_yup()/the constant gravity vector produced them, with no matrix
         * multiply - reproducing that exactly, not a simplification. */
        mat4_transform_point(emitter->worldMatrix, localX, localY, localZ, &p->posX, &p->posY, &p->posZ);
        p->velX = dirX * emitter->speed;
        p->velY = dirY * emitter->speed;
        p->velZ = dirZ * emitter->speed;
        /* Gravity is the constant world -Y (down) in this project's Y-up convention - the
         * direct equivalent of desktop's constant world -Z (r_mdx_geoset.c:
         * Vector3_scale(&(VECTOR3){0,0,-1}, Gravity)), never rotated by any node/model matrix. */
        p->accelX = 0.0f;
        p->accelY = -emitter->gravity;
        p->accelZ = 0.0f;
        p->ageSec = 0.0f;
        p->lifespanSec = emitter->lifeSpan;
        p->midtimeFrac = clamp_alpha_component(emitter->timeMiddle * 255.0f);
        p->rows = (uint8_t)(emitter->rows > 255 ? 255 : emitter->rows);
        p->columns = (uint8_t)(emitter->columns > 255 ? 255 : emitter->columns);
        p->blendMode = emitter->blendMode;
        if (emitter->textureIdentity)
            snprintf(p->textureIdentity, sizeof(p->textureIdentity), "%s", emitter->textureIdentity);
        else
            p->textureIdentity[0] = '\0';
        for (int seg = 0; seg < 3; seg++) {
            uint8_t *dst = seg == 0 ? p->color0 : seg == 1 ? p->color1 : p->color2;
            dst[0] = clamp_alpha_component(emitter->segmentColor[seg * 3 + 0] * 255.0f);
            dst[1] = clamp_alpha_component(emitter->segmentColor[seg * 3 + 1] * 255.0f);
            dst[2] = clamp_alpha_component(emitter->segmentColor[seg * 3 + 2] * 255.0f);
            dst[3] = emitter->segmentAlpha[seg];
        }
        p->size0 = emitter->particleScaling[0];
        p->size1 = emitter->particleScaling[1];
        p->size2 = emitter->particleScaling[2];
        spawned++;
    }
    if (outOverflowed) *outOverflowed = overflowed;
    return spawned;
}

/* -- Per-frame evaluation: FX_BlendColor/FX_BlendFloat/FX_GetFrame
 * (renderer/r_particles.c), transcribed exactly, including the k==0/
 * midtimeFrac==0 edge case's inherited division (see this file's header
 * comment: this module transcribes desktop's authoritative formulas rather
 * than silently patching an edge case desktop itself never guards against -
 * "never guess at a bug fix" without evidence this is reachable/wrong in
 * practice; a particle's own age is 0 only for one instant at spawn, before
 * this same frame's bz_quest_wc3_particles_age() call ages it). -- */
static void lerp_color4(const uint8_t a[4], const uint8_t b[4], float t, uint8_t out[4]) {
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)((float)a[i] + ((float)b[i] - (float)a[i]) * t);
}

static void blend_color(const bzQuestWc3Particle_t *p, float k, uint8_t out[4]) {
    float t = (float)p->midtimeFrac / 255.0f;
    if (k > t) lerp_color4(p->color1, p->color2, (k - t) / (1.0f - t), out);
    else lerp_color4(p->color0, p->color1, k / t, out);
}

static float blend_float3(const float v[3], float k, float t) {
    if (k > t) return v[1] + (v[2] - v[1]) * ((k - t) / (1.0f - t));
    return v[0] + (v[1] - v[0]) * (k / t);
}

/* FX_GetFrame (renderer/r_particles.c): atlas frame advances over the
 * particle's OWN lifetime fraction (never a shared clock - see that
 * function's own comment on why: a shared clock flips every particle's
 * frame in unison, a crude strobing artifact), 256-unit integer division
 * transcribed exactly (not a floating-point 0..1 UV - matches desktop's own
 * COLOR32-packed byte precision). */
static void atlas_rect(uint32_t rows, uint32_t columns, float lifetimeFrac, uint8_t *outU0, uint8_t *outV0,
                      uint8_t *outU1, uint8_t *outV1) {
    uint32_t r = rows ? rows : 1, c = columns ? columns : 1;
    uint32_t total = r * c;
    uint32_t frame = (uint32_t)(lifetimeFrac * (float)total);
    if (frame >= total) frame = total - 1;
    uint32_t u = frame % c, v = frame / c;
    uint32_t usize = 256 / c, vsize = 256 / r;
    *outU0 = (uint8_t)(usize * u);
    *outV0 = (uint8_t)(vsize * v);
    *outU1 = (uint8_t)(usize * (u + 1) - 1);
    *outV1 = (uint8_t)(vsize * (v + 1) - 1);
}

/* One quad's 6 corner (axisX,axisY) pairs - matches R_AddParticle's exact
 * two-triangle fan (renderer/r_particles.c): (0,0),(1,0),(1,1),(1,1),(0,1),(0,0). */
static const uint8_t kQuadAxisX[6] = {0, 255, 255, 255, 0, 0};
static const uint8_t kQuadAxisY[6] = {0, 0, 255, 255, 255, 0};

typedef struct {
    uint16_t particleIndex;
    uint32_t blendMode;
    const char *textureIdentity;
} bzQuestWc3ParticleSortKey_t;

static int compare_sort_key(const void *a, const void *b) {
    const bzQuestWc3ParticleSortKey_t *ka = a, *kb = b;
    if (ka->blendMode != kb->blendMode) return ka->blendMode < kb->blendMode ? -1 : 1;
    int cmp = strcmp(ka->textureIdentity, kb->textureIdentity);
    if (cmp != 0) return cmp;
    /* Tiebreaker: original particle slot index, so two particles sharing a (blendMode,
     * textureIdentity) key still sort into one fully deterministic total order regardless of
     * qsort's own (unspecified, not-guaranteed-stable) algorithm - see this file's header
     * comment on why determinism matters for host tests. */
    return ka->particleIndex < kb->particleIndex ? -1 : (ka->particleIndex > kb->particleIndex ? 1 : 0);
}

uint32_t bz_quest_wc3_particles_pack(const bzQuestWc3ParticlePool_t *pool,
                                     bzQuestWc3ParticleVertex_t *outVertices, uint32_t maxVertices,
                                     bzQuestWc3ParticleDrawRun_t *outRuns, uint32_t maxRuns,
                                     uint32_t *outRunCount) {
    bzQuestWc3ParticleSortKey_t keys[BZ_QUEST_WC3_MAX_PARTICLES];
    uint32_t keyCount = 0;
    for (uint16_t cur = pool->activeHead; cur != BZ_QUEST_WC3_PARTICLE_NONE; cur = pool->particles[cur].next) {
        keys[keyCount].particleIndex = cur;
        keys[keyCount].blendMode = pool->particles[cur].blendMode;
        keys[keyCount].textureIdentity = pool->particles[cur].textureIdentity;
        keyCount++;
    }
    qsort(keys, keyCount, sizeof(keys[0]), compare_sort_key);

    uint32_t vertexCount = 0, runCount = 0;
    for (uint32_t i = 0; i < keyCount; i++) {
        const bzQuestWc3Particle_t *p = &pool->particles[keys[i].particleIndex];
        if (vertexCount + 6 > maxVertices) break; /* clean truncation - see this file's doc comment */
        bool newRun = runCount == 0 || outRuns[runCount - 1].blendMode != p->blendMode ||
                     strcmp(outRuns[runCount - 1].textureIdentity, p->textureIdentity) != 0;
        if (newRun) {
            if (runCount >= maxRuns) {
                /* Merge the excess into the last run rather than overflow outRuns - see this
                 * file's doc comment; the caller logs this once via a run-count comparison. */
                newRun = false;
            } else {
                outRuns[runCount].firstVertex = vertexCount;
                outRuns[runCount].vertexCount = 0;
                outRuns[runCount].blendMode = p->blendMode;
                snprintf(outRuns[runCount].textureIdentity, sizeof(outRuns[runCount].textureIdentity), "%s",
                        p->textureIdentity);
                runCount++;
            }
        }
        float k = p->lifespanSec > 0.0f ? p->ageSec / p->lifespanSec : 0.0f;
        if (k > 1.0f) k = 1.0f;
        if (k < 0.0f) k = 0.0f;
        float t = (float)p->midtimeFrac / 255.0f;
        uint8_t color[4];
        blend_color(p, k, color);
        float sizes[3] = {p->size0, p->size1, p->size2};
        float size = blend_float3(sizes, k, t) * 2.0f; /* matches R_DrawParticles' own `size * 2.0` call site */
        uint8_t u0, v0, u1, v1;
        atlas_rect(p->rows, p->columns, k, &u0, &v0, &u1, &v1);
        /* Kinematics: org = org0 + vel0*t + 0.5*accel*t^2 - the closed form of desktop's own
         * semi-implicit-Euler per-frame integration (see this file's header comment). */
        float halfAT2 = 0.5f * p->ageSec * p->ageSec;
        float posX = p->posX + p->velX * p->ageSec + p->accelX * halfAT2;
        float posY = p->posY + p->velY * p->ageSec + p->accelY * halfAT2;
        float posZ = p->posZ + p->velZ * p->ageSec + p->accelZ * halfAT2;
        for (int corner = 0; corner < 6; corner++) {
            bzQuestWc3ParticleVertex_t *vtx = &outVertices[vertexCount + corner];
            vtx->posX = posX; vtx->posY = posY; vtx->posZ = posZ;
            memcpy(vtx->color, color, 4);
            vtx->size = size;
            vtx->u0 = u0; vtx->v0 = v0; vtx->u1 = u1; vtx->v1 = v1;
            vtx->axisX = kQuadAxisX[corner];
            vtx->axisY = kQuadAxisY[corner];
        }
        vertexCount += 6;
        if (runCount > 0) outRuns[runCount - 1].vertexCount += 6;
    }
    if (outRunCount) *outRunCount = runCount;
    return vertexCount;
}

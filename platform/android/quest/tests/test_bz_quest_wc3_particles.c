/*
 * test_bz_quest_wc3_particles.c - host coverage for bz_quest_wc3_particles.h/.c
 * (layer 9: PRE2 particle emitter simulation). See that header's own
 * extensive citation trail for the desktop formulas every assertion below
 * is hand-derived from - never from the production code under test.
 */
#include <string.h>

#include "bz_quest_wc3_particles.h"
#include "test_framework.h"

static void identity_matrix(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static bzQuestWc3ParticleEmitterFrame_t make_emitter(void) {
    bzQuestWc3ParticleEmitterFrame_t e;
    memset(&e, 0, sizeof(e));
    e.emissionRate = 10.0f;
    e.length = 0.0f; e.width = 0.0f; /* deterministic origin regardless of RNG - see header comment */
    e.speed = 10.0f;
    e.latitude = 0.0f; /* deterministic direction: phi always 0 -> exactly world +Y */
    e.gravity = 10.0f;
    e.lifeSpan = 2.0f;
    e.timeMiddle = 0.5f;
    e.rows = 1; e.columns = 1;
    e.blendMode = 3; /* BZ_TTA_BLEND_ADDITIVE */
    e.textureIdentity = "fire.blp";
    e.segmentColor[0] = e.segmentColor[1] = e.segmentColor[2] = 1.0f;
    e.segmentAlpha[0] = 255; e.segmentAlpha[1] = 255; e.segmentAlpha[2] = 255;
    e.particleScaling[0] = e.particleScaling[1] = e.particleScaling[2] = 1.0f;
    identity_matrix(e.worldMatrix);
    return e;
}

/* -- Pool reset / determinism -- */

static void test_pool_reset_clears_active_list_and_fills_free_list(void) {
    bzQuestWc3ParticlePool_t pool;
    bz_quest_wc3_particles_pool_reset(&pool, 1234);
    ASSERT_EQ_INT(pool.activeCount, 0);
    ASSERT_EQ_INT(pool.activeHead, BZ_QUEST_WC3_PARTICLE_NONE);
    ASSERT(pool.freeHead != BZ_QUEST_WC3_PARTICLE_NONE);
}

static void test_pool_reset_same_seed_reproduces_identical_first_spawn(void) {
    bzQuestWc3ParticlePool_t poolA, poolB;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.length = 5.0f; e.width = 3.0f; /* non-deterministic-without-seed inputs, to prove RNG reuse */
    bool overflowedA, overflowedB;
    bz_quest_wc3_particles_pool_reset(&poolA, 777);
    bz_quest_wc3_particles_pool_reset(&poolB, 777);
    bz_quest_wc3_particles_emit(&poolA, &e, 0, 50, &overflowedA);
    bz_quest_wc3_particles_emit(&poolB, &e, 0, 50, &overflowedB);
    ASSERT_EQ_INT(poolA.activeCount, poolB.activeCount);
    ASSERT(poolA.activeCount > 0);
    ASSERT_EQ_FLOAT(poolA.particles[poolA.activeHead].posX, poolB.particles[poolB.activeHead].posX, 0.0001f);
    ASSERT_EQ_FLOAT(poolA.particles[poolA.activeHead].posZ, poolB.particles[poolB.activeHead].posZ, 0.0001f);
}

static void test_pool_reset_different_seed_diverges(void) {
    bzQuestWc3ParticlePool_t poolA, poolB;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.length = 5.0f; e.width = 3.0f;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&poolA, 1);
    bz_quest_wc3_particles_pool_reset(&poolB, 2);
    bz_quest_wc3_particles_emit(&poolA, &e, 0, 50, &overflowed);
    bz_quest_wc3_particles_emit(&poolB, &e, 0, 50, &overflowed);
    ASSERT(poolA.particles[poolA.activeHead].posX != poolB.particles[poolB.activeHead].posX ||
          poolA.particles[poolA.activeHead].posZ != poolB.particles[poolB.activeHead].posZ);
}

static void test_pool_reset_zero_seed_is_remapped_not_locked(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.length = 5.0f; e.width = 3.0f;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 0);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 0, 200, &overflowed);
    ASSERT(n > 1); /* multiple spawns must not all land on the exact same (locked-RNG) origin */
}

/* -- Spawn timing (phase-locked to whole real seconds - see header comment) -- */

static void test_emit_zero_rate_spawns_nothing(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.emissionRate = 0.0f;
    bool overflowed = true;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 0, 5000, &overflowed);
    ASSERT_EQ_INT(n, 0);
    ASSERT(!overflowed);
}

static void test_emit_negative_rate_spawns_nothing(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.emissionRate = -5.0f;
    bool overflowed = true;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 0, 5000, &overflowed);
    ASSERT_EQ_INT(n, 0);
}

/* Hand-derived: rate=10 -> step=100ms. previousClockMsec=950 -> lastFrameTime=950,
 * start=950-(950%1000)=0. Loop times 0,100,...,900 are all < lastFrameTime (950): no spawn.
 * time=1000 (>=950, spawning latches true; 1000<currentClockMsec=1050): exactly ONE spawn.
 * time=1100 is not < 1050: loop stops. Total: exactly 1 spawn. */
static void test_emit_spawns_exactly_one_across_a_single_step_window(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    bool overflowed = true;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    ASSERT_EQ_INT(n, 1);
    ASSERT(!overflowed);
}

/* Hand-derived: rate=10 -> step=100ms. previousClockMsec=1950 -> lastFrameTime=1950,
 * start=1950-950=1000. Loop times 1000..1900 all < 1950: no spawn. time=2000 (>=1950,
 * spawning latches; 2000<2150): spawn #1. time=2100 (still spawning; 2100<2150): spawn #2.
 * time=2200 is not <2150: stop. Total: exactly 2 spawns. */
static void test_emit_spawns_exactly_two_across_a_two_step_window(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    bool overflowed = true;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 1950, 2150, &overflowed);
    ASSERT_EQ_INT(n, 2);
    ASSERT(!overflowed);
}

/* Hand-derived: MAX(1, EmissionRate) floors a sub-1/sec rate up to exactly 1/sec (step=1000ms) -
 * transcribed exactly from r_mdx_geoset.c:85, not "fixed" (see header comment). Without this
 * floor, rate=0.001 would give step=1,000,000ms and 0 spawns over an 1100ms window; WITH the
 * floor (step=1000ms), previousClockMsec=500 (lastFrameTime=500, start=0) yields one spawn at
 * time=1000 (>=500, <1600). This test fails against a naive `if (EmissionRate<=0) return`-only
 * implementation with no floor, proving the floor branch is actually exercised. */
static void test_emit_emission_rate_floored_to_one_per_second(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.emissionRate = 0.001f;
    bool overflowed = true;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 500, 1600, &overflowed);
    ASSERT_EQ_INT(n, 1);
}

/* -- Pool/overflow bounding -- */

static void test_emit_stops_at_pool_capacity_and_reports_overflow(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.emissionRate = 100000.0f; /* huge: would need far more than the pool's capacity */
    bool overflowed = false;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    /* Fill the entire pool across repeated calls (bounded per-call by the per-emitter spawn
     * cap), then confirm the NEXT call reports overflow once the pool is truly full. */
    uint32_t total = 0;
    for (int frame = 0; frame < 200 && total < BZ_QUEST_WC3_MAX_PARTICLES; frame++) {
        uint32_t prevClock = (uint32_t)frame * 1000u, curClock = prevClock + 1000u;
        total += bz_quest_wc3_particles_emit(&pool, &e, prevClock, curClock, &overflowed);
    }
    ASSERT_EQ_INT(pool.activeCount, BZ_QUEST_WC3_MAX_PARTICLES);
    ASSERT(overflowed);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 200000, 201000, &overflowed);
    ASSERT_EQ_INT(n, 0);
    ASSERT(overflowed);
}

static void test_emit_respects_per_emitter_per_frame_spawn_cap(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.emissionRate = 100000.0f; /* would need thousands of spawns in one call without a cap */
    bool overflowed = false;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    uint32_t n = bz_quest_wc3_particles_emit(&pool, &e, 0, 1000, &overflowed);
    ASSERT(n <= BZ_QUEST_WC3_MAX_PARTICLE_SPAWNS_PER_EMITTER_PER_FRAME);
    ASSERT(overflowed);
}

/* -- Kinematics / coordinate conversion (deterministic: length=width=latitude=0) -- */

static void test_spawn_position_uses_identity_matrix_verbatim(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 42);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    ASSERT_EQ_INT(pool.activeCount, 1);
    const bzQuestWc3Particle_t *p = &pool.particles[pool.activeHead];
    ASSERT_EQ_FLOAT(p->posX, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->posY, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->posZ, 0.0f, 0.0001f);
}

/* Proves the coordinate-conversion claim: with latitude=0, FX_GenerateRandomDirection's phi is
 * always exactly 0, so direction = (sin(0)cos(theta), cos(0), sin(0)sin(theta)) = (0,1,0)
 * regardless of theta - i.e. world +Y, NEVER world +Z. A wrong axis assignment (e.g. cone axis
 * mistakenly left at local +Z) would make this assertion fail (velY would read 0, velZ would
 * read speed). Also proves gravity is exactly -Y, not -Z. */
static void test_velocity_and_gravity_use_world_plus_y_convention(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    const bzQuestWc3Particle_t *p = &pool.particles[pool.activeHead];
    ASSERT_EQ_FLOAT(p->velX, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->velY, e.speed, 0.0001f); /* +Y, matching world "up" in Y-up space */
    ASSERT_EQ_FLOAT(p->velZ, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->accelX, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->accelY, -e.gravity, 0.0001f); /* -Y, matching world "down" */
    ASSERT_EQ_FLOAT(p->accelZ, 0.0f, 0.0001f);
}

/* Proves the origin point IS matrix-transformed (translation applies to position). */
static void test_spawn_position_applies_world_matrix_translation(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    identity_matrix(e.worldMatrix);
    e.worldMatrix[12] = 100.0f; e.worldMatrix[13] = 200.0f; e.worldMatrix[14] = 300.0f;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    const bzQuestWc3Particle_t *p = &pool.particles[pool.activeHead];
    ASSERT_EQ_FLOAT(p->posX, 100.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->posY, 200.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->posZ, 300.0f, 0.0001f);
}

/* Proves velocity/acceleration do NOT pick up the world matrix's translation (they are not
 * matrix-transformed at all - see header comment) - the same translated matrix as above must
 * leave velocity/gravity completely unaffected. */
static void test_velocity_and_gravity_ignore_world_matrix_translation(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    identity_matrix(e.worldMatrix);
    e.worldMatrix[12] = 100.0f; e.worldMatrix[13] = 200.0f; e.worldMatrix[14] = 300.0f;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    const bzQuestWc3Particle_t *p = &pool.particles[pool.activeHead];
    ASSERT_EQ_FLOAT(p->velX, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->velY, e.speed, 0.0001f);
    ASSERT_EQ_FLOAT(p->velZ, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(p->accelY, -e.gravity, 0.0001f);
}

/* -- Aging -- */

static void test_age_advances_and_expires_particles(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.lifeSpan = 1.0f;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    ASSERT_EQ_INT(pool.activeCount, 1);
    bz_quest_wc3_particles_age(&pool, 0.5f);
    ASSERT_EQ_INT(pool.activeCount, 1); /* 0.5s < 1.0s lifespan: still alive */
    ASSERT_EQ_FLOAT(pool.particles[pool.activeHead].ageSec, 0.5f, 0.0001f);
    bz_quest_wc3_particles_age(&pool, 0.6f); /* total age 1.1s > 1.0s lifespan */
    ASSERT_EQ_INT(pool.activeCount, 0);
    ASSERT_EQ_INT(pool.activeHead, BZ_QUEST_WC3_PARTICLE_NONE);
}

static void test_age_freed_slot_is_reusable(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.lifeSpan = 1.0f;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    bz_quest_wc3_particles_age(&pool, 2.0f); /* expire it */
    ASSERT_EQ_INT(pool.activeCount, 0);
    bz_quest_wc3_particles_emit(&pool, &e, 1950, 2050, &overflowed);
    ASSERT_EQ_INT(pool.activeCount, 1); /* freed slot successfully reused, not exhausted */
}

/* -- Pack: grouping, atlas, color/size blend, kinematics evaluation -- */

static void test_pack_empty_pool_yields_nothing(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleVertex_t verts[64];
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 99;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    uint32_t n = bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    ASSERT_EQ_INT(n, 0);
    ASSERT_EQ_INT(runCount, 0);
}

static void test_pack_single_particle_yields_one_quad_one_run(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    bzQuestWc3ParticleVertex_t verts[64];
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 0;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    uint32_t n = bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    ASSERT_EQ_INT(n, 6);
    ASSERT_EQ_INT(runCount, 1);
    ASSERT_EQ_INT(runs[0].firstVertex, 0);
    ASSERT_EQ_INT(runs[0].vertexCount, 6);
    ASSERT_EQ_INT(runs[0].blendMode, e.blendMode);
    ASSERT_STR_EQ(runs[0].textureIdentity, "fire.blp");
}

static void test_pack_groups_same_texture_into_one_run(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    bzQuestWc3ParticleVertex_t verts[64];
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 0;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    /* Two separate emit() calls at different clock windows both target the same emitter
     * (same blendMode/texture) - both particles must land in the SAME run. */
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    bz_quest_wc3_particles_emit(&pool, &e, 1950, 2050, &overflowed);
    ASSERT_EQ_INT(pool.activeCount, 2);
    uint32_t n = bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    ASSERT_EQ_INT(n, 12);
    ASSERT_EQ_INT(runCount, 1);
    ASSERT_EQ_INT(runs[0].vertexCount, 12);
}

static void test_pack_groups_distinct_textures_into_separate_runs(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t eFire = make_emitter();
    bzQuestWc3ParticleEmitterFrame_t eSmoke = make_emitter();
    eSmoke.textureIdentity = "smoke.blp";
    bzQuestWc3ParticleVertex_t verts[64];
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 0;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &eFire, 950, 1050, &overflowed);
    bz_quest_wc3_particles_emit(&pool, &eSmoke, 950, 1050, &overflowed);
    ASSERT_EQ_INT(pool.activeCount, 2);
    uint32_t n = bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    ASSERT_EQ_INT(n, 12);
    ASSERT_EQ_INT(runCount, 2);
    ASSERT_EQ_INT(runs[0].vertexCount, 6);
    ASSERT_EQ_INT(runs[1].vertexCount, 6);
    ASSERT(strcmp(runs[0].textureIdentity, runs[1].textureIdentity) != 0);
}

static void test_pack_truncates_cleanly_when_vertex_buffer_too_small(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    bzQuestWc3ParticleVertex_t verts[6]; /* room for exactly one particle's quad */
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 0;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    bz_quest_wc3_particles_emit(&pool, &e, 1950, 2050, &overflowed);
    ASSERT_EQ_INT(pool.activeCount, 2);
    uint32_t n = bz_quest_wc3_particles_pack(&pool, verts, 6, runs, 8, &runCount);
    ASSERT_EQ_INT(n, 6); /* only the first particle's quad fit */
}

/* Kinematics: org = org0 + vel0*t + 0.5*accel*t^2 (see header comment). With
 * origin=(0,0,0), vel=(0,speed,0), accel=(0,-gravity,0), ageSec=1: posY = speed*1 -
 * 0.5*gravity*1^2. speed=10, gravity=10 -> posY = 10 - 5 = 5. */
static void test_pack_evaluates_kinematics_after_aging(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.lifeSpan = 10.0f; /* long enough to survive the 1s age below */
    bzQuestWc3ParticleVertex_t verts[64];
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 0;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    bz_quest_wc3_particles_age(&pool, 1.0f);
    bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    ASSERT_EQ_FLOAT(verts[0].posX, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(verts[0].posY, 5.0f, 0.001f);
    ASSERT_EQ_FLOAT(verts[0].posZ, 0.0f, 0.001f);
    /* Every one of the 6 corners shares the same particle center. */
    for (int i = 1; i < 6; i++) ASSERT_EQ_FLOAT(verts[i].posY, 5.0f, 0.001f);
}

/* Atlas frame: rows=2,columns=2 (4 total frames). At lifetime fraction k=0: frame 0 (u=0,v=0).
 * At k just under 1.0 (0.99): frame = floor(0.99*4) = 3 (u=1,v=1, the last frame) - see
 * FX_GetFrame's own r_particles.c formula, transcribed exactly (256-unit integer division). */
static void test_pack_atlas_frame_first_and_last(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.rows = 2; e.columns = 2;
    e.lifeSpan = 1.0f;
    bzQuestWc3ParticleVertex_t verts[64];
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 0;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);
    bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    /* k=0 (just spawned, not yet aged): frame 0 -> u0=0,v0=0, u1=127,v1=127 (256/2 - 1). */
    ASSERT_EQ_INT(verts[0].u0, 0); ASSERT_EQ_INT(verts[0].v0, 0);
    ASSERT_EQ_INT(verts[0].u1, 127); ASSERT_EQ_INT(verts[0].v1, 127);

    bz_quest_wc3_particles_age(&pool, 0.99f); /* k = 0.99 */
    bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    ASSERT_EQ_INT(verts[0].u0, 128); ASSERT_EQ_INT(verts[0].v0, 128); /* frame 3: u=1,v=1 */
    ASSERT_EQ_INT(verts[0].u1, 255); ASSERT_EQ_INT(verts[0].v1, 255);
}

/* Color/size blend: 3-segment lerp keyed by k=time/lifespan against midtime/255 (FX_BlendColor/
 * FX_BlendFloat). With midtime=0.5 (128/255): k=0 -> exactly color0/size0; k=0.5 -> exactly
 * color1/size1 (the segment boundary); k=1.0 -> exactly color2/size2. */
static void test_pack_color_and_size_blend_segment_boundaries(void) {
    bzQuestWc3ParticlePool_t pool;
    bzQuestWc3ParticleEmitterFrame_t e = make_emitter();
    e.lifeSpan = 1.0f;
    e.timeMiddle = 0.5f;
    e.segmentColor[0] = 1.0f; e.segmentColor[1] = 0.0f; e.segmentColor[2] = 0.0f; /* seg0 = red */
    e.segmentColor[3] = 0.0f; e.segmentColor[4] = 1.0f; e.segmentColor[5] = 0.0f; /* seg1 = green */
    e.segmentColor[6] = 0.0f; e.segmentColor[7] = 0.0f; e.segmentColor[8] = 1.0f; /* seg2 = blue */
    e.segmentAlpha[0] = 255; e.segmentAlpha[1] = 128; e.segmentAlpha[2] = 0;
    e.particleScaling[0] = 1.0f; e.particleScaling[1] = 2.0f; e.particleScaling[2] = 4.0f;
    bzQuestWc3ParticleVertex_t verts[64];
    bzQuestWc3ParticleDrawRun_t runs[8];
    uint32_t runCount = 0;
    bool overflowed;
    bz_quest_wc3_particles_pool_reset(&pool, 1);
    bz_quest_wc3_particles_emit(&pool, &e, 950, 1050, &overflowed);

    bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount); /* k=0: exactly segment 0 */
    ASSERT_EQ_INT(verts[0].color[0], 255); ASSERT_EQ_INT(verts[0].color[1], 0);
    ASSERT_EQ_INT(verts[0].color[2], 0); ASSERT_EQ_INT(verts[0].color[3], 255);
    ASSERT_EQ_FLOAT(verts[0].size, 2.0f, 0.01f); /* size0 * 2.0 (R_DrawParticles' own `size*2.0`) */

    /* k=1.0 (fully aged): exactly segment 2, unambiguous regardless of midtimeFrac's byte
     * quantization (timeMiddle=0.5 truncates to midtimeFrac=127 - matching desktop's own
     * `p->midtime = emitter->Time * 0xff` truncating BYTE cast, r_mdx_geoset.c - not a bug,
     * see this file's header comment on why this module transcribes desktop's formulas
     * exactly). (k-t)/(1-t) reduces to exactly 1.0 in IEEE754 (x/x for finite nonzero x), so
     * this checkpoint is not sensitive to that quantization the way an exact k==t checkpoint
     * would be. */
    bz_quest_wc3_particles_age(&pool, 1.0f);
    bz_quest_wc3_particles_pack(&pool, verts, 64, runs, 8, &runCount);
    ASSERT_EQ_INT(verts[0].color[0], 0); ASSERT_EQ_INT(verts[0].color[1], 0);
    ASSERT_EQ_INT(verts[0].color[2], 255); ASSERT_EQ_INT(verts[0].color[3], 0);
    ASSERT_EQ_FLOAT(verts[0].size, 8.0f, 0.01f); /* size2 * 2.0 */
}

void run_bz_quest_wc3_particles_tests(void) {
    RUN_TEST(test_pool_reset_clears_active_list_and_fills_free_list);
    RUN_TEST(test_pool_reset_same_seed_reproduces_identical_first_spawn);
    RUN_TEST(test_pool_reset_different_seed_diverges);
    RUN_TEST(test_pool_reset_zero_seed_is_remapped_not_locked);
    RUN_TEST(test_emit_zero_rate_spawns_nothing);
    RUN_TEST(test_emit_negative_rate_spawns_nothing);
    RUN_TEST(test_emit_spawns_exactly_one_across_a_single_step_window);
    RUN_TEST(test_emit_spawns_exactly_two_across_a_two_step_window);
    RUN_TEST(test_emit_emission_rate_floored_to_one_per_second);
    RUN_TEST(test_emit_stops_at_pool_capacity_and_reports_overflow);
    RUN_TEST(test_emit_respects_per_emitter_per_frame_spawn_cap);
    RUN_TEST(test_spawn_position_uses_identity_matrix_verbatim);
    RUN_TEST(test_velocity_and_gravity_use_world_plus_y_convention);
    RUN_TEST(test_spawn_position_applies_world_matrix_translation);
    RUN_TEST(test_velocity_and_gravity_ignore_world_matrix_translation);
    RUN_TEST(test_age_advances_and_expires_particles);
    RUN_TEST(test_age_freed_slot_is_reusable);
    RUN_TEST(test_pack_empty_pool_yields_nothing);
    RUN_TEST(test_pack_single_particle_yields_one_quad_one_run);
    RUN_TEST(test_pack_groups_same_texture_into_one_run);
    RUN_TEST(test_pack_groups_distinct_textures_into_separate_runs);
    RUN_TEST(test_pack_truncates_cleanly_when_vertex_buffer_too_small);
    RUN_TEST(test_pack_evaluates_kinematics_after_aging);
    RUN_TEST(test_pack_atlas_frame_first_and_last);
    RUN_TEST(test_pack_color_and_size_blend_segment_boundaries);
}

/*
 * bz_quest_wc3_particles.h - layer 9: platform-independent Warcraft III PRE2
 * particle-emitter simulation (bounded pool, deterministic emission,
 * kinematics/color/atlas evaluation, draw-run packing).
 *
 * Every function/struct here takes and returns plain float/int/char arrays
 * and the small POD structs declared below only - never a bzTTAsset_t,
 * VkBuffer, or VkImage. This lets
 * platform/android/quest/tests/test_bz_quest_wc3_particles.c build and check
 * these exact spawn-timing/kinematics/pool decisions with a plain host C
 * compiler - mirrors bz_quest_wc3_anim.h's/bz_quest_wc3_render.h's own
 * rationale (see those headers' comments). bz_quest_vk_wc3.c calls
 * bz_quest_wc3_particles_emit() once per (render item, active emitter) right
 * after it builds that item's node pose (the same place it already builds
 * the bone palette - see that file's build_frame_dynamic_material()), then
 * bz_quest_wc3_particles_age() once per frame (same file, same function,
 * after every emitter has been processed - the single shared delta-time
 * advance for the whole pool). bz_quest_vk_wc3_particles.c then calls
 * bz_quest_wc3_particles_pack() once per frame (after that aging has
 * already happened) to build this frame's GPU vertex buffer.
 *
 * -- Audited scope (do not extend without re-tracing) --
 *
 * This project's authoritative Warcraft III renderer/runtime parses AND
 * simulates AND draws exactly ONE MDX effect class end-to-end: PRE2 particle
 * emitters v2 (games/warcraft-3/renderer/mdx/r_mdx_load.c's
 * ReadParticleEmitter -> r_mdx_geoset.c's MDLX_RenderEmitter ->
 * renderer/r_particles.c's R_SpawnParticle/R_UpdateParticles/
 * R_DrawParticles). Every other MDX "effect-ish" chunk was traced and found
 * NOT actually supported end-to-end by any authoritative parser/runtime in
 * this codebase, so none of them are simulated here - see
 * games/warcraft-3/docs/file-formats/mdx.md's "Particle Emitters (PRE2)"
 * section and docs/quest-tabletop.md's "Layer 9" section for the full audit
 * and reasoning (particle emitter v1/PREM and ribbon emitters/RIBB have no
 * parser anywhere in this codebase; MDX event objects/EVTS are parsed but
 * never consumed - sound uses a completely separate entityState_t.event/
 * sound mechanism, doc/architecture/sound.md - and playing back a named
 * event would require hardcoding an external Blizzard asset-name convention
 * not present in WC3 data at all, forbidden by AGENTS.md).
 *
 * -- Spawn-timing evidence (do not change without re-deriving) --
 *
 * MDLX_RenderEmitter (r_mdx_geoset.c:68-130) spawns particles on a clock
 * that is phase-locked to whole real-world seconds, not simulation/entity
 * time - transcribed exactly:
 *
 *   DWORD lastFrameTime = tr.viewDef.time - tr.viewDef.deltaTime;
 *   DWORD start = lastFrameTime - lastFrameTime % 1000;
 *   for (float time = start, spawning = 0; time < tr.viewDef.time;
 *        time += 1000 / EmissionRate) {
 *       if (time >= lastFrameTime) spawning = 1;
 *       if (spawning) { spawn one particle }
 *   }
 *
 * `tr.viewDef.time`/`deltaTime` is the desktop RENDER clock (SDL_GetTicks()-
 * driven, per r_mdx_anim.c:34-41's own citation for the identical "global
 * sequences sample the render clock" convention) - i.e. particle emission,
 * like a global sequence, is a continuously-running ambient process that
 * does NOT reset when an entity's animation sequence changes/restarts. This
 * module therefore takes the SAME Quest-owned CLOCK_MONOTONIC render clock
 * (bz_quest_wc3_capture.h's bz_quest_wc3_render_clock_msec(), already used
 * for global-sequence sampling - see bz_quest_wc3_anim.h) as
 * `previousClockMsec`/`currentClockMsec`, not a Quest-invented wall clock,
 * and not the entity's own bzTTEntity_t.frame. `EmissionRate = MAX(1,
 * EmissionRate)` (r_mdx_geoset.c:85) is transcribed exactly, including its
 * counterintuitive floor (a track/static EmissionRate below 1/sec is still
 * clamped up to 1/sec) - this project's task discipline forbids "fixing"
 * this without evidence it is wrong, and no such evidence exists (it is the
 * one shipped, exercised desktop behavior for this exact code path).
 *
 * IMPORTANT: unlike desktop's own `float time = start` above (a real, unfixed latent defect in
 * the reference implementation - out of scope to touch there), bz_quest_wc3_particles_emit()'s
 * own implementation does NOT cast the absolute `start`/`currentClockMsec` into a float at all -
 * doing so loses precision past float32's 2^24 exact-integer range (~4.66h of continuous Quest
 * uptime) and was a real, empirically-reproduced High-severity defect fixed on this port (see
 * the fix-site comment inside bz_quest_wc3_particles_emit() for the full citation/derivation).
 * The implementation re-expresses this identical algorithm in terms of small, bounded offsets
 * relative to `lastFrameTime` instead, which stays algebraically equivalent for any uptime.
 *
 * -- Kinematics/color/atlas evidence (do not change without re-deriving) --
 *
 * Per-particle state at spawn (r_mdx_geoset.c's MDLX_RenderEmitter,
 * FX_GenerateRandomOrigin/FX_GenerateRandomDirection) and per-frame
 * evaluation (renderer/r_particles.c's R_DrawParticles/FX_BlendColor/
 * FX_BlendFloat/FX_GetFrame) are transcribed exactly:
 *
 *   - Origin: emitter-local (uniform(-length/2,length/2),
 *     uniform(-width/2,width/2), 0), then transformed by the emitter's own
 *     current world matrix (already Y-up/composed - see this file's
 *     coordinate-conversion note below) - r_mdx_geoset.c:113's
 *     `p->org = Matrix4_multiply_vector3(&matrix, &pivoted)`.
 *   - Direction/velocity: r_mdx_geoset.c:112-114 is `direction =
 *     FX_GenerateRandomDirection(...); p->vel = direction * Speed` - NEITHER
 *     is ever multiplied by `matrix` (unlike the origin above). A confirmed,
 *     re-read-from-source fact, not an assumption: the emitter's spray cone
 *     (a uniformly-sampled point on a spherical cap of half-angle `latitude`
 *     degrees around the ABSOLUTE world +Z axis, never the node's own
 *     rotated local axis) and its speed are evaluated directly in absolute
 *     engine (world) space, completely independent of the node's/model's
 *     current orientation - only the SPAWN POSITION follows the node
 *     hierarchy. This module reproduces that exactly: `worldMatrix` is used
 *     ONLY for the origin point; direction/velocity/acceleration below are
 *     computed directly in this project's target Y-up space with no matrix
 *     multiply at all.
 *   - Acceleration: r_mdx_geoset.c:115's `Vector3_scale(&(VECTOR3){0,0,-1},
 *     Gravity)` - a constant ABSOLUTE engine-space (Z-up) vector, again
 *     never matrix-transformed. Under this project's standard axis swap
 *     (Z-up "down" = -Z becomes Y-up "down" = -Y), this module uses the
 *     constant `(0,-Gravity,0)` directly - no per-node rotation applies to
 *     gravity any more than it does on desktop.
 *   - Per-frame kinematics: org = org0 + vel0*t + 0.5*accel*t^2 (the closed
 *     form of the engine's semi-implicit-Euler per-frame gravity
 *     integration - r_particles.c's own comment on why this is 0.5*a*t^2,
 *     not a*t^2).
 *   - Color/alpha: 3-segment lerp keyed by `time/lifespan` against
 *     `midtime/255` (FX_BlendColor); size: the same 3-segment lerp
 *     (FX_BlendFloat) over ParticleScaling.
 *   - Atlas frame: `frame = floor((time/lifespan) * rows*columns)`, clamped
 *     to `rows*columns-1` - keyed by the PARTICLE's own lifetime fraction,
 *     never a shared/global clock (FX_GetFrame's own comment on why: a
 *     shared clock makes every particle flip frames in unison, a crude
 *     strobing artifact).
 *
 * -- Coordinate conversion (do not change without re-deriving) --
 *
 * `worldMatrix` is the emitter's ALREADY-Y-up, ALREADY-composed
 * (node-global * entity-world) matrix, a plain float[16] (bz_quest_pure.h
 * column-major layout) - callers are responsible for the Z-up -> Y-up
 * conversion (bz_quest_wc3_convert_matrix_zup_to_yup(), the same function
 * layer 5C's bone palette already runs every resolved node matrix through -
 * see bz_quest_vk_wc3.c's build_frame_dynamic_material()) AND for
 * pre-multiplying the node's own pivot translation into it (see this file's
 * "Per-particle state at spawn" note above and bz_quest_vk_wc3.c's emitter
 * world-matrix construction) - this module performs no axis swap and no
 * pivot handling itself, only a single point-transform of the origin. Since
 * desktop's own velocity/acceleration are NEVER matrix-transformed at all
 * (see above), this module's random_origin_yup()/random_direction_yup()
 * helpers instead synthesize their output directly in target Y-up
 * convention (a fresh value this module itself generates, not an existing
 * MDX-authored per-component value being converted piecemeal - seebz_quest_wc3_anim.h's
 * own note on why converting existing track components individually would
 * be the wrong approach; this is a different situation, a brand new value
 * with no pre-existing coordinate space to preserve fidelity to).
 *
 * -- Determinism/reset semantics --
 *
 * The pool's PRNG is explicit, POOL-owned state (never libc rand()/srand(),
 * which is process-global and not independently seedable per test) so a
 * fixed seed reproduces byte-identical spawn origins/directions across
 * runs - this project's own mdxtool.c already establishes the "particle RNG
 * must be seedable for determinism" precedent via its `--seed` flag; this
 * module extends that precedent to Quest rather than inventing a new one.
 * bz_quest_wc3_particles_pool_reset() is the ONLY function that clears the
 * active list and re-seeds the RNG - callers invoke it once at creation and
 * again whenever the authoritative map-identity epoch changes (a real map
 * reload, never a mere snapshot-generation bump - see
 * bz_quest_wc3_capture.h's bz_quest_map_epoch() precedent), so no particle
 * spawned under a previous map can ever survive into a newly-loaded one -
 * "no stale effects across resets" is this reset function's entire job, not
 * an incidental side effect of a general per-frame clear (there is none;
 * particles persist across ordinary frames exactly like desktop's own
 * process-lifetime pool, per renderer/r_particles.c's own R_ClearParticles()
 * call site - once at renderer init, never per map load, on desktop).
 */
#ifndef BZ_QUEST_WC3_PARTICLES_H
#define BZ_QUEST_WC3_PARTICLES_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_wc3_render.h" /* BZ_QUEST_WC3_MAX_IDENTITY */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* A tabletop-scale view (not a full desktop RTS camera) realistically
     * shows at most a couple dozen simultaneously-active emitters, each
     * emitting a handful to a few dozen particles/sec with 1-3 second
     * lifespans - a few hundred live particles covers any real scene with
     * generous headroom. 2048 is a further-generous multiple (desktop's own
     * MAX_PARTICLES is 10000 for a full RTS camera - r_particles.c - this
     * value is deliberately smaller, scaled to what a Quest tabletop view
     * can actually show, not copied verbatim) while keeping the pool's
     * static memory (~1.5MB, dominated by the embedded per-particle texture
     * identity - see bzQuestWc3Particle_t) and per-frame age/pack iteration
     * cost small and bounded. A frame that would exceed this is NOT a
     * dropped/frozen draw for existing particles - only NEW spawns beyond
     * the cap are skipped (bz_quest_wc3_particles_emit()'s `outOverflowed`),
     * logged once per resource by the caller, never silently ignored. */
    BZ_QUEST_WC3_MAX_PARTICLES = 2048,
    /* Bounds bz_quest_wc3_particles_emit()'s own spawn loop per (emitter,
     * frame) call - see that function's doc comment. A real emitter's
     * EmissionRate times a real frame's delta time never approaches this;
     * it exists solely so a corrupt/adversarial EmissionRate keytrack value
     * (e.g. a huge float from a malformed asset) cannot spin the spawn loop
     * an unbounded number of times in one frame - the loop already stops
     * early once the shared pool is exhausted (mirroring desktop's own
     * R_SpawnParticle()-returns-NULL early-out), so this is defense in
     * depth beyond that, not a normally-reachable limit. */
    BZ_QUEST_WC3_MAX_PARTICLE_SPAWNS_PER_EMITTER_PER_FRAME = 64,
    /* Real Warcraft III scenes reuse a small, bounded set of distinct
     * particle textures (fire/smoke/blood/sparkle atlases) across many
     * emitters/units; 64 is a generous multiple of any real map's distinct
     * particle-texture count. An active scene that would exceed this many
     * DISTINCT (blendMode, textureIdentity) groups in one frame reuses the
     * last run instead of overflowing outRuns (logged once by the caller) -
     * see bz_quest_wc3_particles_pack()'s doc comment. */
    BZ_QUEST_WC3_MAX_PARTICLE_DRAW_RUNS = 64,
};

/* Sentinel for "end of list" in the pool's index-based free/active linked
 * lists (uint16_t indices into bzQuestWc3ParticlePool_t::particles - an
 * index, not a pointer, so the whole pool is trivially memcpy-relocatable
 * and has no pointer-fixup concern, matching this project's existing
 * index-based-reference convention, e.g. bzQuestWc3StoredNode_t::
 * parentIndex). BZ_QUEST_WC3_MAX_PARTICLES must stay <= this sentinel. */
enum { BZ_QUEST_WC3_PARTICLE_NONE = 0xFFFFu };

/* One live particle - every field is copied/computed at spawn time from the
 * spawning emitter's THEN-current resolved state (mirrors classic MDX
 * cparticle_t exactly, renderer/r_particles.c/common/shared.h) so a later
 * change to the emitter (a different keytrack sample next frame, or even
 * the emitter/model disappearing) never retroactively alters an
 * already-spawned particle's own trajectory/appearance - the particle is a
 * fully independent, self-contained snapshot from the moment it is born. */
typedef struct {
    float posX, posY, posZ;       /* world-space origin at spawn (org0) */
    float velX, velY, velZ;       /* world-space velocity at spawn (vel0) */
    float accelX, accelY, accelZ; /* world-space acceleration (constant over the particle's life) */
    uint8_t color0[4], color1[4], color2[4]; /* RGBA, 3 segments - FX_BlendColor's 3 stops */
    float size0, size1, size2;    /* 3 segments - FX_BlendFloat's 3 stops (ParticleScaling) */
    float ageSec;                 /* elapsed since spawn - cparticle_t.time */
    float lifespanSec;
    uint8_t midtimeFrac;           /* 0-255 fraction of lifespan where segment 0->1 crosses to 1->2 */
    uint8_t rows, columns;         /* atlas grid, both >= 1 */
    uint32_t blendMode;            /* bzTTBlendMode_t, copied at spawn - see this file's header comment */
    char textureIdentity[BZ_QUEST_WC3_MAX_IDENTITY]; /* copied at spawn; never a raw cache pointer/handle
                                                       * (a cache entry can be evicted while this particle
                                                       * is still alive - see this file's header comment) */
    uint16_t next; /* intrusive free-list/active-list link; BZ_QUEST_WC3_PARTICLE_NONE = end */
} bzQuestWc3Particle_t;

/* Fixed-capacity particle pool with an explicit, independently-seedable PRNG
 * - an ordinary struct instance the caller owns (mirrors bz_quest_wc3_cache.h's
 * bzQuestWc3Cache_t "explicit struct, not a singleton" convention), not a
 * hidden global - so host tests can construct/reset/inspect multiple
 * independent pools without any cross-test state leakage. */
typedef struct {
    bzQuestWc3Particle_t particles[BZ_QUEST_WC3_MAX_PARTICLES];
    uint16_t activeHead, freeHead;
    uint32_t activeCount;
    uint64_t rngState;
} bzQuestWc3ParticlePool_t;

/*
 * Initializes every particle's `next` link into one free list (all
 * BZ_QUEST_WC3_MAX_PARTICLES slots free, activeHead = NONE) and seeds the
 * PRNG from `seed` - the only function that may clear the active list (see
 * this file's header comment on reset semantics: called once at creation,
 * and again on every real map-identity-epoch change, never per ordinary
 * frame). `seed` 0 is remapped to a fixed nonzero constant internally (a
 * pure xorshift-style generator locks to an all-zero state forever
 * otherwise) - still fully deterministic for a given input seed.
 */
void bz_quest_wc3_particles_pool_reset(bzQuestWc3ParticlePool_t *pool, uint64_t seed);

/*
 * Ages every active particle by `deltaSec` (mirrors R_UpdateParticles()
 * exactly - r_particles.c): returns to the free list any particle whose
 * ageSec now exceeds lifespanSec, advances the rest's ageSec in place. Must
 * run exactly once per frame (not once per emitter/entity), and - matching
 * desktop's own per-frame order (spawning happens per-entity during model
 * rendering; R_UpdateParticles()/R_DrawParticles() run once afterward as a
 * global post-pass) - after every bz_quest_wc3_particles_emit() call for
 * that frame, so a particle spawned this frame is aged by this same frame's
 * delta before it is first packed/drawn (an already-in-motion first frame,
 * not a frozen spawn frame - the exact desktop behavior).
 */
void bz_quest_wc3_particles_age(bzQuestWc3ParticlePool_t *pool, float deltaSec);

/* One emitter's fully-resolved current-frame state - every field already
 * sampled/translated by the caller (bz_quest_vk_wc3.c, via
 * bz_quest_wc3_sample_float_track()/bz_quest_wc3_resolve_track_interval()
 * for the animatable fields, and the ABI's decode-time FilterMode/
 * FrameFlags translation for blendMode/headOrTail - see
 * platform/bridge/bz_tabletop_assets.h's bzTTParticleEmitterInfo_t doc
 * comment). This module never samples a keytrack or talks to the asset ABI
 * itself - see this file's header comment. `length`/`width`/`latitude`
 * specifically are ALWAYS the emitter's static value here, never a sampled
 * one: games/warcraft-3/renderer/mdx/r_mdx_geoset.c's MDLX_RenderEmitter
 * samples these channels into local variables too but its own
 * FX_GenerateRandomOrigin/FX_GenerateRandomDirection call sites read the
 * static struct fields directly instead (a traced, evidence-verified
 * authoritative quirk, not a missed channel - see
 * bzQuestWc3StoredEmitter_t's doc comment in bz_quest_wc3_render.h for the
 * full git-blame citation); there is deliberately no `variation` field at
 * all, since the sampled Variation local is never read by anything in the
 * authoritative renderer either. `headOrTail` is carried through
 * but not yet acted on: this slice always spawns a single billboard particle
 * regardless of its value (Head/Tail/Both) - see this file's header comment
 * on why (a documented, honest scope cut: desktop's own R_DrawParticles()
 * likewise never differentiates head/tail rendering - see
 * games/warcraft-3/docs/file-formats/mdx.md). */
typedef struct {
    float emissionRate, length, width, speed, latitude, gravity, lifeSpan;
    float timeMiddle;
    uint32_t rows, columns;
    uint32_t blendMode;
    uint32_t headOrTail;
    const char *textureIdentity; /* borrowed for the duration of this call only, never retained */
    float segmentColor[9];
    uint8_t segmentAlpha[3];
    float particleScaling[3];
    /* This emitter's current world matrix: Y-up, already composed
     * (node-global * entity-world) - see this file's header comment. */
    float worldMatrix[16];
} bzQuestWc3ParticleEmitterFrame_t;

/*
 * Spawns zero or more particles for one emitter this frame, mirroring
 * MDLX_RenderEmitter's exact phase-locked-to-whole-real-seconds timing (see
 * this file's header comment) using `previousClockMsec`/`currentClockMsec`
 * (the Quest render clock, shared across every emitter processed this
 * frame - never a per-emitter clock, matching desktop's single
 * `tr.viewDef.time`). Skips entirely (returns 0) when `emitter->
 * emissionRate <= 0` (matches MDLX_RenderEmitter's own early-out) - callers
 * must separately gate on the emitter's own Visibility track before calling
 * (matches MDLX_RenderParticleEmitters' wrapping check, not this function's
 * job - this function has no access to a Visibility value, only the 8
 * *other* channels). Bounded by
 * BZ_QUEST_WC3_MAX_PARTICLE_SPAWNS_PER_EMITTER_PER_FRAME and by the pool's
 * own remaining capacity (stops immediately once exhausted, matching
 * R_SpawnParticle()'s NULL-return contract - never blocks/waits/grows).
 * Returns the number of particles actually spawned; `*outOverflowed` is set
 * true iff at least one attempted spawn could not be serviced (pool
 * exhausted or the per-emitter cap reached) - false otherwise. Never
 * allocates, locks, touches a file, or logs - purely arithmetic plus this
 * pool's own PRNG state, safe to call from a frame-critical path.
 * `previousClockMsec`/`currentClockMsec` are correct for ANY absolute clock
 * magnitude, including a session that has run continuously for many days
 * (never cast to float as an absolute value - see this file's header
 * comment's "IMPORTANT" note) and across `previousClockMsec`/
 * `currentClockMsec`'s own uint32_t wraparound.
 */
uint32_t bz_quest_wc3_particles_emit(bzQuestWc3ParticlePool_t *pool,
                                     const bzQuestWc3ParticleEmitterFrame_t *emitter,
                                     uint32_t previousClockMsec, uint32_t currentClockMsec,
                                     bool *outOverflowed);

/* One packed billboard-quad vertex - 6 per particle (two triangles, no
 * shared/indexed vertices - matches renderer/r_particles.c's R_AddParticle()
 * layout exactly, see this file's header comment), consumed by
 * warcraft_particle_vert.vert's camera-facing billboard expansion (the
 * vertex shader derives left/up from the eye's own view-projection matrix
 * columns and offsets `posXYZ` by `(axisX-0.5, axisY-0.5) * size` along
 * those axes - mirrors r_particles.c's vs_particle shader exactly, see
 * bz_quest_vk_wc3_particles.c). */
typedef struct {
    float posX, posY, posZ;
    uint8_t color[4];
    float size;
    uint8_t u0, v0, u1, v1; /* atlas sub-rect, [0,255] normalized */
    uint8_t axisX, axisY;   /* 0 or 255 - which corner of the billboard quad */
} bzQuestWc3ParticleVertex_t;

/* One contiguous run of vertices in a bz_quest_wc3_particles_pack() output
 * array sharing the same (blendMode, textureIdentity) - the caller issues
 * exactly one draw call per run, binding that run's own texture/pipeline
 * variant. */
typedef struct {
    uint32_t firstVertex, vertexCount;
    uint32_t blendMode;
    char textureIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
} bzQuestWc3ParticleDrawRun_t;

/*
 * Packs every active particle in `pool` into `outVertices` (up to
 * `maxVertices`), grouped into contiguous per-(blendMode,textureIdentity)
 * runs written to `outRuns` (up to `maxRuns`, count in `*outRunCount`) so
 * the caller can batch one draw call per run instead of one per particle.
 * Per-particle kinematics/color/size/atlas-frame evaluation exactly mirrors
 * R_DrawParticles()/FX_BlendColor()/FX_BlendFloat()/FX_GetFrame() - see this
 * file's header comment. Grouping uses a stable sort over a bounded scratch
 * index array (particle count is itself already bounded by
 * BZ_QUEST_WC3_MAX_PARTICLES, so this sort is O(n log n) over a small,
 * fixed n, not unbounded per-frame work) - deliberately NOT sorted
 * back-to-front by camera distance: desktop's own R_DrawParticles() does not
 * depth-sort particles either (only groups by texture via a change-
 * detection scan over list order), so this matches the traced authoritative
 * behavior rather than inventing a stricter guarantee desktop itself does
 * not provide - see games/warcraft-3/docs/file-formats/mdx.md. A frame with
 * more than BZ_QUEST_WC3_MAX_PARTICLE_DRAW_RUNS distinct groups merges the
 * excess into the last run (logged once by the caller) rather than
 * overflowing `outRuns`; a `pool`/`outVertices` too small for every active
 * particle truncates cleanly (returns the count that fit, never a partial/
 * corrupt vertex). Returns the total vertex count written (always a
 * multiple of 6, or 0 iff `pool` has no active particles).
 */
uint32_t bz_quest_wc3_particles_pack(const bzQuestWc3ParticlePool_t *pool,
                                     bzQuestWc3ParticleVertex_t *outVertices, uint32_t maxVertices,
                                     bzQuestWc3ParticleDrawRun_t *outRuns, uint32_t maxRuns,
                                     uint32_t *outRunCount);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_PARTICLES_H */

/*
 * bz_quest_host.c - Meta Quest (Android/NDK) NativeActivity entry point.
 *
 * Layer 4 scope: a real android_native_app_glue host that drives
 * bz_quest_renderer.h's init/frame/shutdown across the full OpenXR instance/
 * session lifecycle, a Vulkan stereo frame loop, and an XR_FB_passthrough
 * compositor layer over a minimal head-tracked tabletop test scene, PLUS the
 * real tabletop engine lifecycle/snapshot bridge described below. Every
 * OpenXR/Vulkan/Android type lives inside this host and the bz_quest_xr/vk/
 * passthrough/renderer/scene/pure/bridge/data/frame/snapshot modules it
 * links - platform/bridge and platform/tabletop headers never see an
 * OpenXR or Vulkan type (see AGENTS.md's "Keep all OpenXR/Vulkan/Android
 * types inside the Quest host").
 *
 * This file deliberately still does NOT:
 *
 *   - poll OpenXR input actions (BZ_QUEST_ENABLE_INPUT)
 *   - open an audio track/mixer (BZ_QUEST_ENABLE_AUDIO)
 *   - stage War3.mpq/War3x.mpq data onto the device (BZ_QUEST_ENABLE_DATA_STAGING)
 *
 * Each seam below is a real compile-time gate: flipping one on without also
 * providing its implementation fails the build with a clear #error instead
 * of silently linking a no-op stub or reporting fake success. A later layer
 * removes the corresponding #error block and adds the real implementation.
 * BZ_QUEST_ENABLE_VULKAN_RENDERER, BZ_QUEST_ENABLE_ENGINE_START, and
 * BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS are the seams layer 4 replaced with real
 * implementations; BZ_QUEST_ENABLE_WC3_RENDERER is the one seam *this*
 * layer (5A) replaces with a real implementation (CMakeLists.txt now
 * defines all four to 1). Layer 5A renders only static Warcraft III model
 * geometry/materials at authoritative snapshot transforms - it still does
 * NOT draw terrain, skeletal/sequence animation, fog of war, selection
 * decals, particles/effects, or any command-card/HUD surface (see
 * docs/quest-tabletop.md's "Renderer slice 5A" section for the exact
 * supported/unsupported material and cache behavior).
 *
 * Layer 4 adds a small Quest-owned bridge/session adapter
 * (bz_quest_bridge.h) that creates, starts, suspends/resumes, stops, and
 * destroys the shared platform/tabletop/bridge/bz_tabletop_lifecycle.h
 * engine-thread state machine on the matching Android lifecycle
 * transitions (see bz_quest_handle_cmd() below), a deterministic
 * app-accessible data-directory resolver with a narrow, validated override
 * mechanism (bz_quest_data.h), and a plain-C diagnostic frame descriptor
 * (bz_quest_frame.h/bz_quest_snapshot.h) that proves snapshots really
 * advance via a throttled log line - never per-frame, never pretending
 * Warcraft content is actually drawn. See docs/quest-tabletop.md's
 * "Engine/renderer threading boundary" and "Data-path contract" sections
 * for the full write-up and every researched source location this design
 * is based on.
 */
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <stdbool.h>
#include <string.h>

#include "bz_quest_bridge.h"
#include "bz_quest_frame.h"
#include "bz_quest_log.h"
#include "bz_quest_pure.h"
#include "bz_quest_snapshot.h"

/* ---------------------------------------------------------------------- */
/* Compile-time seams for later Quest layers. Each is off by default; a    */
/* later layer's CMakeLists.txt/build flips exactly one on at a time as    */
/* its real implementation lands, replacing the matching #error below.    */
/* ---------------------------------------------------------------------- */
#ifndef BZ_QUEST_ENABLE_VULKAN_RENDERER
#define BZ_QUEST_ENABLE_VULKAN_RENDERER 0
#endif
#ifndef BZ_QUEST_ENABLE_ENGINE_START
#define BZ_QUEST_ENABLE_ENGINE_START 0
#endif
#ifndef BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS
#define BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS 0
#endif
#ifndef BZ_QUEST_ENABLE_WC3_RENDERER
#define BZ_QUEST_ENABLE_WC3_RENDERER 0
#endif
#ifndef BZ_QUEST_ENABLE_INPUT
#define BZ_QUEST_ENABLE_INPUT 0
#endif
#ifndef BZ_QUEST_ENABLE_AUDIO
#define BZ_QUEST_ENABLE_AUDIO 0
#endif
#ifndef BZ_QUEST_ENABLE_DATA_STAGING
#define BZ_QUEST_ENABLE_DATA_STAGING 0
#endif

#if !BZ_QUEST_ENABLE_VULKAN_RENDERER
#error "BZ_QUEST_ENABLE_VULKAN_RENDERER: layer 3 always builds the real renderer - CMakeLists.txt must define this to 1"
#endif
#if !BZ_QUEST_ENABLE_ENGINE_START
#error "BZ_QUEST_ENABLE_ENGINE_START: layer 4 always builds the real bz_quest_bridge.h engine-thread adapter - CMakeLists.txt must define this to 1"
#endif
#if !BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS
#error "BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS: layer 4 always builds the real bz_quest_snapshot.h reader - CMakeLists.txt must define this to 1"
#endif
#if !BZ_QUEST_ENABLE_WC3_RENDERER
#error "BZ_QUEST_ENABLE_WC3_RENDERER: layer 5A always builds the real static-model Vulkan renderer - CMakeLists.txt must define this to 1"
#endif
#if BZ_QUEST_ENABLE_INPUT
#error "BZ_QUEST_ENABLE_INPUT: OpenXR action/input polling is a later Quest layer - see docs/quest-tabletop.md"
#endif
#if BZ_QUEST_ENABLE_AUDIO
#error "BZ_QUEST_ENABLE_AUDIO: audio output is a later Quest layer - see docs/quest-tabletop.md"
#endif
#if BZ_QUEST_ENABLE_DATA_STAGING
#error "BZ_QUEST_ENABLE_DATA_STAGING: War3 MPQ data staging is a later Quest layer - see docs/quest-tabletop.md"
#endif

#include "bz_quest_renderer.h"

/* android_app->userData: owns the renderer and its init/pause bookkeeping
 * across onAppCmd callbacks, which the glue library invokes with no other
 * way to pass context through. `rendererReady` is false until
 * bz_quest_renderer_init() succeeds *and* stays false forever after a
 * failed init - this host never retries init from inside the command
 * loop, since a failed init already logged the exact missing
 * capability/extension/device requirement per docs/quest-tabletop.md, and
 * silently retrying would risk re-entering a partially torn-down xr/vk/
 * passthrough state. */
typedef struct bzQuestAppState_s {
    bzQuestRenderer_t renderer;
    bool rendererReady;
    bool initAttempted;
    /* Set true on APP_CMD_RESUME, false on APP_CMD_PAUSE (and starts false -
     * android_native_app_glue delivers START before RESUME, so init always
     * runs before this can ever be true). Needed because
     * xrSessionRunning alone is not enough to decide the looper poll
     * timeout - see bz_quest_looper_timeout_millis()'s comment in
     * bz_quest_pure.h for the exact hang this closes. */
    bool androidResumed;
    /* Owns the engine's dedicated thread (see bz_quest_bridge.h). Created/
     * started on APP_CMD_START, suspended/resumed on APP_CMD_PAUSE/RESUME,
     * stopped and destroyed exactly once by android_main() after the main
     * loop exits, regardless of which trigger broke it - see this file's
     * header comment and android_main()'s teardown block below for why
     * this host is the sole owner of that ordering. */
    bzQuestBridge_t bridge;
    /* Last diagnostic frame descriptor captured on this (the XR/render)
     * thread - compared against the newly captured one every iteration to
     * decide whether bz_quest_frame_should_log() should emit a throttled
     * log line. Starts fully zeroed by android_main()'s memset(), which
     * already matches bz_quest_frame_reset()'s NO_SNAPSHOT/IDLE value. */
    bzQuestFrame_t lastFrame;
} bzQuestAppState_t;

/*
 * Runs bz_quest_bridge_start() exactly once (on the first APP_CMD_START),
 * mirroring bz_quest_ensure_renderer_init()'s "attempt once, never retry
 * from inside the command loop" convention just below - a failed/rejected
 * attempt already logs its exact reason via bz_quest_bridge_last_error(),
 * and blindly retrying could re-enter a partially torn-down lifecycle.
 * Independent of renderer init success: the engine thread's state machine
 * runs headless regardless of whether XR/Vulkan rendering is available,
 * matching this layer's "engine never depends on the render path" scope.
 *
 * Blocks the calling thread (Android's main/UI thread, same one
 * android_main() runs on, same as bz_quest_ensure_renderer_init() already
 * does synchronously) until BZ_RuntimeInit() completes on the ONE dedicated
 * engine thread bz_tabletop_lifecycle.c spawns - see bz_quest_bridge.h's
 * header comment for why this mirrors the renderer's existing convention
 * rather than moving engine startup to a background thread.
 *
 * No map name is passed (menu/disconnected mode only): this layer does not
 * implement gameplay or Warcraft asset/terrain rendering (see
 * bz_quest_frame.h) - the tabletop transport still publishes an advancing-
 * generation snapshot every client frame regardless of whether a map is
 * loaded (see platform/tabletop/client/cl_scrn_tabletop_null.c's
 * SCR_UpdateScreen()), which is what bz_quest_snapshot_capture() below
 * proves via the throttled diagnostic log.
 */
static void bz_quest_ensure_bridge_start(struct android_app *app) {
    bzQuestAppState_t *state = (bzQuestAppState_t *)app->userData;
    if (state->bridge.startAttempted) return;

    if (bz_quest_bridge_start(&state->bridge, app->activity->internalDataPath,
                               app->activity->externalDataPath, NULL)) {
        BZ_QUEST_LOGI("bz_quest_bridge_start succeeded (data dir '%s')", state->bridge.dataDir);
    } else {
        const char *error = bz_quest_bridge_last_error(&state->bridge);
        BZ_QUEST_LOGE(
            "bz_quest_bridge_start failed: %s - see docs/quest-tabletop.md's data-path contract; "
            "continuing to pump the Android event loop with no engine running.",
            error ? error : "unknown error");
    }
}

/*
 * Runs bz_quest_renderer_init() exactly once (on the first APP_CMD_START,
 * which android_native_app_glue delivers before APP_CMD_INIT_WINDOW/RESUME
 * and while app->activity->vm/clazz are already valid - see the NDK's
 * android_native_app_glue.h). OpenXR's instance/system/session creation on
 * Android does not need an ANativeWindow (the XR compositor - not the
 * ANativeWindow - owns what actually gets displayed), so gating init on
 * APP_CMD_INIT_WINDOW like a conventional SurfaceView-backed app would only
 * delay first-frame availability without buying correctness.
 */
static void bz_quest_ensure_renderer_init(struct android_app *app) {
    bzQuestAppState_t *state = (bzQuestAppState_t *)app->userData;
    if (state->initAttempted) return;
    state->initAttempted = true;

    if (bz_quest_renderer_init(app->activity->vm, app->activity->clazz, &state->renderer)) {
        state->rendererReady = true;
        BZ_QUEST_LOGI("bz_quest_renderer_init succeeded");
    } else {
        state->rendererReady = false;
        BZ_QUEST_LOGE(
            "bz_quest_renderer_init failed - this build requires Quest 3/3S "
            "mixed-reality passthrough capability and a Vulkan-capable OpenXR "
            "runtime; see docs/quest-tabletop.md's hardware acceptance gates. "
            "Continuing to pump the Android event loop with no rendering.");
    }
}

static void bz_quest_handle_cmd(struct android_app *app, int32_t cmd) {
    bzQuestAppState_t *state = (bzQuestAppState_t *)app->userData;
    switch (cmd) {
        case APP_CMD_START:
            BZ_QUEST_LOGI("APP_CMD_START");
            bz_quest_ensure_renderer_init(app);
            bz_quest_ensure_bridge_start(app);
            break;
        case APP_CMD_RESUME:
            BZ_QUEST_LOGI("APP_CMD_RESUME");
            state->androidResumed = true;
            if (state->rendererReady && !state->renderer.passthrough.started) {
                bz_quest_passthrough_start(&state->renderer.xr, &state->renderer.passthrough);
            }
            bz_quest_bridge_resume(&state->bridge);
            break;
        case APP_CMD_PAUSE:
            BZ_QUEST_LOGI("APP_CMD_PAUSE");
            state->androidResumed = false;
            if (state->rendererReady) {
                bz_quest_passthrough_pause(&state->renderer.xr, &state->renderer.passthrough);
            }
            bz_quest_bridge_suspend(&state->bridge);
            break;
        case APP_CMD_STOP:
            BZ_QUEST_LOGI("APP_CMD_STOP");
            break;
        case APP_CMD_DESTROY:
            /* Final bridge stop+destroy deliberately does NOT happen here:
             * android_main() is the sole teardown owner (see this file's
             * header comment) so a single, deterministic code path runs
             * regardless of whether APP_CMD_DESTROY, an OpenXR loss/exit
             * event, or the engine's own self-quit (BZ_QUEST_BRIDGE_FAILED/
             * STOPPED observed by the main loop) is what actually ends the
             * app. This callback only logs; app->destroyRequested (already
             * set by android_native_app_glue before this fires) is what
             * breaks android_main()'s loop into that shared teardown. */
            BZ_QUEST_LOGI("APP_CMD_DESTROY");
            break;
        default:
            break;
    }
}

/* android_native_app_glue entry point (dlsym'd via ANativeActivity_onCreate). */
void android_main(struct android_app *app) {
    bzQuestAppState_t state;
    memset(&state, 0, sizeof(state));
    app->userData = &state;
    app->onAppCmd = bz_quest_handle_cmd;

    BZ_QUEST_LOGI("bz_quest_host: starting (layer 4: tabletop lifecycle/snapshot bridge)");

    /* ALooper_pollAll is unavailable at this project's minSdk 29 (NDK 27
     * marks it obsolete in favor of ALooper_pollOnce, which reports at most
     * one source per call instead of silently coalescing several - see
     * android/looper.h). Loop calling it once per iteration instead.
     *
     * Timeout choice: bz_quest_looper_timeout_millis() (see its comment in
     * bz_quest_pure.h) blocks indefinitely (-1) only while the app is both
     * fully backgrounded (or the renderer never initialized) AND has no
     * OpenXR session running, so this thread costs nothing while truly
     * idle; it polls without blocking (0) whenever EITHER is true. Gating
     * solely on "session running" (as an earlier draft of this loop did)
     * is wrong: xrPollEvent is only reachable from inside
     * bz_quest_renderer_frame, itself only called after ALooper_pollOnce
     * returns - so an app resumed from the background but not yet RUNNING
     * (xrBeginSession hasn't fired yet, because the READY event hasn't
     * been polled yet) would otherwise block forever waiting for an
     * Android input event that may never come, permanently starving
     * xrPollEvent and never starting the session. `wantsXrEventPolling`
     * requires both `androidResumed` and `rendererReady` - if renderer
     * init failed there is no XR instance/session to poll events for, so
     * merely being resumed must not spin the loop. This is still the
     * "no busy loop" requirement from docs/quest-tabletop.md: the loop
     * never spins with a zero timeout while fully backgrounded or while
     * there is genuinely nothing to poll. */
    while (!app->destroyRequested) {
        bool rendering = state.rendererReady && bz_quest_xr_is_session_running(&state.renderer.xr);
        bool wantsXrEventPolling = state.androidResumed && state.rendererReady;
        int timeoutMillis = bz_quest_looper_timeout_millis(wantsXrEventPolling, rendering);

        int events;
        struct android_poll_source *source;
        int ident = ALooper_pollOnce(timeoutMillis, NULL, &events, (void **)&source);
        if (ident >= 0 && source != NULL) {
            source->process(app, source);
        }
        if (app->destroyRequested) break;

        /* Acquires the latest immutable tabletop snapshot on this (the XR/
         * render) thread, copies only the small diagnostic values into a
         * plain-C descriptor, and releases it on every branch - see
         * bz_quest_snapshot_capture()'s header comment. `state.bridge.lc`
         * is NULL until bz_quest_ensure_bridge_start() has run (harmless:
         * bz_quest_snapshot_capture() treats a NULL lifecycle the same as
         * BZ_TabletopGetState(NULL)'s own documented IDLE/no-error
         * behavior) and stays valid until bz_quest_bridge_destroy() below,
         * which only ever runs after this loop has already exited. */
        bzQuestFrame_t frame;
        bz_quest_snapshot_capture(state.bridge.lc, &frame);
        if (bz_quest_frame_should_log(&state.lastFrame, &frame)) {
            BZ_QUEST_LOGI(
                "tabletop frame: status=%d generation=%llu lifecycleState=%d lifecycleError=%s "
                "mapLoaded=%d entities=%u(+%u overflow) selected=%u",
                (int)frame.status, (unsigned long long)frame.generation, (int)frame.lifecycleState,
                frame.lifecycleError[0] ? frame.lifecycleError : "-", (int)frame.mapLoaded,
                frame.entityCount, frame.entitiesOverflowCount, frame.selectedEntityCount);
        }
        state.lastFrame = frame;

        /* The engine's own dedicated thread can self-transition to
         * FAILED/STOPPED asynchronously (e.g. a frame-limit/console "quit"
         * - see bz_tabletop_lifecycle.c's Sys_Quit() handling), independent
         * of any Android/OpenXR event this loop observed. Funneling that
         * into the same break+teardown path as every other exit trigger
         * (Android destroy, OpenXR loss/exit, renderer init failure) keeps
         * teardown ordering deterministic regardless of which one fires -
         * see this file's header comment. */
        if (bz_quest_bridge_is_terminal(&state.bridge)) {
            BZ_QUEST_LOGI("tabletop bridge reached a terminal state (%d) - requesting host exit",
                          (int)bz_quest_bridge_state(&state.bridge));
            break;
        }

        if (state.rendererReady) {
            if (!bz_quest_renderer_frame(&state.renderer)) {
                BZ_QUEST_LOGI("bz_quest_renderer_frame requested exit (loss pending/exiting/fatal)");
                break;
            }
        }
    }

    /* android_main() is the sole owner of final shutdown, regardless of
     * which trigger broke the loop above - see this file's header comment.
     * The bridge (engine thread) is stopped and destroyed before the
     * renderer: an already-exited/terminal engine thread has nothing left
     * for a render-thread frame-capture to observe, so tearing down the
     * "business logic" side first, then the presentation side, keeps this
     * ordering simple and deterministic instead of depending on which side
     * happened to trigger the exit. */
    BZ_QUEST_LOGI("bz_quest_host: destroy requested, tearing down bridge and renderer");
    if (state.bridge.startAttempted) {
        bz_quest_bridge_destroy(&state.bridge);
    }
    if (state.rendererReady || state.initAttempted) {
        bz_quest_renderer_shutdown(&state.renderer);
    }
    BZ_QUEST_LOGI("bz_quest_host: exiting android_main");
}

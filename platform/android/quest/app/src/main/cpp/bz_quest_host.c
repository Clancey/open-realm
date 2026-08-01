/*
 * bz_quest_host.c - Meta Quest (Android/NDK) NativeActivity entry point.
 *
 * Layer 3 scope: a real android_native_app_glue host that drives
 * bz_quest_renderer.h's init/frame/shutdown across the full OpenXR instance/
 * session lifecycle, a Vulkan stereo frame loop, and an XR_FB_passthrough
 * compositor layer over a minimal head-tracked tabletop test scene. Every
 * OpenXR/Vulkan/Android type lives inside this host and the bz_quest_xr/vk/
 * passthrough/renderer/scene/pure modules it links - platform/bridge and
 * platform/tabletop headers never see an OpenXR or Vulkan type (see
 * AGENTS.md's "Keep all OpenXR/Vulkan/Android types inside the Quest host").
 *
 * This file deliberately still does NOT:
 *
 *   - call BZ_TabletopStart() to run the engine thread (BZ_QUEST_ENABLE_ENGINE_START)
 *   - read bz_tabletop_transport.h snapshots (BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS)
 *   - poll OpenXR input actions (BZ_QUEST_ENABLE_INPUT)
 *   - open an audio track/mixer (BZ_QUEST_ENABLE_AUDIO)
 *   - stage War3.mpq/War3x.mpq data onto the device (BZ_QUEST_ENABLE_DATA_STAGING)
 *
 * Each seam below is a real compile-time gate: flipping one on without also
 * providing its implementation fails the build with a clear #error instead
 * of silently linking a no-op stub or reporting fake success. A later layer
 * removes the corresponding #error block and adds the real implementation.
 * BZ_QUEST_ENABLE_VULKAN_RENDERER is the one seam *this* layer replaces with
 * a real implementation (CMakeLists.txt now defines it to 1).
 */
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <stdbool.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"
#include "platform/tabletop/bridge/bz_tabletop_lifecycle.h"

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
#if BZ_QUEST_ENABLE_ENGINE_START
#error "BZ_QUEST_ENABLE_ENGINE_START: engine-thread bootstrap (BZ_TabletopStart) is a later Quest layer - see docs/quest-tabletop.md"
#endif
#if BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS
#error "BZ_QUEST_ENABLE_BRIDGE_SNAPSHOTS: bz_tabletop_transport.h snapshot consumption is a later Quest layer - see docs/quest-tabletop.md"
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
} bzQuestAppState_t;

/*
 * Constructs and immediately destroys a bzTabletopLifecycle_t without ever
 * calling BZ_TabletopStart() - deliberately proving the statically linked
 * openwarcraft3-engine/-game/-assets/-jass/-sheet/-shared/-bridge archives
 * resolve every symbol (Sys_Quit() included, see bz_tabletop_lifecycle.c's
 * header comment) without running BZ_RuntimeInit(), which needs real WC3
 * data files this layer does not stage yet (BZ_QUEST_ENABLE_DATA_STAGING).
 * Mirrors platform/apple/visionos/tabletop/bridge/smoke/bz_tabletop_link_smoke.mm's
 * create/destroy-only link proof exactly. Unchanged from layer 2.
 */
static void bz_quest_probe_tabletop_lifecycle(void) {
    const char *argv[] = { "openwarcraft3-quest" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(1, argv);
    if (!lc) {
        BZ_QUEST_LOGE("BZ_TabletopCreate failed");
        return;
    }
    BZ_QUEST_LOGI("BZ_TabletopCreate/Destroy link proof OK (state=%d, not started)",
                  (int)BZ_TabletopGetState(lc));
    BZ_TabletopDestroy(lc);
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
            break;
        case APP_CMD_RESUME:
            BZ_QUEST_LOGI("APP_CMD_RESUME");
            state->androidResumed = true;
            if (state->rendererReady && !state->renderer.passthrough.started) {
                bz_quest_passthrough_start(&state->renderer.xr, &state->renderer.passthrough);
            }
            break;
        case APP_CMD_PAUSE:
            BZ_QUEST_LOGI("APP_CMD_PAUSE");
            state->androidResumed = false;
            if (state->rendererReady) {
                bz_quest_passthrough_pause(&state->renderer.xr, &state->renderer.passthrough);
            }
            break;
        case APP_CMD_STOP:
            BZ_QUEST_LOGI("APP_CMD_STOP");
            break;
        case APP_CMD_DESTROY:
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

    BZ_QUEST_LOGI("bz_quest_host: starting (layer 3: OpenXR/Vulkan/passthrough renderer)");
    bz_quest_probe_tabletop_lifecycle();

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

        if (state.rendererReady) {
            if (!bz_quest_renderer_frame(&state.renderer)) {
                BZ_QUEST_LOGI("bz_quest_renderer_frame requested exit (loss pending/exiting/fatal)");
                break;
            }
        }
    }

    BZ_QUEST_LOGI("bz_quest_host: destroy requested, tearing down renderer");
    if (state.rendererReady || state.initAttempted) {
        bz_quest_renderer_shutdown(&state.renderer);
    }
    BZ_QUEST_LOGI("bz_quest_host: exiting android_main");
}

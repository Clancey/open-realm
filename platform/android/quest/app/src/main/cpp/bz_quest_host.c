/*
 * bz_quest_host.c - Meta Quest (Android/NDK) NativeActivity entry point.
 *
 * Layer 2 scope only: correctly initialize the Khronos OpenXR Android
 * loader (XR_KHR_loader_init_android) and prove an XrInstance can be
 * created (XR_KHR_android_create_instance), then prove the portable
 * tabletop lifecycle host (platform/tabletop/bridge/bz_tabletop_lifecycle.c,
 * shared verbatim with visionOS - see docs/quest-tabletop.md) links and can
 * be constructed/destroyed. This file deliberately does NOT:
 *
 *   - create a Vulkan device/session/swapchain (BZ_QUEST_ENABLE_VULKAN_RENDERER)
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
 */
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <stdbool.h>
#include <string.h>

#define XR_USE_PLATFORM_ANDROID 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "platform/tabletop/bridge/bz_tabletop_lifecycle.h"

#define BZ_QUEST_LOG_TAG "OpenRealmQuest"
#define BZ_QUEST_LOGI(...) __android_log_print(ANDROID_LOG_INFO, BZ_QUEST_LOG_TAG, __VA_ARGS__)
#define BZ_QUEST_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, BZ_QUEST_LOG_TAG, __VA_ARGS__)

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

#if BZ_QUEST_ENABLE_VULKAN_RENDERER
#error "BZ_QUEST_ENABLE_VULKAN_RENDERER: Vulkan swapchain/renderer is a later Quest layer, not implemented here - see docs/quest-tabletop.md"
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

/*
 * Resolves and calls xrInitializeLoaderKHR, the mandatory first OpenXR call
 * on Android (XR_KHR_loader_init_android) - it hands the loader the JavaVM
 * and Activity Context it needs to locate the on-device OpenXR runtime
 * broker. Must run before any other OpenXR call, including
 * xrEnumerateInstanceExtensionProperties/xrCreateInstance. Mirrors the
 * Khronos OpenXR-SDK-Source hello_xr Android sample's android_main()
 * (src/tests/hello_xr/main.cpp) exactly - see docs/quest-tabletop.md.
 */
static bool bz_quest_init_openxr_loader(struct android_app *app) {
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = NULL;
    XrResult result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&xrInitializeLoaderKHR);
    if (result != XR_SUCCESS || xrInitializeLoaderKHR == NULL) {
        BZ_QUEST_LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed: %d", (int)result);
        return false;
    }

    XrLoaderInitInfoAndroidKHR loaderInitInfoAndroid;
    memset(&loaderInitInfoAndroid, 0, sizeof(loaderInitInfoAndroid));
    loaderInitInfoAndroid.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
    loaderInitInfoAndroid.applicationVM = app->activity->vm;
    loaderInitInfoAndroid.applicationContext = app->activity->clazz;

    result = xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInitInfoAndroid);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrInitializeLoaderKHR failed: %d", (int)result);
        return false;
    }
    BZ_QUEST_LOGI("xrInitializeLoaderKHR succeeded");
    return true;
}

/*
 * Creates (and immediately destroys) a minimal XrInstance with the
 * XR_KHR_android_create_instance extension to prove the loader can actually
 * reach a runtime end to end, not just that it linked. Without a Quest
 * runtime present (e.g. running this on a non-Quest device/emulator),
 * xrCreateInstance is expected to fail - that failure is logged explicitly
 * and treated as non-fatal to the lifecycle shell below, never silently
 * treated as success. Physical-device verification is deferred (see
 * docs/quest-tabletop.md's "Verified vs must-recheck" section).
 */
static void bz_quest_probe_openxr_instance(struct android_app *app) {
    XrInstanceCreateInfoAndroidKHR androidCreateInfo;
    memset(&androidCreateInfo, 0, sizeof(androidCreateInfo));
    androidCreateInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidCreateInfo.applicationVM = app->activity->vm;
    androidCreateInfo.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.next = &androidCreateInfo;
    createInfo.enabledExtensionCount = 1;
    const char *extensions[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME };
    createInfo.enabledExtensionNames = extensions;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strncpy(createInfo.applicationInfo.applicationName, "OpenRealmQuest",
            sizeof(createInfo.applicationInfo.applicationName) - 1);
    strncpy(createInfo.applicationInfo.engineName, "OpenRealmEngine",
            sizeof(createInfo.applicationInfo.engineName) - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult result = xrCreateInstance(&createInfo, &instance);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE(
            "xrCreateInstance failed: %d (expected off-device/without a Quest "
            "OpenXR runtime installed - see docs/quest-tabletop.md)",
            (int)result);
        return;
    }

    XrInstanceProperties props;
    memset(&props, 0, sizeof(props));
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (xrGetInstanceProperties(instance, &props) == XR_SUCCESS) {
        BZ_QUEST_LOGI("xrCreateInstance succeeded: runtime=%s version=%u.%u.%u",
                       props.runtimeName, (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
                       (unsigned)XR_VERSION_MINOR(props.runtimeVersion),
                       (unsigned)XR_VERSION_PATCH(props.runtimeVersion));
    } else {
        BZ_QUEST_LOGI("xrCreateInstance succeeded");
    }
    xrDestroyInstance(instance);
}

/*
 * Constructs and immediately destroys a bzTabletopLifecycle_t without ever
 * calling BZ_TabletopStart() - deliberately proving the statically linked
 * openwarcraft3-engine/-game/-assets/-jass/-sheet/-shared/-bridge archives
 * resolve every symbol (Sys_Quit() included, see bz_tabletop_lifecycle.c's
 * header comment) without running BZ_RuntimeInit(), which needs real WC3
 * data files this layer does not stage yet (BZ_QUEST_ENABLE_DATA_STAGING).
 * Mirrors platform/apple/visionos/tabletop/bridge/smoke/bz_tabletop_link_smoke.mm's
 * create/destroy-only link proof exactly.
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

static void bz_quest_handle_cmd(struct android_app *app, int32_t cmd) {
    (void)app;
    switch (cmd) {
        case APP_CMD_START:   BZ_QUEST_LOGI("APP_CMD_START");   break;
        case APP_CMD_RESUME:  BZ_QUEST_LOGI("APP_CMD_RESUME");  break;
        case APP_CMD_PAUSE:   BZ_QUEST_LOGI("APP_CMD_PAUSE");   break;
        case APP_CMD_STOP:    BZ_QUEST_LOGI("APP_CMD_STOP");    break;
        case APP_CMD_DESTROY: BZ_QUEST_LOGI("APP_CMD_DESTROY"); break;
        default: break;
    }
}

/* android_native_app_glue entry point (dlsym'd via ANativeActivity_onCreate). */
void android_main(struct android_app *app) {
    app->onAppCmd = bz_quest_handle_cmd;

    BZ_QUEST_LOGI("bz_quest_host: starting (layer 2: loader init + lifecycle link shell only)");

    if (bz_quest_init_openxr_loader(app)) {
        bz_quest_probe_openxr_instance(app);
    }
    bz_quest_probe_tabletop_lifecycle();

    /* ALooper_pollAll is unavailable at this project's minSdk 29 (NDK 27
     * marks it obsolete in favor of ALooper_pollOnce, which reports at most
     * one source per call instead of silently coalescing several - see
     * android/looper.h). Loop calling it once per iteration instead. */
    int events;
    struct android_poll_source *source;
    while (true) {
        int ident = ALooper_pollOnce(app->destroyRequested ? 0 : -1, NULL, &events, (void **)&source);
        if (ident >= 0) {
            if (source != NULL) {
                source->process(app, source);
            }
        }
        if (app->destroyRequested) {
            BZ_QUEST_LOGI("bz_quest_host: destroy requested, exiting android_main");
            return;
        }
    }
}

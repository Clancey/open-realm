/*
 * bz_quest_xr.c - see bz_quest_xr.h.
 */
#include "bz_quest_xr.h"

#include <stdlib.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"

/* The three instance extensions this layer requires and enables - no more,
 * no less. XR_KHR_android_create_instance is mandatory on Android;
 * XR_KHR_vulkan_enable2 is this renderer's only supported graphics API;
 * XR_FB_passthrough is a hard startup requirement per this layer's Quest
 * 3/3S MR scope (see docs/quest-tabletop.md), not optional. */
static const char *const BZ_QUEST_REQUIRED_INSTANCE_EXTENSIONS[] = {
    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
    XR_FB_PASSTHROUGH_EXTENSION_NAME,
};
#define BZ_QUEST_REQUIRED_INSTANCE_EXTENSION_COUNT \
    (sizeof(BZ_QUEST_REQUIRED_INSTANCE_EXTENSIONS) / sizeof(BZ_QUEST_REQUIRED_INSTANCE_EXTENSIONS[0]))

/* Defensive cap on XR_TIMEOUT_EXPIRED retries in bz_quest_xr_wait_swapchain_image()
 * below. XR_TIMEOUT_EXPIRED should never actually occur there since it always
 * passes XR_INFINITE_DURATION, but the OpenXR spec ("Rendering" chapter,
 * xrWaitSwapchainImage) permits a runtime to return it regardless and
 * requires the app to retry the wait on the same image rather than release
 * or abandon it - this bounds that retry loop so a non-conformant runtime
 * can't hang the frame loop forever, matching the same spec section's "must
 * not block indefinitely" requirement on the runtime side. */
#define BZ_QUEST_SWAPCHAIN_WAIT_MAX_RETRIES 64

bool bz_quest_xr_init_loader(void *vm, void *context) {
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
    loaderInitInfoAndroid.applicationVM = vm;
    loaderInitInfoAndroid.applicationContext = context;

    result = xrInitializeLoaderKHR((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInitInfoAndroid);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrInitializeLoaderKHR failed: %d", (int)result);
        return false;
    }
    BZ_QUEST_LOGI("xrInitializeLoaderKHR succeeded");
    return true;
}

bool bz_quest_xr_create_instance(void *vm, void *context, bzQuestXr_t *xr) {
    memset(xr, 0, sizeof(*xr));
    xr->sessionState = XR_SESSION_STATE_UNKNOWN;

    uint32_t availableCount = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(NULL, 0, &availableCount, NULL);
    if (result != XR_SUCCESS || availableCount == 0) {
        BZ_QUEST_LOGE("xrEnumerateInstanceExtensionProperties(count) failed: %d", (int)result);
        return false;
    }
    XrExtensionProperties *available = calloc(availableCount, sizeof(XrExtensionProperties));
    if (!available) {
        BZ_QUEST_LOGE("out of memory enumerating %u instance extensions", availableCount);
        return false;
    }
    for (uint32_t i = 0; i < availableCount; i++) available[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    result = xrEnumerateInstanceExtensionProperties(NULL, availableCount, &availableCount, available);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrEnumerateInstanceExtensionProperties(list) failed: %d", (int)result);
        free(available);
        return false;
    }

    const char **availableNames = calloc(availableCount, sizeof(char *));
    if (!availableNames) {
        BZ_QUEST_LOGE("out of memory building extension name array");
        free(available);
        return false;
    }
    for (uint32_t i = 0; i < availableCount; i++) availableNames[i] = available[i].extensionName;

    const char *missing = NULL;
    bool haveAll = bz_quest_check_required_names(availableNames, availableCount,
                                                  BZ_QUEST_REQUIRED_INSTANCE_EXTENSIONS,
                                                  BZ_QUEST_REQUIRED_INSTANCE_EXTENSION_COUNT, &missing);
    free(availableNames);
    free(available);
    if (!haveAll) {
        BZ_QUEST_LOGE("required OpenXR instance extension not supported by this runtime: %s",
                      missing ? missing : "(unknown)");
        return false;
    }

    XrInstanceCreateInfoAndroidKHR androidCreateInfo;
    memset(&androidCreateInfo, 0, sizeof(androidCreateInfo));
    androidCreateInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidCreateInfo.applicationVM = vm;
    androidCreateInfo.applicationActivity = context;

    XrInstanceCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.next = &androidCreateInfo;
    createInfo.enabledExtensionCount = BZ_QUEST_REQUIRED_INSTANCE_EXTENSION_COUNT;
    createInfo.enabledExtensionNames = BZ_QUEST_REQUIRED_INSTANCE_EXTENSIONS;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strncpy(createInfo.applicationInfo.applicationName, "OpenRealmQuest",
            sizeof(createInfo.applicationInfo.applicationName) - 1);
    strncpy(createInfo.applicationInfo.engineName, "OpenRealmEngine",
            sizeof(createInfo.applicationInfo.engineName) - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;

    result = xrCreateInstance(&createInfo, &xr->instance);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreateInstance failed: %d", (int)result);
        return false;
    }

    XrInstanceProperties props;
    memset(&props, 0, sizeof(props));
    props.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (xrGetInstanceProperties(xr->instance, &props) == XR_SUCCESS) {
        BZ_QUEST_LOGI("xrCreateInstance succeeded: runtime=%s version=%u.%u.%u", props.runtimeName,
                      (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
                      (unsigned)XR_VERSION_MINOR(props.runtimeVersion),
                      (unsigned)XR_VERSION_PATCH(props.runtimeVersion));
    }
    return true;
}

bool bz_quest_xr_get_system(bzQuestXr_t *xr) {
    XrSystemGetInfo getInfo;
    memset(&getInfo, 0, sizeof(getInfo));
    getInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrResult result = xrGetSystem(xr->instance, &getInfo, &xr->systemId);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrGetSystem(HEAD_MOUNTED_DISPLAY) failed: %d", (int)result);
        return false;
    }

    XrSystemProperties props;
    memset(&props, 0, sizeof(props));
    props.type = XR_TYPE_SYSTEM_PROPERTIES;
    XrSystemPassthroughProperties2FB passthroughProps;
    memset(&passthroughProps, 0, sizeof(passthroughProps));
    passthroughProps.type = XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB;
    props.next = &passthroughProps;
    result = xrGetSystemProperties(xr->instance, xr->systemId, &props);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrGetSystemProperties failed: %d", (int)result);
        return false;
    }
    xr->passthroughCapabilities = passthroughProps.capabilities;
    BZ_QUEST_LOGI("xrGetSystem succeeded: systemName=%s vendorId=%u passthroughCapabilities=0x%llx",
                  props.systemName, (unsigned)props.vendorId,
                  (unsigned long long)passthroughProps.capabilities);
    /* Quest 3/3S MR is a hard requirement for this prototype (see
     * docs/quest-tabletop.md) - a runtime/device without base passthrough
     * capability fails startup instead of silently rendering an opaque
     * scene. */
    if (!bz_quest_passthrough_capable(xr->passthroughCapabilities, XR_PASSTHROUGH_CAPABILITY_BIT_FB)) {
        BZ_QUEST_LOGE(
            "system lacks XR_PASSTHROUGH_CAPABILITY_BIT_FB (capabilities=0x%llx) - Quest 3/3S MR "
            "passthrough is a hard requirement for this layer, not an optional fallback",
            (unsigned long long)xr->passthroughCapabilities);
        return false;
    }
    return true;
}

/* Resolves one extension function pointer via xrGetInstanceProcAddr,
 * logging and returning false on any failure - shared by every entry in
 * bz_quest_xr_load_functions() below so a missing/renamed function fails
 * loudly with the exact name, not a generic "some function was NULL". */
static bool bz_quest_xr_resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *out) {
    XrResult result = xrGetInstanceProcAddr(instance, name, out);
    if (result != XR_SUCCESS || *out == NULL) {
        BZ_QUEST_LOGE("xrGetInstanceProcAddr(%s) failed: %d", name, (int)result);
        return false;
    }
    return true;
}

bool bz_quest_xr_load_functions(bzQuestXr_t *xr) {
    bool ok = true;
    ok &= bz_quest_xr_resolve(xr->instance, "xrGetVulkanGraphicsRequirements2KHR",
                              (PFN_xrVoidFunction *)&xr->fns.getVulkanGraphicsRequirements2KHR);
    ok &= bz_quest_xr_resolve(xr->instance, "xrCreateVulkanInstanceKHR",
                              (PFN_xrVoidFunction *)&xr->fns.createVulkanInstanceKHR);
    ok &= bz_quest_xr_resolve(xr->instance, "xrCreateVulkanDeviceKHR",
                              (PFN_xrVoidFunction *)&xr->fns.createVulkanDeviceKHR);
    ok &= bz_quest_xr_resolve(xr->instance, "xrGetVulkanGraphicsDevice2KHR",
                              (PFN_xrVoidFunction *)&xr->fns.getVulkanGraphicsDevice2KHR);
    ok &= bz_quest_xr_resolve(xr->instance, "xrCreatePassthroughFB",
                              (PFN_xrVoidFunction *)&xr->fns.createPassthroughFB);
    ok &= bz_quest_xr_resolve(xr->instance, "xrDestroyPassthroughFB",
                              (PFN_xrVoidFunction *)&xr->fns.destroyPassthroughFB);
    ok &= bz_quest_xr_resolve(xr->instance, "xrPassthroughStartFB",
                              (PFN_xrVoidFunction *)&xr->fns.passthroughStartFB);
    ok &= bz_quest_xr_resolve(xr->instance, "xrPassthroughPauseFB",
                              (PFN_xrVoidFunction *)&xr->fns.passthroughPauseFB);
    ok &= bz_quest_xr_resolve(xr->instance, "xrCreatePassthroughLayerFB",
                              (PFN_xrVoidFunction *)&xr->fns.createPassthroughLayerFB);
    ok &= bz_quest_xr_resolve(xr->instance, "xrDestroyPassthroughLayerFB",
                              (PFN_xrVoidFunction *)&xr->fns.destroyPassthroughLayerFB);
    return ok;
}

bool bz_quest_xr_get_vulkan_requirements(bzQuestXr_t *xr, XrGraphicsRequirementsVulkanKHR *outReq) {
    memset(outReq, 0, sizeof(*outReq));
    outReq->type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    XrResult result = xr->fns.getVulkanGraphicsRequirements2KHR(xr->instance, xr->systemId, outReq);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrGetVulkanGraphicsRequirements2KHR failed: %d", (int)result);
        return false;
    }
    BZ_QUEST_LOGI("Vulkan API version bound: min=%u.%u max=%u.%u",
                  (unsigned)XR_VERSION_MAJOR(outReq->minApiVersionSupported),
                  (unsigned)XR_VERSION_MINOR(outReq->minApiVersionSupported),
                  (unsigned)XR_VERSION_MAJOR(outReq->maxApiVersionSupported),
                  (unsigned)XR_VERSION_MINOR(outReq->maxApiVersionSupported));
    return true;
}

bool bz_quest_xr_select_blend_mode(bzQuestXr_t *xr) {
    uint32_t count = 0;
    XrResult result = xrEnumerateEnvironmentBlendModes(
        xr->instance, xr->systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, NULL);
    if (result != XR_SUCCESS || count == 0) {
        BZ_QUEST_LOGE("xrEnumerateEnvironmentBlendModes(count) failed: %d", (int)result);
        return false;
    }
    XrEnvironmentBlendMode *modes = calloc(count, sizeof(XrEnvironmentBlendMode));
    if (!modes) {
        BZ_QUEST_LOGE("out of memory enumerating %u blend modes", count);
        return false;
    }
    result = xrEnumerateEnvironmentBlendModes(xr->instance, xr->systemId,
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count,
                                              &count, modes);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrEnumerateEnvironmentBlendModes(list) failed: %d", (int)result);
        free(modes);
        return false;
    }
    bool foundAlphaBlend = false;
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
            foundAlphaBlend = true;
            break;
        }
    }
    free(modes);
    if (!foundAlphaBlend) {
        /* Explicit startup failure, not a fallback to OPAQUE/ADDITIVE - see
         * docs/quest-tabletop.md's "Quest 3/3S MR is required" contract. */
        BZ_QUEST_LOGE(
            "XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND not supported by this runtime/device - "
            "passthrough MR is a hard requirement for this layer, not an optional fallback");
        return false;
    }
    xr->blendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    return true;
}

bool bz_quest_xr_enumerate_views(bzQuestXr_t *xr) {
    uint32_t typeCount = 0;
    XrResult result = xrEnumerateViewConfigurations(xr->instance, xr->systemId, 0, &typeCount, NULL);
    if (result != XR_SUCCESS || typeCount == 0) {
        BZ_QUEST_LOGE("xrEnumerateViewConfigurations(count) failed: %d", (int)result);
        return false;
    }
    XrViewConfigurationType *types = calloc(typeCount, sizeof(XrViewConfigurationType));
    if (!types) {
        BZ_QUEST_LOGE("out of memory enumerating %u view configuration types", typeCount);
        return false;
    }
    result = xrEnumerateViewConfigurations(xr->instance, xr->systemId, typeCount, &typeCount, types);
    bool haveStereo = false;
    if (result == XR_SUCCESS) {
        for (uint32_t i = 0; i < typeCount; i++) {
            if (types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                haveStereo = true;
                break;
            }
        }
    }
    free(types);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrEnumerateViewConfigurations(list) failed: %d", (int)result);
        return false;
    }
    if (!haveStereo) {
        /* Every OpenXR HMD runtime must support PRIMARY_STEREO; this
         * renderer never falls back to mono/quad-view, so treat its
         * absence as a hard failure rather than trying another type. */
        BZ_QUEST_LOGE("XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO not supported by this runtime");
        return false;
    }

    uint32_t viewCount = 0;
    result = xrEnumerateViewConfigurationViews(xr->instance, xr->systemId,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                               &viewCount, NULL);
    if (result != XR_SUCCESS || viewCount != BZ_QUEST_VIEW_COUNT) {
        BZ_QUEST_LOGE(
            "xrEnumerateViewConfigurationViews(count) failed or returned unexpected view count: "
            "result=%d count=%u (expected %u)",
            (int)result, viewCount, BZ_QUEST_VIEW_COUNT);
        return false;
    }
    for (uint32_t i = 0; i < BZ_QUEST_VIEW_COUNT; i++) xr->viewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    result = xrEnumerateViewConfigurationViews(xr->instance, xr->systemId,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                               &viewCount, xr->viewConfigs);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrEnumerateViewConfigurationViews(list) failed: %d", (int)result);
        return false;
    }
    for (uint32_t i = 0; i < BZ_QUEST_VIEW_COUNT; i++) {
        BZ_QUEST_LOGI("view[%u]: recommended=%ux%u max=%ux%u recommendedSamples=%u", i,
                      xr->viewConfigs[i].recommendedImageRectWidth,
                      xr->viewConfigs[i].recommendedImageRectHeight,
                      xr->viewConfigs[i].maxImageRectWidth, xr->viewConfigs[i].maxImageRectHeight,
                      xr->viewConfigs[i].recommendedSwapchainSampleCount);
    }
    return true;
}

bool bz_quest_xr_create_session(bzQuestXr_t *xr, const XrGraphicsBindingVulkanKHR *binding) {
    XrSessionCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    createInfo.next = binding;
    createInfo.systemId = xr->systemId;

    XrResult result = xrCreateSession(xr->instance, &createInfo, &xr->session);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreateSession failed: %d", (int)result);
        return false;
    }
    BZ_QUEST_LOGI("xrCreateSession succeeded");
    return true;
}

bool bz_quest_xr_create_space(bzQuestXr_t *xr) {
    XrReferenceSpaceCreateInfo createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    createInfo.poseInReferenceSpace.orientation.w = 1.0f;

    XrResult result = xrCreateReferenceSpace(xr->session, &createInfo, &xr->appSpace);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreateReferenceSpace(LOCAL) failed: %d", (int)result);
        return false;
    }
    return true;
}

bool bz_quest_xr_create_swapchains(bzQuestXr_t *xr, const int64_t *preferredFormats,
                                   uint32_t preferredCount) {
    uint32_t formatCount = 0;
    XrResult result = xrEnumerateSwapchainFormats(xr->session, 0, &formatCount, NULL);
    if (result != XR_SUCCESS || formatCount == 0) {
        BZ_QUEST_LOGE("xrEnumerateSwapchainFormats(count) failed: %d", (int)result);
        return false;
    }
    int64_t *runtimeFormats = calloc(formatCount, sizeof(int64_t));
    if (!runtimeFormats) {
        BZ_QUEST_LOGE("out of memory enumerating %u swapchain formats", formatCount);
        return false;
    }
    result = xrEnumerateSwapchainFormats(xr->session, formatCount, &formatCount, runtimeFormats);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrEnumerateSwapchainFormats(list) failed: %d", (int)result);
        free(runtimeFormats);
        return false;
    }

    int64_t chosenFormat = 0;
    bool haveFormat = bz_quest_select_swapchain_format(runtimeFormats, formatCount, preferredFormats,
                                                        preferredCount, &chosenFormat);
    free(runtimeFormats);
    if (!haveFormat) {
        BZ_QUEST_LOGE("none of this renderer's preferred swapchain formats are runtime-supported");
        return false;
    }
    BZ_QUEST_LOGI("selected swapchain color format: %lld", (long long)chosenFormat);

    for (uint32_t i = 0; i < BZ_QUEST_VIEW_COUNT; i++) {
        bzQuestXrSwapchain_t *sc = &xr->swapchains[i];
        sc->format = chosenFormat;
        sc->width = xr->viewConfigs[i].recommendedImageRectWidth;
        sc->height = xr->viewConfigs[i].recommendedImageRectHeight;

        XrSwapchainCreateInfo createInfo;
        memset(&createInfo, 0, sizeof(createInfo));
        createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        createInfo.format = chosenFormat;
        /* sampleCount is always 1 (no MSAA) - see docs/quest-tabletop.md's
         * "Current limitations" for why this is an explicit, documented
         * seam rather than a claimed-but-untested optimization. */
        createInfo.sampleCount = 1;
        createInfo.width = sc->width;
        createInfo.height = sc->height;
        createInfo.faceCount = 1;
        createInfo.arraySize = 1;
        createInfo.mipCount = 1;

        result = xrCreateSwapchain(xr->session, &createInfo, &sc->handle);
        if (result != XR_SUCCESS) {
            BZ_QUEST_LOGE("xrCreateSwapchain[%u] failed: %d", i, (int)result);
            return false;
        }

        uint32_t imageCount = 0;
        result = xrEnumerateSwapchainImages(sc->handle, 0, &imageCount, NULL);
        if (result != XR_SUCCESS || imageCount == 0 || imageCount > BZ_QUEST_MAX_SWAPCHAIN_IMAGES) {
            BZ_QUEST_LOGE(
                "xrEnumerateSwapchainImages[%u](count) failed or out of range: result=%d count=%u "
                "(max %u)",
                i, (int)result, imageCount, BZ_QUEST_MAX_SWAPCHAIN_IMAGES);
            return false;
        }
        XrSwapchainImageVulkanKHR images[BZ_QUEST_MAX_SWAPCHAIN_IMAGES];
        memset(images, 0, sizeof(images));
        for (uint32_t j = 0; j < imageCount; j++) images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        result = xrEnumerateSwapchainImages(sc->handle, imageCount, &imageCount,
                                           (XrSwapchainImageBaseHeader *)images);
        if (result != XR_SUCCESS) {
            BZ_QUEST_LOGE("xrEnumerateSwapchainImages[%u](list) failed: %d", i, (int)result);
            return false;
        }
        sc->imageCount = imageCount;
        for (uint32_t j = 0; j < imageCount; j++) sc->images[j] = images[j].image;
        BZ_QUEST_LOGI("swapchain[%u]: %ux%u, %u images", i, sc->width, sc->height, imageCount);
    }
    return true;
}

/* Handles exactly one XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED event: the
 * one non-trivial branch in bz_quest_xr_poll_events()'s loop, split out so
 * each transition (READY/SYNCHRONIZED/STOPPING/LOSS_PENDING/EXITING) is one
 * unambiguous, individually readable case instead of buried inline. */
static void bz_quest_xr_handle_session_state_changed(bzQuestXr_t *xr,
                                                     const XrEventDataSessionStateChanged *event) {
    xr->sessionState = event->state;
    BZ_QUEST_LOGI("XrEventDataSessionStateChanged: state=%d", (int)event->state);

    switch (event->state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo;
            memset(&beginInfo, 0, sizeof(beginInfo));
            beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            XrResult result = xrBeginSession(xr->session, &beginInfo);
            if (result != XR_SUCCESS) {
                BZ_QUEST_LOGE("xrBeginSession failed: %d", (int)result);
                break;
            }
            xr->sessionRunning = true;
            BZ_QUEST_LOGI("xrBeginSession succeeded");
            break;
        }
        case XR_SESSION_STATE_STOPPING: {
            XrResult result = xrEndSession(xr->session);
            if (result != XR_SUCCESS) BZ_QUEST_LOGE("xrEndSession failed: %d", (int)result);
            xr->sessionRunning = false;
            break;
        }
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            xr->exitRequested = true;
            break;
        default:
            break;
    }
}

void bz_quest_xr_poll_events(bzQuestXr_t *xr) {
    for (;;) {
        XrEventDataBuffer event;
        memset(&event, 0, sizeof(event));
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult result = xrPollEvent(xr->instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) break;
        if (result != XR_SUCCESS) {
            BZ_QUEST_LOGE("xrPollEvent failed: %d", (int)result);
            break;
        }

        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                bz_quest_xr_handle_session_state_changed(
                    xr, (const XrEventDataSessionStateChanged *)&event);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                BZ_QUEST_LOGE("XrEventDataInstanceLossPending received");
                xr->exitRequested = true;
                break;
            case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
                const XrEventDataEventsLost *lost = (const XrEventDataEventsLost *)&event;
                BZ_QUEST_LOGE("XrEventDataEventsLost: lostEventCount=%u", lost->lostEventCount);
                break;
            }
            default:
                break;
        }
    }
}

bool bz_quest_xr_is_session_running(const bzQuestXr_t *xr) { return xr->sessionRunning; }

bool bz_quest_xr_wait_frame(bzQuestXr_t *xr, XrFrameState *outState) {
    XrFrameWaitInfo waitInfo;
    memset(&waitInfo, 0, sizeof(waitInfo));
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(outState, 0, sizeof(*outState));
    outState->type = XR_TYPE_FRAME_STATE;
    XrResult result = xrWaitFrame(xr->session, &waitInfo, outState);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrWaitFrame failed: %d", (int)result);
        return false;
    }
    return true;
}

bool bz_quest_xr_begin_frame(bzQuestXr_t *xr) {
    XrFrameBeginInfo beginInfo;
    memset(&beginInfo, 0, sizeof(beginInfo));
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    XrResult result = xrBeginFrame(xr->session, &beginInfo);
    if (result != XR_SUCCESS && result != XR_FRAME_DISCARDED) {
        BZ_QUEST_LOGE("xrBeginFrame failed: %d", (int)result);
        return false;
    }
    return true;
}

bool bz_quest_xr_locate_views(bzQuestXr_t *xr, XrTime displayTime, XrView outViews[BZ_QUEST_VIEW_COUNT]) {
    XrViewLocateInfo locateInfo;
    memset(&locateInfo, 0, sizeof(locateInfo));
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = displayTime;
    locateInfo.space = xr->appSpace;

    XrViewState viewState;
    memset(&viewState, 0, sizeof(viewState));
    viewState.type = XR_TYPE_VIEW_STATE;

    for (uint32_t i = 0; i < BZ_QUEST_VIEW_COUNT; i++) outViews[i].type = XR_TYPE_VIEW;
    uint32_t viewCount = 0;
    XrResult result = xrLocateViews(xr->session, &locateInfo, &viewState, BZ_QUEST_VIEW_COUNT,
                                    &viewCount, outViews);
    if (result != XR_SUCCESS || viewCount != BZ_QUEST_VIEW_COUNT) {
        BZ_QUEST_LOGE("xrLocateViews failed or returned unexpected count: result=%d count=%u",
                      (int)result, viewCount);
        return false;
    }
    if (!(viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
        !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        /* Tracking not yet valid (e.g. the very first frames after
         * xrBeginSession) - not a hard error, but the caller must not treat
         * outViews as usable this frame. */
        return false;
    }
    return true;
}

bool bz_quest_xr_end_frame(bzQuestXr_t *xr, XrTime displayTime,
                           const XrCompositionLayerBaseHeader *const *layers, uint32_t layerCount) {
    XrFrameEndInfo endInfo;
    memset(&endInfo, 0, sizeof(endInfo));
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = xr->blendMode;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    XrResult result = xrEndFrame(xr->session, &endInfo);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrEndFrame failed: %d", (int)result);
        return false;
    }
    return true;
}

/* Acquiring an image does not wait for it to be writable - the caller must
 * pair a successful acquire with bz_quest_xr_wait_swapchain_image() before
 * rendering, per the OpenXR spec's xrAcquireSwapchainImage description. On
 * failure, no image has been acquired, so the caller must not call
 * bz_quest_xr_wait_swapchain_image()/bz_quest_xr_release_swapchain_image()
 * for this attempt (bz_quest_renderer.c's frame loop already skips both on
 * a false return here). */
bool bz_quest_xr_acquire_swapchain_image(bzQuestXrSwapchain_t *swapchain, uint32_t *outIndex) {
    XrSwapchainImageAcquireInfo acquireInfo;
    memset(&acquireInfo, 0, sizeof(acquireInfo));
    acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    XrResult result = xrAcquireSwapchainImage(swapchain->handle, &acquireInfo, outIndex);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrAcquireSwapchainImage failed: %d", (int)result);
        return false;
    }
    return true;
}

/* XR_TIMEOUT_EXPIRED (value 1) is a *qualified success*, not a failure -
 * XR_SUCCEEDED(XR_TIMEOUT_EXPIRED) is true (openxr.h's XR_SUCCEEDED macro is
 * `(result) >= 0`). The OpenXR spec's "Rendering" chapter, xrWaitSwapchainImage
 * description, is explicit that on XR_TIMEOUT_EXPIRED "the next call to
 * xrWaitSwapchainImage will wait on the same image index again until the
 * function succeeds with XR_SUCCESS": the acquired image's ownership is
 * preserved, and the spec-correct response is to retry the wait, never to
 * treat it as a fatal error or call release/abandon the image. This function
 * always requests XR_INFINITE_DURATION, so a conformant runtime should not
 * return XR_TIMEOUT_EXPIRED here at all (there is no requested timeout to
 * expire against) - the retry loop below is a defensive fallback bounded by
 * BZ_QUEST_SWAPCHAIN_WAIT_MAX_RETRIES, not something normal operation should
 * ever exercise. A genuine failure (XR_FAILED(result), i.e. a negative
 * XrResult) means the image was never successfully waited on, so per the
 * spec's xrReleaseSwapchainImage precondition ("must have been successfully
 * waited on without timeout before it is released") the caller must not call
 * bz_quest_xr_release_swapchain_image() for this image - returning false here
 * signals exactly that, and bz_quest_renderer.c's frame loop already breaks
 * out of its per-eye loop without calling release when this returns false. */
bool bz_quest_xr_wait_swapchain_image(bzQuestXrSwapchain_t *swapchain) {
    for (int attempt = 0; attempt < BZ_QUEST_SWAPCHAIN_WAIT_MAX_RETRIES; attempt++) {
        XrSwapchainImageWaitInfo waitInfo;
        memset(&waitInfo, 0, sizeof(waitInfo));
        waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
        waitInfo.timeout = XR_INFINITE_DURATION;
        XrResult result = xrWaitSwapchainImage(swapchain->handle, &waitInfo);
        if (result == XR_SUCCESS) return true;
        if (result == XR_TIMEOUT_EXPIRED) continue; /* spec-mandated retry, same image, ownership preserved */
        BZ_QUEST_LOGE("xrWaitSwapchainImage failed: %d", (int)result);
        return false; /* terminal failure: caller must not release this image */
    }
    BZ_QUEST_LOGE("xrWaitSwapchainImage: exceeded %d XR_TIMEOUT_EXPIRED retries",
                   BZ_QUEST_SWAPCHAIN_WAIT_MAX_RETRIES);
    return false;
}

/* Preconditioned on a preceding *successful* (XR_SUCCESS, not
 * XR_TIMEOUT_EXPIRED) bz_quest_xr_wait_swapchain_image() call for this image -
 * see that function's comment and bz_quest_renderer.c's frame loop, which
 * only reaches this call after both acquire and wait returned true. */
bool bz_quest_xr_release_swapchain_image(bzQuestXrSwapchain_t *swapchain) {
    XrSwapchainImageReleaseInfo releaseInfo;
    memset(&releaseInfo, 0, sizeof(releaseInfo));
    releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    XrResult result = xrReleaseSwapchainImage(swapchain->handle, &releaseInfo);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrReleaseSwapchainImage failed: %d", (int)result);
        return false;
    }
    return true;
}

void bz_quest_xr_destroy(bzQuestXr_t *xr) {
    for (uint32_t i = 0; i < BZ_QUEST_VIEW_COUNT; i++) {
        if (xr->swapchains[i].handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(xr->swapchains[i].handle);
            xr->swapchains[i].handle = XR_NULL_HANDLE;
        }
    }
    if (xr->appSpace != XR_NULL_HANDLE) {
        xrDestroySpace(xr->appSpace);
        xr->appSpace = XR_NULL_HANDLE;
    }
    if (xr->session != XR_NULL_HANDLE) {
        xrDestroySession(xr->session);
        xr->session = XR_NULL_HANDLE;
    }
    if (xr->instance != XR_NULL_HANDLE) {
        xrDestroyInstance(xr->instance);
        xr->instance = XR_NULL_HANDLE;
    }
}

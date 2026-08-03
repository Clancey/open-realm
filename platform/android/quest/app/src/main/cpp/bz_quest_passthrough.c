/*
 * bz_quest_passthrough.c - see bz_quest_passthrough.h.
 */
#include "bz_quest_passthrough.h"

#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"

bool bz_quest_passthrough_create(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt) {
    memset(pt, 0, sizeof(*pt));

    if (!bz_quest_passthrough_capable(xr->passthroughCapabilities, XR_PASSTHROUGH_CAPABILITY_BIT_FB)) {
        BZ_QUEST_LOGE(
            "passthrough create: system lacks XR_PASSTHROUGH_CAPABILITY_BIT_FB "
            "(capabilities=0x%llx) - refusing to create an opaque-scene fallback",
            (unsigned long long)xr->passthroughCapabilities);
        return false;
    }

    XrPassthroughCreateInfoFB createInfo;
    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB;
    createInfo.flags = 0; /* not running at creation - bz_quest_passthrough_start() starts it explicitly */
    XrResult result = xr->fns.createPassthroughFB(xr->session, &createInfo, &pt->passthrough);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreatePassthroughFB failed: %d", (int)result);
        return false;
    }

    XrPassthroughLayerCreateInfoFB layerCreateInfo;
    memset(&layerCreateInfo, 0, sizeof(layerCreateInfo));
    layerCreateInfo.type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB;
    layerCreateInfo.passthrough = pt->passthrough;
    layerCreateInfo.flags = 0;
    layerCreateInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    result = xr->fns.createPassthroughLayerFB(xr->session, &layerCreateInfo, &pt->layer);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreatePassthroughLayerFB failed: %d", (int)result);
        xr->fns.destroyPassthroughFB(pt->passthrough);
        pt->passthrough = XR_NULL_HANDLE;
        return false;
    }

    BZ_QUEST_LOGI("passthrough object + reconstruction layer created");
    return true;
}

bool bz_quest_passthrough_start(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt) {
    if (pt->passthrough == XR_NULL_HANDLE) {
        BZ_QUEST_LOGE("passthrough_start: passthrough object not created");
        return false;
    }
    XrResult result = xr->fns.passthroughStartFB(pt->passthrough);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrPassthroughStartFB failed: %d", (int)result);
        return false;
    }
    pt->started = true;
    BZ_QUEST_LOGI("passthrough started");
    return true;
}

bool bz_quest_passthrough_pause(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt) {
    if (pt->passthrough == XR_NULL_HANDLE || !pt->started) return true; /* nothing to pause */
    XrResult result = xr->fns.passthroughPauseFB(pt->passthrough);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrPassthroughPauseFB failed: %d", (int)result);
        return false;
    }
    pt->started = false;
    BZ_QUEST_LOGI("passthrough paused");
    return true;
}

bool bz_quest_passthrough_build_layer(const bzQuestPassthrough_t *pt, XrSpace space,
                                      XrCompositionLayerPassthroughFB *outLayer) {
    if (pt->layer == XR_NULL_HANDLE) {
        BZ_QUEST_LOGE("passthrough_build_layer: layer not created");
        return false;
    }
    memset(outLayer, 0, sizeof(*outLayer));
    outLayer->type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
    outLayer->space = space;
    outLayer->layerHandle = pt->layer;
    return true;
}

void bz_quest_passthrough_destroy(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt) {
    if (pt->started) bz_quest_passthrough_pause(xr, pt);
    if (pt->layer != XR_NULL_HANDLE) {
        xr->fns.destroyPassthroughLayerFB(pt->layer);
        pt->layer = XR_NULL_HANDLE;
    }
    if (pt->passthrough != XR_NULL_HANDLE) {
        xr->fns.destroyPassthroughFB(pt->passthrough);
        pt->passthrough = XR_NULL_HANDLE;
    }
}

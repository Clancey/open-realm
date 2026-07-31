#ifndef R_M3_UTILS_H
#define R_M3_UTILS_H

#include "games/starcraft-2/common/sc2_m3.h"

#define BZ_M3_RENDERER_MAX_BONES 128

typedef struct {
    DWORD left, right;
    float fraction;
} m3KeySpan_t;

/* Desktop shaders and scratch matrices share one fixed 128-entry bone capability. */
static BOOL m3_renderer_model_supported(m3Model_t const *model) {
    if (!model || !model->head || !SC2_M3ValidateGeometry(model) ||
        model->bonesNum > BZ_M3_RENDERER_MAX_BONES)
        return false;
    /* The old bone-count-only check let a region base address uBones[128]. */
    FOR_LOOP(d, model->divisionsNum)
        FOR_LOOP(r, model->divisions[d].regionsNum)
            if ((DWORD)model->divisions[d].regions[r].firstBoneLookupIndex +
                model->divisions[d].regions[r].boneLookupIndicesCount > BZ_M3_RENDERER_MAX_BONES)
                return false;
    return true;
}

/* Exact/single keys select their authored value; only a time before the first key has no span. */
static BOOL m3_find_key_span(m3Model_t const *model, Reference keys, DWORD time, m3KeySpan_t *span) {
    m3ReferenceRead_t read = { .reference = keys, .element_size = sizeof(m3Uint32_t),
        .section_id = "_23I" };
    DWORD count, previous, current;
    if (!SC2_M3ReferenceCount(model, &read, &count) || !count ||
        !SC2_M3ReferenceElement(model, &read, &previous) || previous > time)
        return false;
    for (DWORD i = 1; i < count; i++) {
        read.element_index = i;
        if (!SC2_M3ReferenceElement(model, &read, &current) || current <= previous) return false;
        if (current > time) {
            *span = (m3KeySpan_t){ .left = i - 1, .right = i,
                .fraction = (float)(time - previous) / (float)(current - previous) };
            return true;
        }
        previous = current;
    }
    *span = (m3KeySpan_t){ .left = count - 1, .right = count - 1 };
    return true;
}

#endif

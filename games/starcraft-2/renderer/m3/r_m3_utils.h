#ifndef R_M3_UTILS_H
#define R_M3_UTILS_H

#include "games/starcraft-2/common/sc2_m3.h"

typedef struct {
    DWORD left, right;
    float fraction;
} m3KeySpan_t;

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

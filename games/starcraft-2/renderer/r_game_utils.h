#ifndef SC2_R_GAME_UTILS_H
#define SC2_R_GAME_UTILS_H

#include <stddef.h>
#include <string.h>

#include "common/shared.h"

/* Model dispatch must not dereference short or potentially unaligned provider buffers. */
static BOOL r_sc2_model_is_m3(void const *buffer, size_t size) {
    DWORD magic;
    if (!buffer || size < sizeof(magic)) return false;
    memcpy(&magic, buffer, sizeof(magic));
    return magic == ID_43DM;
}

#endif

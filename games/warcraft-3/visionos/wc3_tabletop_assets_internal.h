#ifndef WC3_TABLETOP_ASSETS_INTERNAL_H
#define WC3_TABLETOP_ASSETS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "platform/bridge/bz_tabletop_assets_internal.h"

/* Archive identities must remain relative and may not contain empty/traversal components. */
static inline bool wc3_tta_path_is_confined(const char *identity) {
    const char *segment, *end;
    if (!identity || !*identity || identity[0] == '/' || identity[0] == '\\')
        return false;
    for (const unsigned char *p = (const unsigned char *)identity; *p; p++)
        if (*p < 0x20 || *p == ':' || *p == 0x7f)
            return false;
    for (segment = identity;; segment = end + 1) {
        for (end = segment; *end && *end != '/' && *end != '\\'; end++);
        if (end == segment || (end - segment == 1 && segment[0] == '.') ||
            (end - segment == 2 && segment[0] == '.' && segment[1] == '.'))
            return false;
        if (!*end) return true;
        if (!end[1]) return false;
    }
}

bzTTAsset_t *BZ_WC3_TTA_DecodeBLP(const void *data, size_t size, const char *identity,
                                   const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status);
bzTTAsset_t *BZ_WC3_TTA_DecodeMDX(const void *data, size_t size, const char *identity,
                                   const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status);

#endif

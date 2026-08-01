/*
 * bz_quest_wc3_render.c - see bz_quest_wc3_render.h.
 */
#include "bz_quest_wc3_render.h"

#include <math.h>
#include <string.h>

#include "bz_quest_pure.h"

/* Per-category diorama scale multiplier - WarcraftRenderDescriptors.swift:
 * 375-391 (WarcraftCategoryScale.scale). Indexed by bzTTAssetCategory_t
 * (platform/bridge/bz_tabletop_assets.h): UNKNOWN=0, MOBILE=1, BUILDING=2,
 * RESOURCE=3, DOODAD=4, DESTRUCTABLE=5, ITEM=6. MOBILE and ITEM share the
 * 0.72 "unit/item" multiplier in the reviewed source. */
static float bz_quest_wc3_category_scale(uint32_t category) {
    switch (category) {
        case 2: return 1.0f;  /* BZ_TTA_CATEGORY_BUILDING */
        case 3: return 0.9f;  /* BZ_TTA_CATEGORY_RESOURCE */
        case 4: return 0.8f;  /* BZ_TTA_CATEGORY_DOODAD */
        case 5: return 0.86f; /* BZ_TTA_CATEGORY_DESTRUCTABLE */
        case 1: /* BZ_TTA_CATEGORY_MOBILE */
        case 6: /* BZ_TTA_CATEGORY_ITEM */
        case 0: /* BZ_TTA_CATEGORY_UNKNOWN */
        default: return 0.72f;
    }
}

void bz_quest_wc3_build_world_matrix(const bzQuestWc3EntityInput_t *entity, float outWorld[16]) {
    /* --- Position: engine Z-up -> target Y-up (Y/Z swap) --------------- */
    /* LiveTabletopTransport.swift:56 - see this file's header comment. */
    const float tx = entity->originX;
    const float ty = entity->originZ;
    const float tz = entity->originY;

    /* --- Scale: category multiplier * max(footprint, 0.25) for X/Z, bare -
     * category multiplier for Y, then the ".world" space scale-down
     * (min(x,2)*0.06, y*0.08, min(z,2)*0.06) - WarcraftRenderDescriptors.
     * swift:375-391 and WarcraftRenderMath.swift:498-514. See this file's
     * header comment for full citations. */
    const float categoryScale = bz_quest_wc3_category_scale(entity->category);
    const float footprintXZ = fmaxf(fmaxf(entity->footprintX, entity->footprintY), 0.25f);
    const float dioramaX = categoryScale * footprintXZ;
    const float dioramaY = categoryScale;
    const float dioramaZ = categoryScale * footprintXZ;
    const float sx = fminf(dioramaX, 2.0f) * 0.06f;
    const float sy = dioramaY * 0.08f;
    const float sz = fminf(dioramaZ, 2.0f) * 0.06f;

    /* --- Heading: negate engine yaw, rotate about the target Y axis ---- */
    /* TabletopAdapter.swift:54 (negation) + RealityTabletopView.swift:268
     * (rotation about [0,1,0]) - see this file's header comment. */
    const float theta = -entity->angle;
    const float c = cosf(theta), s = sinf(theta);

    float rotScale[16] = {
        c * sx,  0.0f,   -s * sx, 0.0f,
        0.0f,    sy,     0.0f,    0.0f,
        s * sz,  0.0f,   c * sz,  0.0f,
        0.0f,    0.0f,   0.0f,    1.0f,
    };
    /* rotScale above is R(theta) * S folded directly (avoids a temporary
     * matrix multiply for a rotation restricted to the Y axis): column 0 is
     * R's column 0 (c, 0, -s) scaled by sx, column 1 is R's column 1 (0,1,0)
     * scaled by sy, column 2 is R's column 2 (s, 0, c) scaled by sz. */
    memcpy(outWorld, rotScale, sizeof(rotScale));
    outWorld[12] = tx;
    outWorld[13] = ty;
    outWorld[14] = tz;
}

void bz_quest_wc3_build_render_list(const bzQuestWc3EntityInput_t *entities, uint32_t entityCount,
                                    bzQuestWc3RenderList_t *outList) {
    memset(outList, 0, sizeof(*outList));
    for (uint32_t i = 0; i < entityCount; i++) {
        const bzQuestWc3EntityInput_t *entity = &entities[i];
        if (entity->modelIdentity[0] == '\0') continue; /* no model resolved this frame */
        if (outList->count >= BZ_QUEST_WC3_MAX_RENDER_ITEMS) {
            outList->overflowCount++;
            continue;
        }
        bzQuestWc3RenderItem_t *item = &outList->items[outList->count++];
        memcpy(item->modelIdentity, entity->modelIdentity, sizeof(item->modelIdentity));
        bz_quest_wc3_build_world_matrix(entity, item->world);
    }
}

bool bz_quest_wc3_identity_equal(const char *a, const char *b) {
    return strncmp(a, b, BZ_QUEST_WC3_MAX_IDENTITY) == 0;
}

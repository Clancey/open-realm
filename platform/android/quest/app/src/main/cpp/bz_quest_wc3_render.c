/*
 * bz_quest_wc3_render.c - see bz_quest_wc3_render.h.
 */
#include "bz_quest_wc3_render.h"

#include <math.h>
#include <stdlib.h>
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

/* 1.08 - the same diorama-box target size bz_quest_wc3_terrain.c's
 * bz_quest_wc3_terrain_measure() has always used (WarcraftAssetAdapter.swift:
 * 560-719's terrain adapter target). Shared here so the world-position
 * transform and terrain's own scale can never drift apart. */
#define BZ_QUEST_WC3_WORLD_TARGET_SPAN 1.08f

bool bz_quest_wc3_world_transform_measure(float minX, float minZ, float maxX, float maxZ,
                                          bzQuestWc3WorldTransform_t *out) {
    if (!isfinite(minX) || !isfinite(minZ) || !isfinite(maxX) || !isfinite(maxZ)) return false;
    const float spanX = maxX - minX;
    const float spanZ = maxZ - minZ;
    if (!(spanX > 0.0f) || !(spanZ > 0.0f)) return false; /* also rejects NaN via !(x>0) */
    out->scale = BZ_QUEST_WC3_WORLD_TARGET_SPAN / fmaxf(spanX, spanZ);
    out->centerX = (minX + maxX) * 0.5f;
    out->centerZ = (minZ + maxZ) * 0.5f;
    return true;
}

void bz_quest_wc3_world_transform_point(const bzQuestWc3WorldTransform_t *transform, float x, float y,
                                        float z, float outXYZ[3]) {
    if (!transform) {
        outXYZ[0] = x;
        outXYZ[1] = y;
        outXYZ[2] = z;
        return;
    }
    outXYZ[0] = (x - transform->centerX) * transform->scale;
    outXYZ[1] = y * transform->scale;
    outXYZ[2] = (z - transform->centerZ) * transform->scale;
}

void bz_quest_wc3_entity_footprint_scale(uint32_t category, float footprintX, float footprintY,
                                         float *outScaleX, float *outScaleY, float *outScaleZ) {
    /* --- Scale: category multiplier * max(footprint, 0.25) INDEPENDENTLY
     * per axis (X uses footprintX/width, Z uses footprintY/depth - a
     * rectangular footprint must stay rectangular, never forced square by
     * sharing one max() of both), bare category multiplier for Y, then the
     * ".world" space scale-down (min(x,2)*0.06, y*0.08, min(z,2)*0.06) -
     * WarcraftRenderDescriptors.swift:375-391 and WarcraftRenderMath.swift:
     * 498-514. See this file's header comment for full citations. */
    const float categoryScale = bz_quest_wc3_category_scale(category);
    const float footprintXClamped = fmaxf(footprintX, 0.25f);
    const float footprintZClamped = fmaxf(footprintY, 0.25f);
    const float dioramaX = categoryScale * footprintXClamped;
    const float dioramaY = categoryScale;
    const float dioramaZ = categoryScale * footprintZClamped;
    *outScaleX = fminf(dioramaX, 2.0f) * 0.06f;
    *outScaleY = dioramaY * 0.08f;
    *outScaleZ = fminf(dioramaZ, 2.0f) * 0.06f;
}

void bz_quest_wc3_build_world_matrix(const bzQuestWc3EntityInput_t *entity,
                                     const bzQuestWc3WorldTransform_t *transform, float outWorld[16]) {
    /* --- Position: engine Z-up -> target Y-up (Y/Z swap), then the shared
     * world/tabletop transform (raw passthrough if transform is NULL) ----- */
    /* LiveTabletopTransport.swift:56 (axis swap) - see this file's header
     * comment for both the swap and the shared-transform citation. */
    float txyz[3];
    bz_quest_wc3_world_transform_point(transform, entity->originX, entity->originZ, entity->originY, txyz);

    float sx, sy, sz;
    bz_quest_wc3_entity_footprint_scale(entity->category, entity->footprintX, entity->footprintY, &sx, &sy, &sz);

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
    outWorld[12] = txyz[0];
    outWorld[13] = txyz[1];
    outWorld[14] = txyz[2];
}

void bz_quest_wc3_convert_matrix_zup_to_yup(const float inZup[16], float outYup[16]) {
    /* Same Y<->Z axis swap as the position/normal conversion above, expressed
     * as a column-major 4x4 with no translation component: maps (x,y,z,1)
     * -> (x,z,y,1). Symmetric and its own inverse (S*S = Identity) - see
     * this function's declaration comment. */
    static const float kAxisSwap[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    float tmp[16];
    bz_quest_mat4_multiply(kAxisSwap, inZup, tmp);
    bz_quest_mat4_multiply(tmp, kAxisSwap, outYup);
}

void bz_quest_wc3_build_render_list(const bzQuestWc3EntityInput_t *entities, uint32_t entityCount,
                                    const bzQuestWc3WorldTransform_t *transform,
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
        bz_quest_wc3_build_world_matrix(entity, transform, item->world);
        bz_quest_wc3_entity_footprint_scale(entity->category, entity->footprintX, entity->footprintY,
                                            &item->footprintScaleX, &item->footprintScaleY, &item->footprintScaleZ);
        item->tintR = entity->tintR;
        item->tintG = entity->tintG;
        item->tintB = entity->tintB;
        item->tintA = entity->tintA;
        item->frame = entity->frame;
        item->selected = entity->selected;
        memcpy(item->teamColorTextureIdentity, entity->teamColorTextureIdentity,
              sizeof(item->teamColorTextureIdentity));
        memcpy(item->teamGlowTextureIdentity, entity->teamGlowTextureIdentity,
              sizeof(item->teamGlowTextureIdentity));
    }
}

bool bz_quest_wc3_identity_equal(const char *a, const char *b) {
    return strncmp(a, b, BZ_QUEST_WC3_MAX_IDENTITY) == 0;
}

void bz_quest_wc3_model_anim_free(bzQuestWc3ModelAnim_t *anim) {
    if (!anim) return;
    free(anim->arena);
    free(anim);
}

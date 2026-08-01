/*
 * bz_quest_input_state.c - implementation of the layer 6 pure interaction
 * state machine / board transform / ray-hit priority / command mapping.
 * See bz_quest_input_state.h for the design contract and the authority /
 * module-boundary rules this file obeys (no OpenXR/Vulkan/engine/transport
 * types, no logging, no allocation - a plain host C compiler builds this).
 */
#include "bz_quest_input_state.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ board */

void bz_quest_board_transform_default(bzQuestBoardTransform_t *out) {
    out->tx = BZ_QUEST_BOARD_DEFAULT_TX;
    out->ty = BZ_QUEST_BOARD_DEFAULT_TY;
    out->tz = BZ_QUEST_BOARD_DEFAULT_TZ;
    out->yaw = 0.0f;
    out->scale = 1.0f;
}

static float bz_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void bz_quest_board_transform_clamp(bzQuestBoardTransform_t *bt) {
    bt->scale = bz_clampf(bt->scale, BZ_QUEST_BOARD_SCALE_MIN, BZ_QUEST_BOARD_SCALE_MAX);
    bt->ty = bz_clampf(bt->ty, BZ_QUEST_BOARD_TY_MIN, BZ_QUEST_BOARD_TY_MAX);
    bt->tx = bz_clampf(bt->tx, -BZ_QUEST_BOARD_TRANSLATE_LIMIT, BZ_QUEST_BOARD_TRANSLATE_LIMIT);
    bt->tz = bz_clampf(bt->tz, -BZ_QUEST_BOARD_TRANSLATE_LIMIT, BZ_QUEST_BOARD_TRANSLATE_LIMIT);
    /* Wrap yaw into (-pi, pi] so repeated rotation never grows unbounded. */
    while (bt->yaw > (float)M_PI) bt->yaw -= 2.0f * (float)M_PI;
    while (bt->yaw <= -(float)M_PI) bt->yaw += 2.0f * (float)M_PI;
}

/* Column-major T * Ryaw * S(uniform) - see header derivation. */
void bz_quest_board_transform_matrix(const bzQuestBoardTransform_t *bt, float out[16]) {
    const float c = cosf(bt->yaw), s = sinf(bt->yaw), k = bt->scale;
    out[0] = k * c;  out[1] = 0.0f; out[2] = -k * s; out[3] = 0.0f;
    out[4] = 0.0f;   out[5] = k;    out[6] = 0.0f;   out[7] = 0.0f;
    out[8] = k * s;  out[9] = 0.0f; out[10] = k * c; out[11] = 0.0f;
    out[12] = bt->tx; out[13] = bt->ty; out[14] = bt->tz; out[15] = 1.0f;
}

void bz_quest_board_transform_apply_point(const bzQuestBoardTransform_t *bt, const float in[3],
                                          float out[3]) {
    const float c = cosf(bt->yaw), s = sinf(bt->yaw), k = bt->scale;
    out[0] = k * (c * in[0] + s * in[2]) + bt->tx;
    out[1] = k * in[1] + bt->ty;
    out[2] = k * (-s * in[0] + c * in[2]) + bt->tz;
}

void bz_quest_board_transform_inverse_point(const bzQuestBoardTransform_t *bt, const float in[3],
                                            float out[3]) {
    const float c = cosf(bt->yaw), s = sinf(bt->yaw), invK = 1.0f / bt->scale;
    const float qx = in[0] - bt->tx, qy = in[1] - bt->ty, qz = in[2] - bt->tz;
    out[0] = invK * (c * qx - s * qz);
    out[1] = invK * qy;
    out[2] = invK * (s * qx + c * qz);
}

void bz_quest_board_transform_inverse_ray(const bzQuestBoardTransform_t *bt, const float origin[3],
                                          const float dir[3], float outOrigin[3], float outDir[3]) {
    const float c = cosf(bt->yaw), s = sinf(bt->yaw), invK = 1.0f / bt->scale;
    bz_quest_board_transform_inverse_point(bt, origin, outOrigin);
    /* Direction: inverse linear part only (no translation), same rotation. */
    outDir[0] = invK * (c * dir[0] - s * dir[2]);
    outDir[1] = invK * dir[1];
    outDir[2] = invK * (s * dir[0] + c * dir[2]);
}

/* ------------------------------------------------------------------ edges */

bool bz_quest_edge_update(bzQuestButtonEdge_t *edge, bool active, bool pressed) {
    if (!active) { edge->prev = false; return false; } /* inactive is never "pressed" */
    const bool rising = pressed && !edge->prev;
    edge->prev = pressed;
    return rising;
}

/* ------------------------------------------------------------- hit testing */

/* Scans the frame's hit regions for the action the HUD hit-test returned and
 * classifies it: enabled+CANCEL -> HUD_CANCEL, enabled+BUTTON -> HUD_BUTTON,
 * anything else (disabled/hidden slot that still produced a region, or an
 * unsupported semantic) -> HUD_DISABLED (consumes the ray, posts nothing). */
static bzQuestInputHitKind_t bz_classify_hud_action(const bzQuestHudFrame_t *frame,
                                                    const bzQuestHudActionId_t *action) {
    for (uint32_t i = 0; i < frame->hitRegionCount; ++i) {
        const bzQuestHudHitRegion_t *r = &frame->hitRegions[i];
        if (r->action.gridX != action->gridX || r->action.gridY != action->gridY) continue;
        if (r->action.semantic != action->semantic) continue;
        if (strncmp(r->action.actionCode, action->actionCode, BZ_QUEST_HUD_MAX_ACTION_CODE) != 0) continue;
        if (!r->enabled) return BZ_QUEST_INPUT_HIT_HUD_DISABLED;
        if (action->semantic == BZ_QUEST_HUD_SEMANTIC_CANCEL) return BZ_QUEST_INPUT_HIT_HUD_CANCEL;
        if (action->semantic == BZ_QUEST_HUD_SEMANTIC_BUTTON) return BZ_QUEST_INPUT_HIT_HUD_BUTTON;
        return BZ_QUEST_INPUT_HIT_HUD_DISABLED;
    }
    return BZ_QUEST_INPUT_HIT_HUD_DISABLED; /* region vanished (stale) - treat as consumed, no command */
}

void bz_quest_input_hit_test(const bzQuestInputWorld_t *world, const float rayOrigin[3],
                             const float rayDir[3], bzQuestInputHit_t *out) {
    memset(out, 0, sizeof(*out));
    out->kind = BZ_QUEST_INPUT_HIT_NONE;

    /* (1) HUD panel - highest priority. hud_hit_test rejects stale frameId,
     * parallel rays, and behind-origin hits itself. */
    if (world->hudFrame) {
        bzQuestHudActionId_t action;
        if (bz_quest_wc3_hud_hit_test(world->hudFrame, world->generation, rayOrigin[0], rayOrigin[1],
                                      rayOrigin[2], rayDir[0], rayDir[1], rayDir[2], &action)) {
            out->kind = bz_classify_hud_action(world->hudFrame, &action);
            out->hudAction = action;
            return; /* the panel always consumes the ray, enabled or not */
        }
    }

    /* (2) nearest entity sphere (smallest positive t wins). */
    float bestT = 0.0f;
    bool haveEntity = false;
    for (uint32_t i = 0; i < world->entityCount; ++i) {
        const bzQuestInputEntity_t *e = &world->entities[i];
        const float ox = rayOrigin[0] - e->centerX, oy = rayOrigin[1] - e->centerY, oz = rayOrigin[2] - e->centerZ;
        const float a = rayDir[0] * rayDir[0] + rayDir[1] * rayDir[1] + rayDir[2] * rayDir[2];
        if (a <= 1e-12f) break; /* degenerate direction */
        const float b = 2.0f * (ox * rayDir[0] + oy * rayDir[1] + oz * rayDir[2]);
        const float cc = ox * ox + oy * oy + oz * oz - e->radius * e->radius;
        const float disc = b * b - 4.0f * a * cc;
        if (disc < 0.0f) continue;
        const float sq = sqrtf(disc);
        float t = (-b - sq) / (2.0f * a);
        if (t < 0.0f) t = (-b + sq) / (2.0f * a); /* origin inside sphere -> far root */
        if (t < 0.0f) continue;
        if (!haveEntity || t < bestT) {
            haveEntity = true;
            bestT = t;
            out->entityNumber = e->number;
            out->entityEngineX = e->engineX;
            out->entityEngineNorth = e->engineNorth;
        }
    }
    if (haveEntity) {
        out->kind = BZ_QUEST_INPUT_HIT_ENTITY;
        out->distance = bestT;
        return;
    }

    /* (3) board base plane (composed-space y = planeY). */
    if (fabsf(rayDir[1]) > 1e-6f) {
        const float t = (world->planeY - rayOrigin[1]) / rayDir[1];
        if (t > 0.0f) {
            const float hx = rayOrigin[0] + t * rayDir[0];
            const float hy = rayOrigin[1] + t * rayDir[1];
            const float hz = rayOrigin[2] + t * rayDir[2];
            float engine[3];
            bz_quest_wc3_world_transform_point_inverse(world->transform, hx, hy, hz, engine);
            out->kind = BZ_QUEST_INPUT_HIT_TERRAIN;
            out->pointEngineX = engine[0];   /* engine world X */
            out->pointEngineNorth = engine[2]; /* engine world "north" (Y in engine's 2D plane) */
            out->distance = t;
            return;
        }
    }
    /* (4) no hit - out->kind stays NONE. */
}

/* --------------------------------------------------------------- haptics */

/* Named pulse constants: an accepted pulse is a short, sharp click; a
 * rejected pulse is a longer, softer double-feel buzz. Values chosen to be
 * clearly distinguishable on Touch controllers without being fatiguing;
 * frequency 0 => let the runtime pick (XR_FREQUENCY_UNSPECIFIED). */
#define BZ_QUEST_HAPTIC_ACCEPT_AMPLITUDE 0.6f
#define BZ_QUEST_HAPTIC_ACCEPT_NANOS 20000000LL /* 20 ms crisp tap */
#define BZ_QUEST_HAPTIC_REJECT_AMPLITUDE 0.35f
#define BZ_QUEST_HAPTIC_REJECT_NANOS 90000000LL /* 90 ms soft buzz */

bzQuestHapticPulse_t bz_quest_haptic_pulse(bool accepted) {
    bzQuestHapticPulse_t p;
    p.frequency = 0.0f;
    if (accepted) {
        p.amplitude = BZ_QUEST_HAPTIC_ACCEPT_AMPLITUDE;
        p.durationNanos = BZ_QUEST_HAPTIC_ACCEPT_NANOS;
    } else {
        p.amplitude = BZ_QUEST_HAPTIC_REJECT_AMPLITUDE;
        p.durationNanos = BZ_QUEST_HAPTIC_REJECT_NANOS;
    }
    return p;
}

/* ------------------------------------------------------------- lifecycle */

void bz_quest_input_state_init(bzQuestInputState_t *state) {
    memset(state, 0, sizeof(*state));
    bz_quest_board_transform_default(&state->board);
    state->phase = BZ_QUEST_INPUT_PHASE_IDLE_RAY;
}

void bz_quest_input_state_clear(bzQuestInputState_t *state, bool resetBoard) {
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        state->selectEdge[h].prev = false;
        state->squeezeEdge[h].prev = false;
        state->secondaryEdge[h].prev = false;
    }
    state->menuEdge.prev = false;
    state->panDragging = false;
    state->phase = BZ_QUEST_INPUT_PHASE_IDLE_RAY;
    if (resetBoard) bz_quest_board_transform_default(&state->board);
}

/* ------------------------------------------------------------ command map */

/* Table-driven target-mode -> command resolution for an ENTITY hit and a
 * TERRAIN hit (AGENTS.md: table, not if/else ladder). ENTITY row's "point"
 * column encodes the documented ABI-gap workaround: because the transport has
 * NO PostTargetEntity (confirmed in bz_tabletop_transport.h), a target-entity
 * submission is posted as PostTargetPoint at the entity's own ground origin
 * (option (a) in the task brief - preserves server authority, fabricates no
 * new data beyond the entity's real position, which the server re-resolves).
 * CMD_NONE means "reject: no valid command for this target mode + hit". */
typedef struct {
    bzQuestHudActionTarget_t mode;
    bzQuestInputCommandType_t onEntity; /* command when the ray hit an entity */
    bzQuestInputCommandType_t onTerrain;/* command when the ray hit terrain */
} bzQuestTargetRow_t;

static const bzQuestTargetRow_t kTargetTable[] = {
    /* NONE: normal play - entity selects, terrain issues a smart order. */
    {BZ_QUEST_HUD_TARGET_NONE, BZ_QUEST_INPUT_CMD_SELECT, BZ_QUEST_INPUT_CMD_SMART_POINT},
    /* POINT: any hit resolves to a ground point (entity -> its origin). */
    {BZ_QUEST_HUD_TARGET_POINT, BZ_QUEST_INPUT_CMD_TARGET_POINT, BZ_QUEST_INPUT_CMD_TARGET_POINT},
    /* ENTITY: entity -> point-at-entity (ABI gap); terrain -> reject. */
    {BZ_QUEST_HUD_TARGET_ENTITY, BZ_QUEST_INPUT_CMD_TARGET_POINT, BZ_QUEST_INPUT_CMD_NONE},
    /* ENTITY_OR_POINT: either hit resolves to a point. */
    {BZ_QUEST_HUD_TARGET_ENTITY_OR_POINT, BZ_QUEST_INPUT_CMD_TARGET_POINT, BZ_QUEST_INPUT_CMD_TARGET_POINT},
};

static const bzQuestTargetRow_t *bz_target_row(bzQuestHudActionTarget_t mode) {
    for (size_t i = 0; i < sizeof(kTargetTable) / sizeof(kTargetTable[0]); ++i)
        if (kTargetTable[i].mode == mode) return &kTargetTable[i];
    return &kTargetTable[0]; /* defensive: unknown mode behaves as NONE */
}

/* Builds the additive/replace selection set for a SELECT command. */
static void bz_build_select(bzQuestInputCommand_t *cmd, uint32_t entity, bool additive,
                            const uint32_t *cur, uint32_t curCount) {
    cmd->selectCount = 0;
    if (additive && cur) {
        for (uint32_t i = 0; i < curCount && cmd->selectCount < BZ_QUEST_INPUT_MAX_SELECT_IDS; ++i) {
            if (cur[i] == entity) continue; /* de-dup */
            cmd->selectIds[cmd->selectCount++] = cur[i];
        }
    }
    if (cmd->selectCount < BZ_QUEST_INPUT_MAX_SELECT_IDS) cmd->selectIds[cmd->selectCount++] = entity;
}

/* Resolves the single gameplay command for a resolved hit + press kind. Returns
 * true and fills *cmd if a command should post; false = no command (and sets
 * *reject true when the no-command outcome should still buzz "rejected"). */
static bool bz_map_command(const bzQuestInputFrame_t *frame, const bzQuestInputHit_t *hit, uint8_t hand,
                           bool selectPress, bool smartPress, bzQuestInputCommand_t *cmd, bool *reject) {
    memset(cmd, 0, sizeof(*cmd));
    cmd->hand = hand;
    *reject = false;
    const bzQuestTargetRow_t *row = bz_target_row(frame->world.targetMode);

    switch (hit->kind) {
    case BZ_QUEST_INPUT_HIT_HUD_BUTTON:
        cmd->type = BZ_QUEST_INPUT_CMD_BUTTON;
        strncpy(cmd->code, hit->hudAction.actionCode, BZ_QUEST_INPUT_MAX_BUTTON_CODE - 1);
        cmd->code[BZ_QUEST_INPUT_MAX_BUTTON_CODE - 1] = '\0';
        return true;
    case BZ_QUEST_INPUT_HIT_HUD_CANCEL:
        cmd->type = BZ_QUEST_INPUT_CMD_CANCEL;
        return true;
    case BZ_QUEST_INPUT_HIT_HUD_DISABLED:
        *reject = true; /* consumed the ray, posts nothing, buzzes reject */
        return false;
    case BZ_QUEST_INPUT_HIT_ENTITY:
        if (frame->world.targetMode == BZ_QUEST_HUD_TARGET_NONE) {
            if (smartPress) {
                cmd->type = BZ_QUEST_INPUT_CMD_SMART_ENTITY;
                cmd->targetEntity = hit->entityNumber;
                return true;
            }
            cmd->type = BZ_QUEST_INPUT_CMD_SELECT;
            bz_build_select(cmd, hit->entityNumber, frame->hands[hand].primaryDown, frame->selectedIds,
                            frame->selectedCount);
            return true;
        }
        /* target mode: entity resolves per table (point-at-entity ABI gap). */
        if (row->onEntity == BZ_QUEST_INPUT_CMD_TARGET_POINT) {
            cmd->type = BZ_QUEST_INPUT_CMD_TARGET_POINT;
            cmd->x = hit->entityEngineX;
            cmd->y = hit->entityEngineNorth;
            return true;
        }
        *reject = true;
        return false;
    case BZ_QUEST_INPUT_HIT_TERRAIN:
        if (row->onTerrain == BZ_QUEST_INPUT_CMD_SMART_POINT) {
            cmd->type = BZ_QUEST_INPUT_CMD_SMART_POINT;
            cmd->x = hit->pointEngineX;
            cmd->y = hit->pointEngineNorth;
            return true;
        }
        if (row->onTerrain == BZ_QUEST_INPUT_CMD_TARGET_POINT) {
            cmd->type = BZ_QUEST_INPUT_CMD_TARGET_POINT;
            cmd->x = hit->pointEngineX;
            cmd->y = hit->pointEngineNorth;
            return true;
        }
        *reject = true; /* ENTITY-only mode + terrain tap = no valid target */
        return false;
    default:
        (void)selectPress;
        return false; /* NONE: silent, no buzz (empty space tap) */
    }
}

/* --------------------------------------------------------------- board sim */

static float bz_deadzone(float v) {
    const float d = BZ_QUEST_INPUT_THUMBSTICK_DEADZONE;
    if (v > d) return (v - d) / (1.0f - d);
    if (v < -d) return (v + d) / (1.0f - d);
    return 0.0f;
}

/* Applies pan/rotate/zoom/height from the controller samples. Returns true if
 * any board input was active this frame (=> phase BOARD_MANIPULATE). Only
 * called when targetMode == NONE (gesture-ownership exclusivity). */
static bool bz_update_board(bzQuestInputState_t *state, const bzQuestInputFrame_t *frame) {
    bool active = false;
    const bzQuestInputHandSample_t *L = &frame->hands[BZ_QUEST_INPUT_HAND_LEFT];
    const bzQuestInputHandSample_t *R = &frame->hands[BZ_QUEST_INPUT_HAND_RIGHT];

    /* Left grip-drag = translate (pan) in the tracking-space horizontal plane. */
    if (L->active && L->squeezeDown) {
        if (!state->panDragging) {
            state->panDragging = true;
            memcpy(state->panAnchor, L->gripPos, sizeof(state->panAnchor));
        } else {
            state->board.tx += L->gripPos[0] - state->panAnchor[0];
            state->board.tz += L->gripPos[2] - state->panAnchor[2];
            memcpy(state->panAnchor, L->gripPos, sizeof(state->panAnchor));
            active = true;
        }
    } else {
        state->panDragging = false;
    }

    /* Right thumbstick X = yaw rotate, Y = zoom; left thumbstick Y = height. */
    if (R->active) {
        const float rx = bz_deadzone(R->thumbstick[0]), ry = bz_deadzone(R->thumbstick[1]);
        if (rx != 0.0f) { state->board.yaw += rx * BZ_QUEST_BOARD_YAW_RATE * frame->dt; active = true; }
        if (ry != 0.0f) { state->board.scale += ry * BZ_QUEST_BOARD_ZOOM_RATE * frame->dt; active = true; }
    }
    if (L->active) {
        const float ly = bz_deadzone(L->thumbstick[1]);
        if (ly != 0.0f) { state->board.ty += ly * BZ_QUEST_BOARD_HEIGHT_RATE * frame->dt; active = true; }
    }
    bz_quest_board_transform_clamp(&state->board);
    return active;
}

/* --------------------------------------------------------------- per-frame */

/* Runs the ray-hit for one hand into `hit`, fills that hand's reticle feedback,
 * and returns whether a hit point exists. */
static bool bz_hand_hit(const bzQuestInputState_t *state, const bzQuestInputFrame_t *frame, uint8_t hand,
                        bzQuestInputHit_t *hit, bzQuestInputFeedback_t *fb) {
    const bzQuestInputHandSample_t *H = &frame->hands[hand];
    fb->visible[hand] = H->active && H->aimValid;
    fb->hasReticle[hand] = false;
    fb->hitKind[hand] = BZ_QUEST_INPUT_HIT_NONE;
    memset(hit, 0, sizeof(*hit));
    if (!fb->visible[hand]) return false;

    float ro[3], rd[3];
    bz_quest_board_transform_inverse_ray(&state->board, H->aimOrigin, H->aimDir, ro, rd);
    bz_quest_input_hit_test(&frame->world, ro, rd, hit);
    fb->hitKind[hand] = hit->kind;
    if (hit->kind == BZ_QUEST_INPUT_HIT_TERRAIN || hit->kind == BZ_QUEST_INPUT_HIT_ENTITY) {
        /* reticle drawn at the composed-space hit point (renderer re-applies
         * board transform). Recompute the composed point from ray + t. */
        fb->hasReticle[hand] = true;
        fb->reticle[hand][0] = ro[0] + hit->distance * rd[0];
        fb->reticle[hand][1] = ro[1] + hit->distance * rd[1];
        fb->reticle[hand][2] = ro[2] + hit->distance * rd[2];
    }
    return true;
}

void bz_quest_input_state_update(bzQuestInputState_t *state, const bzQuestInputFrame_t *frame,
                                 bzQuestInputOutput_t *out) {
    memset(out, 0, sizeof(*out));
    out->command.type = BZ_QUEST_INPUT_CMD_NONE;

    const bool leftActive = frame->hands[BZ_QUEST_INPUT_HAND_LEFT].active;
    const bool rightActive = frame->hands[BZ_QUEST_INPUT_HAND_RIGHT].active;

    /* --- idempotent lifecycle clears (exactly once per condition entry) --- */
    if (!state->initialized) {
        state->initialized = true;
        state->wasFocused = frame->focused;
        state->leftWasActive = leftActive;
        state->rightWasActive = rightActive;
        state->lastGeneration = frame->world.generation;
        state->lastMapEpoch = frame->mapEpoch;
    } else {
        bool clear = false, resetBoard = false;
        if (state->wasFocused && !frame->focused) clear = true;               /* focus loss */
        if (state->leftWasActive && !leftActive) clear = true;                /* left controller loss */
        if (state->rightWasActive && !rightActive) clear = true;             /* right controller loss */
        if (frame->mapEpoch != state->lastMapEpoch) { clear = true; resetBoard = true; } /* map reload */
        if (clear) {
            bz_quest_input_state_clear(state, resetBoard);
            out->clearedThisFrame = true;
        }
        state->wasFocused = frame->focused;
        state->leftWasActive = leftActive;
        state->rightWasActive = rightActive;
        state->lastGeneration = frame->world.generation;
        state->lastMapEpoch = frame->mapEpoch;
    }

    /* Advance all edge latches every frame so an inactive controller can never
     * strand a latched press (bz_quest_edge_update clears prev when inactive).
     * We compute rising edges here and consume them below. */
    bool selRise[BZ_QUEST_INPUT_HAND_COUNT], smtRise[BZ_QUEST_INPUT_HAND_COUNT];
    bool secRise[BZ_QUEST_INPUT_HAND_COUNT];
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        const bzQuestInputHandSample_t *H = &frame->hands[h];
        selRise[h] = bz_quest_edge_update(&state->selectEdge[h], H->active, H->selectDown);
        smtRise[h] = bz_quest_edge_update(&state->squeezeEdge[h], H->active, H->squeezeDown);
        secRise[h] = bz_quest_edge_update(&state->secondaryEdge[h], H->active, H->secondaryDown);
    }
    const bool menuRise = bz_quest_edge_update(&state->menuEdge, leftActive, frame->menuDown);

    /* When unfocused, produce no commands / no pointers. Edges were advanced
     * above (all cleared, since focus-loss also cleared them). */
    if (!frame->focused) {
        state->phase = BZ_QUEST_INPUT_PHASE_IDLE_RAY;
        return;
    }

    /* MENU (left) resets board placement to default (not a full clear). */
    if (menuRise) bz_quest_board_transform_default(&state->board);

    /* --- phase + board manipulation (exclusive with target mode) --- */
    bool boardActive = false;
    if (frame->world.targetMode == BZ_QUEST_HUD_TARGET_NONE) boardActive = bz_update_board(state, frame);
    else state->panDragging = false; /* target mode owns input: drop any board drag */

    if (frame->world.targetMode != BZ_QUEST_HUD_TARGET_NONE)
        state->phase = BZ_QUEST_INPUT_PHASE_TARGET_POINT_MODE;
    else if (boardActive)
        state->phase = BZ_QUEST_INPUT_PHASE_BOARD_MANIPULATE;
    else
        state->phase = BZ_QUEST_INPUT_PHASE_IDLE_RAY;

    /* --- ray hits + reticle feedback for both hands --- */
    bzQuestInputHit_t hit[BZ_QUEST_INPUT_HAND_COUNT];
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h)
        bz_hand_hit(state, frame, (uint8_t)h, &hit[h], &out->feedback);

    /* --- single gameplay command: right hand primary, then left --- *
     * Board manipulation owns input exclusively: when the board is actively
     * being manipulated this frame we suppress gameplay command posting so a
     * grip-drag/thumbstick gesture never doubles as a tap (visionOS gesture-
     * ownership exclusivity). Cancel (secondary) is always allowed. */
    const int order[BZ_QUEST_INPUT_HAND_COUNT] = {BZ_QUEST_INPUT_HAND_RIGHT, BZ_QUEST_INPUT_HAND_LEFT};
    for (int oi = 0; oi < BZ_QUEST_INPUT_HAND_COUNT; ++oi) {
        const uint8_t h = (uint8_t)order[oi];
        if (!frame->hands[h].active) continue;

        if (secRise[h]) { /* B/Y = cancel, regardless of hit or board state */
            out->hasCommand = true;
            out->command.type = BZ_QUEST_INPUT_CMD_CANCEL;
            out->command.hand = h;
            break;
        }
        if (state->phase == BZ_QUEST_INPUT_PHASE_BOARD_MANIPULATE) continue; /* board owns input */
        if (!selRise[h] && !smtRise[h]) continue;
        if (!frame->hands[h].aimValid) continue;

        bzQuestInputCommand_t cmd;
        bool reject = false;
        if (bz_map_command(frame, &hit[h], h, selRise[h], smtRise[h], &cmd, &reject)) {
            out->hasCommand = true;
            out->command = cmd;
            break;
        }
        if (reject) { /* consumed but no command (disabled HUD / invalid target) */
            out->wantHaptic = true;
            out->hapticAccepted = false;
            out->hapticHand = h;
            break;
        }
    }
}

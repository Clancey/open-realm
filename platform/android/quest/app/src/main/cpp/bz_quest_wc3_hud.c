/*
 * bz_quest_wc3_hud.c - see bz_quest_wc3_hud.h.
 */
#include "bz_quest_wc3_hud.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bz_quest_wc3_render.h" /* BZ_QUEST_WC3_WORLD_TARGET_SPAN_F only - no transform/entity types used */

/* Compile-time proof that BZ_QUEST_HUD_MAX_STATUS_TEXT (see its own comment
 * in bz_quest_wc3_hud.h) actually fits both status/resource snprintf()
 * calls below at their UINT32_MAX/max-length-name worst case, with the -1
 * (buffer size vs. max content length) and 10-digit-%u assumption spelled
 * out rather than a guessed round number. A previous too-small value here
 * (40) silently cut numeric fields mid-digit - see bzQuestHudFrame_t.
 * statusTextTruncated for the runtime counterpart of this same guarantee. */
_Static_assert((BZ_QUEST_HUD_MAX_NAME - 1) + 11 + 10 < BZ_QUEST_HUD_MAX_STATUS_TEXT,
               "\"%s  Selected:%u\" (name + 11 literal chars + a 10-digit count) must fit "
               "BZ_QUEST_HUD_MAX_STATUS_TEXT without truncation");
_Static_assert(28 + 10 * 5 < BZ_QUEST_HUD_MAX_STATUS_TEXT,
               "\"Gold:%u Lumber:%u Food:%u/%u Tokens:%u\" (28 literal chars + five 10-digit "
               "UINT32_MAX fields) must fit BZ_QUEST_HUD_MAX_STATUS_TEXT without truncation");

/* --- Local layout constants (panel-local units; the panel transform's
 * right/down vector LENGTHS are the only place a real-world scale is
 * applied - see bz_quest_wc3_hud_panel_transform()) --------------------- */
#define HUD_CELL_W 1.0f
#define HUD_CELL_H 0.6f
#define HUD_CELL_SPACING 0.12f
#define HUD_MARGIN 0.15f
#define HUD_STATUS_LINE_H 0.22f
#define HUD_TEXT_SCALE 0.55f /* local units per glyph cell - fed straight into bz_quest_wc3_hud_font_layout_text() */

/* Flat placeholder-icon/panel tints - see this file's header comment for
 * why no real icon texture is ever substituted. Colors are otherwise
 * arbitrary but fixed/documented so tests can assert on them. */
static const float HUD_COLOR_STATUS_BG[4] = {0.05f, 0.08f, 0.12f, 0.85f};
static const float HUD_COLOR_COMMAND_BG[4] = {0.08f, 0.08f, 0.10f, 0.85f};
static const float HUD_COLOR_BUTTON_NORMAL[4] = {0.15f, 0.35f, 0.55f, 0.90f};
static const float HUD_COLOR_BUTTON_DISABLED[4] = {0.12f, 0.12f, 0.12f, 0.60f};
static const float HUD_COLOR_CANCEL[4] = {0.55f, 0.15f, 0.15f, 0.90f};

static void hud_copy_bounded(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void hud_set_quad(bzQuestHudQuad_t *q, float x, float y, float w, float h, const float color[4]) {
    q->x = x; q->y = y; q->w = w; q->h = h;
    q->r = color[0]; q->g = color[1]; q->b = color[2]; q->a = color[3];
}

static void hud_set_text(bzQuestHudTextRun_t *t, float x, float y, float scale, const char *text) {
    t->x = x; t->y = y; t->scale = scale;
    hud_copy_bounded(t->text, sizeof(t->text), text);
}

/* snprintf() into `line` (capacity `cap`, always BZ_QUEST_HUD_MAX_STATUS_TEXT
 * in this file's only two callers below) and OR any truncation into
 * `*truncated` - snprintf's return is the number of bytes that WOULD have
 * been written were the buffer unbounded, so >= cap means real content was
 * cut off (never just "used exactly cap-1 bytes", which is `== cap - 1`,
 * a normal full-but-untruncated line). See BZ_QUEST_HUD_MAX_STATUS_TEXT's
 * comment for why this should be unreachable today - this is the runtime
 * half of that compile-time guarantee, not an expected code path. */
static void hud_snprintf_status(char *line, size_t cap, bool *truncated, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, cap, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= cap) *truncated = true;
}

void bz_quest_wc3_hud_panel_transform(bzQuestHudPanelTransform_t *out) {
    if (!out) return;
    /* The diorama's transformed half-extent never exceeds
     * BZ_QUEST_WC3_WORLD_TARGET_SPAN_F/2 on either horizontal axis (see
     * bz_quest_wc3_render.h's bz_quest_wc3_world_transform_measure()). This
     * panel is anchored a fixed margin beyond that conservative maximum, on
     * the +X side, at a small height above the table, independent of the
     * CURRENT map's actual bounds - it must still have a well-defined
     * position with no map loaded (loading/error states). */
    const float halfExtent = BZ_QUEST_WC3_WORLD_TARGET_SPAN_F * 0.5f;
    const float panelMargin = 0.25f;
    const float panelHeight = 0.55f;   /* height above the table plane */
    const float tilt = -0.35f;         /* radians, tilted back slightly for readability from above */
    const float localScale = 0.10f;    /* fixed (map-size-independent) world units per local unit - "stable scale/readability" */
    const float c = cosf(tilt), s = sinf(tilt);

    out->originX = halfExtent + panelMargin;
    out->originY = panelHeight;
    out->originZ = -halfExtent - panelMargin * 0.25f;

    /* right = world +X (unscaled local columns grow away from the diorama's edge) */
    out->rightX = localScale; out->rightY = 0.0f; out->rightZ = 0.0f;
    /* down = local +Y grows "down" the panel: tilt about the world X axis so the
     * panel leans back like a sign, matching bz_quest_wc3_build_world_matrix()'s
     * translate+rotate composition style (see this file's header comment). */
    out->downX = 0.0f; out->downY = -localScale * c; out->downZ = localScale * s;
    /* normal = normalize(right x down) */
    out->normalX = 0.0f; out->normalY = -s; out->normalZ = -c;
}

/* Stable insertion sort of visible-button indices by (gridY, gridX)
 * ascending - RealityTabletopView.swift:851-852's `.sorted { ($0.gridY,
 * $0.gridX) < ($1.gridY, $1.gridX) }`. Bounded to
 * BZ_QUEST_HUD_MAX_BUTTONS entries, so an O(n^2) insertion sort is simple,
 * deterministic, and stable (ties keep original ABI order). */
static void hud_sort_visible_buttons(const bzQuestHudActionLayoutInput_t *layout, uint8_t *outIndices,
                                     uint32_t *outCount) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < layout->numButtons && i < BZ_QUEST_HUD_MAX_BUTTONS; i++)
        if (!layout->buttons[i].hidden) outIndices[n++] = (uint8_t)i;
    for (uint32_t i = 1; i < n; i++) {
        uint8_t key = outIndices[i];
        const bzQuestHudButtonInput_t *kb = &layout->buttons[key];
        uint32_t j = i;
        while (j > 0) {
            const bzQuestHudButtonInput_t *pb = &layout->buttons[outIndices[j - 1]];
            bool keyBeforePrev = (kb->gridY < pb->gridY) || (kb->gridY == pb->gridY && kb->gridX < pb->gridX);
            if (!keyBeforePrev) break;
            outIndices[j] = outIndices[j - 1];
            j--;
        }
        outIndices[j] = key;
    }
    *outCount = n;
}

void bz_quest_wc3_hud_build(const bzQuestHudInput_t *input, bzQuestHudFrame_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!input) return;
    out->frameId = input->frameId;
    bz_quest_wc3_hud_panel_transform(&out->panel);

    const float panelWidth =
        HUD_MARGIN * 2.0f + BZ_QUEST_HUD_GRID_COLUMNS * HUD_CELL_W + (BZ_QUEST_HUD_GRID_COLUMNS - 1) * HUD_CELL_SPACING;
    float cursorY = HUD_MARGIN;

    /* --- Status bar: always shown (even with no player - a distinct
     * "no player data" line IS the loading/error state, never a blank
     * silent gap - see this file's header comment) ---------------------- */
    {
        const uint32_t statusLines = input->player.present ? (input->player.gameResult != BZ_QUEST_HUD_GAME_RESULT_NONE ? 3u : 2u) : 1u;
        const float statusHeight = HUD_MARGIN * 2.0f + statusLines * HUD_STATUS_LINE_H;
        bzQuestHudQuad_t *bg = &out->quads[out->quadCount++];
        hud_set_quad(bg, 0.0f, cursorY, panelWidth, statusHeight, HUD_COLOR_STATUS_BG);

        float textY = cursorY + HUD_MARGIN;
        if (!input->player.present) {
            bzQuestHudTextRun_t *t = &out->texts[out->textCount++];
            hud_set_text(t, HUD_MARGIN, textY, HUD_TEXT_SCALE, "No player data");
        } else {
            char line[BZ_QUEST_HUD_MAX_STATUS_TEXT];
            hud_snprintf_status(line, sizeof(line), &out->statusTextTruncated, "%s  Selected:%u",
                                 input->player.name, input->selectedCount);
            hud_set_text(&out->texts[out->textCount++], HUD_MARGIN, textY, HUD_TEXT_SCALE, line);
            textY += HUD_STATUS_LINE_H;

            hud_snprintf_status(line, sizeof(line), &out->statusTextTruncated,
                                 "Gold:%u Lumber:%u Food:%u/%u Tokens:%u", input->player.gold,
                                 input->player.lumber, input->player.foodUsed, input->player.foodCap,
                                 input->player.heroTokens);
            hud_set_text(&out->texts[out->textCount++], HUD_MARGIN, textY, HUD_TEXT_SCALE, line);
            textY += HUD_STATUS_LINE_H;

            if (input->player.gameResult != BZ_QUEST_HUD_GAME_RESULT_NONE) {
                const char *resultText = input->player.gameResult == BZ_QUEST_HUD_GAME_RESULT_VICTORY ? "VICTORY"
                                        : input->player.gameResult == BZ_QUEST_HUD_GAME_RESULT_DEFEAT ? "DEFEAT"
                                        : "DRAW";
                hud_set_text(&out->texts[out->textCount++], HUD_MARGIN, textY, HUD_TEXT_SCALE, resultText);
            }
        }
        cursorY += statusHeight + HUD_MARGIN;
    }

    /* --- Command card: RealityTabletopView.swift:846's present && visible
     * && valid gate, identical here ------------------------------------- */
    const bzQuestHudActionLayoutInput_t *layout = &input->actionLayout;
    if (layout->present && layout->visible && layout->valid) {
        uint8_t order[BZ_QUEST_HUD_MAX_BUTTONS];
        uint32_t visibleCount = 0;
        hud_sort_visible_buttons(layout, order, &visibleCount);
        uint32_t rows = visibleCount == 0 ? 0 : (visibleCount + BZ_QUEST_HUD_GRID_COLUMNS - 1) / BZ_QUEST_HUD_GRID_COLUMNS;
        bool showCancel = layout->currentTarget != BZ_QUEST_HUD_TARGET_NONE;
        uint32_t totalRows = rows + (showCancel ? 1u : 0u);
        float commandHeight = totalRows == 0 ? 0.0f
                             : HUD_MARGIN * 2.0f + totalRows * HUD_CELL_H + (totalRows - 1) * HUD_CELL_SPACING;

        if (commandHeight > 0.0f) {
            bzQuestHudQuad_t *bg = &out->quads[out->quadCount++];
            hud_set_quad(bg, 0.0f, cursorY, panelWidth, commandHeight, HUD_COLOR_COMMAND_BG);

            for (uint32_t slot = 0; slot < visibleCount && out->quadCount < BZ_QUEST_HUD_MAX_QUADS; slot++) {
                const bzQuestHudButtonInput_t *b = &layout->buttons[order[slot]];
                uint32_t col = slot % BZ_QUEST_HUD_GRID_COLUMNS;
                uint32_t row = slot / BZ_QUEST_HUD_GRID_COLUMNS;
                float x = HUD_MARGIN + col * (HUD_CELL_W + HUD_CELL_SPACING);
                float y = cursorY + HUD_MARGIN + row * (HUD_CELL_H + HUD_CELL_SPACING);
                /* Matches RealityTabletopView.swift:867's `.disabled(button.disabled || semantic == .unsupported)`
                 * exactly - computed once, reused for both the tint and the hit region's `enabled` flag below. */
                bool visuallyDisabled = b->disabled || b->semantic == BZ_QUEST_HUD_SEMANTIC_UNSUPPORTED;

                hud_set_quad(&out->quads[out->quadCount++], x, y, HUD_CELL_W, HUD_CELL_H,
                             visuallyDisabled ? HUD_COLOR_BUTTON_DISABLED : HUD_COLOR_BUTTON_NORMAL);

                if (out->textCount < BZ_QUEST_HUD_MAX_TEXT_RUNS) {
                    const char *label = b->tooltip[0] ? b->tooltip : b->actionCode;
                    hud_set_text(&out->texts[out->textCount++], x + 0.05f, y + 0.05f, HUD_TEXT_SCALE * 0.7f, label);
                }
                if (b->cooldown > 0.0f && out->textCount < BZ_QUEST_HUD_MAX_TEXT_RUNS) {
                    char cd[16];
                    snprintf(cd, sizeof(cd), "%.1f", (double)b->cooldown);
                    hud_set_text(&out->texts[out->textCount++], x + 0.05f, y + HUD_CELL_H - 0.2f, HUD_TEXT_SCALE * 0.6f, cd);
                }

                if (out->hitRegionCount < BZ_QUEST_HUD_MAX_HIT_REGIONS) {
                    bzQuestHudHitRegion_t *hr = &out->hitRegions[out->hitRegionCount++];
                    hr->x = x; hr->y = y; hr->w = HUD_CELL_W; hr->h = HUD_CELL_H;
                    hr->action.gridX = b->gridX;
                    hr->action.gridY = b->gridY;
                    hr->action.semantic = b->semantic;
                    hud_copy_bounded(hr->action.actionCode, sizeof(hr->action.actionCode), b->actionCode);
                    hr->enabled = !visuallyDisabled;
                }
            }

            if (showCancel && out->quadCount < BZ_QUEST_HUD_MAX_QUADS) {
                float y = cursorY + HUD_MARGIN + rows * (HUD_CELL_H + HUD_CELL_SPACING);
                float w = BZ_QUEST_HUD_GRID_COLUMNS * HUD_CELL_W + (BZ_QUEST_HUD_GRID_COLUMNS - 1) * HUD_CELL_SPACING;
                hud_set_quad(&out->quads[out->quadCount++], HUD_MARGIN, y, w, HUD_CELL_H, HUD_COLOR_CANCEL);
                if (out->textCount < BZ_QUEST_HUD_MAX_TEXT_RUNS)
                    hud_set_text(&out->texts[out->textCount++], HUD_MARGIN + 0.05f, y + 0.05f, HUD_TEXT_SCALE, "Cancel");
                if (out->hitRegionCount < BZ_QUEST_HUD_MAX_HIT_REGIONS) {
                    bzQuestHudHitRegion_t *hr = &out->hitRegions[out->hitRegionCount++];
                    hr->x = HUD_MARGIN; hr->y = y; hr->w = w; hr->h = HUD_CELL_H;
                    /* Sentinel grid coords (0xFF,0xFF): the cancel slot is not any buttons[] entry -
                     * RealityTabletopView.swift:870-872 renders it unconditionally from currentTarget,
                     * never from a specific gridX/gridY - mirrored identically here. */
                    hr->action.gridX = 0xFF; hr->action.gridY = 0xFF;
                    hr->action.semantic = BZ_QUEST_HUD_SEMANTIC_CANCEL;
                    hr->action.actionCode[0] = '\0';
                    hr->enabled = true;
                }
            }
        }
    }
}

bool bz_quest_wc3_hud_hit_test(const bzQuestHudFrame_t *frame, uint64_t currentFrameId, float rayOriginX,
                               float rayOriginY, float rayOriginZ, float rayDirX, float rayDirY, float rayDirZ,
                               bzQuestHudActionId_t *outAction) {
    if (!frame || !outAction) return false;
    if (frame->frameId != currentFrameId) return false; /* stale frame: caller must re-fetch before acting */

    const bzQuestHudPanelTransform_t *p = &frame->panel;
    float dotDirNormal = rayDirX * p->normalX + rayDirY * p->normalY + rayDirZ * p->normalZ;
    if (fabsf(dotDirNormal) < 1e-6f) return false; /* ray parallel to panel plane */

    float toOriginX = p->originX - rayOriginX, toOriginY = p->originY - rayOriginY, toOriginZ = p->originZ - rayOriginZ;
    float t = (toOriginX * p->normalX + toOriginY * p->normalY + toOriginZ * p->normalZ) / dotDirNormal;
    if (t < 0.0f) return false; /* intersection behind the ray origin */

    float hitX = rayOriginX + t * rayDirX - p->originX;
    float hitY = rayOriginY + t * rayDirY - p->originY;
    float hitZ = rayOriginZ + t * rayDirZ - p->originZ;

    float rightLenSq = p->rightX * p->rightX + p->rightY * p->rightY + p->rightZ * p->rightZ;
    float downLenSq = p->downX * p->downX + p->downY * p->downY + p->downZ * p->downZ;
    if (!(rightLenSq > 0.0f) || !(downLenSq > 0.0f)) return false; /* degenerate panel basis */

    float localX = (hitX * p->rightX + hitY * p->rightY + hitZ * p->rightZ) / rightLenSq;
    float localY = (hitX * p->downX + hitY * p->downY + hitZ * p->downZ) / downLenSq;

    for (uint32_t i = 0; i < frame->hitRegionCount; i++) {
        const bzQuestHudHitRegion_t *hr = &frame->hitRegions[i];
        if (localX >= hr->x && localX < hr->x + hr->w && localY >= hr->y && localY < hr->y + hr->h) {
            *outAction = hr->action;
            return true;
        }
    }
    return false;
}

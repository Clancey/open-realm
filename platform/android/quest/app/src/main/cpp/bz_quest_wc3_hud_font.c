/*
 * bz_quest_wc3_hud_font.c - see bz_quest_wc3_hud_font.h.
 */
#include "bz_quest_wc3_hud_font.h"

#include "share/fonts/fixed_8x13.h"

#include <string.h>

bool bz_quest_wc3_hud_font_bit(uint32_t glyphIndex, uint32_t col, uint32_t row) {
    if (glyphIndex >= BZ_QUEST_HUD_FONT_GLYPH_COUNT) glyphIndex = 0;
    if (col >= BZ_QUEST_HUD_FONT_GLYPH_WIDTH || row >= BZ_QUEST_HUD_FONT_GLYPH_HEIGHT) return false;
    /* LSB-left: bit `col` (0 = leftmost column) - see this module's header
     * comment for the verified derivation against the 'F' glyph. */
    return ((fixed_8x13[glyphIndex][row] >> col) & 1u) != 0;
}

bool bz_quest_wc3_hud_font_build_atlas(uint8_t *dst, uint32_t dstCapacity) {
    if (!dst || dstCapacity < (uint32_t)BZ_QUEST_HUD_FONT_ATLAS_BYTES) return false;
    memset(dst, 0, BZ_QUEST_HUD_FONT_ATLAS_BYTES);
    for (uint32_t g = 0; g < BZ_QUEST_HUD_FONT_GLYPH_COUNT; g++) {
        uint32_t cellX = (g % BZ_QUEST_HUD_FONT_ATLAS_COLUMNS) * BZ_QUEST_HUD_FONT_GLYPH_WIDTH;
        uint32_t cellY = (g / BZ_QUEST_HUD_FONT_ATLAS_COLUMNS) * BZ_QUEST_HUD_FONT_GLYPH_HEIGHT;
        for (uint32_t row = 0; row < BZ_QUEST_HUD_FONT_GLYPH_HEIGHT; row++) {
            for (uint32_t col = 0; col < BZ_QUEST_HUD_FONT_GLYPH_WIDTH; col++) {
                if (!bz_quest_wc3_hud_font_bit(g, col, row)) continue;
                dst[(cellY + row) * BZ_QUEST_HUD_FONT_ATLAS_WIDTH + (cellX + col)] = 255;
            }
        }
    }
    return true;
}

bool bz_quest_wc3_hud_font_glyph_uv(unsigned char c, float *outU0, float *outV0, float *outU1, float *outV1) {
    bool supported = c < BZ_QUEST_HUD_FONT_GLYPH_COUNT;
    unsigned char glyph = supported ? c : (unsigned char)'?';
    uint32_t cellX = (glyph % BZ_QUEST_HUD_FONT_ATLAS_COLUMNS);
    uint32_t cellY = (glyph / BZ_QUEST_HUD_FONT_ATLAS_COLUMNS);
    if (outU0) *outU0 = (float)(cellX * BZ_QUEST_HUD_FONT_GLYPH_WIDTH) / (float)BZ_QUEST_HUD_FONT_ATLAS_WIDTH;
    if (outV0) *outV0 = (float)(cellY * BZ_QUEST_HUD_FONT_GLYPH_HEIGHT) / (float)BZ_QUEST_HUD_FONT_ATLAS_HEIGHT;
    if (outU1) *outU1 = (float)(cellX * BZ_QUEST_HUD_FONT_GLYPH_WIDTH + BZ_QUEST_HUD_FONT_GLYPH_WIDTH) /
                         (float)BZ_QUEST_HUD_FONT_ATLAS_WIDTH;
    if (outV1) *outV1 = (float)(cellY * BZ_QUEST_HUD_FONT_GLYPH_HEIGHT + BZ_QUEST_HUD_FONT_GLYPH_HEIGHT) /
                         (float)BZ_QUEST_HUD_FONT_ATLAS_HEIGHT;
    return supported;
}

bool bz_quest_wc3_hud_font_layout_text(const char *text, float originX, float originY, float scale,
                                       bzQuestHudGlyphQuad_t *outQuads, uint32_t cap, uint32_t *outCount) {
    if (outCount) *outCount = 0;
    if (!text || !outQuads || !outCount || scale <= 0.0f) return false;
    float advance = (float)BZ_QUEST_HUD_FONT_GLYPH_WIDTH * scale;
    float glyphH = (float)BZ_QUEST_HUD_FONT_GLYPH_HEIGHT * scale;
    uint32_t written = 0;
    float cursorX = originX;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p != ' ') {
            if (written >= cap) {
                *outCount = written; /* truncated: caller logs once per string if it cares, count reflects what fit */
                return false;
            }
            bzQuestHudGlyphQuad_t *q = &outQuads[written];
            q->x = cursorX;
            q->y = originY;
            q->w = advance;
            q->h = glyphH;
            bz_quest_wc3_hud_font_glyph_uv(*p, &q->u0, &q->v0, &q->u1, &q->v1);
            written++;
        }
        cursorX += advance;
    }
    *outCount = written;
    return true;
}

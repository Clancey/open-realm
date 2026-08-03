/*
 * bz_quest_wc3_hud_font.h - layer 5E: platform-independent bitmap-font atlas
 * packing and monospace glyph-quad text layout for the Quest HUD.
 *
 * Like bz_quest_wc3_fog.h/bz_quest_wc3_render.h, every type/function here is
 * plain C POD/math only: no Vulkan/Android/OpenXR type appears. The impure
 * bz_quest_vk_wc3_hud.c calls bz_quest_wc3_hud_font_build_atlas() once (at
 * first use, retried on failure) to obtain R8_UNORM pixel data for a real
 * Vulkan texture, and calls bz_quest_wc3_hud_font_layout_text() every frame
 * a text run changes to obtain the glyph quads it uploads into its dynamic
 * vertex buffer.
 *
 * -- Font source: reused, not fabricated --
 * share/fonts/fixed_8x13.h is an existing, source-committed, public-domain
 * (X11 misc-fixed 8x13.bdf) bitmap font already vendored in this repository
 * for the desktop sysfont - see that file's header comment. This module is
 * the "document/build a deterministic project-owned atlas" option: it packs
 * that same font's 128 ASCII glyphs into one small texture at build/init
 * time via a pure, host-testable function, rather than shipping a second,
 * opaque generated binary asset or taking on a platform text-rendering
 * library dependency (which would violate the UI module boundary - see
 * AGENTS.md's "UI Module Boundary" and this project's Quest host/UI
 * separation).
 *
 * -- Bit convention: verified against asymmetric glyphs, not assumed --
 * fixed_8x13.h's own comment says "LSB-left row bytes". Verified concretely
 * against the 'F' glyph (row bytes 0x7e/0x02x4/0x1e/0x02x4): interpreting
 * bit `col` (0..7, col 0 = leftmost) as `(rowByte >> col) & 1` places F's
 * vertical stroke at column 1 for every row and its shorter middle bar
 * (0x1e = bits 1..4) flush against the same left edge as its full-width top
 * bar (0x7e = bits 1..6) - exactly F's real glyph shape. The opposite (MSB-
 * left) reading would put the stroke one column from the RIGHT edge
 * instead, which is wrong for 'F'. This module's bz_quest_wc3_hud_font_bit()
 * therefore reads `(fixed_8x13[glyph][row] >> col) & 1`.
 */
#ifndef BZ_QUEST_WC3_HUD_FONT_H
#define BZ_QUEST_WC3_HUD_FONT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_HUD_FONT_GLYPH_WIDTH = 8,
    BZ_QUEST_HUD_FONT_GLYPH_HEIGHT = 13,
    BZ_QUEST_HUD_FONT_GLYPH_COUNT = 128, /* mirrors FIXED_8X13_GLYPHS - full 7-bit ASCII range */
    BZ_QUEST_HUD_FONT_ATLAS_COLUMNS = 16, /* 128 glyphs / 16 columns = 8 rows, a compact near-square atlas */
    BZ_QUEST_HUD_FONT_ATLAS_ROWS = BZ_QUEST_HUD_FONT_GLYPH_COUNT / BZ_QUEST_HUD_FONT_ATLAS_COLUMNS,
    BZ_QUEST_HUD_FONT_ATLAS_WIDTH = BZ_QUEST_HUD_FONT_ATLAS_COLUMNS * BZ_QUEST_HUD_FONT_GLYPH_WIDTH,
    BZ_QUEST_HUD_FONT_ATLAS_HEIGHT = BZ_QUEST_HUD_FONT_ATLAS_ROWS * BZ_QUEST_HUD_FONT_GLYPH_HEIGHT,
    BZ_QUEST_HUD_FONT_ATLAS_BYTES = BZ_QUEST_HUD_FONT_ATLAS_WIDTH * BZ_QUEST_HUD_FONT_ATLAS_HEIGHT, /* R8_UNORM, 1 byte/texel */
    /* Bounded per-text-run glyph cap: long strings are truncated (never
     * overflow a fixed buffer) - matches every other bounded-buffer
     * convention in this Quest port (see bz_quest_wc3_capture.h). */
    BZ_QUEST_HUD_FONT_MAX_GLYPHS_PER_RUN = 96,
};

/* One glyph's position (already scaled/offset into caller units) plus its
 * atlas UV rect - the exact per-glyph data bz_quest_vk_wc3_hud.c needs to
 * append one textured quad (two triangles) per glyph into its dynamic
 * vertex buffer. */
typedef struct {
    float x, y, w, h;       /* caller-space quad rect (already includes originX/Y and scale) */
    float u0, v0, u1, v1;   /* atlas UV rect */
} bzQuestHudGlyphQuad_t;

/* Returns the single bit for glyph `glyphIndex` (0..127; out-of-range
 * clamps to 0, the NUL glyph, which fixed_8x13.h defines as fully blank) at
 * (col, row) - see this file's header comment for the verified LSB-left bit
 * convention. col/row outside [0,width)/[0,height) return 0 (blank),
 * matching an implicit glyph-cell margin. */
bool bz_quest_wc3_hud_font_bit(uint32_t glyphIndex, uint32_t col, uint32_t row);

/*
 * Packs every glyph cell into `dst` (row-major, BZ_QUEST_HUD_FONT_ATLAS_WIDTH
 * texels/row, one R8_UNORM byte/texel: 0 or 255) in atlas cell order (glyph
 * g at cell (g % BZ_QUEST_HUD_FONT_ATLAS_COLUMNS, g / BZ_QUEST_HUD_FONT_ATLAS_COLUMNS)).
 * Returns false (leaving `dst` untouched) if `dstCapacity` is smaller than
 * BZ_QUEST_HUD_FONT_ATLAS_BYTES - a caller bug, never a partial/truncated
 * atlas.
 */
bool bz_quest_wc3_hud_font_build_atlas(uint8_t *dst, uint32_t dstCapacity);

/*
 * Fills `outU0/outV0/outU1/outV1` with glyph `c`'s atlas UV rect. Any byte
 * outside the supported 7-bit ASCII range (>= BZ_QUEST_HUD_FONT_GLYPH_COUNT)
 * is remapped to '?' (a visibly-present "unsupported character" glyph,
 * matching this project's "never silently drop, stay visibly diagnosable"
 * rule - see AGENTS.md) rather than the blank NUL glyph, which would render
 * as invisible, undiagnosable empty space. Returns false iff the remap
 * happened (the caller/capture layer uses this to log once per unique
 * unsupported byte value - see bz_quest_vk_wc3_hud.c).
 */
bool bz_quest_wc3_hud_font_glyph_uv(unsigned char c, float *outU0, float *outV0, float *outU1, float *outV1);

/*
 * Lays out `text` (a NUL-terminated, caller-owned bounded string) as a row
 * of fixed-advance glyph quads starting at (originX, originY) in caller
 * units, each glyph cell scaled by `scale` (glyph pixel size *
 * BZ_QUEST_HUD_FONT_GLYPH_WIDTH/HEIGHT * scale) with no kerning/wrapping -
 * a deterministic monospace layout, matching the source font's own fixed
 * pitch. Writes up to `cap` glyphs (cap must be <=
 * BZ_QUEST_HUD_FONT_MAX_GLYPHS_PER_RUN) into `outQuads`, sets *outCount to
 * the number written, and returns true iff every character in `text` fit
 * (false means the string was truncated at `cap` glyphs - the caller,
 * bz_quest_vk_wc3_hud.c's build_text_vertices(), is responsible for any
 * once-per-run truncation diagnostic since this function is pure and
 * never logs). Space (' ')
 * characters still consume advance width but write no glyph quad (nothing
 * to draw). Returns false immediately (leaving *outCount at 0) for a NULL
 * text/outQuads/outCount or non-positive scale.
 */
bool bz_quest_wc3_hud_font_layout_text(const char *text, float originX, float originY, float scale,
                                       bzQuestHudGlyphQuad_t *outQuads, uint32_t cap, uint32_t *outCount);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_HUD_FONT_H */

#version 450
/*
 * warcraft_hud_text.frag - layer 5E font-atlas glyph fragment shader.
 * uFontAtlas is R8_UNORM (bz_quest_wc3_hud_font_build_atlas(): 0 or 255 per
 * texel) - the single channel is both the glyph's coverage and its alpha,
 * tinted flat white text (readable against every panel/button background
 * tint this slice uses - see bz_quest_wc3_hud.c).
 */

layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    float coverage = texture(uFontAtlas, fragUV).r;
    outColor = vec4(1.0, 1.0, 1.0, coverage);
}

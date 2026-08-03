#version 450
/*
 * warcraft_hud_text.vert - layer 5E font-atlas glyph vertex shader.
 * Same panel-local-position + shared-mvp convention as
 * warcraft_hud_panel_vert.vert; the only addition is a per-vertex atlas UV
 * (bz_quest_wc3_hud_font_layout_text()'s glyph rect) instead of a color.
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragUV = inUV;
}

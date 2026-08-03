#version 450
/*
 * warcraft_hud_panel.vert - layer 5E flat-tint HUD quad vertex shader.
 *
 * Vertices carry bz_quest_wc3_hud.h's panel-local (x, y, 0) coordinates
 * unchanged (bz_quest_vk_wc3_hud.c never re-derives a second position from
 * the same quad); `mvp` folds this frame's fixed panel world placement
 * (bz_quest_wc3_hud_panel_transform()) together with the eye's view/proj,
 * matching layer 5D's marker.mvp precedent (warcraft_marker_vert.vert).
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
}

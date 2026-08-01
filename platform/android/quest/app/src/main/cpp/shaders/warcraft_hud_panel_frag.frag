#version 450
/*
 * warcraft_hud_panel.frag - layer 5E flat-tint HUD quad fragment shader.
 * Unlit passthrough of the per-vertex authoritative state tint (status/
 * command background, or one button's enabled/disabled/target-tinted
 * placeholder slot - see bz_quest_wc3_hud.c's quad-color derivation).
 */

layout(location = 0) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = fragColor;
}

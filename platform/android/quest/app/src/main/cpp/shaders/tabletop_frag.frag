#version 450
/*
 * tabletop.frag - layer-3 test-scene fragment shader. Unlit passthrough of
 * the per-vertex color baked in by bz_quest_scene.c; alpha is always 1.0 so
 * the XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND compositor draws the table/
 * cubes fully opaque over the passthrough background (see
 * bz_quest_vk_create_render_resources()'s render-pass clear value, which
 * clears alpha to 0.0 so untouched pixels show passthrough through).
 */

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}

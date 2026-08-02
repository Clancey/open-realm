#version 450
/*
 * warcraft_particle.frag - layer 9 PRE2 particle-emitter fragment shader.
 * Mirrors renderer/r_particles.c's fs_particle GLSL shader exactly: sample
 * the emitter's atlas texture at the per-vertex-interpolated UV and modulate
 * by the per-vertex color (already the fully FX_BlendColor-evaluated,
 * alpha-included RGBA for this particle's age - see
 * bz_quest_wc3_particles.c's bz_quest_wc3_particles_pack()). No alpha-test
 * discard here - unlike the opaque geoset shader (warcraft.frag), every
 * particle blend mode is expressed purely via this pipeline's blend-state
 * (see bz_quest_vk_wc3_blend_state_for_mode(), reused by
 * bz_quest_vk_wc3_particles.c), matching desktop's own fs_particle which
 * has no discard either.
 */

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTexture, fragUV) * fragColor;
}

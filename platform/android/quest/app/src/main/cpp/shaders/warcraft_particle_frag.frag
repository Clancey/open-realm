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
 *
 * Coverage alpha (coverageParams.x, High-severity reviewer fix, PR #28):
 * BZ_TTA_BLEND_TRANSPARENT particle runs (blendEnable=false - no blend
 * equation runs, this shader's own alpha is written verbatim to the
 * framebuffer; only mode reachable from real PRE2 FilterMode data among the
 * two blendEnable=false modes, see games/warcraft-3/docs/file-formats/
 * mdx.md's FilterMode mapping table) write coverage alpha = 1.0 exactly,
 * never the emitter atlas texture's own alpha channel or the per-particle
 * vertex color's alpha, which would otherwise leave passthrough-visible
 * "pinholes" through an intended-opaque-cutout particle. Every blended mode
 * still writes its real computed alpha, needed by the pipeline's own blend
 * equation - see bz_quest_vk_wc3_particles.c's _record() doc comment.
 */

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConsts {
    mat4 viewProjUnused; /* vertex-stage-only, not read here - see warcraft_particle_vert.vert */
    vec4 coverageParams;
} pc;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(uTexture, fragUV) * fragColor;
    float coverageAlpha = pc.coverageParams.x > 0.5 ? 1.0 : texColor.a;
    outColor = vec4(texColor.rgb, coverageAlpha);
}

#version 450
/*
 * terrain.frag - layer 5B static Warcraft III terrain fragment shader.
 * Samples the bound terrain texture and modulates by per-vertex color so
 * water can carry authoritatively-derived corner alpha while ground/cliff
 * vertices stay opaque white.
 *
 * Coverage alpha (coverageParams.x, High-severity reviewer fix, PR #28): the
 * opaque ground/cliff pipeline (blendEnable=false - no blend equation runs,
 * this shader's own alpha is written verbatim to the framebuffer) writes
 * coverage alpha = 1.0 exactly, never the ground/cliff texture's own alpha
 * channel (fragColor.a is always 1.0 there - opaque_white() - but the
 * TEXTURE's own alpha is not guaranteed to be, which would otherwise leave
 * passthrough-visible "pinholes" through solid ground). The blended water/
 * splat pipeline still writes its real per-corner water-opacity alpha,
 * needed by bz_quest_vk_straight_over_blend_state()'s blend equation.
 */

layout(push_constant) uniform PushConsts {
    mat4 mvpUnused; /* vertex-stage-only, not read here - see terrain_vert.vert */
    vec4 coverageParams;
} pc;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(uTexture, fragUV) * fragColor;
    float coverageAlpha = pc.coverageParams.x > 0.5 ? 1.0 : texColor.a;
    outColor = vec4(texColor.rgb, coverageAlpha);
}

#version 450
/*
 * warcraft.frag - layer 5A static Warcraft III geoset fragment shader.
 * Samples the layer's bound texture (set 0 binding 0, one combined-image-
 * sampler descriptor per cached texture - see bz_quest_vk_wc3.c's
 * descriptor-set-per-texture design) and modulates by the layer's alpha
 * (materialParams.x - already includes this frame's geoset alpha, GEOA/
 * KGAO, folded in CPU-side by bz_quest_vk_wc3.c's draw_layer() - see
 * warcraft_vert.vert's materialParams doc comment; no separate geoset-alpha
 * uniform needed here), discarding below the alpha-test cutoff for
 * BZ_TTA_BLEND_TRANSPARENT layers (cutoff 0.0 for every other blend mode, so
 * this branch is a no-op for them).
 *
 * Coverage alpha (materialParams.z, High-severity reviewer fix, PR #28):
 * every surviving fragment of a BZ_TTA_BLEND_OPAQUE/TRANSPARENT layer
 * (blendEnable=false - no blend equation runs, this shader's own alpha
 * output is written VERBATIM to the framebuffer) writes coverage alpha =
 * 1.0 exactly, NEVER the source texture's own alpha channel - many WC3
 * textures carry an alpha channel unrelated to real transparency even on
 * materials with no intended transparency, which would otherwise leave
 * passthrough-visible "pinholes" through solid geometry once composited by
 * the XR compositor. Every other (blended) mode still writes its real
 * computed `alpha`, needed by bz_quest_vk_wc3_blend_state_for_mode()'s own
 * per-mode blend equation.
 * Deliberately unlit - see that shader's header comment for why.
 */

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform PushConsts {
    mat4 mvp;
    vec4 materialParams;
} pc;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 texColor = texture(uTexture, fragUV);
    float alpha = texColor.a * pc.materialParams.x;
    if (alpha < pc.materialParams.y) {
        discard;
    }
    float coverageAlpha = pc.materialParams.z > 0.5 ? 1.0 : alpha;
    outColor = vec4(texColor.rgb, coverageAlpha);
}

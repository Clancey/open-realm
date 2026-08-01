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
    outColor = vec4(texColor.rgb, alpha);
}

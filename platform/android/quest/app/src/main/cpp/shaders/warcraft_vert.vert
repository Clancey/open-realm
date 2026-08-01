#version 450
/*
 * warcraft.vert - layer 5A static Warcraft III geoset vertex shader.
 * Transforms bz_quest_wc3_capture.c-decoded, bz_quest_vk_wc3.c-uploaded
 * geometry by a per-eye/per-instance combined world*view*projection matrix
 * supplied as a vertex push constant (see bz_quest_vk_wc3_render_target()
 * in bz_quest_vk_wc3.c, which multiplies bz_quest_wc3_build_world_matrix()'s
 * per-entity world matrix by the eye's view*projection using
 * bz_quest_pure.h's bz_quest_mat4_multiply(), matching bz_quest_renderer.c's
 * own build_mvp() convention for the procedural scene). Vertex position is
 * already axis-swapped (Z-up -> Y-up) by the capture step - see
 * bz_quest_wc3_render.h's coordinate evidence - so no further conversion
 * happens here.
 *
 * Deliberately unlit (no normal transform/lighting term), matching this
 * project's existing tabletop_frag.frag convention for the Quest renderer
 * (see that shader's own "Unlit passthrough" comment) - this slice's task
 * scope is static geometry/material/transform correctness, not a lighting
 * model, and no MDX/visionOS evidence specifies one to replicate (guessing
 * one would risk the "Do not claim full gameplay rendering" instruction).
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
    /* x = layer alpha (bzTTMaterialLayerInfo_t.alpha), y = alpha-test
     * cutoff (0.0 = never discard; 0.5 for BZ_TTA_BLEND_TRANSPARENT - see
     * renderer/r_shader.c:308's matching desktop-engine
     * constant), z/w unused. */
    vec4 materialParams;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragUV = inUV;
}

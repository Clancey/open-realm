#version 450
/*
 * warcraft.vert - layer 5A static + layer 5C animated Warcraft III geoset
 * vertex shader. Transforms bz_quest_wc3_capture.c-decoded,
 * bz_quest_vk_wc3.c-uploaded geometry by a per-eye/per-instance combined
 * world*view*projection matrix supplied as a vertex push constant (see
 * bz_quest_vk_wc3_render_target() in bz_quest_vk_wc3.c, which multiplies
 * bz_quest_wc3_build_world_matrix()'s per-entity world matrix by the eye's
 * view*projection using bz_quest_pure.h's bz_quest_mat4_multiply(), matching
 * bz_quest_renderer.c's own build_mvp() convention for the procedural
 * scene). Vertex position is already axis-swapped (Z-up -> Y-up) by the
 * capture step - see bz_quest_wc3_render.h's coordinate evidence - so no
 * further conversion happens here.
 *
 * GPU skinning (layer 5C): every vertex carries a bone index/weight quad
 * (see bz_quest_wc3_render.h's bzQuestWc3Vertex_t doc comment - a genuinely
 * static geoset still gets boneIndex=[0,0,0,0]/boneWeight=[255,0,0,0],
 * resolving through uBonePalette's slot-0 permanent identity matrix, see
 * bz_quest_vk_wc3.c's create_palette_resources()), so this ONE shader path
 * draws both animated and static geometry uniformly - matching that
 * struct's own header comment. The weighted-bone-matrix sum below is
 * transcribed verbatim from the desktop engine's shared GLSL vertex shader
 * (renderer/r_shader.c:227-229's `position += uBones[boneIdx] * pos4 *
 * i_boneWeight1[i]` loop over 4 bones) - not a re-derivation. inBoneIndex is
 * VK_FORMAT_R8G8B8A8_UINT (raw, unnormalized - matches
 * renderer/r_buffer.c:87's `glVertexAttribPointer(..., GL_UNSIGNED_BYTE,
 * GL_FALSE, ...)` for i_skin1) and inBoneWeight is VK_FORMAT_R8G8B8A8_UNORM
 * (hardware-normalized 0..255 -> 0.0..1.0, matching that same file's
 * i_boneWeight1 attribute, `GL_TRUE`) - see bz_quest_vk_wc3.c's
 * create_pipeline_variant() vertex-attribute setup. uFirstBoneLookupIndex is
 * always 0.0 for MDX (r_mdx_load.c:270-271 - only nonzero for other games
 * sharing this same desktop shader), so boneIdx = inBoneIndex[i] directly,
 * with no offset to add.
 *
 * Deliberately unlit (no normal transform/lighting term), matching this
 * project's existing tabletop_frag.frag convention for the Quest renderer
 * (see that shader's own "Unlit passthrough" comment) - this slice's task
 * scope is animation/material correctness, not a lighting model, and no
 * MDX/visionOS evidence specifies one to replicate (guessing one would risk
 * the "Do not claim full gameplay rendering" instruction).
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
    /* x = layer alpha (bzTTMaterialLayerInfo_t.alpha) already multiplied by
     * this frame's geoset alpha (GEOA/KGAO - see bz_quest_vk_wc3.c's
     * draw_layer() materialParams comment; folded in CPU-side, no separate
     * uniform needed here), y = alpha-test cutoff (0.0 = never discard; 0.5
     * for BZ_TTA_BLEND_TRANSPARENT - see renderer/r_shader.c:308's matching
     * desktop-engine constant), z/w unused. */
    vec4 materialParams;
} pc;

/* set 1 binding 0 - the shared bone-palette dynamic-offset UBO (see
 * bzQuestVkWc3_t's paletteDescriptorSetLayout doc comment); one draw call
 * binds this same descriptor set with a different dynamic offset to select
 * which model/geoset's already-posed palette to read. */
layout(set = 1, binding = 0) uniform BonePalette {
    mat4 bones[128]; /* BZ_QUEST_WC3_MAX_MATRIX_PALETTE - keep these two constants in lockstep */
} uBonePalette;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in uvec4 inBoneIndex;
layout(location = 3) in vec4 inBoneWeight;

layout(location = 0) out vec2 fragUV;

void main() {
    vec4 pos4 = vec4(inPosition, 1.0);
    vec4 skinned = vec4(0.0);
    for (int i = 0; i < 4; i++) {
        skinned += (uBonePalette.bones[inBoneIndex[i]] * pos4) * inBoneWeight[i];
    }
    gl_Position = pc.mvp * skinned;
    fragUV = inUV;
}

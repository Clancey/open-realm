#version 450
/*
 * tabletop.vert - layer-3 test-scene vertex shader. Transforms the
 * bz_quest_scene.c checkerboard-table/proxy-cube geometry by a per-eye
 * model-view-projection matrix supplied as a push constant (see
 * bz_quest_vk_render_target() in bz_quest_vk.c, which rebuilds this matrix
 * every frame from the tracked head pose via bz_quest_pure.c's
 * bz_quest_pose_to_view_matrix()/bz_quest_fov_projection_vk()).
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
}

#version 450
/*
 * warcraft_particle.vert - layer 9 PRE2 particle-emitter billboard vertex
 * shader. Mirrors renderer/r_particles.c's vs_particle GLSL shader exactly
 * (see that file's own header comment - the desktop engine's own reference
 * implementation for this exact billboard technique): the camera-facing
 * "left"/"up" basis is derived directly from the view*projection matrix's
 * own first two rows (NOT a separate inverse-view uniform) - this is
 * desktop's own established, working technique for this renderer, not
 * re-derived or improved on here. `viewProj` is this eye's board-folded
 * matrix (bz_quest_renderer.c's mvpBoard - the same composed space models/
 * terrain/fog use), so a particle's CPU-computed world-space position
 * (bz_quest_wc3_particles.c's kinematics) places correctly among them.
 */

layout(push_constant) uniform PushConsts {
    mat4 viewProj;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in float inSize;
layout(location = 3) in vec4 inUvRect; /* u0, v0, u1, v1 - atlas sub-rect, [0,1] normalized */
layout(location = 4) in vec2 inAxis;   /* 0.0 or 1.0 - which corner of the billboard quad */

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    mat4 m = pc.viewProj;
    vec3 left = normalize(vec3(m[0][0], m[1][0], m[2][0])) * inSize;
    vec3 up = normalize(vec3(m[0][1], m[1][1], m[2][1])) * inSize;
    mat3 bbMat = mat3(left, up, inPosition);
    vec3 pos = bbMat * vec3(inAxis - vec2(0.5), 1.0);
    gl_Position = pc.viewProj * vec4(pos, 1.0);
    fragColor = inColor;
    fragUV = mix(inUvRect.xy, inUvRect.zw, inAxis);
}

#version 450
/*
 * terrain.vert - layer 5B static Warcraft III terrain vertex shader.
 * Terrain chunk vertices already live in the final tabletop space built by
 * bz_quest_wc3_terrain.c's point(x,z,height) reproduction of the visionOS
 * builder, so this shader applies only the per-eye view*projection matrix.
 * Deliberately unlit: layer 5B's scope is geometry/layer/water-alpha
 * correctness, not lighting.
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
    /* Fragment-stage-only (not read here) - see terrain_frag.frag's own doc comment; matches
     * warcraft_vert.vert's own "declare the whole struct on both stages" convention. */
    vec4 coverageParams;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}

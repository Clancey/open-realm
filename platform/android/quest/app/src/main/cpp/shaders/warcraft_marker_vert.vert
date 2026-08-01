#version 450
/*
 * warcraft_marker.vert - layer 5D selection-marker vertex shader.
 *
 * The procedural annulus mesh is authored around the origin on the X/Z plane
 * with unit outer radius 1.0, so the CPU's world matrix only needs a uniform
 * scale equal to the authoritative bzTTEntity_t.radius and a translated center.
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
}

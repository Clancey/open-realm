#version 450
/*
 * warcraft_marker.frag - layer 5D selection-marker fragment shader.
 * Unlit, authoritative tint only.
 */

layout(push_constant) uniform PushConsts {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = pc.tint;
}

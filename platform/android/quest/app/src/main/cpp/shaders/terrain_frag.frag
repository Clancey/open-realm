#version 450
/*
 * terrain.frag - layer 5B static Warcraft III terrain fragment shader.
 * Samples the bound terrain texture and modulates by per-vertex color so
 * water can carry authoritatively-derived corner alpha while ground/cliff
 * vertices stay opaque white.
 */

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTexture, fragUV) * fragColor;
}

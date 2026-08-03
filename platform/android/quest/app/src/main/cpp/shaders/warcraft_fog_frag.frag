#version 450
/*
 * warcraft_fog.frag - layer 5D fog-of-war overlay fragment shader.
 *
 * Samples the Quest R8_UNORM fog texture packed from the authoritative visible/
 * explored planes using the desktop client's exact 0/128/255 rule
 * (client/cl_parse.c:409). That maps cleanly to alpha = 1.0 - sample:
 *   255 -> fully transparent (visible)
 *   128 -> ~0.5 alpha darkening (explored, not visible)
 *   0   -> fully black (unseen)
 */

layout(set = 0, binding = 0) uniform sampler2D uFogMask;

layout(push_constant) uniform PushConsts {
    mat4 viewProj;
    vec4 bounds;
    vec4 fogParams; /* width, height, cellSize(64), unused */
} pc;

layout(location = 0) in vec2 fragWorld;
layout(location = 0) out vec4 outColor;

void main() {
    float cellX = floor((fragWorld.x - pc.bounds.x) / pc.fogParams.z);
    float cellY = floor((fragWorld.y - pc.bounds.y) / pc.fogParams.z);
    if (cellX < 0.0 || cellY < 0.0 || cellX >= pc.fogParams.x || cellY >= pc.fogParams.y) {
        outColor = vec4(0.0);
        return;
    }
    vec2 uv = vec2((cellX + 0.5) / pc.fogParams.x, (cellY + 0.5) / pc.fogParams.y);
    float fog = texture(uFogMask, uv).r;
    outColor = vec4(0.0, 0.0, 0.0, 1.0 - fog);
}

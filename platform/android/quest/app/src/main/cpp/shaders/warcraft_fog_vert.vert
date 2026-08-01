#version 450
/*
 * warcraft_fog.vert - layer 5D fog-of-war overlay vertex shader.
 *
 * Emits one world-space ground-plane quad covering the authoritative map
 * bounds rectangle. The fragment shader converts the interpolated world X/Z
 * back into fog cell coordinates using the same 64-unit cell size
 * g_fow.c uses, so the CPU does NOT have to bake per-map UVs.
 */

layout(push_constant) uniform PushConsts {
    mat4 viewProj;
    vec4 bounds;    /* minX, minY(engine north -> target Z), maxX, maxY */
    vec4 fogParams; /* width, height, cellSize(64), unused */
} pc;

layout(location = 0) out vec2 fragWorld;

void main() {
    vec2 unit;
    switch (gl_VertexIndex) {
        case 0: unit = vec2(0.0, 0.0); break;
        case 1: unit = vec2(1.0, 0.0); break;
        case 2: unit = vec2(0.0, 1.0); break;
        default: unit = vec2(1.0, 1.0); break;
    }
    float worldX = mix(pc.bounds.x, pc.bounds.z, unit.x);
    float worldZ = mix(pc.bounds.y, pc.bounds.w, unit.y);
    gl_Position = pc.viewProj * vec4(worldX, 0.0, worldZ, 1.0);
    fragWorld = vec2(worldX, worldZ);
}

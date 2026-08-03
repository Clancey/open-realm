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
    vec4 transform; /* centerX, centerZ, scale, unused - shared world/tabletop
                      * transform (bz_quest_wc3_render.h): the SAME transform
                      * terrain/entities/markers use, applied here ONLY to
                      * gl_Position so the quad lands in the correct on-screen
                      * diorama space; fragWorld stays in RAW world space
                      * (the transform is affine, so barycentric
                      * interpolation of the raw varying across the
                      * transformed quad is still exact at every pixel) for
                      * the fragment shader's cell-index math, which must
                      * match bz_quest_wc3_fog_world_to_cell()'s raw-space
                      * contract unchanged. */
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
    float renderX = (worldX - pc.transform.x) * pc.transform.z;
    float renderZ = (worldZ - pc.transform.y) * pc.transform.z;
    gl_Position = pc.viewProj * vec4(renderX, 0.0, renderZ, 1.0);
    fragWorld = vec2(worldX, worldZ);
}

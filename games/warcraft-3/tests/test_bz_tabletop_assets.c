#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/common.h"
#include "platform/bridge/bz_tabletop_assets.h"
#include "platform/bridge/bz_tabletop_assets_internal.h"
#include "platform/bridge/bz_tabletop_transport.h"
#include "test_framework.h"
#include "../common/wc3_asset_path.h"
#include "wc3_tabletop_assets_internal.h"

#define FOURCC(a,b,c,d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

struct bzTTSnapshot {
    char configstrings[16][BZ_TT_MAX_CONFIGSTRING_LEN];
};

void test_assets_set_tft(bool enabled);
void test_assets_set_cliff_specific(bool enabled);
void test_assets_set_cliff_generic(bool enabled);
void test_assets_set_water_available(bool available);
unsigned test_assets_water_reads(void);
void test_assets_set_team_available(bool available);
unsigned test_assets_team_reads(void);
void test_assets_set_metadata_map(unsigned index);
void test_assets_block_reads(bool blocked);
void test_assets_wait_for_blocked_reads(unsigned count);
void test_assets_set_configstring(struct bzTTSnapshot *snapshot, uint32_t index, const char *value);
static bzTTAssetMetadata_t resolve_metadata(uint32_t class_id, bzTTAResult_t expected);

static void reset_assets(void) {
    test_assets_set_metadata_map(0);
    BZ_TTA_Shutdown();
    BZ_TTA_Init();
}

static void test_abi_and_asymmetric_blp_orientation(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *asset;
    bzTTImageInfo_t info;
    uint8_t pixels[16];
    reset_assets();
    ASSERT_EQ_INT(BZ_TTA_AbiVersion(), BZ_TABLETOP_ASSETS_ABI_VERSION);
    ASSERT_EQ_INT(BZ_TABLETOP_ASSETS_ABI_VERSION, 3);
    ASSERT_EQ_INT(BZ_TTA_CATEGORY_ITEM, 6);
    ASSERT_EQ_INT(sizeof(bzTTAssetMetadata_t), 36);
    ASSERT_EQ_INT(sizeof(bzTTImageInfo_t), 24);
    ASSERT_EQ_INT(sizeof(bzTTMaterialLayerInfo_t), 24);
    ASSERT_EQ_INT(sizeof(bzTTTerrainCorner_t), 20);
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/orientation_2x2.blp");
    asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(asset);
    ASSERT(!BZ_TTAsset_IsPlaceholder(asset));
    ASSERT(BZ_TTAsset_ImageInfo(asset, &info));
    ASSERT_EQ_INT(info.width, 2);
    ASSERT_EQ_INT(info.height, 2);
    ASSERT_EQ_INT(info.origin, BZ_TTA_ORIGIN_TOP_LEFT);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(asset, pixels, sizeof(pixels)), sizeof(pixels));
    ASSERT_EQ_INT(pixels[0], 255); ASSERT_EQ_INT(pixels[1], 0); ASSERT_EQ_INT(pixels[2], 0);
    ASSERT_EQ_INT(pixels[4], 0); ASSERT_EQ_INT(pixels[5], 255); ASSERT_EQ_INT(pixels[6], 0);
    ASSERT_EQ_INT(pixels[8], 0); ASSERT_EQ_INT(pixels[9], 0); ASSERT_EQ_INT(pixels[10], 255);
    BZ_TTAsset_Release(asset);
}

static void test_blp1_paletted_decode(void) {
    enum { HEADER = 156, PALETTE = 1024, DATA = HEADER + PALETTE };
    uint8_t file[DATA + 2];
    uint32_t value;
    bzTTAResult_t status = BZ_TTA_OK;
    bzTTAsset_t *asset;
    uint8_t pixel[4];
    memset(file, 0, sizeof(file));
    value = 0x31504c42u; memcpy(file, &value, 4);
    value = 1; memcpy(file + 4, &value, 4);
    value = 8; memcpy(file + 8, &value, 4);
    value = 1; memcpy(file + 12, &value, 4); memcpy(file + 16, &value, 4);
    value = DATA; memcpy(file + 28, &value, 4);
    value = 2; memcpy(file + 92, &value, 4);
    file[HEADER + 0] = 3; file[HEADER + 1] = 2; file[HEADER + 2] = 1; file[HEADER + 3] = 255;
    file[DATA] = 0; file[DATA + 1] = 127;
    asset = BZ_WC3_TTA_DecodeBLP(file, sizeof(file), "test.blp", NULL, &status);
    ASSERT_NOT_NULL(asset);
    ASSERT_EQ_INT(status, BZ_TTA_OK);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(asset, pixel, sizeof(pixel)), 4);
    ASSERT_EQ_INT(pixel[0], 1); ASSERT_EQ_INT(pixel[1], 2); ASSERT_EQ_INT(pixel[2], 3);
    ASSERT_EQ_INT(pixel[3], 127);
    BZ_TTAsset_Release(asset);
}

static void test_blp1_jpeg_dimension_limit(void) {
    enum { HEADER = 156 };
    uint8_t file[HEADER + 4] = { 0 };
    uint32_t value;
    bzTTAResult_t status = BZ_TTA_OK;
    value = FOURCC('B','L','P','1'); memcpy(file, &value, 4);
    value = 0; memcpy(file + 4, &value, 4);
    value = 9000; memcpy(file + 12, &value, 4);
    value = 1; memcpy(file + 16, &value, 4);
    value = sizeof(file); memcpy(file + 28, &value, 4);
    ASSERT_NULL(BZ_WC3_TTA_DecodeBLP(file, sizeof(file), "oversized-jpeg.blp", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
}

static void test_roc_tft_resolution_and_cache(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *roc, *roc_again, *tft;
    uint8_t pixels[16];
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/variant.blp");
    test_assets_set_tft(false);
    reset_assets();
    roc = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                      BZ_TTA_ASSET_IMAGE, NULL);
    roc_again = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                            BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT(roc == roc_again);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1);
    ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(roc, pixels, sizeof(pixels)), 4);
    ASSERT_EQ_INT(pixels[0], 255); ASSERT_EQ_INT(pixels[1], 255); ASSERT_EQ_INT(pixels[2], 255);
    BZ_TTAsset_Release(roc); BZ_TTAsset_Release(roc_again);
    test_assets_set_tft(true);
    reset_assets();
    tft = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                      BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(tft, pixels, sizeof(pixels)), sizeof(pixels));
    ASSERT_EQ_INT(pixels[0], 255); ASSERT_EQ_INT(pixels[1], 0);
    BZ_TTAsset_Release(tft);
}

static void test_placeholder_path_confinement_and_log_once_cache(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *missing, *again, *confined, *empty_component;
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/missing.blp");
    test_assets_set_configstring(&snapshot, 2, "../outside.blp");
    test_assets_set_configstring(&snapshot, 3, "TestUI//Textures/missing.blp");
    reset_assets();
    missing = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                          BZ_TTA_ASSET_IMAGE, NULL);
    again = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    confined = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 2,
                                           BZ_TTA_ASSET_IMAGE, NULL);
    empty_component = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 3,
                                                  BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(missing); ASSERT(missing == again); ASSERT(BZ_TTAsset_IsPlaceholder(missing));
    ASSERT_EQ_INT(BZ_TTAsset_Status(missing), BZ_TTA_ERR_NOT_FOUND);
    ASSERT(BZ_TTAsset_IsPlaceholder(confined));
    ASSERT_EQ_INT(BZ_TTAsset_Status(confined), BZ_TTA_ERR_PATH_CONFINEMENT);
    ASSERT_EQ_INT(BZ_TTAsset_Status(empty_component), BZ_TTA_ERR_PATH_CONFINEMENT);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 3); ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 3);
    BZ_TTAsset_Release(missing); BZ_TTAsset_Release(again); BZ_TTAsset_Release(confined);
    BZ_TTAsset_Release(empty_component);
}

static void test_mdx_geometry_materials_sequences_and_bounds(void) {
    struct bzTTSnapshot snapshot = { 0 };
    bzTTAssetMetadata_t metadata = {
        .category = BZ_TTA_CATEGORY_BUILDING, .class_id = 0x68746f77, .team_color = 3,
        .tint_r = 1, .tint_g = 0.5f, .tint_b = 0.25f, .tint_a = 1,
        .footprint_x = 64, .footprint_y = 96,
    };
    const bzTTAsset_t *asset;
    bzTTModelInfo_t model = { 0 };
    bzTTGeosetInfo_t geoset;
    bzTTMaterialInfo_t material;
    bzTTMaterialLayerInfo_t layer;
    bzTTModelTextureInfo_t texture;
    bzTTSequenceInfo_t sequence;
    bzTTNodeInfo_t node;
    bzTTAssetMetadata_t texture_metadata;
    const bzTTAsset_t *image;
    uint16_t indices[6];
    bzTTVec2_t uvs[4];
    reset_assets();
    test_assets_set_configstring(&snapshot, 1, "TestUI/Models/quad_sprite.mdx");
    asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_MODEL, &metadata);
    ASSERT_NOT_NULL(asset); ASSERT(!BZ_TTAsset_IsPlaceholder(asset));
    ASSERT(BZ_TTAsset_ModelInfo(asset, &model));
    ASSERT_EQ_INT(model.geoset_count, 1); ASSERT_EQ_INT(model.material_count, 1);
    ASSERT_EQ_INT(model.layer_count, 1); ASSERT_EQ_INT(model.texture_count, 1);
    ASSERT_EQ_INT(model.sequence_count, 1); ASSERT_EQ_INT(model.node_count, 1);
    ASSERT_EQ_FLOAT(model.bounds.min.x, -0.5f, 0.001f);
    ASSERT_EQ_FLOAT(model.bounds.max.x, 0.5f, 0.001f);
    ASSERT(BZ_TTAsset_GeosetInfo(asset, 0, &geoset));
    ASSERT_EQ_INT(geoset.vertex_count, 4); ASSERT_EQ_INT(geoset.uv_count, 4);
    ASSERT_EQ_INT(geoset.index_count, 6);
    ASSERT_EQ_INT(geoset.matrix_palette_count, 1); /* unanimated: single-entry palette, node 0 */
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetIndices(asset, 0, indices, 6), 6);
    ASSERT_EQ_INT(indices[5], 3);
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetUVs(asset, 0, uvs, 4), 4);
    ASSERT_EQ_FLOAT(uvs[0].x, 0.125f, 0.001f); ASSERT_EQ_FLOAT(uvs[0].y, 0.875f, 0.001f);
    ASSERT_EQ_FLOAT(uvs[1].x, 0.75f, 0.001f); ASSERT_EQ_FLOAT(uvs[1].y, 0.625f, 0.001f);
    ASSERT_EQ_FLOAT(uvs[2].x, 0.9f, 0.001f); ASSERT_EQ_FLOAT(uvs[2].y, 0.2f, 0.001f);
    ASSERT_EQ_FLOAT(uvs[3].x, 0.3f, 0.001f); ASSERT_EQ_FLOAT(uvs[3].y, 0.1f, 0.001f);
    ASSERT(BZ_TTAsset_MaterialInfo(asset, 0, &material));
    ASSERT(BZ_TTAsset_MaterialLayerInfo(asset, material.first_layer, &layer));
    ASSERT_EQ_INT(layer.texture_index, 0);
    ASSERT(BZ_TTAsset_ModelTextureInfo(asset, 0, &texture));
    ASSERT_STR_EQ(texture.identity, "TestUI/Textures/checker_8x8.blp");
    image = BZ_TTA_RegisterModelTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, asset, 0);
    ASSERT_NOT_NULL(image); ASSERT(!BZ_TTAsset_IsPlaceholder(image));
    ASSERT(BZ_TTAsset_Metadata(image, &texture_metadata));
    ASSERT_EQ_INT(texture_metadata.category, BZ_TTA_CATEGORY_BUILDING);
    ASSERT(BZ_TTAsset_SequenceInfo(asset, 0, &sequence)); ASSERT_STR_EQ(sequence.name, "Stand");
    ASSERT(BZ_TTAsset_NodeInfo(asset, 0, &node)); ASSERT_STR_EQ(node.name, "Bone_Root");
    BZ_TTAsset_Release(image); BZ_TTAsset_Release(asset);
}

/* Exercises the slice 5C animation/dynamic-material decode path against the rigged_anim
 * fixture (tools/mdxgen.c "rigged_anim" preset): a 2-bone parent/child hierarchy with a
 * globally-looping translation track and a sequence-relative rotation track, one global
 * sequence, a geoset alpha animation, and a 3-matrix-group vertex skin (single-bone +
 * blended). Expected values are hand-derived from the fixture's authored keys/groups per
 * classic MDX ReadKeyTrack/R_SetupGeosetVertexBuffer semantics, not from the production code
 * under test. */
static void test_mdx_animation_hierarchy_tracks_and_dynamic_material(void) {
    DWORD size;
    uint8_t *source;
    bzTTAResult_t status = BZ_TTA_OK;
    bzTTAsset_t *asset;
    bzTTModelInfo_t model = { 0 };
    bzTTGeosetInfo_t geoset = { 0 };
    bzTTNodeInfo_t root, child;
    bzTTTrackInfo_t track;
    bzTTGeosetAnimInfo_t anim = { 0 };
    uint32_t duration = 0, palette[2] = { 0 };
    bzTTVec3Key_t vec3_keys[2];
    bzTTQuatKey_t quat_keys[2];
    bzTTFloatKey_t alpha_keys[2];
    bzTTVertexSkin_t skin[4];

    source = FS_ReadFile("TestUI/Models/rigged_anim.mdx", &size);
    ASSERT_NOT_NULL(source);
    asset = BZ_WC3_TTA_DecodeMDX(source, size, "rigged_anim.mdx", NULL, &status);
    FS_FreeFile(source);
    ASSERT_NOT_NULL(asset); ASSERT_EQ_INT(status, BZ_TTA_OK);

    ASSERT(BZ_TTAsset_ModelInfo(asset, &model));
    ASSERT_EQ_INT(model.node_count, 2);
    ASSERT_EQ_INT(model.global_sequence_count, 1);
    ASSERT(BZ_TTAsset_GlobalSequenceInfo(asset, 0, &duration));
    ASSERT_EQ_INT(duration, 500);
    ASSERT(!BZ_TTAsset_GlobalSequenceInfo(asset, 1, &duration)); /* out of range */

    ASSERT(BZ_TTAsset_NodeInfo(asset, 0, &root));
    ASSERT_STR_EQ(root.name, "Bone_Root");
    ASSERT_EQ_INT(root.object_id, 0); ASSERT_EQ_INT(root.parent_id, 0xFFFFFFFFu);
    ASSERT_EQ_FLOAT(root.pivot.x, 0.0f, 0.001f); ASSERT_EQ_FLOAT(root.pivot.z, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(root.initial_translation.z, 0.0f, 0.001f); /* rest pose = first KGTR key */
    ASSERT_EQ_FLOAT(root.initial_rotation_w, 1.0f, 0.001f); /* no KGRT: identity default */

    ASSERT(BZ_TTAsset_NodeInfo(asset, 1, &child));
    ASSERT_STR_EQ(child.name, "Bone_Child");
    ASSERT_EQ_INT(child.object_id, 1); ASSERT_EQ_INT(child.parent_id, 0);
    ASSERT_EQ_FLOAT(child.pivot.z, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(child.initial_scale.x, 1.0f, 0.001f); /* no KGSC: identity default */
    ASSERT_EQ_FLOAT(child.initial_rotation_w, 1.0f, 0.001f); /* rest pose = first KGRT key (identity) */

    /* Bone_Root translation: global-sequence track, 2 linear keys 0->500ms, (0,0,0)->(0,0,2). */
    ASSERT(BZ_TTAsset_NodeTrackInfo(asset, 0, BZ_TTA_NODE_TRANSLATION, &track));
    ASSERT_EQ_INT(track.key_count, 2); ASSERT_EQ_INT(track.interp, BZ_TTA_INTERP_LINEAR);
    ASSERT_EQ_INT(track.global_sequence, 0);
    ASSERT_EQ_INT(BZ_TTAsset_CopyNodeTranslationKeys(asset, 0, vec3_keys, 2), 2);
    ASSERT_EQ_INT(vec3_keys[0].time_msec, 0); ASSERT_EQ_FLOAT(vec3_keys[0].value.z, 0.0f, 0.001f);
    ASSERT_EQ_INT(vec3_keys[1].time_msec, 500); ASSERT_EQ_FLOAT(vec3_keys[1].value.z, 2.0f, 0.001f);
    /* Bone_Root has no rotation/scale track. */
    ASSERT(BZ_TTAsset_NodeTrackInfo(asset, 0, BZ_TTA_NODE_ROTATION, &track));
    ASSERT_EQ_INT(track.key_count, 0);

    /* Bone_Child rotation: sequence-relative track, 2 linear keys 0->2000ms, identity->90deg-Z. */
    ASSERT(BZ_TTAsset_NodeTrackInfo(asset, 1, BZ_TTA_NODE_ROTATION, &track));
    ASSERT_EQ_INT(track.key_count, 2); ASSERT_EQ_INT(track.global_sequence, BZ_TTA_NO_GLOBAL_SEQUENCE);
    ASSERT_EQ_INT(BZ_TTAsset_CopyNodeRotationKeys(asset, 1, quat_keys, 2), 2);
    ASSERT_EQ_INT(quat_keys[0].time_msec, 0); ASSERT_EQ_FLOAT(quat_keys[0].value.w, 1.0f, 0.001f);
    ASSERT_EQ_INT(quat_keys[1].time_msec, 2000);
    ASSERT_EQ_FLOAT(quat_keys[1].value.z, 0.70710678f, 0.0001f);
    ASSERT_EQ_FLOAT(quat_keys[1].value.w, 0.70710678f, 0.0001f);

    /* Geoset alpha animation: track present, fades 1.0 -> 0.25 over [0,2000). */
    ASSERT(BZ_TTAsset_GeosetAnimInfo(asset, 0, &anim));
    ASSERT(anim.has_alpha_track);
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetAlphaKeys(asset, 0, alpha_keys, 2), 2);
    ASSERT_EQ_INT(alpha_keys[0].time_msec, 0); ASSERT_EQ_FLOAT(alpha_keys[0].value, 1.0f, 0.001f);
    ASSERT_EQ_INT(alpha_keys[1].time_msec, 2000); ASSERT_EQ_FLOAT(alpha_keys[1].value, 0.25f, 0.001f);

    /* Matrix palette: Bone_Root first (slot 0), Bone_Child second (slot 1), dedup by node index. */
    ASSERT(BZ_TTAsset_GeosetInfo(asset, 0, &geoset));
    ASSERT_EQ_INT(geoset.matrix_palette_count, 2);
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetMatrixPalette(asset, 0, palette, 2), 2);
    ASSERT_EQ_INT(palette[0], 0); ASSERT_EQ_INT(palette[1], 1);

    /* Vertex skin: v0,v1 bound solely to bone0 (slot 0, weight 255); v2 solely to bone1
     * (slot 1, weight 255); v3 blended bone0+bone1 (slots [1,0], weights [128,127] after
     * the top-weighted insertion sort — the equal-split-then-renormalize algorithm already
     * sums to 255 here with no rounding remainder). */
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetVertexSkin(asset, 0, skin, 4), 4);
    ASSERT_EQ_INT(skin[0].bone_index[0], 0); ASSERT_EQ_INT(skin[0].bone_weight[0], 255);
    ASSERT_EQ_INT(skin[1].bone_index[0], 0); ASSERT_EQ_INT(skin[1].bone_weight[0], 255);
    ASSERT_EQ_INT(skin[2].bone_index[0], 1); ASSERT_EQ_INT(skin[2].bone_weight[0], 255);
    ASSERT_EQ_INT(skin[3].bone_index[0], 1); ASSERT_EQ_INT(skin[3].bone_weight[0], 128);
    ASSERT_EQ_INT(skin[3].bone_index[1], 0); ASSERT_EQ_INT(skin[3].bone_weight[1], 127);
    ASSERT_EQ_INT(skin[3].bone_weight[0] + skin[3].bone_weight[1], 255);

    BZ_TTAsset_Release(asset);
}

static void test_desktop_model_identity_fallbacks(void) {
    static const char *identities[] = {
        "Doodads\\LordaeronSummer\\Plants\\Wheat\\Wheat0.mdx",
        "Doodads\\Ashenvale\\Plants\\AshenBush0\\AshenBush00.mdx",
        "Doodads\\LordaeronSummer\\Props\\Cage\\Cage0.mdx",
        "Doodads\\LordaeronSummer\\Props\\TorchHuman\\TorchHuman0.mdx",
        "TestUI/Models/quad_sprite.mdl",
    };
    static const char *resolved[] = {
        "Doodads\\LordaeronSummer\\Plants\\Wheat\\Wheat.mdx",
        "Doodads\\Ashenvale\\Plants\\AshenBush0\\AshenBush0.mdx",
        "Doodads\\LordaeronSummer\\Props\\Cage\\Cage.mdx",
        "Doodads\\LordaeronSummer\\Props\\TorchHuman\\TorchHuman.mdx",
        "TestUI/Models/quad_sprite.mdx",
    };
    struct bzTTSnapshot snapshot = { 0 };
    char identity[BZ_TTA_MAX_IDENTITY];
    reset_assets();
    for (uint32_t i = 0; i < sizeof(identities) / sizeof(*identities); i++) {
        const bzTTAsset_t *asset, *again;
        test_assets_set_configstring(&snapshot, i + 1, identities[i]);
        asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, i + 1,
                                             BZ_TTA_ASSET_MODEL, NULL);
        ASSERT_NOT_NULL(asset);
        ASSERT(!BZ_TTAsset_IsPlaceholder(asset));
        ASSERT(BZ_TTAsset_Identity(asset, identity, sizeof(identity)));
        ASSERT_STR_EQ(identity, resolved[i]);
        again = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, i + 1,
                                            BZ_TTA_ASSET_MODEL, NULL);
        ASSERT(asset == again);
        BZ_TTAsset_Release(asset); BZ_TTAsset_Release(again);
    }
}

typedef struct {
    const char **identities;
    uint32_t count;
} modelProbe_t;

static bool model_probe(const char *identity, void *opaque) {
    const modelProbe_t *probe = opaque;
    for (uint32_t i = 0; i < probe->count; i++)
        if (!strcmp(identity, probe->identities[i])) return true;
    return false;
}

static void test_spawn_model_variation_resolution(void) {
    static const char *authored[] = {
        "Doodads\\Plants\\Wheat\\Wheat.mdx",
        "Doodads\\Plants\\AshenBush0\\AshenBush0.mdx",
        "Doodads\\Props\\Rocks\\Rocks2.mdx",
        "Doodads\\Props\\Cage\\Cage.mdx",
    };
    modelProbe_t probe = { .identities = authored, .count = sizeof(authored) / sizeof(*authored) };
    char identity[512];
    wc3SpawnModelResolve_t resolve = {
        .probe = model_probe, .context = &probe, .out = identity, .cap = sizeof(identity),
    };
    resolve.dir = "Doodads\\Plants"; resolve.file = "Wheat"; resolve.variation = 0;
    resolve.num_variations = 1;
    ASSERT(wc3_resolve_spawn_model_identity(&resolve));
    ASSERT_STR_EQ(identity, authored[0]);
    resolve.file = "AshenBush0"; resolve.variation = 0;
    ASSERT(wc3_resolve_spawn_model_identity(&resolve));
    ASSERT_STR_EQ(identity, authored[1]);
    resolve.dir = "Doodads\\Props"; resolve.file = "Rocks"; resolve.variation = 2;
    resolve.num_variations = 3;
    ASSERT(wc3_resolve_spawn_model_identity(&resolve));
    ASSERT_STR_EQ(identity, authored[2]);
    resolve.file = "Cage"; resolve.variation = 0; resolve.num_variations = 2;
    ASSERT(wc3_resolve_spawn_model_identity(&resolve));
    ASSERT_STR_EQ(identity, authored[3]);
    resolve.file = "Missing"; resolve.num_variations = 1;
    ASSERT(!wc3_resolve_spawn_model_identity(&resolve));
    ASSERT_STR_EQ(identity, "Doodads\\Props\\Missing\\Missing.mdx");
}

static void test_model_identity_output_bounds(void) {
    static const char *authored[] = { "LongModel.mdx", "Model.mdx" };
    modelProbe_t probe = { .identities = authored, .count = sizeof(authored) / sizeof(*authored) };
    char small[8] = "stale", long_file[600], output[512] = "stale";
    wc3ModelResolve_t model = {
        .identity = authored[0], .probe = model_probe, .context = &probe, .out = small, .cap = sizeof(small),
    };
    wc3ModelFallback_t fallback = {
        .identity = "LongModel0.mdx", .fallback = BZ_WC3_MODEL_STRIP_VARIATION,
        .out = small, .cap = sizeof(small),
    };
    wc3SpawnModelResolve_t spawn = {
        .file = long_file, .num_variations = 1, .probe = model_probe, .context = &probe,
        .out = output, .cap = sizeof(output),
    };
    ASSERT(!wc3_resolve_model_identity(&model)); ASSERT_STR_EQ(small, "");
    memcpy(small, "stale", 6);
    ASSERT(!wc3_model_fallback_identity(&fallback)); ASSERT_STR_EQ(small, "");
    memset(long_file, 'A', sizeof(long_file) - 1); long_file[sizeof(long_file) - 1] = 0;
    ASSERT(!wc3_resolve_spawn_model_identity(&spawn)); ASSERT_STR_EQ(output, "");
}

/* Duplicate retail-shaped records inside MTLS/GEOS chunks to exercise inclusive record boundaries. */
static uint8_t *duplicate_mdx_records(const uint8_t *src, size_t size, size_t *out_size) {
    size_t extra = 0, pos = 4, dst_pos = 4;
    uint8_t *dst;
    while (pos + 8 <= size) {
        uint32_t tag, bytes;
        memcpy(&tag, src + pos, 4); memcpy(&bytes, src + pos + 4, 4);
        if ((size_t)bytes > size - pos - 8) return NULL;
        if (tag == FOURCC('M','T','L','S') || tag == FOURCC('G','E','O','S')) extra += bytes;
        pos += 8 + bytes;
    }
    if (pos != size || extra > SIZE_MAX - size || !(dst = malloc(size + extra))) return NULL;
    memcpy(dst, src, 4); pos = 4;
    while (pos < size) {
        uint32_t tag, bytes, output_bytes;
        memcpy(&tag, src + pos, 4); memcpy(&bytes, src + pos + 4, 4);
        output_bytes = bytes;
        if (tag == FOURCC('M','T','L','S') || tag == FOURCC('G','E','O','S')) output_bytes *= 2;
        memcpy(dst + dst_pos, &tag, 4); memcpy(dst + dst_pos + 4, &output_bytes, 4);
        memcpy(dst + dst_pos + 8, src + pos + 8, bytes);
        if (output_bytes != bytes) memcpy(dst + dst_pos + 8 + bytes, src + pos + 8, bytes);
        pos += 8 + bytes; dst_pos += 8 + output_bytes;
    }
    *out_size = dst_pos;
    return dst;
}

typedef struct {
    size_t chunk_pos, record_start, record_end, uv_start;
    uint32_t chunk_size, record_size;
} mdxUVLayout_t;

/* Locate the fixture's first retail-order UV pair without trusting its containing sizes. */
static bool first_geoset_uv_layout(const uint8_t *src, size_t size, mdxUVLayout_t *layout) {
    uint32_t tag = 0;
    memset(layout, 0, sizeof(*layout)); layout->chunk_pos = 4;
    while (layout->chunk_pos + 8 <= size) {
        memcpy(&tag, src + layout->chunk_pos, 4);
        memcpy(&layout->chunk_size, src + layout->chunk_pos + 4, 4);
        if ((size_t)layout->chunk_size > size - layout->chunk_pos - 8) return false;
        if (tag == FOURCC('G','E','O','S')) break;
        layout->chunk_pos += 8 + layout->chunk_size;
    }
    if (layout->chunk_pos + 12 > size || tag != FOURCC('G','E','O','S') || layout->chunk_size < 52)
        return false;
    layout->record_start = layout->chunk_pos + 8;
    memcpy(&layout->record_size, src + layout->record_start, 4);
    if (layout->record_size > layout->chunk_size || layout->record_size < 52) return false;
    layout->record_end = layout->record_start + layout->record_size;
    layout->uv_start = layout->record_end - 48;
    return layout->record_end <= size && !memcmp(src + layout->uv_start, "UVAS", 4) &&
           !memcmp(src + layout->uv_start + 8, "UVBS", 4);
}

/* Remove the fixture's trailing retail-order UVAS/UVBS pair to cover valid textureless geosets. */
static uint8_t *strip_first_geoset_uvs(const uint8_t *src, size_t size, size_t *out_size) {
    mdxUVLayout_t layout;
    uint32_t reduced;
    uint8_t *out;
    if (!first_geoset_uv_layout(src, size, &layout) || !(out = malloc(size - 48))) return NULL;
    memcpy(out, src, layout.uv_start);
    memcpy(out + layout.uv_start, src + layout.record_end, size - layout.record_end);
    reduced = layout.chunk_size - 48; memcpy(out + layout.chunk_pos + 4, &reduced, 4);
    reduced = layout.record_size - 48; memcpy(out + layout.record_start, &reduced, 4);
    *out_size = size - 48;
    return out;
}

static void test_mdx_multiple_inclusive_records(void) {
    DWORD size;
    uint8_t *single = FS_ReadFile("TestUI/Models/quad_sprite.mdx", &size);
    size_t multi_size;
    uint8_t *multi;
    bzTTAResult_t status = BZ_TTA_OK;
    bzTTAsset_t *asset;
    bzTTModelInfo_t model = { 0 };
    bzTTGeosetInfo_t geoset = { 0 };
    bzTTVec2_t uvs[4];
    ASSERT_NOT_NULL(single);
    multi = duplicate_mdx_records(single, size, &multi_size);
    FS_FreeFile(single);
    ASSERT_NOT_NULL(multi);
    asset = BZ_WC3_TTA_DecodeMDX(multi, multi_size, "multi.mdx", NULL, &status);
    free(multi);
    ASSERT_NOT_NULL(asset);
    ASSERT_EQ_INT(status, BZ_TTA_OK);
    ASSERT(BZ_TTAsset_ModelInfo(asset, &model));
    ASSERT_EQ_INT(model.geoset_count, 2); ASSERT_EQ_INT(model.material_count, 2);
    ASSERT_EQ_INT(model.layer_count, 2);
    ASSERT(BZ_TTAsset_GeosetInfo(asset, 1, &geoset)); ASSERT_EQ_INT(geoset.uv_count, 4);
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetUVs(asset, 1, uvs, 4), 4);
    ASSERT_EQ_FLOAT(uvs[0].x, 0.125f, 0.001f); ASSERT_EQ_FLOAT(uvs[3].y, 0.1f, 0.001f);
    BZ_TTAsset_Release(asset);
}

static void test_mdx_post_mats_uvs_and_absent_inverse(void) {
    DWORD size;
    uint8_t *source = FS_ReadFile("TestUI/Models/quad_sprite.mdx", &size), *without_uvs, *mutated;
    size_t without_uvs_size, partial_size;
    mdxUVLayout_t layout;
    uint32_t value;
    bzTTAResult_t status = BZ_TTA_OK;
    bzTTAsset_t *asset;
    bzTTGeosetInfo_t geoset = { 0 };
    bzTTVec2_t uvs[4];
    ASSERT_NOT_NULL(source);
    ASSERT(first_geoset_uv_layout(source, size, &layout));
    asset = BZ_WC3_TTA_DecodeMDX(source, size, "post-mats-uv.mdx", NULL, &status);
    ASSERT_NOT_NULL(asset); ASSERT_EQ_INT(status, BZ_TTA_OK);
    ASSERT(BZ_TTAsset_GeosetInfo(asset, 0, &geoset)); ASSERT_EQ_INT(geoset.uv_count, 4);
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetUVs(asset, 0, uvs, 4), 4);
    ASSERT_EQ_FLOAT(uvs[0].x, 0.125f, 0.001f); ASSERT_EQ_FLOAT(uvs[0].y, 0.875f, 0.001f);
    ASSERT_EQ_FLOAT(uvs[1].x, 0.75f, 0.001f); ASSERT_EQ_FLOAT(uvs[1].y, 0.625f, 0.001f);
    ASSERT_EQ_FLOAT(uvs[2].x, 0.9f, 0.001f); ASSERT_EQ_FLOAT(uvs[2].y, 0.2f, 0.001f);
    ASSERT_EQ_FLOAT(uvs[3].x, 0.3f, 0.001f); ASSERT_EQ_FLOAT(uvs[3].y, 0.1f, 0.001f);
    BZ_TTAsset_Release(asset);
    without_uvs = strip_first_geoset_uvs(source, size, &without_uvs_size);
    ASSERT_NOT_NULL(without_uvs);
    asset = BZ_WC3_TTA_DecodeMDX(
        without_uvs, without_uvs_size, "post-mats-no-uv.mdx", NULL, &status);
    free(without_uvs);
    ASSERT_NOT_NULL(asset); ASSERT_EQ_INT(status, BZ_TTA_OK);
    ASSERT(BZ_TTAsset_GeosetInfo(asset, 0, &geoset)); ASSERT_EQ_INT(geoset.uv_count, 0);
    BZ_TTAsset_Release(asset);

    partial_size = size - sizeof(bzTTVec2_t); mutated = malloc(partial_size);
    ASSERT_NOT_NULL(mutated);
    memcpy(mutated, source, layout.record_end - sizeof(bzTTVec2_t));
    memcpy(mutated + layout.record_end - sizeof(bzTTVec2_t), source + layout.record_end,
           size - layout.record_end);
    value = 3; memcpy(mutated + layout.uv_start + 12, &value, 4);
    value = layout.chunk_size - sizeof(bzTTVec2_t); memcpy(mutated + layout.chunk_pos + 4, &value, 4);
    value = layout.record_size - sizeof(bzTTVec2_t); memcpy(mutated + layout.record_start, &value, 4);
    ASSERT_NULL(BZ_WC3_TTA_DecodeMDX(mutated, partial_size, "post-mats-partial-uv.mdx", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED); free(mutated);

    mutated = malloc(size); ASSERT_NOT_NULL(mutated); memcpy(mutated, source, size);
    value = 5; memcpy(mutated + layout.uv_start + 12, &value, 4);
    ASSERT_NULL(BZ_WC3_TTA_DecodeMDX(mutated, size, "post-mats-truncated-uv.mdx", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
    memcpy(mutated, source, size); memcpy(mutated + layout.uv_start, "JUNK", 4);
    ASSERT_NULL(BZ_WC3_TTA_DecodeMDX(mutated, size, "post-mats-unknown-tag.mdx", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
    free(mutated); FS_FreeFile(source);
}

static void test_mdx_zero_counted_array_is_malformed(void) {
    uint8_t file[36] = { 'M','D','L','X', 'V','E','R','S', 4,0,0,0, 32,3,0,0,
                         'G','E','O','S', 12,0,0,0, 12,0,0,0, 'V','R','T','X', 0,0,0,0 };
    bzTTAResult_t status = BZ_TTA_OK;
    ASSERT_NULL(BZ_WC3_TTA_DecodeMDX(file, sizeof(file), "zero.mdx", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
}

static void test_malformed_blp_and_mdx_bounds(void) {
    uint8_t malformed_blp[32] = { 'B', 'L', 'P', '2' };
    uint8_t malformed_mdx[12] = { 'M', 'D', 'L', 'X', 'G', 'E', 'O', 'S', 0xff, 0xff, 0xff, 0x7f };
    bzTTAResult_t status = BZ_TTA_OK;
    ASSERT_NULL(BZ_WC3_TTA_DecodeBLP(malformed_blp, sizeof(malformed_blp), "bad.blp", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
    status = BZ_TTA_OK;
    ASSERT_NULL(BZ_WC3_TTA_DecodeMDX(malformed_mdx, sizeof(malformed_mdx), "bad.mdx", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
}

static void make_terrain(uint32_t width, uint32_t height, uintptr_t salt) {
    LPWAR3MAP map = calloc(1, sizeof(*map));
    map->width = width; map->height = height; map->tileset = 'L';
    map->center = (VECTOR2){ -2048, -1024 };
    map->num_grounds = 2; map->num_cliffs = 1;
    map->grounds = calloc(2, sizeof(DWORD)); map->cliffs = calloc(1, sizeof(DWORD));
    map->vertices = calloc((size_t)width * height, sizeof(WAR3MAPVERTEX));
    map->grounds[0] = FOURCC('L','d','r','t'); map->grounds[1] = FOURCC('L','g','r','s');
    map->cliffs[0] = FOURCC('C','L','i','f');
    for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) {
        LPWAR3MAPVERTEX corner = (LPWAR3MAPVERTEX)map->vertices + y * width + x;
        corner->accurate_height = (USHORT)(0x2000 + x + y + salt);
        corner->waterlevel = (USHORT)(0x2000 + 8);
        corner->ground = (BYTE)((x + y) & 1); corner->groundVariation = (BYTE)(x & 31);
        corner->cliff = 0; corner->cliffVariation = (BYTE)(y & 7); corner->level = (BYTE)(y & 3);
        corner->water = x == 1 && y == 1; corner->ramp = x == 2 && y == 2;
    }
    world.map = map;
}

static void free_terrain(void) {
    if (!world.map) return;
    free(world.map->grounds); free(world.map->cliffs); free(world.map->vertices); free(world.map);
    world.map = NULL;
}

static void test_terrain_dimensions_corners_water_cliffs_and_chunks(void) {
    const bzTTTerrain_t *terrain, *again;
    bzTTTerrainInfo_t info;
    bzTTTerrainCorner_t corner;
    bzTTTerrainTextureInfo_t texture;
    uint32_t ground, cliff;
    make_terrain(34, 34, 0);
    reset_assets();
    BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain(); again = BZ_TTA_LatestTerrain();
    ASSERT_NOT_NULL(terrain); ASSERT(terrain == again); ASSERT(BZ_TTTerrain_Info(terrain, &info));
    ASSERT_EQ_INT(info.width, 34); ASSERT_EQ_INT(info.tile_width, 33);
    ASSERT_EQ_INT(info.chunk_tiles, 32); ASSERT_EQ_INT(info.chunk_count_x, 2);
    ASSERT_EQ_FLOAT(info.min_x, -2048, 0.001f);
    ASSERT(BZ_TTTerrain_Corner(terrain, 1, 1, &corner));
    ASSERT(corner.flags & BZ_TTA_TERRAIN_WATER);
    ASSERT_EQ_INT(corner.ground_id, FOURCC('L','d','r','t'));
    ASSERT_EQ_FLOAT(corner.water_height, -78, 0.001f);
    ASSERT(BZ_TTTerrain_GroundType(terrain, 1, &ground)); ASSERT_EQ_INT(ground, FOURCC('L','g','r','s'));
    ASSERT(BZ_TTTerrain_CliffType(terrain, 0, &cliff)); ASSERT_EQ_INT(cliff, FOURCC('C','L','i','f'));
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND), 2);
    ASSERT(BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND, 1, &texture));
    ASSERT_EQ_INT(texture.type_index, 1); ASSERT_EQ_INT(texture.type_id, FOURCC('L','g','r','s'));
    ASSERT_EQ_INT(texture.corner_count, 578);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, 0), 0);
    ASSERT(!BZ_TTTerrain_ReferencedTexture(terrain, 0, 0, &texture));
    ASSERT(!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND, 0, NULL));
    BZ_TTTerrain_Release(terrain); BZ_TTTerrain_Release(again);
    free_terrain();
}

static void test_malformed_terrain_type_index(void) {
    const bzTTTerrain_t *terrain;
    make_terrain(4, 4, 0);
    ((LPWAR3MAPVERTEX)world.map->vertices)[0].ground = 7;
    reset_assets(); BZ_TTA_PublishTerrainFromGame(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain();
    ASSERT_NULL(terrain);
    free_terrain();
}

static void test_human02_shape_no_cliff_sentinel(void) {
    const bzTTAsset_t *unused;
    const bzTTTerrain_t *terrain;
    bzTTTerrainCorner_t corner;
    bzTTTerrainTextureInfo_t texture;
    make_terrain(129, 129, 0);
    free(world.map->cliffs); world.map->num_cliffs = 3;
    world.map->cliffs = calloc(world.map->num_cliffs, sizeof(DWORD));
    world.map->cliffs[0] = FOURCC('C','L','d','i');
    world.map->cliffs[1] = FOURCC('C','L','g','r');
    world.map->cliffs[2] = FOURCC('C','L','n','o');
    ((LPWAR3MAPVERTEX)world.map->vertices)[0].cliff = 1;
    ((LPWAR3MAPVERTEX)world.map->vertices)[2732].cliff = 0x0f;
    reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain();
    ASSERT_NOT_NULL(terrain);
    ASSERT(BZ_TTTerrain_Corner(terrain, 23, 21, &corner));
    ASSERT(corner.flags & BZ_TTA_TERRAIN_NO_CLIFF);
    ASSERT_EQ_INT(corner.cliff_id, 0);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF), 2);
    ASSERT(BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, 0, &texture));
    ASSERT_EQ_INT(texture.type_index, 0); ASSERT_EQ_INT(texture.type_id, FOURCC('C','L','d','i'));
    ASSERT_EQ_INT(texture.corner_count, 16639);
    ASSERT(BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, 1, &texture));
    ASSERT_EQ_INT(texture.type_index, 1); ASSERT_EQ_INT(texture.type_id, FOURCC('C','L','g','r'));
    ASSERT_EQ_INT(texture.corner_count, 1);
    ASSERT(!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, 2, &texture));
    unused = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                           BZ_TTA_TERRAIN_TEXTURE_CLIFF, 2);
    ASSERT_NULL(unused); ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 0);
    BZ_TTTerrain_Release(terrain);
    free_terrain();

    make_terrain(4, 4, 0);
    ((LPWAR3MAPVERTEX)world.map->vertices)[5].cliff = 14;
    reset_assets(); BZ_TTA_PublishTerrainFromGame();
    ASSERT_NULL(BZ_TTA_LatestTerrain());
    free_terrain();
}

static void assert_asset_identity(const bzTTAsset_t *asset, const char *expected) {
    char identity[BZ_TTA_MAX_IDENTITY];
    ASSERT(BZ_TTAsset_Identity(asset, identity, sizeof(identity)));
    ASSERT_STR_EQ(identity, expected);
}

static void test_terrain_texture_resolution_roc_tft_and_fallback(void) {
    const bzTTTerrain_t *terrain;
    const bzTTAsset_t *ground, *again, *cliff;
    bzTTAssetMetadata_t metadata;
    bzTTTerrainTextureInfo_t texture;
    test_assets_set_tft(false); test_assets_set_cliff_specific(false);
    make_terrain(4, 4, 0); reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain(); ASSERT_NOT_NULL(terrain);
    ground = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                           BZ_TTA_TERRAIN_TEXTURE_GROUND, 0);
    again = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_GROUND, 0);
    cliff = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_CLIFF, 0);
    ASSERT_NOT_NULL(ground); ASSERT(!BZ_TTAsset_IsPlaceholder(ground)); ASSERT(ground == again);
    ASSERT_NOT_NULL(cliff); ASSERT(!BZ_TTAsset_IsPlaceholder(cliff));
    ASSERT(BZ_TTAsset_Metadata(ground, &metadata));
    ASSERT_EQ_INT(metadata.team_color, BZ_TTA_TEAM_COLOR_NONE);
    assert_asset_identity(ground, "TerrainArt\\ROC\\Dirt.blp");
    assert_asset_identity(cliff, "ReplaceableTextures\\Cliff\\Cliff0.blp");
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 2); ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
    ASSERT_NULL(BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                              BZ_TTA_TERRAIN_TEXTURE_GROUND, 9));
    BZ_TTAsset_Release(ground); BZ_TTAsset_Release(again); BZ_TTAsset_Release(cliff);
    BZ_TTTerrain_Release(terrain); free_terrain();

    test_assets_set_tft(true); test_assets_set_cliff_specific(true);
    make_terrain(4, 4, 1); reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain();
    ground = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                           BZ_TTA_TERRAIN_TEXTURE_GROUND, 0);
    cliff = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_CLIFF, 0);
    assert_asset_identity(ground, "TerrainArt\\TFT\\Dirt.blp");
    assert_asset_identity(cliff, "ReplaceableTextures\\Cliff\\L_Cliff0.blp");
    BZ_TTAsset_Release(ground); BZ_TTAsset_Release(cliff); BZ_TTTerrain_Release(terrain);
    free_terrain();

    test_assets_set_tft(false); test_assets_set_cliff_specific(false); test_assets_set_cliff_generic(false);
    make_terrain(4, 4, 2); reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain();
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF), 1);
    ASSERT(BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, 0, &texture));
    ASSERT_EQ_INT(texture.type_index, 0); ASSERT_EQ_INT(texture.corner_count, 16);
    cliff = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_CLIFF, 0);
    again = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_CLIFF, 0);
    ASSERT_NOT_NULL(cliff); ASSERT(cliff == again); ASSERT(BZ_TTAsset_IsPlaceholder(cliff));
    ASSERT_EQ_INT(BZ_TTAsset_Status(cliff), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 1);
    BZ_TTAsset_Release(cliff); BZ_TTAsset_Release(again); BZ_TTTerrain_Release(terrain);
    free_terrain(); test_assets_set_cliff_generic(true);
}

typedef struct {
    const bzTTTerrain_t *terrain;
    const bzTTAsset_t *asset;
} waterRegistrationCtx_t;

static void *register_water(void *opaque) {
    waterRegistrationCtx_t *ctx = opaque;
    ctx->asset = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, ctx->terrain,
                                                BZ_TTA_TERRAIN_TEXTURE_WATER, 0);
    return NULL;
}

/* Water is one C-authored image, referenced only when desktop would render at least one tile. */
static void test_water_texture_semantic_success_missing_and_concurrency(void) {
    enum { THREADS = 8 };
    const bzTTTerrain_t *terrain;
    const bzTTAsset_t *water, *again;
    bzTTTerrainTextureInfo_t texture;
    waterRegistrationCtx_t ctx[THREADS];
    pthread_t threads[THREADS];
    LPWAR3MAPVERTEX vertices;

    for (int tft = 0; tft < 2; tft++) {
        test_assets_set_tft(tft); test_assets_set_water_available(true);
        make_terrain(4, 4, 0); reset_assets(); BZ_TTA_PublishTerrainFromGame();
        terrain = BZ_TTA_LatestTerrain(); ASSERT_NOT_NULL(terrain);
        ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER), 1);
        ASSERT(BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 0, &texture));
        ASSERT_EQ_INT(texture.type_index, 0); ASSERT_EQ_INT(texture.type_id, 0);
        ASSERT_EQ_INT(texture.corner_count, 1);
        ASSERT(!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 1, &texture));
        water = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                              BZ_TTA_TERRAIN_TEXTURE_WATER, 0);
        ASSERT_NOT_NULL(water); ASSERT(!BZ_TTAsset_IsPlaceholder(water));
        ASSERT_EQ_INT(BZ_TTAsset_Status(water), BZ_TTA_OK);
        assert_asset_identity(water, "ReplaceableTextures\\Water\\Water12.blp");
        ASSERT_NULL(BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                                  BZ_TTA_TERRAIN_TEXTURE_WATER, 1));
        ASSERT_EQ_INT(test_assets_water_reads(), 1);
        BZ_TTAsset_Release(water); BZ_TTTerrain_Release(terrain); free_terrain();
    }

    test_assets_set_water_available(true);
    make_terrain(4, 4, 0); vertices = world.map->vertices;
    vertices[15].water = true; vertices[15].mapedge = true;
    reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain(); ASSERT_NOT_NULL(terrain);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER), 1);
    ASSERT(BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 0, &texture));
    ASSERT_EQ_INT(texture.corner_count, 2);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND), 2);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF), 1);
    BZ_TTTerrain_Release(terrain); free_terrain();

    test_assets_set_water_available(false);
    make_terrain(4, 4, 0); vertices = world.map->vertices; vertices[5].mapedge = true;
    reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain(); ASSERT_NOT_NULL(terrain);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER), 0);
    ASSERT(!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 0, &texture));
    ASSERT_NULL(BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                              BZ_TTA_TERRAIN_TEXTURE_WATER, 0));
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND), 2);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF), 1);
    ASSERT_EQ_INT(test_assets_water_reads(), 0); ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 0);
    BZ_TTTerrain_Release(terrain); free_terrain();

    test_assets_set_water_available(false);
    make_terrain(4, 4, 0);
    for (uint32_t i = 0; i < world.map->width * world.map->height; i++)
        ((LPWAR3MAPVERTEX)world.map->vertices)[i].water = false;
    reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain(); ASSERT_NOT_NULL(terrain);
    ASSERT_EQ_INT(BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER), 0);
    ASSERT(!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 0, &texture));
    ASSERT_NULL(BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                              BZ_TTA_TERRAIN_TEXTURE_WATER, 0));
    ASSERT_EQ_INT(test_assets_water_reads(), 0); ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 0);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 0);
    BZ_TTTerrain_Release(terrain); free_terrain();

    make_terrain(4, 4, 0); reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain();
    water = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_WATER, 0);
    again = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_WATER, 0);
    ASSERT_NOT_NULL(water); ASSERT(water == again); ASSERT(BZ_TTAsset_IsPlaceholder(water));
    ASSERT_EQ_INT(BZ_TTAsset_Status(water), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1); ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 1); ASSERT_EQ_INT(test_assets_water_reads(), 1);
    BZ_TTAsset_Release(water);
    ASSERT_EQ_INT(BZ_TTAsset_Status(again), BZ_TTA_ERR_NOT_FOUND);
    BZ_TTAsset_Release(again); BZ_TTTerrain_Release(terrain); free_terrain();

    test_assets_set_water_available(true);
    make_terrain(4, 4, 0); reset_assets(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain();
    for (int i = 0; i < THREADS; i++) {
        ctx[i] = (waterRegistrationCtx_t){ .terrain = terrain };
        ASSERT_EQ_INT(pthread_create(threads + i, NULL, register_water, ctx + i), 0);
    }
    for (int i = 0; i < THREADS; i++) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0); ASSERT_NOT_NULL(ctx[i].asset);
        ASSERT(ctx[i].asset == ctx[0].asset); BZ_TTAsset_Release(ctx[i].asset);
    }
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1); ASSERT_EQ_INT(BZ_TTA_CacheHits(), THREADS - 1);
    ASSERT_EQ_INT(test_assets_water_reads(), 1);
    BZ_TTTerrain_Release(terrain); free_terrain();
    test_assets_set_tft(false);
}

typedef struct {
    bzTTTeamTextureKind_t kind;
    uint32_t team_color;
    const bzTTAsset_t *asset;
} teamRegistrationCtx_t;

static void *register_team_texture(void *opaque) {
    teamRegistrationCtx_t *ctx = opaque;
    ctx->asset = BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, ctx->kind, ctx->team_color);
    return NULL;
}

/* Team semantics stay per entity while immutable decoded images share the normal asset cache. */
static void test_team_texture_registration_range_cache_lifecycle_and_inverses(void) {
    enum { THREADS = 8 };
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *asset, *again, *model, *texture;
    const bzTTTerrain_t *terrain;
    teamRegistrationCtx_t ctx[THREADS];
    pthread_t threads[THREADS];
    char expected[BZ_TTA_MAX_IDENTITY];

    for (int tft = 0; tft < 2; tft++) {
        test_assets_set_tft(tft); test_assets_set_team_available(true); reset_assets();
        ASSERT_EQ_INT(BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                              BZ_TTA_TEAM_TEXTURE_COLOR), MAX_PLAYERS);
        ASSERT_EQ_INT(BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                              BZ_TTA_TEAM_TEXTURE_GLOW), MAX_PLAYERS);
        for (int kind = BZ_TTA_TEAM_TEXTURE_COLOR; kind <= BZ_TTA_TEAM_TEXTURE_GLOW; kind++) {
            for (uint32_t team = 0; team < MAX_PLAYERS; team++) {
                asset = BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                                   (bzTTTeamTextureKind_t)kind, team);
                ASSERT_NOT_NULL(asset); ASSERT(!BZ_TTAsset_IsPlaceholder(asset));
                ASSERT_EQ_INT(BZ_TTAsset_Status(asset), BZ_TTA_OK);
                snprintf(expected, sizeof(expected), "ReplaceableTextures\\Team%s\\Team%s%02u.blp",
                         kind == BZ_TTA_TEAM_TEXTURE_COLOR ? "Color" : "Glow",
                         kind == BZ_TTA_TEAM_TEXTURE_COLOR ? "Color" : "Glow", team);
                assert_asset_identity(asset, expected); BZ_TTAsset_Release(asset);
            }
        }
        ASSERT_EQ_INT(BZ_TTA_CacheMisses(), MAX_PLAYERS * 2);
        ASSERT_EQ_INT(test_assets_team_reads(), MAX_PLAYERS * 2);
    }

    test_assets_set_team_available(false); reset_assets();
    asset = BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_TEAM_TEXTURE_GLOW, 0);
    again = BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_TEAM_TEXTURE_GLOW, 0);
    ASSERT_NOT_NULL(asset); ASSERT(asset == again); ASSERT(BZ_TTAsset_IsPlaceholder(asset));
    ASSERT_EQ_INT(BZ_TTAsset_Status(asset), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1); ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 1); ASSERT_EQ_INT(test_assets_team_reads(), 1);
    BZ_TTAsset_Release(asset); ASSERT_EQ_INT(BZ_TTAsset_Status(again), BZ_TTA_ERR_NOT_FOUND);
    BZ_TTAsset_Release(again);

    test_assets_set_team_available(false); reset_assets();
    ASSERT_EQ_INT(BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                          (bzTTTeamTextureKind_t)0), 0);
    ASSERT_EQ_INT(BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                          (bzTTTeamTextureKind_t)3), 0);
    ASSERT_NULL(BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                           (bzTTTeamTextureKind_t)0, 0));
    ASSERT_NULL(BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                           (bzTTTeamTextureKind_t)3, 0));
    ASSERT_NULL(BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                           BZ_TTA_TEAM_TEXTURE_GLOW, MAX_PLAYERS));
    ASSERT_EQ_INT(BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION + 1,
                                          BZ_TTA_TEAM_TEXTURE_GLOW), 0);
    ASSERT_NULL(BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION + 1,
                                           BZ_TTA_TEAM_TEXTURE_GLOW, 0));
    ASSERT_EQ_INT(test_assets_team_reads(), 0); ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 0);
    BZ_TTA_Shutdown();
    ASSERT_EQ_INT(BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                          BZ_TTA_TEAM_TEXTURE_GLOW), 0);
    ASSERT_NULL(BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                           BZ_TTA_TEAM_TEXTURE_GLOW, 0));

    test_assets_set_team_available(true); reset_assets();
    for (int i = 0; i < THREADS; i++) {
        ctx[i] = (teamRegistrationCtx_t){ .kind = BZ_TTA_TEAM_TEXTURE_GLOW, .team_color = 0 };
        ASSERT_EQ_INT(pthread_create(threads + i, NULL, register_team_texture, ctx + i), 0);
    }
    for (int i = 0; i < THREADS; i++) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0); ASSERT_NOT_NULL(ctx[i].asset);
        ASSERT(ctx[i].asset == ctx[0].asset); BZ_TTAsset_Release(ctx[i].asset);
    }
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1); ASSERT_EQ_INT(BZ_TTA_CacheHits(), THREADS - 1);
    ASSERT_EQ_INT(test_assets_team_reads(), 1);

    test_assets_set_configstring(&snapshot, 1, "TestUI/Models/quad_sprite.mdx");
    make_terrain(4, 4, 0); reset_assets(); BZ_TTA_PublishTerrainFromGame();
    model = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_MODEL, NULL);
    ASSERT_NOT_NULL(model);
    texture = BZ_TTA_RegisterModelTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, model, 0);
    ASSERT_NOT_NULL(texture); ASSERT(!BZ_TTAsset_IsPlaceholder(texture));
    terrain = BZ_TTA_LatestTerrain(); ASSERT_NOT_NULL(terrain);
    asset = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain,
                                          BZ_TTA_TERRAIN_TEXTURE_WATER, 0);
    ASSERT_NOT_NULL(asset); ASSERT(!BZ_TTAsset_IsPlaceholder(asset));
    BZ_TTAsset_Release(asset); BZ_TTTerrain_Release(terrain);
    BZ_TTAsset_Release(texture); BZ_TTAsset_Release(model); free_terrain();
    test_assets_set_tft(false);
}

static void test_entity_metadata_map_readiness_and_cache_scope(void) {
    bzTTAssetMetadata_t metadata;
    bzTTEntityMetadataInput_t input = { .class_id = FOURCC('h','p','e','a') };
    reset_assets();
    test_assets_set_metadata_map(2);
    ASSERT_EQ_INT(BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION, &input, &metadata),
                  BZ_TTA_ERR_NOT_INITIALIZED);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 0);
    test_assets_set_metadata_map(0);
    metadata = resolve_metadata(input.class_id, BZ_TTA_OK);
    ASSERT_EQ_FLOAT(metadata.footprint_x, 32, 0.001f);
    test_assets_set_metadata_map(1);
    metadata = resolve_metadata(input.class_id, BZ_TTA_OK);
    ASSERT_EQ_FLOAT(metadata.footprint_x, 48, 0.001f);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 2);
    test_assets_set_metadata_map(0);
}

static bzTTAssetMetadata_t resolve_metadata(uint32_t class_id, bzTTAResult_t expected) {
    bzTTEntityMetadataInput_t input = { .class_id = class_id };
    bzTTAssetMetadata_t metadata;
    ASSERT_EQ_INT(BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION, &input, &metadata),
                  expected);
    return metadata;
}

static void test_entity_metadata_categories_footprints_and_overrides(void) {
    bzTTAssetMetadata_t metadata;
    bzTTEntityMetadataInput_t override = {
        .class_id = FOURCC('h','p','e','a'),
        .override_mask = BZ_TTA_METADATA_OVERRIDE_TEAM_COLOR | BZ_TTA_METADATA_OVERRIDE_TINT,
        .team_color = 7, .tint_r = 0.1f, .tint_g = 0.2f, .tint_b = 0.3f, .tint_a = 0.4f,
    };
    reset_assets();
    metadata = resolve_metadata(FOURCC('h','p','e','a'), BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_MOBILE);
    ASSERT_EQ_FLOAT(metadata.footprint_x, 32, 0.001f);
    ASSERT_EQ_INT(metadata.team_color, BZ_TTA_TEAM_COLOR_NONE);
    ASSERT_EQ_INT(BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION, &override, &metadata),
                  BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.team_color, 7); ASSERT_EQ_FLOAT(metadata.tint_b, 0.3f, 0.001f);
    metadata = resolve_metadata(FOURCC('h','t','o','w'), BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_BUILDING);
    ASSERT_EQ_FLOAT(metadata.footprint_x, 128, 0.001f);
    ASSERT_EQ_FLOAT(metadata.footprint_y, 192, 0.001f);
    ASSERT_EQ_INT(metadata.team_color, 3);
    metadata = resolve_metadata(FOURCC('n','g','o','l'), BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_RESOURCE);
    ASSERT_EQ_FLOAT(metadata.footprint_x, 512, 0.001f);
    metadata = resolve_metadata(FOURCC('L','T','l','t'), BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_RESOURCE);
    ASSERT_EQ_FLOAT(metadata.footprint_x, 64, 0.001f);
    metadata = resolve_metadata(FOURCC('B','0','0','1'), BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_DESTRUCTABLE);
    ASSERT_EQ_FLOAT(metadata.footprint_y, 128, 0.001f);
    metadata = resolve_metadata(FOURCC('D','O','O','D'), BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_DOODAD);
    ASSERT_EQ_FLOAT(metadata.footprint_x, 64, 0.001f);
    ASSERT_EQ_FLOAT(metadata.footprint_y, 96, 0.001f);
    ASSERT_EQ_FLOAT(metadata.tint_r, 128.0f / 255.0f, 0.001f);
}

static void test_roc_tft_item_metadata_fourcc_category_footprint_and_models(void) {
    static const uint32_t class_ids[] = {
        0x34656472, 0x66746172, 0x66696c72, 0x7a697772, 0x74767270, 0x676e6b63,
    };
    static const char *names[] = { "rde4", "ratf", "rlif", "rwiz", "prvt", "ckng" };
    struct bzTTSnapshot snapshot = { 0 };
    bzTTAssetMetadata_t metadata;
    const bzTTAsset_t *model, *again;
    char row[5];
    for (unsigned tft = 0; tft < 2; tft++) {
        test_assets_set_tft(tft); reset_assets();
        for (size_t i = 0; i < sizeof(class_ids) / sizeof(*class_ids); i++) {
            memcpy(row, &class_ids[i], 4); row[4] = 0;
            ASSERT_STR_EQ(row, names[i]);
            metadata = resolve_metadata(class_ids[i], BZ_TTA_OK);
            ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_ITEM);
            ASSERT_EQ_INT(metadata.class_id, class_ids[i]);
            ASSERT_EQ_FLOAT(metadata.footprint_x, 0, 0.001f);
            ASSERT_EQ_FLOAT(metadata.footprint_y, 0, 0.001f);
        }
        resolve_metadata(class_ids[0], BZ_TTA_OK);
        ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 6); ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
        ASSERT_EQ_INT(BZ_TTA_MetadataLogs(), 0);

        metadata = resolve_metadata(FOURCC('i','t','s','t'), BZ_TTA_OK);
        ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_ITEM);
        ASSERT_EQ_FLOAT(metadata.tint_r, tft ? 100.0f / 255.0f : 1, 0.001f);
        ASSERT_EQ_FLOAT(metadata.tint_g, tft ? 140.0f / 255.0f : 1, 0.001f);
        test_assets_set_configstring(&snapshot, 1, "TestUI/Models/quad_sprite.mdl");
        model = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                            BZ_TTA_ASSET_MODEL, &metadata);
        again = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                            BZ_TTA_ASSET_MODEL, &metadata);
        ASSERT_NOT_NULL(model); ASSERT(model == again); ASSERT(!BZ_TTAsset_IsPlaceholder(model));
        BZ_TTAsset_Release(model); BZ_TTAsset_Release(again);
    }

    test_assets_set_tft(false); reset_assets();
    metadata = resolve_metadata(FOURCC('i','b','a','d'), BZ_TTA_ERR_PATH_CONFINEMENT);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_ITEM);
    resolve_metadata(FOURCC('i','b','a','d'), BZ_TTA_ERR_PATH_CONFINEMENT);
    ASSERT_EQ_INT(BZ_TTA_MetadataLogs(), 1);
    metadata = resolve_metadata(FOURCC('M','I','S','S'), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_UNKNOWN);

    reset_assets(); test_assets_set_metadata_map(1);
    metadata = resolve_metadata(FOURCC('i','0','0','0'), BZ_TTA_OK);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_ITEM);
    ASSERT_EQ_INT(metadata.class_id, FOURCC('i','0','0','0'));
    test_assets_set_metadata_map(0);

    reset_assets();
    metadata = resolve_metadata(FOURCC('i','m','i','s'), BZ_TTA_OK);
    test_assets_set_configstring(&snapshot, 1, "TestUI/Models/missing_item.mdx");
    model = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_MODEL, &metadata);
    again = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_MODEL, &metadata);
    ASSERT_NOT_NULL(model); ASSERT(model == again); ASSERT(BZ_TTAsset_IsPlaceholder(model));
    ASSERT_EQ_INT(BZ_TTAsset_Status(model), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 1);
    BZ_TTAsset_Release(model); BZ_TTAsset_Release(again);
}

static void test_roc_tft_non_pathing_doodad_metadata(void) {
    static const uint32_t class_ids[] = {
        FOURCC('L','P','w','h'), FOURCC('L','O','f','l'), FOURCC('L','O','t','h'),
        FOURCC('L','P','r','s'), FOURCC('L','P','l','p'), FOURCC('L','O','t','z'),
        FOURCC('L','O','s','m'), FOURCC('A','W','f','s'), FOURCC('L','P','c','w'),
        FOURCC('A','O','s','r'),
    };
    bzTTAssetMetadata_t metadata;
    for (unsigned tft = 0; tft < 2; tft++) {
        test_assets_set_tft(tft); reset_assets();
        for (size_t i = 0; i < sizeof(class_ids) / sizeof(*class_ids); i++) {
            metadata = resolve_metadata(class_ids[i], BZ_TTA_OK);
            ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_DOODAD);
            ASSERT_EQ_FLOAT(metadata.footprint_x, 0, 0.001f);
            ASSERT_EQ_FLOAT(metadata.footprint_y, 0, 0.001f);
        }
        ASSERT_EQ_INT(BZ_TTA_MetadataLogs(), 0);
    }
    test_assets_set_tft(false); reset_assets();
    metadata = resolve_metadata(FOURCC('D','m','i','s'), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_DOODAD);
    ASSERT_EQ_INT(BZ_TTA_MetadataLogs(), 1);
}

static void test_entity_metadata_error_log_once_and_cache(void) {
    bzTTAssetMetadata_t metadata;
    uint64_t misses, hits;
    reset_assets();
    metadata = resolve_metadata(FOURCC('B','b','a','d'), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(metadata.category, BZ_TTA_CATEGORY_DESTRUCTABLE);
    misses = BZ_TTA_CacheMisses(); hits = BZ_TTA_CacheHits();
    resolve_metadata(FOURCC('B','b','a','d'), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), misses);
    ASSERT_EQ_INT(BZ_TTA_CacheHits(), hits + 1);
    ASSERT_EQ_INT(BZ_TTA_MetadataLogs(), 1);
    resolve_metadata(FOURCC('M','I','S','S'), BZ_TTA_ERR_NOT_FOUND);
    resolve_metadata(FOURCC('M','I','S','S'), BZ_TTA_ERR_NOT_FOUND);
    resolve_metadata(FOURCC('B','m','a','l'), BZ_TTA_ERR_MALFORMED);
    resolve_metadata(FOURCC('B','e','s','c'), BZ_TTA_ERR_PATH_CONFINEMENT);
    resolve_metadata(FOURCC('h','f','o','o'), BZ_TTA_ERR_NOT_FOUND);
    ASSERT_EQ_INT(BZ_TTA_MetadataLogs(), 5);
}

typedef struct {
    uint32_t class_id;
    bzTTAResult_t status;
} metadataCtx_t;

static void *metadata_resolver(void *opaque) {
    metadataCtx_t *ctx = opaque;
    bzTTEntityMetadataInput_t input = { .class_id = ctx->class_id };
    bzTTAssetMetadata_t metadata;
    ctx->status = BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION, &input, &metadata);
    return NULL;
}

typedef struct {
    atomic_bool started, finished;
} restartCtx_t;

static void *restart_assets(void *opaque) {
    restartCtx_t *ctx = opaque;
    atomic_store(&ctx->started, true);
    BZ_TTA_Shutdown();
    BZ_TTA_Init();
    atomic_store(&ctx->finished, true);
    return NULL;
}

static void test_entity_metadata_concurrency_and_lifecycle(void) {
    enum { THREADS = 8 };
    metadataCtx_t ctx[THREADS];
    pthread_t threads[THREADS];
    pthread_t restart_thread;
    restartCtx_t restart = { 0 };
    struct timespec drain_wait = { .tv_nsec = 10000000 };
    reset_assets();
    for (int i = 0; i < THREADS; i++) {
        ctx[i] = (metadataCtx_t){ .class_id = FOURCC('r','d','e','4') };
        ASSERT_EQ_INT(pthread_create(threads + i, NULL, metadata_resolver, ctx + i), 0);
    }
    for (int i = 0; i < THREADS; i++) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        ASSERT_EQ_INT(ctx[i].status, BZ_TTA_OK);
    }
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1); ASSERT_EQ_INT(BZ_TTA_CacheHits(), THREADS - 1);
    reset_assets(); test_assets_block_reads(true);
    ctx[0] = (metadataCtx_t){ .class_id = FOURCC('h','t','o','w') };
    ASSERT_EQ_INT(pthread_create(threads, NULL, metadata_resolver, ctx), 0);
    test_assets_wait_for_blocked_reads(1);
    ASSERT_EQ_INT(pthread_create(&restart_thread, NULL, restart_assets, &restart), 0);
    while (!atomic_load(&restart.started)) sched_yield();
    nanosleep(&drain_wait, NULL);
    ASSERT(!atomic_load(&restart.finished));
    test_assets_block_reads(false);
    ASSERT_EQ_INT(pthread_join(threads[0], NULL), 0);
    ASSERT_EQ_INT(pthread_join(restart_thread, NULL), 0);
    ASSERT_EQ_INT(ctx[0].status, BZ_TTA_ERR_TERMINAL);
}

typedef struct {
    const bzTTAsset_t *asset;
    atomic_bool *running;
} readerCtx_t;

static void *asset_reader(void *opaque) {
    readerCtx_t *ctx = opaque;
    bzTTImageInfo_t info;
    uint8_t pixels[16];
    while (atomic_load(ctx->running)) {
        BZ_TTAsset_Retain(ctx->asset);
        BZ_TTAsset_ImageInfo(ctx->asset, &info);
        BZ_TTAsset_CopyImagePixels(ctx->asset, pixels, sizeof(pixels));
        BZ_TTAsset_Release(ctx->asset);
    }
    return NULL;
}

static void test_concurrent_readers_and_shutdown_lifetime(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *asset;
    pthread_t threads[4];
    atomic_bool running = true;
    readerCtx_t ctx;
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/orientation_2x2.blp");
    reset_assets();
    asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(asset);
    ctx = (readerCtx_t){ asset, &running };
    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(pthread_create(&threads[i], NULL, asset_reader, &ctx), 0);
    BZ_TTA_Shutdown(); /* Outstanding caller reference and reader retains remain valid. */
    atomic_store(&running, false);
    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
    {
        uint8_t pixels[16];
        ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(asset, pixels, sizeof(pixels)), sizeof(pixels));
    }
    BZ_TTAsset_Release(asset);
}

typedef struct {
    struct bzTTSnapshot *snapshot;
    const bzTTAsset_t *asset;
} registrationCtx_t;

static void *blocked_registration(void *opaque) {
    registrationCtx_t *ctx = opaque;
    ctx->asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, ctx->snapshot, 1,
                                              BZ_TTA_ASSET_IMAGE, NULL);
    return NULL;
}

static void test_inflight_load_cannot_cross_restart(void) {
    struct bzTTSnapshot snapshot = { 0 };
    registrationCtx_t ctx = { .snapshot = &snapshot };
    const bzTTAsset_t *fresh;
    pthread_t thread, restart_thread;
    restartCtx_t restart = { 0 };
    struct timespec drain_wait = { .tv_nsec = 10000000 };
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/orientation_2x2.blp");
    reset_assets();
    test_assets_block_reads(true);
    ASSERT_EQ_INT(pthread_create(&thread, NULL, blocked_registration, &ctx), 0);
    test_assets_wait_for_blocked_reads(1);
    ASSERT_EQ_INT(pthread_create(&restart_thread, NULL, restart_assets, &restart), 0);
    while (!atomic_load(&restart.started)) sched_yield();
    nanosleep(&drain_wait, NULL);
    ASSERT(!atomic_load(&restart.finished));
    test_assets_block_reads(false);
    ASSERT_EQ_INT(pthread_join(thread, NULL), 0);
    ASSERT_EQ_INT(pthread_join(restart_thread, NULL), 0);
    ASSERT_NULL(ctx.asset);
    fresh = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(fresh); ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1);
    BZ_TTAsset_Release(fresh);
}

static void test_concurrent_missing_asset_logs_once(void) {
    enum { THREADS = 8 };
    struct bzTTSnapshot snapshot = { 0 };
    registrationCtx_t ctx[THREADS];
    pthread_t threads[THREADS];
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/concurrent-missing.blp");
    reset_assets(); test_assets_block_reads(true);
    for (int i = 0; i < THREADS; i++) {
        ctx[i] = (registrationCtx_t){ .snapshot = &snapshot };
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, blocked_registration, ctx + i), 0);
    }
    test_assets_wait_for_blocked_reads(1);
    test_assets_block_reads(false);
    for (int i = 0; i < THREADS; i++) ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
    for (int i = 0; i < THREADS; i++) {
        ASSERT_NOT_NULL(ctx[i].asset); ASSERT(ctx[i].asset == ctx[0].asset);
        BZ_TTAsset_Release(ctx[i].asset);
    }
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1);
    ASSERT_EQ_INT(BZ_TTA_CacheHits(), THREADS - 1);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 1);
}

typedef struct {
    atomic_bool *running;
    atomic_uint *reads;
} terrainReaderCtx_t;

static void *terrain_reader(void *opaque) {
    terrainReaderCtx_t *ctx = opaque;
    while (atomic_load(ctx->running)) {
        const bzTTTerrain_t *terrain = BZ_TTA_LatestTerrain();
        if (terrain) {
            bzTTTerrainCorner_t corner;
            bzTTTerrainTextureInfo_t texture;
            BZ_TTTerrain_Corner(terrain, 0, 0, &corner);
            if (BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF))
                BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, 0, &texture);
            atomic_fetch_add(ctx->reads, 1);
            BZ_TTTerrain_Release(terrain);
        }
    }
    return NULL;
}

static void *terrain_publisher(void *opaque) {
    atomic_bool *running = opaque;
    while (atomic_load(running)) BZ_TTA_PublishTerrainFromGame();
    return NULL;
}

static void test_cleanup_publish_race(void) {
    pthread_t reader, publisher;
    atomic_bool running = true;
    atomic_uint reads = 0;
    terrainReaderCtx_t ctx = { &running, &reads };
    make_terrain(4, 4, 0);
    reset_assets(); BZ_TTA_PublishTerrainFromGame();
    ASSERT_EQ_INT(pthread_create(&reader, NULL, terrain_reader, &ctx), 0);
    ASSERT_EQ_INT(pthread_create(&publisher, NULL, terrain_publisher, &running), 0);
    for (int i = 0; i < 10000 && !atomic_load(&reads); i++) sched_yield();
    for (int i = 0; i < 8; i++) {
        BZ_TTA_Shutdown(); BZ_TTA_Init(); BZ_TTA_PublishTerrainFromGame();
    }
    atomic_store(&running, false);
    ASSERT_EQ_INT(pthread_join(reader, NULL), 0);
    ASSERT_EQ_INT(pthread_join(publisher, NULL), 0);
    ASSERT(atomic_load(&reads) > 0);
    free_terrain();
}

void run_bz_tabletop_assets_tests(void) {
    RUN_TEST(test_abi_and_asymmetric_blp_orientation);
    RUN_TEST(test_blp1_paletted_decode);
    RUN_TEST(test_blp1_jpeg_dimension_limit);
    RUN_TEST(test_roc_tft_resolution_and_cache);
    RUN_TEST(test_placeholder_path_confinement_and_log_once_cache);
    RUN_TEST(test_mdx_geometry_materials_sequences_and_bounds);
    RUN_TEST(test_mdx_animation_hierarchy_tracks_and_dynamic_material);
    RUN_TEST(test_desktop_model_identity_fallbacks);
    RUN_TEST(test_spawn_model_variation_resolution);
    RUN_TEST(test_model_identity_output_bounds);
    RUN_TEST(test_mdx_multiple_inclusive_records);
    RUN_TEST(test_mdx_post_mats_uvs_and_absent_inverse);
    RUN_TEST(test_mdx_zero_counted_array_is_malformed);
    RUN_TEST(test_malformed_blp_and_mdx_bounds);
    RUN_TEST(test_terrain_dimensions_corners_water_cliffs_and_chunks);
    RUN_TEST(test_malformed_terrain_type_index);
    RUN_TEST(test_human02_shape_no_cliff_sentinel);
    RUN_TEST(test_terrain_texture_resolution_roc_tft_and_fallback);
    RUN_TEST(test_water_texture_semantic_success_missing_and_concurrency);
    RUN_TEST(test_team_texture_registration_range_cache_lifecycle_and_inverses);
    RUN_TEST(test_entity_metadata_categories_footprints_and_overrides);
    RUN_TEST(test_roc_tft_item_metadata_fourcc_category_footprint_and_models);
    RUN_TEST(test_roc_tft_non_pathing_doodad_metadata);
    RUN_TEST(test_entity_metadata_map_readiness_and_cache_scope);
    RUN_TEST(test_entity_metadata_error_log_once_and_cache);
    RUN_TEST(test_entity_metadata_concurrency_and_lifecycle);
    RUN_TEST(test_concurrent_readers_and_shutdown_lifetime);
    RUN_TEST(test_inflight_load_cannot_cross_restart);
    RUN_TEST(test_concurrent_missing_asset_logs_once);
    RUN_TEST(test_cleanup_publish_race);
    BZ_TTA_Shutdown();
}

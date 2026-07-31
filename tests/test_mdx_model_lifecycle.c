#include <stdlib.h>

#include "games/warcraft-3/renderer/mdx/r_mdx.h"
#include "games/warcraft-3/renderer/mdx/r_mdx_lifecycle.h"
#include "test_framework.h"

int _tests_run = 0, _tests_failed = 0;
static void *g_allocations[128];
static int g_alloc_count, g_free_count, g_gpu_count, g_texture_count;

static void *tracked_alloc(size_t size) {
    void *ptr = calloc(1, size);
    ASSERT_NOT_NULL(ptr); g_allocations[g_alloc_count++] = ptr;
    return ptr;
}

static void tracked_free(void *ptr) {
    FOR_LOOP(i, g_alloc_count) {
        if (g_allocations[i] != ptr) continue;
        g_allocations[i] = NULL; g_free_count++; return;
    }
    ASSERT(false);
}

static mdxKeyTrack_t *keytrack(void) { return tracked_alloc(sizeof(mdxKeyTrack_t)); }

static void populate_node(mdxNode_t *node) {
    node->translation = keytrack(); node->rotation = keytrack(); node->scale = keytrack();
}

static void release_gpu(mdxGeoset_t *geoset) { ASSERT_NOT_NULL(geoset); g_gpu_count++; }
static void unregister_texture(int texture) { ASSERT(texture == 101 || texture == 102); g_texture_count++; }

static mdxModel_t *build_populated_model(void) {
    mdxModel_t *model = tracked_alloc(sizeof(*model));
    mdxGeoset_t *geoset = model->geosets = tracked_alloc(sizeof(*geoset));
    geoset->vertices = tracked_alloc(1); geoset->normals = tracked_alloc(1);
    geoset->texcoord = tracked_alloc(1); geoset->bounds = tracked_alloc(1);
    geoset->matrices = tracked_alloc(1); geoset->matrixPalette = tracked_alloc(1);
    geoset->primitiveTypes = tracked_alloc(1); geoset->primitiveCounts = tracked_alloc(1);
    geoset->triangles = tracked_alloc(1); geoset->vertexGroups = tracked_alloc(1);
    geoset->matrixGroupSizes = tracked_alloc(1);

    mdxMaterial_t *material = model->materials = tracked_alloc(sizeof(*material));
    material->num_layers = 2; material->layers = tracked_alloc(2 * sizeof(*material->layers));
    FOR_LOOP(i, material->num_layers) {
        material->layers[i].alpha = keytrack(); material->layers[i].flipbook = keytrack();
    }
    material->emission = keytrack(); material->alpha = keytrack(); material->flipbook = keytrack();

    mdxTextureAnim_t *texture_anim = model->textureAnims = tracked_alloc(sizeof(*texture_anim));
    texture_anim->translation = keytrack(); texture_anim->rotation = keytrack(); texture_anim->scale = keytrack();

    mdxBone_t *bone = model->bones = tracked_alloc(sizeof(*bone)); populate_node(&bone->node);
    model->nodes[0] = &bone->node;
    mdxGeosetAnim_t *geoset_anim = model->geosetAnims = tracked_alloc(sizeof(*geoset_anim));
    geoset_anim->alphas = keytrack(); geoset_anim->colors = keytrack(); geoset->geosetAnim = geoset_anim;
    mdxHelper_t *helper = model->helpers = tracked_alloc(sizeof(*helper)); populate_node(&helper->node);

    mdxLight_t *light = model->lights = tracked_alloc(sizeof(*light)); populate_node(&light->node);
    light->keytracks.Visibility = keytrack(); light->keytracks.Color = keytrack();
    light->keytracks.Intensity = keytrack(); light->keytracks.AmbColor = keytrack();
    light->keytracks.AmbIntensity = keytrack(); light->keytracks.AttenuationStart = keytrack();
    light->keytracks.AttenuationEnd = keytrack();

    mdxEvent_t *event = model->events = tracked_alloc(sizeof(*event)); populate_node(&event->node);
    event->keys = tracked_alloc(sizeof(*event->keys));
    mdxCollisionShape_t *collision = model->collisionShapes = tracked_alloc(sizeof(*collision));
    populate_node(&collision->node);
    mdxCamera_t *camera = model->cameras = tracked_alloc(sizeof(*camera));
    camera->translation = keytrack(); camera->roll = keytrack(); camera->targetTranslation = keytrack();

    mdxParticleEmitter_t *emitter = model->emitters = tracked_alloc(sizeof(*emitter));
    populate_node(&emitter->node); emitter->keytracks.Visibility = keytrack();
    emitter->keytracks.EmissionRate = keytrack(); emitter->keytracks.Width = keytrack();
    emitter->keytracks.Length = keytrack(); emitter->keytracks.Speed = keytrack();
    emitter->keytracks.Latitude = keytrack(); emitter->keytracks.Gravity = keytrack();
    emitter->keytracks.Variation = keytrack();
    mdxAttachment_t *attachment = model->attachments = tracked_alloc(sizeof(*attachment));
    populate_node(&attachment->node); attachment->Visibility = keytrack();

    model->num_textures = 3; model->textures = tracked_alloc(3 * sizeof(*model->textures));
    model->textures[0].texid = 101; model->textures[1].texid = -1; model->textures[2].texid = 102;
    model->sequences = tracked_alloc(1); model->globalSequences = tracked_alloc(1); model->pivots = tracked_alloc(1);
    return model;
}

static void test_complete_model_release(void) {
    mdxModel_t *model;
    mdxReleaseAPI_t api = {
        .mem_free = tracked_free, .release_geoset_gpu = release_gpu,
        .unregister_texture = unregister_texture,
    };
    g_alloc_count = g_free_count = g_gpu_count = g_texture_count = 0;
    model = build_populated_model();
    mdx_release_owned_model(&model, &api);
    ASSERT_NULL(model); ASSERT_EQ_INT(g_free_count, g_alloc_count);
    ASSERT_EQ_INT(g_gpu_count, 1); ASSERT_EQ_INT(g_texture_count, 2);
    mdx_release_owned_model(&model, &api);
    ASSERT_EQ_INT(g_free_count, g_alloc_count);
}

int main(void) {
    RUN_TEST(test_complete_model_release);
    TEST_RESULTS();
}

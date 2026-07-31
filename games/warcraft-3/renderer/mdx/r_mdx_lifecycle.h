#ifndef R_MDX_LIFECYCLE_H
#define R_MDX_LIFECYCLE_H

typedef struct {
    void (*mem_free)(void *ptr);
    void (*release_geoset_gpu)(mdxGeoset_t *geoset);
    void (*unregister_texture)(int texture);
} mdxReleaseAPI_t;

#define MDX_FREE(ptr) do { if (ptr) { api->mem_free(ptr); ptr = NULL; } } while (0)

static void mdx_release_node(mdxNode_t *node, mdxReleaseAPI_t const *api) {
    MDX_FREE(node->translation); MDX_FREE(node->rotation); MDX_FREE(node->scale);
}

/* The loader has one owning edge for every nested allocation; borrowed node/geoset links are never freed here. */
static void mdx_release_owned_model(mdxModel_t **owner, mdxReleaseAPI_t const *api) {
    mdxModel_t *model;
    if (!owner || !(model = *owner)) return;
    while (model->geosets) {
        mdxGeoset_t *item = model->geosets; model->geosets = item->next;
        api->release_geoset_gpu(item);
        MDX_FREE(item->vertices); MDX_FREE(item->normals); MDX_FREE(item->texcoord); MDX_FREE(item->bounds);
        MDX_FREE(item->matrices); MDX_FREE(item->matrixPalette); MDX_FREE(item->primitiveTypes);
        MDX_FREE(item->primitiveCounts); MDX_FREE(item->triangles); MDX_FREE(item->vertexGroups);
        MDX_FREE(item->matrixGroupSizes); MDX_FREE(item);
    }
    while (model->materials) {
        mdxMaterial_t *item = model->materials; model->materials = item->next;
        FOR_LOOP(i, item->num_layers) { MDX_FREE(item->layers[i].alpha); MDX_FREE(item->layers[i].flipbook); }
        MDX_FREE(item->layers); MDX_FREE(item->emission); MDX_FREE(item->alpha); MDX_FREE(item->flipbook);
        MDX_FREE(item);
    }
    while (model->textureAnims) {
        mdxTextureAnim_t *item = model->textureAnims; model->textureAnims = item->next;
        MDX_FREE(item->translation); MDX_FREE(item->rotation); MDX_FREE(item->scale); MDX_FREE(item);
    }
    while (model->bones) {
        mdxBone_t *item = model->bones; model->bones = item->next; mdx_release_node(&item->node, api); MDX_FREE(item);
    }
    while (model->geosetAnims) {
        mdxGeosetAnim_t *item = model->geosetAnims; model->geosetAnims = item->next;
        MDX_FREE(item->alphas); MDX_FREE(item->colors); MDX_FREE(item);
    }
    while (model->helpers) {
        mdxHelper_t *item = model->helpers; model->helpers = item->next;
        mdx_release_node(&item->node, api); MDX_FREE(item);
    }
    while (model->lights) {
        mdxLight_t *item = model->lights; model->lights = item->next; mdx_release_node(&item->node, api);
        MDX_FREE(item->keytracks.Visibility); MDX_FREE(item->keytracks.Color);
        MDX_FREE(item->keytracks.Intensity); MDX_FREE(item->keytracks.AmbColor);
        MDX_FREE(item->keytracks.AmbIntensity); MDX_FREE(item->keytracks.AttenuationStart);
        MDX_FREE(item->keytracks.AttenuationEnd); MDX_FREE(item);
    }
    while (model->events) {
        mdxEvent_t *item = model->events; model->events = item->next;
        mdx_release_node(&item->node, api); MDX_FREE(item->keys); MDX_FREE(item);
    }
    while (model->collisionShapes) {
        mdxCollisionShape_t *item = model->collisionShapes; model->collisionShapes = item->next;
        mdx_release_node(&item->node, api); MDX_FREE(item);
    }
    while (model->cameras) {
        mdxCamera_t *item = model->cameras; model->cameras = item->next;
        MDX_FREE(item->translation); MDX_FREE(item->roll); MDX_FREE(item->targetTranslation); MDX_FREE(item);
    }
    while (model->emitters) {
        mdxParticleEmitter_t *item = model->emitters; model->emitters = item->next;
        mdx_release_node(&item->node, api); MDX_FREE(item->keytracks.Visibility);
        MDX_FREE(item->keytracks.EmissionRate); MDX_FREE(item->keytracks.Width); MDX_FREE(item->keytracks.Length);
        MDX_FREE(item->keytracks.Speed); MDX_FREE(item->keytracks.Latitude); MDX_FREE(item->keytracks.Gravity);
        MDX_FREE(item->keytracks.Variation); MDX_FREE(item);
    }
    while (model->attachments) {
        mdxAttachment_t *item = model->attachments; model->attachments = item->next;
        mdx_release_node(&item->node, api); MDX_FREE(item->Visibility); MDX_FREE(item);
    }
    FOR_LOOP(i, model->num_textures)
        if (model->textures[i].texid >= 0) api->unregister_texture(model->textures[i].texid);
    MDX_FREE(model->textures); MDX_FREE(model->sequences); MDX_FREE(model->globalSequences);
    MDX_FREE(model->pivots); api->mem_free(model); *owner = NULL;
}

#undef MDX_FREE

#endif

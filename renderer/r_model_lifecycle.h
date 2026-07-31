#ifndef R_MODEL_LIFECYCLE_H
#define R_MODEL_LIFECYCLE_H

typedef struct {
    LPMODEL *models;
    DWORD count;
    void (*release_model)(LPMODEL model);
} rOwnedModels_t;

/* Renderer globals may alias a model; clear every matching slot before releasing its single owner. */
static void R_ReleaseOwnedModels(rOwnedModels_t const *owned) {
    FOR_LOOP(i, owned->count) {
        LPMODEL model = owned->models[i];
        if (!model) continue;
        FOR_LOOP(j, owned->count)
            if (owned->models[j] == model) owned->models[j] = NULL;
        owned->release_model(model);
    }
}

#endif

#ifndef CL_MODEL_LIFECYCLE_H
#define CL_MODEL_LIFECYCLE_H

#include "tr_public.h"

typedef struct {
    LPMODEL *models;
    LPMODEL *portraits;
    LPMODEL *move_confirmation;
    DWORD count;
    void (*release_model)(LPMODEL model);
} clModelHandles_t;

/* Clear every alias before release so independently stored duplicate handles are destroyed exactly once. */
static void CL_ReleaseModelHandles(clModelHandles_t const *handles) {
    FOR_LOOP(i, handles->count) {
        LPMODEL model = handles->models[i];
        if (!model) continue;
        FOR_LOOP(j, handles->count) {
            if (handles->models[j] == model) handles->models[j] = NULL;
            if (handles->portraits[j] == model) handles->portraits[j] = NULL;
        }
        if (*handles->move_confirmation == model) *handles->move_confirmation = NULL;
        handles->release_model(model);
    }
    FOR_LOOP(i, handles->count) {
        LPMODEL model = handles->portraits[i];
        if (!model) continue;
        FOR_LOOP(j, handles->count)
            if (handles->portraits[j] == model) handles->portraits[j] = NULL;
        if (*handles->move_confirmation == model) *handles->move_confirmation = NULL;
        handles->release_model(model);
    }
    SAFE_DELETE(*handles->move_confirmation, handles->release_model);
}

#endif

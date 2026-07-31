#ifndef CL_MODEL_LIFECYCLE_H
#define CL_MODEL_LIFECYCLE_H

#include "tr_public.h"

typedef struct {
    LPMODEL *models;
    LPMODEL *portraits;
    DWORD count;
    void (*release_model)(LPMODEL model);
} clModelHandles_t;

/* Each renderer load returns one owned handle; clear every slot before client state is reset. */
static void CL_ReleaseModelHandles(clModelHandles_t const *handles) {
    FOR_LOOP(i, handles->count) {
        SAFE_DELETE(handles->models[i], handles->release_model);
        SAFE_DELETE(handles->portraits[i], handles->release_model);
    }
}

#endif

#ifndef SC2_TABLETOP_MODELS_INTERNAL_H
#define SC2_TABLETOP_MODELS_INTERNAL_H

#include "sc2_tabletop_models.h"
#include "sc2_tabletop_assets_internal.h"

struct bzSC2Model {
    int refcount;
    bool placeholder;
    bzSC2MResult_t status;
    bzSC2MModelInfo_t info;
    size_t allocation_size;
    uint32_t vertices_offset, indices_offset, divisions_offset, regions_offset, batches_offset;
    uint32_t bone_lookup_offset, material_references_offset, materials_offset, composite_sections_offset;
    uint32_t layers_offset;
    struct bzSC2Model *cache_next;
    unsigned char data[];
};

#endif

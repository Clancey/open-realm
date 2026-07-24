#include "wc3_tabletop_assets_internal.h"

#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MDX_MAGIC 0x584c444du
#define MDX_MAX_RECORDS 65536u
#define MDX_TEXTURE_RECORD 268u
#define MDX_SEQUENCE_RECORD 132u

#define FOURCC(a,b,c,d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct {
    const uint8_t *data;
    size_t size, pos;
} mdxReader_t;

typedef struct {
    bzTTGeosetInfo_t info;
    bzTTVec3_t *vertices, *normals;
    bzTTVec2_t *uvs;
    uint16_t *indices;
} mdxTempGeoset_t;

typedef struct {
    mdxTempGeoset_t *geosets;
    bzTTMaterialInfo_t *materials;
    bzTTMaterialLayerInfo_t *layers;
    bzTTModelTextureInfo_t *textures;
    bzTTSequenceInfo_t *sequences;
    bzTTNodeInfo_t *nodes;
    uint32_t geoset_count, material_count, layer_count, texture_count, sequence_count, node_count;
    bzTTBounds3_t bounds;
    uint32_t version;
} mdxTempModel_t;

static bool read_bytes(mdxReader_t *r, void *out, size_t bytes) {
    if (!r || r->pos > r->size || bytes > r->size - r->pos) return false;
    if (out) memcpy(out, r->data + r->pos, bytes);
    r->pos += bytes;
    return true;
}

static bool read_u32(mdxReader_t *r, uint32_t *out) { return read_bytes(r, out, 4); }

static bool subreader(mdxReader_t *r, size_t bytes, mdxReader_t *out) {
    if (!r || !out || r->pos > r->size || bytes > r->size - r->pos) return false;
    *out = (mdxReader_t){ r->data + r->pos, bytes, 0 };
    r->pos += bytes;
    return true;
}

static size_t mdx_align_size(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static bool append_record(void **items, uint32_t *count, size_t item_size, const void *item) {
    void *next;
    if (*count >= MDX_MAX_RECORDS || item_size > SIZE_MAX / ((size_t)*count + 1)) return false;
    next = realloc(*items, ((size_t)*count + 1) * item_size);
    if (!next) return false;
    *items = next;
    memcpy((uint8_t *)next + (size_t)(*count) * item_size, item, item_size);
    (*count)++;
    return true;
}

static bool read_bounds(mdxReader_t *r, bzTTBounds3_t *bounds) {
    float radius;
    if (!read_bytes(r, &radius, sizeof(radius)) ||
        !read_bytes(r, &bounds->min, sizeof(bounds->min)) ||
        !read_bytes(r, &bounds->max, sizeof(bounds->max)))
        return false;
    bounds->radius = radius;
    return true;
}

static void free_temp_model(mdxTempModel_t *model) {
    if (!model) return;
    for (uint32_t i = 0; i < model->geoset_count; i++) {
        free(model->geosets[i].vertices); free(model->geosets[i].normals);
        free(model->geosets[i].uvs); free(model->geosets[i].indices);
    }
    free(model->geosets); free(model->materials); free(model->layers);
    free(model->textures); free(model->sequences); free(model->nodes);
    memset(model, 0, sizeof(*model));
}

static bool read_counted_array(mdxReader_t *r, uint32_t item_size, void **out, uint32_t *count) {
    size_t bytes;
    if (!read_u32(r, count) || *count > MDX_MAX_RECORDS) return false;
    if (!*count) { *out = NULL; return true; }
    if (
        item_size > SIZE_MAX / *count || (bytes = (size_t)*count * item_size) > r->size - r->pos)
        return false;
    *out = malloc(bytes);
    return *out && read_bytes(r, *out, bytes);
}

static bool parse_sequences(mdxReader_t *r, mdxTempModel_t *model) {
    if (r->size % MDX_SEQUENCE_RECORD) return false;
    while (r->pos < r->size) {
        bzTTSequenceInfo_t sequence;
        mdxReader_t record;
        memset(&sequence, 0, sizeof(sequence));
        if (!subreader(r, MDX_SEQUENCE_RECORD, &record) ||
            !read_bytes(&record, sequence.name, sizeof(sequence.name)) ||
            !read_u32(&record, &sequence.start_msec) || !read_u32(&record, &sequence.end_msec) ||
            !read_bytes(&record, &sequence.move_speed, 4) || !read_u32(&record, &sequence.flags) ||
            !read_bytes(&record, &sequence.rarity, 4) ||
            !read_bytes(&record, &sequence.sync_point, 4) || !read_bounds(&record, &sequence.bounds))
            return false;
        sequence.name[sizeof(sequence.name) - 1] = '\0';
        if (!append_record((void **)&model->sequences, &model->sequence_count, sizeof(sequence), &sequence))
            return false;
    }
    return true;
}

static bool parse_textures(mdxReader_t *r, mdxTempModel_t *model) {
    if (r->size % MDX_TEXTURE_RECORD) return false;
    while (r->pos < r->size) {
        bzTTModelTextureInfo_t texture;
        memset(&texture, 0, sizeof(texture));
        if (!read_u32(r, &texture.replaceable_id) ||
            !read_bytes(r, texture.identity, sizeof(texture.identity)) ||
            !read_u32(r, &texture.wrapping_flags))
            return false;
        texture.identity[sizeof(texture.identity) - 1] = '\0';
        if (texture.identity[0] && !wc3_tta_path_is_confined(texture.identity)) return false;
        if (!append_record((void **)&model->textures, &model->texture_count, sizeof(texture), &texture))
            return false;
    }
    return true;
}

static bool parse_layer(mdxReader_t *r, mdxTempModel_t *model) {
    bzTTMaterialLayerInfo_t layer;
    uint32_t blend;
    memset(&layer, 0, sizeof(layer));
    if (!read_u32(r, &blend) || blend > BZ_TTA_BLEND_MODULATE_2X ||
        !read_u32(r, &layer.flags) || !read_u32(r, &layer.texture_index) ||
        !read_bytes(r, &layer.transform_index, 4) || !read_bytes(r, &layer.uv_channel, 4) ||
        !read_bytes(r, &layer.alpha, 4))
        return false;
    layer.blend_mode = (bzTTBlendMode_t)blend;
    return append_record((void **)&model->layers, &model->layer_count, sizeof(layer), &layer);
}

static bool parse_materials(mdxReader_t *r, mdxTempModel_t *model) {
    while (r->pos < r->size) {
        uint32_t record_size, tag, layer_count;
        size_t record_start = r->pos, record_end;
        bzTTMaterialInfo_t material;
        memset(&material, 0, sizeof(material));
        if (!read_u32(r, &record_size) || record_size < 12 ||
            record_size > r->size - record_start)
            return false;
        record_end = record_start + record_size; /* Classic MDX record sizes include this DWORD. */
        if (!read_bytes(r, &material.priority, 4) || !read_u32(r, &material.flags))
            return false;
        material.first_layer = model->layer_count;
        while (r->pos + 4 <= record_end) {
            if (!read_u32(r, &tag)) return false;
            if (tag != FOURCC('L','A','Y','S')) {
                r->pos = record_end;
                break;
            }
            if (r->pos + 4 > record_end || !read_u32(r, &layer_count) ||
                layer_count > MDX_MAX_RECORDS)
                return false;
            for (uint32_t i = 0; i < layer_count; i++) {
                uint32_t layer_size;
                size_t layer_start = r->pos, layer_end;
                mdxReader_t layer_reader;
                if (r->pos + 4 > record_end || !read_u32(r, &layer_size) || layer_size < 28 ||
                    layer_start > record_end ||
                    layer_size > record_end - layer_start)
                    return false;
                layer_end = layer_start + layer_size;
                layer_reader = (mdxReader_t){ r->data + r->pos, layer_end - r->pos, 0 };
                if (!parse_layer(&layer_reader, model)) return false;
                r->pos = layer_end;
            }
        }
        material.layer_count = model->layer_count - material.first_layer;
        if (!append_record((void **)&model->materials, &model->material_count,
                           sizeof(material), &material))
            return false;
        r->pos = record_end;
    }
    return true;
}

static bool parse_geoset_mats(mdxReader_t *r, mdxTempGeoset_t *geoset) {
    uint32_t matrix_count, bounds_count;
    size_t matrix_bytes, bounds_bytes;
    int32_t material_index;
    uint32_t ignored;
    if (!read_u32(r, &matrix_count) || matrix_count > MDX_MAX_RECORDS ||
        (matrix_bytes = (size_t)matrix_count * 4) > r->size - r->pos ||
        !read_bytes(r, NULL, matrix_bytes) || !read_bytes(r, &material_index, 4) ||
        !read_u32(r, &ignored) || !read_u32(r, &ignored) ||
        !read_bounds(r, &geoset->info.bounds) || !read_u32(r, &bounds_count) ||
        bounds_count > MDX_MAX_RECORDS ||
        (bounds_bytes = (size_t)bounds_count * 28) > r->size - r->pos ||
        !read_bytes(r, NULL, bounds_bytes))
        return false;
    geoset->info.material_index = material_index < 0 ? UINT32_MAX : (uint32_t)material_index;
    return true;
}

static bool parse_geoset(mdxReader_t *r, mdxTempModel_t *model) {
    mdxTempGeoset_t geoset;
    bool saw_mats = false;
    memset(&geoset, 0, sizeof(geoset));
    geoset.info.material_index = UINT32_MAX;
    while (r->pos + 4 <= r->size) {
        uint32_t tag, count, ignored_count;
        void *items = NULL;
        if (!read_u32(r, &tag)) goto fail;
        switch (tag) {
            case FOURCC('V','R','T','X'):
                if (!read_counted_array(r, sizeof(bzTTVec3_t), &items, &count)) goto fail;
                free(geoset.vertices); geoset.vertices = items; geoset.info.vertex_count = count; break;
            case FOURCC('N','R','M','S'):
                if (!read_counted_array(r, sizeof(bzTTVec3_t), &items, &count)) goto fail;
                free(geoset.normals); geoset.normals = items; geoset.info.normal_count = count; break;
            case FOURCC('U','V','B','S'):
                if (!read_counted_array(r, sizeof(bzTTVec2_t), &items, &count)) goto fail;
                free(geoset.uvs); geoset.uvs = items; geoset.info.uv_count = count; break;
            case FOURCC('P','V','T','X'):
                if (!read_counted_array(r, sizeof(uint16_t), &items, &count)) goto fail;
                free(geoset.indices); geoset.indices = items; geoset.info.index_count = count; break;
            case FOURCC('G','N','D','X'):
                if (!read_counted_array(r, 1, &items, &count)) goto fail;
                free(items); geoset.info.vertex_group_count = count; break;
            case FOURCC('P','T','Y','P'):
            case FOURCC('P','C','N','T'):
            case FOURCC('M','T','G','C'):
                if (!read_counted_array(r, 4, &items, &ignored_count)) goto fail;
                free(items); break;
            case FOURCC('U','V','A','S'):
                if (!read_u32(r, &ignored_count)) goto fail; break;
            case FOURCC('M','A','T','S'):
                if (!parse_geoset_mats(r, &geoset)) goto fail;
                saw_mats = true; r->pos = r->size; break;
            default:
                goto fail;
        }
    }
    if (!saw_mats || !geoset.info.vertex_count || !geoset.info.index_count ||
        geoset.info.normal_count != geoset.info.vertex_count ||
        (geoset.info.uv_count && geoset.info.uv_count != geoset.info.vertex_count))
        goto fail;
    for (uint32_t i = 0; i < geoset.info.index_count; i++)
        if (geoset.indices[i] >= geoset.info.vertex_count) goto fail;
    if (!append_record((void **)&model->geosets, &model->geoset_count, sizeof(geoset), &geoset))
        goto fail;
    return true;
fail:
    free(geoset.vertices); free(geoset.normals); free(geoset.uvs); free(geoset.indices);
    return false;
}

static bool parse_geosets(mdxReader_t *r, mdxTempModel_t *model) {
    while (r->pos < r->size) {
        uint32_t record_size;
        size_t record_start = r->pos, record_end;
        mdxReader_t record;
        if (!read_u32(r, &record_size) || record_size < 4 ||
            record_size > r->size - record_start)
            return false;
        record_end = record_start + record_size;
        record = (mdxReader_t){ r->data + r->pos, record_end - r->pos, 0 };
        if (!parse_geoset(&record, model)) return false;
        r->pos = record_end;
    }
    return true;
}

static void parse_initial_track(mdxReader_t *r, uint32_t tag, bzTTNodeInfo_t *node) {
    uint32_t keys, line_type, global, frame;
    size_t values = tag == FOURCC('K','G','R','T') ? 16 : 12;
    if (!read_u32(r, &keys) || !read_u32(r, &line_type) || !read_u32(r, &global) ||
        !keys || keys > MDX_MAX_RECORDS || !read_u32(r, &frame) || values > r->size - r->pos) {
        r->pos = r->size;
        return;
    }
    (void)global; (void)frame;
    if (tag == FOURCC('K','G','T','R')) read_bytes(r, &node->initial_translation, 12);
    else if (tag == FOURCC('K','G','S','C')) read_bytes(r, &node->initial_scale, 12);
    else {
        read_bytes(r, &node->initial_rotation_x, 4); read_bytes(r, &node->initial_rotation_y, 4);
        read_bytes(r, &node->initial_rotation_z, 4); read_bytes(r, &node->initial_rotation_w, 4);
    }
    {
        size_t tangent_sets = line_type >= 2 ? 2 : 0;
        size_t key_size = 4 + values * (1 + tangent_sets);
        size_t remaining = (size_t)(keys - 1) * key_size + tangent_sets * values;
        if (remaining > r->size - r->pos) r->pos = r->size;
        else r->pos += remaining;
    }
}

static bool parse_nodes(mdxReader_t *r, mdxTempModel_t *model, bool bones) {
    while (r->pos < r->size) {
        uint32_t record_size, tag;
        size_t record_start = r->pos, node_end, total_end;
        bzTTNodeInfo_t node;
        mdxReader_t node_reader;
        memset(&node, 0, sizeof(node));
        node.parent_id = UINT32_MAX;
        node.initial_rotation_w = 1;
        node.initial_scale = (bzTTVec3_t){ 1, 1, 1 };
        if (!read_u32(r, &record_size) || record_size < 96) return false;
        node_end = record_start + record_size;
        total_end = node_end + (bones ? 8 : 0);
        if (total_end > r->size || node_end < r->pos) return false;
        node_reader = (mdxReader_t){ r->data + r->pos, node_end - r->pos, 0 };
        if (!read_bytes(&node_reader, node.name, sizeof(node.name)) ||
            !read_u32(&node_reader, &node.object_id) || !read_u32(&node_reader, &node.parent_id) ||
            !read_u32(&node_reader, &node.flags))
            return false;
        node.name[sizeof(node.name) - 1] = '\0';
        while (node_reader.pos + 4 <= node_reader.size) {
            if (!read_u32(&node_reader, &tag)) return false;
            if (tag != FOURCC('K','G','T','R') && tag != FOURCC('K','G','R','T') &&
                tag != FOURCC('K','G','S','C'))
                return false;
            parse_initial_track(&node_reader, tag, &node);
        }
        if (!append_record((void **)&model->nodes, &model->node_count, sizeof(node), &node))
            return false;
        r->pos = total_end;
    }
    return true;
}

static void assign_pivots(mdxReader_t *r, mdxTempModel_t *model) {
    uint32_t count = (uint32_t)(r->size / sizeof(bzTTVec3_t));
    if (r->size % sizeof(bzTTVec3_t)) return;
    for (uint32_t i = 0; i < model->node_count; i++)
        if (model->nodes[i].object_id < count)
            memcpy(&model->nodes[i].pivot, r->data +
                   model->nodes[i].object_id * sizeof(bzTTVec3_t), sizeof(bzTTVec3_t));
}

static bool parse_mdx(const uint8_t *data, size_t size, mdxTempModel_t *model) {
    mdxReader_t file = { data, size, 4 };
    uint32_t tag, chunk_size;
    mdxReader_t chunk;
    if (size < 4 || memcmp(data, "MDLX", 4)) return false;
    while (file.pos < file.size) {
        if (!read_u32(&file, &tag) || !read_u32(&file, &chunk_size) || !subreader(&file, chunk_size, &chunk))
            return false;
        switch (tag) {
            case FOURCC('V','E','R','S'):
                if (!read_u32(&chunk, &model->version) || model->version != 800) return false; break;
            case FOURCC('M','O','D','L'):
                if (chunk.size < 372) return false;
                chunk.pos = 340;
                if (!read_bounds(&chunk, &model->bounds)) return false;
                break;
            case FOURCC('S','E','Q','S'): if (!parse_sequences(&chunk, model)) return false; break;
            case FOURCC('T','E','X','S'): if (!parse_textures(&chunk, model)) return false; break;
            case FOURCC('M','T','L','S'): if (!parse_materials(&chunk, model)) return false; break;
            case FOURCC('G','E','O','S'): if (!parse_geosets(&chunk, model)) return false; break;
            case FOURCC('B','O','N','E'): if (!parse_nodes(&chunk, model, true)) return false; break;
            case FOURCC('H','E','L','P'): if (!parse_nodes(&chunk, model, false)) return false; break;
            case FOURCC('P','I','V','T'): assign_pivots(&chunk, model); break;
            default: break; /* Known-but-unexported chunks remain bounded by the top-level chunk size. */
        }
    }
    return model->version == 800 && model->geoset_count > 0;
}

static bool payload_add(size_t *payload, size_t count, size_t item_size, size_t alignment, uint32_t *offset) {
    size_t start = mdx_align_size(*payload, alignment), bytes;
    if (count > SIZE_MAX / item_size || (bytes = count * item_size) > SIZE_MAX - start ||
        start > UINT32_MAX || start + bytes > UINT32_MAX)
        return false;
    *offset = (uint32_t)start;
    *payload = start + bytes;
    return true;
}

static bzTTAsset_t *flatten_model(const mdxTempModel_t *src, const char *identity,
                                  const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    size_t payload = 0;
    uint32_t geosets_offset, materials_offset, layers_offset, textures_offset, sequences_offset, nodes_offset;
    bzTTAsset_t *asset;
    bzTTGeosetRecord_t *records;
    if (!payload_add(&payload, src->geoset_count, sizeof(*records),
                     _Alignof(bzTTGeosetRecord_t), &geosets_offset) ||
        !payload_add(&payload, src->material_count, sizeof(*src->materials),
                     _Alignof(bzTTMaterialInfo_t), &materials_offset) ||
        !payload_add(&payload, src->layer_count, sizeof(*src->layers),
                     _Alignof(bzTTMaterialLayerInfo_t), &layers_offset) ||
        !payload_add(&payload, src->texture_count, sizeof(*src->textures),
                     _Alignof(bzTTModelTextureInfo_t), &textures_offset) ||
        !payload_add(&payload, src->sequence_count, sizeof(*src->sequences),
                     _Alignof(bzTTSequenceInfo_t), &sequences_offset) ||
        !payload_add(&payload, src->node_count, sizeof(*src->nodes), _Alignof(bzTTNodeInfo_t), &nodes_offset)) {
        *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL;
    }
    for (uint32_t i = 0; i < src->geoset_count; i++) {
        uint32_t ignored;
        if (!payload_add(&payload, src->geosets[i].info.vertex_count, sizeof(bzTTVec3_t),
                         _Alignof(bzTTVec3_t), &ignored) ||
            !payload_add(&payload, src->geosets[i].info.normal_count, sizeof(bzTTVec3_t),
                         _Alignof(bzTTVec3_t), &ignored) ||
            !payload_add(&payload, src->geosets[i].info.uv_count, sizeof(bzTTVec2_t),
                         _Alignof(bzTTVec2_t), &ignored) ||
            !payload_add(&payload, src->geosets[i].info.index_count, sizeof(uint16_t),
                         _Alignof(uint16_t), &ignored)) {
            *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL;
        }
    }
    asset = BZ_TTA_AssetAlloc(payload, BZ_TTA_ASSET_MODEL, identity, metadata);
    if (!asset) { *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL; }
    asset->u.model.info = (bzTTModelInfo_t){
        .version = src->version, .geoset_count = src->geoset_count,
        .material_count = src->material_count, .layer_count = src->layer_count,
        .texture_count = src->texture_count, .sequence_count = src->sequence_count,
        .node_count = src->node_count, .bounds = src->bounds,
    };
    asset->u.model.geosets_offset = geosets_offset; asset->u.model.materials_offset = materials_offset;
    asset->u.model.layers_offset = layers_offset; asset->u.model.textures_offset = textures_offset;
    asset->u.model.sequences_offset = sequences_offset; asset->u.model.nodes_offset = nodes_offset;
    records = BZ_TTA_AssetData(asset, geosets_offset, (size_t)src->geoset_count * sizeof(*records));
    if (src->material_count)
        memcpy(BZ_TTA_AssetData(asset, materials_offset, (size_t)src->material_count * sizeof(*src->materials)),
              src->materials, (size_t)src->material_count * sizeof(*src->materials));
    if (src->layer_count)
        memcpy(BZ_TTA_AssetData(asset, layers_offset, (size_t)src->layer_count * sizeof(*src->layers)),
              src->layers, (size_t)src->layer_count * sizeof(*src->layers));
    if (src->texture_count)
        memcpy(BZ_TTA_AssetData(asset, textures_offset, (size_t)src->texture_count * sizeof(*src->textures)),
              src->textures, (size_t)src->texture_count * sizeof(*src->textures));
    if (src->sequence_count)
        memcpy(BZ_TTA_AssetData(asset, sequences_offset, (size_t)src->sequence_count * sizeof(*src->sequences)),
              src->sequences, (size_t)src->sequence_count * sizeof(*src->sequences));
    if (src->node_count)
        memcpy(BZ_TTA_AssetData(asset, nodes_offset, (size_t)src->node_count * sizeof(*src->nodes)),
              src->nodes, (size_t)src->node_count * sizeof(*src->nodes));
    payload = nodes_offset + (size_t)src->node_count * sizeof(*src->nodes);
    for (uint32_t i = 0; i < src->geoset_count; i++) {
        const mdxTempGeoset_t *geo = src->geosets + i;
        records[i].info = geo->info;
#define COPY_ARRAY(FIELD, COUNT, TYPE, SRC) \
        payload = mdx_align_size(payload, _Alignof(TYPE)); records[i].FIELD = (uint32_t)payload; \
        if (geo->info.COUNT) memcpy(BZ_TTA_AssetData(asset, records[i].FIELD, \
            (size_t)geo->info.COUNT * sizeof(TYPE)), geo->SRC, (size_t)geo->info.COUNT * sizeof(TYPE)); \
        payload += (size_t)geo->info.COUNT * sizeof(TYPE)
        COPY_ARRAY(vertices_offset, vertex_count, bzTTVec3_t, vertices);
        COPY_ARRAY(normals_offset, normal_count, bzTTVec3_t, normals);
        COPY_ARRAY(uvs_offset, uv_count, bzTTVec2_t, uvs);
        COPY_ARRAY(indices_offset, index_count, uint16_t, indices);
#undef COPY_ARRAY
    }
    *status = BZ_TTA_OK;
    return asset;
}

bzTTAsset_t *BZ_WC3_TTA_DecodeMDX(const void *data, size_t size, const char *identity,
                                   const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    mdxTempModel_t model;
    bzTTAsset_t *asset;
    if (!data || !status) return NULL;
    memset(&model, 0, sizeof(model));
    if (!parse_mdx(data, size, &model)) {
        free_temp_model(&model);
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    asset = flatten_model(&model, identity, metadata, status);
    free_temp_model(&model);
    return asset;
}

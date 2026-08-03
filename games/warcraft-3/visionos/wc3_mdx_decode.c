#include "wc3_tabletop_assets_internal.h"

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MDX_MAGIC 0x584c444du
#define MDX_MAX_RECORDS 65536u
#define MDX_TEXTURE_RECORD 268u
#define MDX_SEQUENCE_RECORD 132u
/* PRE2 particle emitter v2 fixed trailing fields (everything after the nested generic/inner
 * node size+header+tracks - see mdxTempEmitter_t's doc comment): Speed..Width (8 floats=32) +
 * FilterMode,Rows,Columns,FrameFlags (4 dwords=16) + TailLength,Time (2 floats=8) +
 * SegmentColor[9] (36) + Alpha[3] (3) + ParticleScaling[3] (12) + 4x{start,end,repeat} UV-anim
 * dword triples (48) + TextureID,Squirt,PriorityPlane,ReplaceableId (4 dwords=16). Matches
 * games/warcraft-3/renderer/mdx/r_mdx_load.c's ReadParticleEmitter field order exactly - see
 * games/warcraft-3/docs/file-formats/mdx.md's "Particle Emitters (PRE2)" section. */
#define MDX_PRE2_TRAILING_FIELDS_SIZE (32 + 16 + 8 + 36 + 3 + 12 + 48 + 16)

#define FOURCC(a,b,c,d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

typedef struct {
    const uint8_t *data;
    size_t size, pos;
} mdxReader_t;

typedef struct {
    uint32_t key_count;
    uint32_t interp;         /* bzTTKeyInterp_t */
    uint32_t global_sequence; /* raw GLBS index as read from file, or BZ_TTA_NO_GLOBAL_SEQUENCE */
    void *keys;              /* bzTTVec3Key_t*, bzTTQuatKey_t*, or bzTTFloatKey_t*, per parser used */
} mdxTempTrack_t;

typedef struct {
    bzTTGeosetInfo_t info;
    bzTTVec3_t *vertices, *normals;
    bzTTVec2_t *uvs;
    uint16_t *indices;
    /* Raw skin-resolution inputs (GNDX/MTGC/MATS), owned until resolve_geoset_skin runs. */
    uint8_t *vertex_groups; uint32_t vertex_group_count;
    uint32_t *matrix_group_sizes; uint32_t matrix_group_size_count;
    uint32_t *matrices; uint32_t matrix_count; /* raw MDX object_ids */
    /* Resolved (post-processed) skin + per-geoset matrix palette. */
    bzTTVertexSkin_t *skin;
    uint32_t *palette; uint32_t palette_count;
    /* GEOA (geoset alpha animation), matched in by geosetId after top-level parsing. */
    bool has_anim;
    float static_alpha;
    mdxTempTrack_t alpha;
} mdxTempGeoset_t;

typedef struct {
    bzTTNodeInfo_t info;
    mdxTempTrack_t translation, rotation, scale;
} mdxTempNode_t;

/* One PRE2 particle emitter. `info.node_index` is filled in immediately after the emitter's
 * own node record is appended to model->nodes (see parse_particle_emitters()) - the emitter IS
 * a node (mdxParticleEmitter_t embeds mdxNode_t in the classic renderer, r_mdx.h), not a
 * separate hierarchy, so no parent/pivot/transform-track fields are duplicated here. The 8
 * tracks below are this emitter's own KP2x channels (see bzTTEmitterChannel_t) - always floats,
 * unlike node translation/rotation/scale which need 3 distinct value types. */
typedef struct {
    bzTTParticleEmitterInfo_t info;
    mdxTempTrack_t visibility, emission_rate, width, length, speed, latitude, gravity, variation;
} mdxTempEmitter_t;

typedef struct {
    uint32_t geoset_id; /* GEOS index, or UINT32_MAX = none */
    float static_alpha;
    mdxTempTrack_t alpha;
} mdxTempGeosetAnim_t;

typedef struct {
    mdxTempGeoset_t *geosets;
    bzTTMaterialInfo_t *materials;
    bzTTMaterialLayerInfo_t *layers;
    bzTTModelTextureInfo_t *textures;
    bzTTSequenceInfo_t *sequences;
    mdxTempNode_t *nodes;
    uint32_t *global_sequences;
    mdxTempGeosetAnim_t *geoset_anims;
    mdxTempEmitter_t *emitters;
    uint32_t geoset_count, material_count, layer_count, texture_count, sequence_count, node_count;
    uint32_t global_sequence_count, geoset_anim_count, emitter_count;
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

/* Reads one full KGxx/KGAO-style keytrack: keyCount(4) + lineType(4) + globalSeqId(4) +
 * keyCount keyframes, each time(4) + value + (interp>=HERMITE ? in_tan + out_tan : nothing).
 * On-disk per-key order is value, in-tangent, out-tangent (renderer/mdx/r_mdx_interpolation.c
 * KEY_VALUE/KEY_INTAN/KEY_OUTTAN), which is exactly the bzTTVec3Key_t/bzTTQuatKey_t/
 * bzTTFloatKey_t field order, so each key is read directly into its final struct layout.
 * GetModelKeyFrameSize (r_mdx_load.c) is 4 + dataTypeSize*(interp>=HERMITE ? 3 : 1). */
#define DEFINE_TRACK_PARSER(NAME, KEYTYPE) \
static bool NAME(mdxReader_t *r, mdxTempTrack_t *out) { \
    uint32_t key_count, interp, global_seq; \
    size_t tan_bytes; KEYTYPE *keys = NULL; \
    if (!read_u32(r, &key_count) || key_count > MDX_MAX_RECORDS || \
        !read_u32(r, &interp) || interp > BZ_TTA_INTERP_BEZIER || !read_u32(r, &global_seq)) \
        return false; \
    tan_bytes = interp >= BZ_TTA_INTERP_HERMITE ? sizeof(keys[0].in_tan) + sizeof(keys[0].out_tan) : 0; \
    if (key_count) { \
        if ((size_t)(4 + sizeof(keys[0].value) + tan_bytes) > (r->size - r->pos) / key_count) return false; \
        keys = calloc(key_count, sizeof(KEYTYPE)); \
        if (!keys) return false; \
    } \
    for (uint32_t i = 0; i < key_count; i++) { \
        if (!read_u32(r, &keys[i].time_msec) || !read_bytes(r, &keys[i].value, sizeof(keys[i].value))) \
            { free(keys); return false; } \
        if (tan_bytes && (!read_bytes(r, &keys[i].in_tan, sizeof(keys[i].in_tan)) || \
                          !read_bytes(r, &keys[i].out_tan, sizeof(keys[i].out_tan)))) \
            { free(keys); return false; } \
    } \
    out->key_count = key_count; out->interp = interp; out->global_sequence = global_seq; out->keys = keys; \
    return true; \
}
DEFINE_TRACK_PARSER(parse_vec3_track, bzTTVec3Key_t)
DEFINE_TRACK_PARSER(parse_quat_track, bzTTQuatKey_t)
DEFINE_TRACK_PARSER(parse_float_track, bzTTFloatKey_t)
#undef DEFINE_TRACK_PARSER

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
        free(model->geosets[i].vertex_groups); free(model->geosets[i].matrix_group_sizes);
        free(model->geosets[i].matrices); free(model->geosets[i].skin); free(model->geosets[i].palette);
        free(model->geosets[i].alpha.keys);
    }
    for (uint32_t i = 0; i < model->node_count; i++) {
        free(model->nodes[i].translation.keys); free(model->nodes[i].rotation.keys);
        free(model->nodes[i].scale.keys);
    }
    for (uint32_t i = 0; i < model->geoset_anim_count; i++) free(model->geoset_anims[i].alpha.keys);
    for (uint32_t i = 0; i < model->emitter_count; i++) {
        mdxTempEmitter_t *e = &model->emitters[i];
        free(e->visibility.keys); free(e->emission_rate.keys); free(e->width.keys); free(e->length.keys);
        free(e->speed.keys); free(e->latitude.keys); free(e->gravity.keys); free(e->variation.keys);
    }
    free(model->geosets); free(model->materials); free(model->layers);
    free(model->textures); free(model->sequences); free(model->nodes);
    free(model->global_sequences); free(model->geoset_anims); free(model->emitters);
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
    size_t bounds_bytes;
    int32_t material_index;
    uint32_t ignored;
    void *matrices = NULL;
    if (!read_counted_array(r, 4, &matrices, &matrix_count)) return false;
    if (!read_bytes(r, &material_index, 4) ||
        !read_u32(r, &ignored) || !read_u32(r, &ignored) ||
        !read_bounds(r, &geoset->info.bounds) || !read_u32(r, &bounds_count) ||
        bounds_count > MDX_MAX_RECORDS ||
        (bounds_bytes = (size_t)bounds_count * 28) > r->size - r->pos ||
        !read_bytes(r, NULL, bounds_bytes)) {
        free(matrices);
        return false;
    }
    geoset->info.material_index = material_index < 0 ? UINT32_MAX : (uint32_t)material_index;
    free(geoset->matrices);
    geoset->matrices = matrices; geoset->matrix_count = matrix_count;
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
                free(geoset.vertex_groups); geoset.vertex_groups = items; geoset.vertex_group_count = count;
                geoset.info.vertex_group_count = count; break;
            case FOURCC('P','T','Y','P'):
            case FOURCC('P','C','N','T'):
                if (!read_counted_array(r, 4, &items, &ignored_count)) goto fail;
                free(items); break;
            case FOURCC('M','T','G','C'):
                if (!read_counted_array(r, 4, &items, &count)) goto fail;
                free(geoset.matrix_group_sizes); geoset.matrix_group_sizes = items;
                geoset.matrix_group_size_count = count; break;
            case FOURCC('U','V','A','S'):
                if (!read_u32(r, &ignored_count)) goto fail; break;
            case FOURCC('M','A','T','S'):
                if (!parse_geoset_mats(r, &geoset)) goto fail;
                /* Retail MDX stores UVAS/UVBS after MATS; stopping here discarded every authored model UV. */
                saw_mats = true; break;
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
    free(geoset.vertex_groups); free(geoset.matrix_group_sizes); free(geoset.matrices);
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

/* Reads one node's header (name/objectId/parentId/flags) plus its own KGTR/KGRT/KGSC tracks
 * from `node_reader` (already bounded to exactly this portion by the caller - see this
 * function's two call sites: parse_nodes() for HELP/BONE, and parse_particle_emitters() for
 * PRE2's nested inner/generic size), then fills the rest-pose initial_translation/rotation/
 * scale from each track's first keyframe when present. Factored out of parse_nodes() (which
 * used to inline this) so PRE2 emitters - which are ordinary nodes wrapped in an additional
 * outer per-emitter record (see mdxTempEmitter_t's doc comment) - reuse the identical
 * header/track parsing rather than a second copy of this control flow. */
static bool parse_node_header_and_tracks(mdxReader_t *node_reader, mdxTempNode_t *node) {
    uint32_t tag;
    node->info.parent_id = UINT32_MAX;
    node->info.initial_rotation_w = 1;
    node->info.initial_scale = (bzTTVec3_t){ 1, 1, 1 };
    if (!read_bytes(node_reader, node->info.name, sizeof(node->info.name)) ||
        !read_u32(node_reader, &node->info.object_id) || !read_u32(node_reader, &node->info.parent_id) ||
        !read_u32(node_reader, &node->info.flags))
        return false;
    node->info.name[sizeof(node->info.name) - 1] = '\0';
    while (node_reader->pos + 4 <= node_reader->size) {
        if (!read_u32(node_reader, &tag)) return false;
        if (tag == FOURCC('K','G','T','R')) { if (!parse_vec3_track(node_reader, &node->translation)) return false; }
        else if (tag == FOURCC('K','G','S','C')) { if (!parse_vec3_track(node_reader, &node->scale)) return false; }
        else if (tag == FOURCC('K','G','R','T')) { if (!parse_quat_track(node_reader, &node->rotation)) return false; }
        else return false;
    }
    /* Rest pose falls back to the track's first keyframe when present, matching the classic
     * renderer's node bind pose (the value used before any sequence samples it). */
    if (node->translation.key_count)
        node->info.initial_translation = ((bzTTVec3Key_t *)node->translation.keys)[0].value;
    if (node->scale.key_count)
        node->info.initial_scale = ((bzTTVec3Key_t *)node->scale.keys)[0].value;
    if (node->rotation.key_count) {
        bzTTQuat_t q = ((bzTTQuatKey_t *)node->rotation.keys)[0].value;
        node->info.initial_rotation_x = q.x; node->info.initial_rotation_y = q.y;
        node->info.initial_rotation_z = q.z; node->info.initial_rotation_w = q.w;
    }
    return true;
}

static bool parse_nodes(mdxReader_t *r, mdxTempModel_t *model, bool bones) {
    while (r->pos < r->size) {
        uint32_t record_size;
        size_t record_start = r->pos, node_end, total_end;
        mdxTempNode_t node;
        mdxReader_t node_reader;
        memset(&node, 0, sizeof(node));
        if (!read_u32(r, &record_size) || record_size < 96) return false;
        node_end = record_start + record_size;
        total_end = node_end + (bones ? 8 : 0);
        if (total_end > r->size || node_end < r->pos) return false;
        node_reader = (mdxReader_t){ r->data + r->pos, node_end - r->pos, 0 };
        if (!parse_node_header_and_tracks(&node_reader, &node)) goto node_fail;
        if (!append_record((void **)&model->nodes, &model->node_count, sizeof(node), &node))
            goto node_fail;
        r->pos = total_end;
        continue;
node_fail:
        free(node.translation.keys); free(node.rotation.keys); free(node.scale.keys);
        return false;
    }
    return true;
}

/* PRE2 particle emitter v2. On-disk layout is a NESTED double size field (see
 * docs/quest-tabletop.md's Layer 9 section and games/warcraft-3/docs/file-formats/mdx.md for
 * the full empirical trace and citations): an OUTER inclusive size (read here, per-record,
 * exactly like parse_nodes()'s own per-record `record_size`) wraps the ENTIRE emitter
 * (generic node header/tracks + all PRE2-specific fields + PRE2's own KP2x tracks); a SECOND,
 * INNER inclusive size immediately follows and wraps ONLY the generic node header + its own
 * KGTR/KGRT/KGSC tracks (parsed via the shared parse_node_header_and_tracks() helper above -
 * an emitter IS a node, mdxParticleEmitter_t embeds mdxNode_t in the classic renderer). This
 * matches Light/LITE and Attachment/ATCH exactly (r_mdx_load.c's ReadLight/ReadAttachment use
 * the identical two-size-field shape) - confirmed empirically against the real host parser
 * (r_mdx_load.c) with a byte-exact synthetic fixture round-trip, not guessed. */
static bool parse_particle_emitters(mdxReader_t *r, mdxTempModel_t *model) {
    /* Raw on-disk FilterMode (Blend/Additive/Modulate/Modulate2x/AlphaKey, in THIS declared
     * order) translated to the shared bzTTBlendMode_t vocabulary - NOT the same numeric
     * convention as material layers' own blend_mode (bzTTMaterialLayerInfo_t), which already
     * matches bzTTBlendMode_t directly with no translation. See games/warcraft-3/docs/
     * file-formats/mdx.md for the citation trail. */
    static const uint32_t kFilterModeToBlend[5] = {
        BZ_TTA_BLEND_ALPHA, BZ_TTA_BLEND_ADDITIVE, BZ_TTA_BLEND_MODULATE,
        BZ_TTA_BLEND_MODULATE_2X, BZ_TTA_BLEND_TRANSPARENT,
    };
    while (r->pos < r->size) {
        uint32_t outer_size, inner_size;
        size_t record_start = r->pos, outer_end, inner_end;
        mdxReader_t record, node_reader;
        mdxTempNode_t node;
        mdxTempEmitter_t emitter;
        uint32_t filter_mode, frame_flags, uv_anim_discard[12], squirt_discard, priority_plane_discard;
        memset(&node, 0, sizeof(node));
        memset(&emitter, 0, sizeof(emitter));
        if (!read_u32(r, &outer_size) || outer_size < 4 + 96 + MDX_PRE2_TRAILING_FIELDS_SIZE) return false;
        outer_end = record_start + outer_size;
        if (outer_end > r->size || outer_end < r->pos) return false;
        record = (mdxReader_t){ r->data + r->pos, outer_end - r->pos, 0 };
        if (!read_u32(&record, &inner_size) || inner_size < 96) goto fail;
        inner_end = record.pos + (inner_size - 4);
        if (inner_end > record.size) goto fail;
        node_reader = (mdxReader_t){ record.data + record.pos, inner_end - record.pos, 0 };
        if (!parse_node_header_and_tracks(&node_reader, &node)) goto fail;
        record.pos = inner_end;
        if (!read_bytes(&record, &emitter.info.speed, 4) || !read_bytes(&record, &emitter.info.variation, 4) ||
            !read_bytes(&record, &emitter.info.latitude, 4) || !read_bytes(&record, &emitter.info.gravity, 4) ||
            !read_bytes(&record, &emitter.info.life_span, 4) ||
            !read_bytes(&record, &emitter.info.emission_rate, 4) ||
            !read_bytes(&record, &emitter.info.length, 4) || !read_bytes(&record, &emitter.info.width, 4) ||
            !read_u32(&record, &filter_mode) ||
            !read_u32(&record, &emitter.info.rows) || !read_u32(&record, &emitter.info.columns) ||
            !read_u32(&record, &frame_flags) ||
            !read_bytes(&record, &emitter.info.tail_length, 4) ||
            !read_bytes(&record, &emitter.info.time_middle, 4) ||
            !read_bytes(&record, emitter.info.segment_color, sizeof(emitter.info.segment_color)) ||
            !read_bytes(&record, emitter.info.segment_alpha, sizeof(emitter.info.segment_alpha)) ||
            !read_bytes(&record, emitter.info.particle_scaling, sizeof(emitter.info.particle_scaling)) ||
            /* LifeSpan/Decay/Tail/TailDecay UV-anim {start,end,repeat} triples: parsed (to stay
             * byte-aligned with the real format) but not exported - the authoritative CPU
             * particle simulation (renderer/r_particles.c's FX_GetFrame) never reads them
             * either, deriving the atlas frame purely from the particle's own lifetime
             * fraction - see games/warcraft-3/docs/file-formats/mdx.md. */
            !read_bytes(&record, uv_anim_discard, sizeof(uv_anim_discard)) ||
            !read_u32(&record, &emitter.info.texture_index) || !read_u32(&record, &squirt_discard) ||
            !read_u32(&record, &priority_plane_discard) || !read_u32(&record, &emitter.info.replaceable_id))
            goto fail;
        (void)squirt_discard; (void)priority_plane_discard;
        if (filter_mode >= 5) goto fail; /* out of the documented raw FilterMode range - malformed */
        emitter.info.blend_mode = kFilterModeToBlend[filter_mode];
        if (frame_flags > BZ_TTA_PARTICLE_BOTH) goto fail; /* out of the documented HeadOrTail range */
        emitter.info.head_or_tail = frame_flags;
        /* Rows/Columns 0 means "1x1" for atlas purposes (r_particles.c's FX_GetFrame applies
         * this same `? 1` fallback at draw time on every frame; normalizing once here at
         * decode time is the identical effective behavior without repeating the check every
         * frame this ABI's consumer draws a particle). */
        if (!emitter.info.rows) emitter.info.rows = 1;
        if (!emitter.info.columns) emitter.info.columns = 1;
        while (record.pos + 4 <= record.size) {
            uint32_t tag;
            if (!read_u32(&record, &tag)) goto fail;
            if (tag == FOURCC('K','P','2','V')) { if (!parse_float_track(&record, &emitter.visibility)) goto fail; }
            else if (tag == FOURCC('K','P','2','E')) { if (!parse_float_track(&record, &emitter.emission_rate)) goto fail; }
            else if (tag == FOURCC('K','P','2','W')) { if (!parse_float_track(&record, &emitter.width)) goto fail; }
            else if (tag == FOURCC('K','P','2','N')) { if (!parse_float_track(&record, &emitter.length)) goto fail; }
            else if (tag == FOURCC('K','P','2','S')) { if (!parse_float_track(&record, &emitter.speed)) goto fail; }
            else if (tag == FOURCC('K','P','2','L')) { if (!parse_float_track(&record, &emitter.latitude)) goto fail; }
            else if (tag == FOURCC('K','P','2','G')) { if (!parse_float_track(&record, &emitter.gravity)) goto fail; }
            else if (tag == FOURCC('K','P','2','R')) { if (!parse_float_track(&record, &emitter.variation)) goto fail; }
            else goto fail;
        }
        if (!append_record((void **)&model->nodes, &model->node_count, sizeof(node), &node)) goto fail;
        emitter.info.node_index = model->node_count - 1;
        if (!append_record((void **)&model->emitters, &model->emitter_count, sizeof(emitter), &emitter)) {
            /* The node was already appended and must stay - only the emitter record failed to
             * append (out of memory); the node otherwise remains a harmless, unreferenced
             * hierarchy entry, matching this decoder's existing all-or-nothing failure
             * contract at the top level (the whole decode fails and free_temp_model() reclaims
             * everything, including this node). */
            goto fail;
        }
        r->pos = outer_end;
        continue;
fail:
        free(node.translation.keys); free(node.rotation.keys); free(node.scale.keys);
        free(emitter.visibility.keys); free(emitter.emission_rate.keys); free(emitter.width.keys);
        free(emitter.length.keys); free(emitter.speed.keys); free(emitter.latitude.keys);
        free(emitter.gravity.keys); free(emitter.variation.keys);
        return false;
    }
    return true;
}

static void assign_pivots(mdxReader_t *r, mdxTempModel_t *model) {
    uint32_t count = (uint32_t)(r->size / sizeof(bzTTVec3_t));
    if (r->size % sizeof(bzTTVec3_t)) return;
    for (uint32_t i = 0; i < model->node_count; i++)
        if (model->nodes[i].info.object_id < count)
            memcpy(&model->nodes[i].info.pivot, r->data +
                   model->nodes[i].info.object_id * sizeof(bzTTVec3_t), sizeof(bzTTVec3_t));
}

static bool parse_global_sequences(mdxReader_t *r, mdxTempModel_t *model) {
    uint32_t count = (uint32_t)(r->size / 4);
    if (r->size % 4 || count > MDX_MAX_RECORDS || model->global_sequences) return false;
    if (!count) return true;
    model->global_sequences = malloc((size_t)count * 4);
    if (!model->global_sequences) return false;
    if (!read_bytes(r, model->global_sequences, (size_t)count * 4)) {
        free(model->global_sequences); model->global_sequences = NULL;
        return false;
    }
    model->global_sequence_count = count;
    return true;
}

/* GEOA record: staticAlpha(4) + flags(4, unused/renderer-only) + staticColor(12, geoset color
 * tint - out of scope, KGAC below covers its animated form) + geosetId(4), followed by optional
 * KGAO (alpha keys, in scope) and KGAC (color keys, explicitly out of scope for slice 5C). */
static bool parse_geoset_anim(mdxReader_t *r, mdxTempModel_t *model) {
    mdxTempGeosetAnim_t anim;
    uint32_t flags, tag;
    float color[3];
    memset(&anim, 0, sizeof(anim));
    if (!read_bytes(r, &anim.static_alpha, 4) || !read_u32(r, &flags) ||
        !read_bytes(r, color, sizeof(color)) || !read_u32(r, &anim.geoset_id))
        return false;
    (void)flags;
    while (r->pos + 4 <= r->size) {
        if (!read_u32(r, &tag)) goto fail;
        if (tag == FOURCC('K','G','A','O')) { if (!parse_float_track(r, &anim.alpha)) goto fail; }
        else if (tag == FOURCC('K','G','A','C')) {
            mdxTempTrack_t discard;
            if (!parse_vec3_track(r, &discard)) goto fail;
            free(discard.keys);
            fprintf(stderr, "wc3_mdx_decode: geoset color animation (KGAC) is unsupported; ignoring track\n");
        } else goto fail;
    }
    if (!append_record((void **)&model->geoset_anims, &model->geoset_anim_count, sizeof(anim), &anim))
        goto fail;
    return true;
fail:
    free(anim.alpha.keys);
    return false;
}

static bool parse_geoset_anims(mdxReader_t *r, mdxTempModel_t *model) {
    while (r->pos < r->size) {
        uint32_t record_size;
        size_t record_start = r->pos, record_end;
        mdxReader_t record;
        if (!read_u32(r, &record_size) || record_size < 28 || record_size > r->size - record_start)
            return false;
        record_end = record_start + record_size;
        record = (mdxReader_t){ r->data + r->pos, record_end - r->pos, 0 };
        if (!parse_geoset_anim(&record, model)) return false;
        r->pos = record_end;
    }
    return true;
}

/* Matches each top-level GEOA entry to its GEOS geoset by index (geoset_id ==
 * BZ_TTA_NO_GLOBAL_SEQUENCE-equivalent UINT32_MAX means "no target", which is valid and
 * inert). An out-of-range or duplicate target indicates a malformed file, so the whole
 * decode fails rather than guessing which geoset was intended, matching this decoder's
 * existing all-or-nothing validation for every other structural reference. */
static bool match_geoset_anims(mdxTempModel_t *model) {
    for (uint32_t i = 0; i < model->geoset_anim_count; i++) {
        mdxTempGeosetAnim_t *anim = &model->geoset_anims[i];
        mdxTempGeoset_t *geo;
        if (anim->geoset_id == UINT32_MAX) { free(anim->alpha.keys); anim->alpha.keys = NULL; continue; }
        if (anim->geoset_id >= model->geoset_count) return false;
        geo = &model->geosets[anim->geoset_id];
        if (geo->has_anim) return false;
        geo->has_anim = true; geo->static_alpha = anim->static_alpha; geo->alpha = anim->alpha;
        anim->alpha.keys = NULL; /* ownership transferred to the geoset */
    }
    return true;
}

/* Mirrors classic MDX R_AddGeosetMatrixPaletteEntry (renderer/mdx/r_mdx_load.c): each geoset
 * keeps its own deduplicated palette of referenced bones, capped at BZ_TTA_MAX_MATRIX_PALETTE;
 * once full, further distinct references collapse onto palette slot 0 (the desktop renderer's
 * own overflow clamp), logged once per affected geoset. */
static uint32_t palette_add(uint32_t *palette, uint32_t *count, uint32_t node_index, bool *overflow) {
    for (uint32_t i = 0; i < *count; i++) if (palette[i] == node_index) return i;
    if (*count >= BZ_TTA_MAX_MATRIX_PALETTE) { *overflow = true; return 0; }
    palette[(*count)++] = node_index;
    return *count - 1;
}

static bool node_index_for_object_id(const mdxTempModel_t *model, uint32_t object_id, uint32_t *out) {
    for (uint32_t i = 0; i < model->node_count; i++)
        if (model->nodes[i].info.object_id == object_id) { *out = i; return true; }
    return false;
}

/* Mirrors classic MDX R_SetupGeosetVertexBuffer (renderer/mdx/r_mdx_load.c) exactly: builds a
 * per-geoset matrix palette plus a top-4-weighted vertex skin, distributing equal weight
 * across each vertex's matrix group and renormalizing to 255. Geosets with no BONE hierarchy
 * at all still resolve a single-entry palette (bound through the same object_id lookup used
 * everywhere else), so unanimated models render through the same skinning path with an
 * identity bone matrix rather than a separate static-only code path. */
static bool resolve_geoset_skin(const mdxTempModel_t *model, mdxTempGeoset_t *geoset) {
    uint32_t palette[BZ_TTA_MAX_MATRIX_PALETTE];
    uint32_t palette_count = 0, offset = 0;
    bool overflow = false;
    bool has_groups = geoset->matrix_group_sizes && geoset->matrices && geoset->matrix_group_size_count;
    uint32_t matrix_group_count = has_groups ? geoset->matrix_group_size_count : 1;
    uint8_t (*groups)[BZ_TTA_MAX_VERTEX_SKIN_BONES];
    bzTTVertexSkin_t *skin;
    if (geoset->vertex_group_count && geoset->vertex_group_count != geoset->info.vertex_count) return false;
    groups = calloc(matrix_group_count, sizeof(*groups));
    skin = calloc(geoset->info.vertex_count, sizeof(*skin));
    if (!groups || !skin) { free(groups); free(skin); return false; }
    for (uint32_t g = 0; g < matrix_group_count; g++) {
        if (!has_groups) {
            uint32_t object_id = geoset->matrix_count ? geoset->matrices[0] : 0;
            uint32_t node_index;
            if (!node_index_for_object_id(model, object_id, &node_index)) { free(groups); free(skin); return false; }
            groups[g][0] = (uint8_t)palette_add(palette, &palette_count, node_index, &overflow);
            continue;
        }
        {
            uint32_t source_size = geoset->matrix_group_sizes[g];
            uint32_t group_size = source_size < BZ_TTA_MAX_VERTEX_SKIN_BONES ? source_size : BZ_TTA_MAX_VERTEX_SKIN_BONES;
            if (offset >= geoset->matrix_count) group_size = 0;
            else if (offset + group_size > geoset->matrix_count) group_size = geoset->matrix_count - offset;
            for (uint32_t m = 0; m < group_size; m++) {
                uint32_t node_index;
                if (!node_index_for_object_id(model, geoset->matrices[offset + m], &node_index)) {
                    free(groups); free(skin); return false;
                }
                groups[g][m] = (uint8_t)palette_add(palette, &palette_count, node_index, &overflow);
            }
            offset += source_size;
        }
    }
    if (!palette_count) { free(groups); free(skin); return false; }
    if (overflow)
        fprintf(stderr,
                "wc3_mdx_decode: geoset uses more than %u unique bones; extra references collapsed to palette slot 0\n",
                (unsigned)BZ_TTA_MAX_MATRIX_PALETTE);
    for (uint32_t v = 0; v < geoset->info.vertex_count; v++) {
        uint32_t group_index = 0, group_size = 1;
        uint8_t leftover = 0xff, leftover_size = 1;
        uint8_t raw_index[BZ_TTA_MAX_VERTEX_SKIN_BONES] = {0}, raw_weight[BZ_TTA_MAX_VERTEX_SKIN_BONES] = {0};
        const uint8_t *group;
        uint32_t weight_sum;
        if (has_groups) {
            group_index = geoset->vertex_groups[v];
            if (group_index >= matrix_group_count) group_index = matrix_group_count - 1;
            group_size = geoset->matrix_group_sizes[group_index];
            if (group_size < 1) group_size = 1;
            if (group_size > BZ_TTA_MAX_VERTEX_SKIN_BONES) group_size = BZ_TTA_MAX_VERTEX_SKIN_BONES;
            leftover_size = (uint8_t)group_size;
        }
        group = groups[group_index];
        memcpy(raw_index, group, BZ_TTA_MAX_VERTEX_SKIN_BONES);
        if (!has_groups) {
            raw_weight[0] = 255;
        } else {
            for (uint32_t m = 0; m < group_size; m++) {
                uint8_t value = (uint8_t)((float)leftover / (float)leftover_size);
                raw_weight[m] = value;
                leftover = leftover > value ? (uint8_t)(leftover - value) : 0;
                leftover_size = leftover_size > 1 ? (uint8_t)(leftover_size - 1) : 1;
            }
        }
        for (uint32_t i = 0; i < BZ_TTA_MAX_VERTEX_SKIN_BONES; i++) {
            if (!raw_weight[i]) continue;
            for (uint32_t j = 0; j < BZ_TTA_MAX_VERTEX_SKIN_BONES; j++) {
                if (raw_weight[i] > skin[v].bone_weight[j]) {
                    for (int k = BZ_TTA_MAX_VERTEX_SKIN_BONES - 1; k > (int)j; k--) {
                        skin[v].bone_index[k] = skin[v].bone_index[k - 1];
                        skin[v].bone_weight[k] = skin[v].bone_weight[k - 1];
                    }
                    skin[v].bone_index[j] = raw_index[i];
                    skin[v].bone_weight[j] = raw_weight[i];
                    break;
                }
            }
        }
        weight_sum = 0;
        for (uint32_t j = 0; j < BZ_TTA_MAX_VERTEX_SKIN_BONES; j++) weight_sum += skin[v].bone_weight[j];
        if (weight_sum && weight_sum != 255) {
            uint32_t accumulated = 0;
            for (uint32_t j = 0; j < BZ_TTA_MAX_VERTEX_SKIN_BONES; j++) {
                uint32_t w = skin[v].bone_weight[j] * 255u / weight_sum;
                skin[v].bone_weight[j] = (uint8_t)w;
                accumulated += w;
            }
            if (accumulated < 255) skin[v].bone_weight[0] = (uint8_t)(skin[v].bone_weight[0] + (255 - accumulated));
        }
    }
    free(groups);
    geoset->skin = skin;
    geoset->palette = malloc(sizeof(uint32_t) * palette_count);
    if (!geoset->palette) { free(skin); geoset->skin = NULL; return false; }
    memcpy(geoset->palette, palette, sizeof(uint32_t) * palette_count);
    geoset->palette_count = palette_count;
    return true;
}

static bool resolve_geoset_skins(mdxTempModel_t *model) {
    for (uint32_t i = 0; i < model->geoset_count; i++)
        if (!resolve_geoset_skin(model, &model->geosets[i])) return false;
    return true;
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
            case FOURCC('G','L','B','S'): if (!parse_global_sequences(&chunk, model)) return false; break;
            case FOURCC('T','E','X','S'): if (!parse_textures(&chunk, model)) return false; break;
            case FOURCC('M','T','L','S'): if (!parse_materials(&chunk, model)) return false; break;
            case FOURCC('G','E','O','S'): if (!parse_geosets(&chunk, model)) return false; break;
            case FOURCC('G','E','O','A'): if (!parse_geoset_anims(&chunk, model)) return false; break;
            case FOURCC('B','O','N','E'): if (!parse_nodes(&chunk, model, true)) return false; break;
            case FOURCC('H','E','L','P'): if (!parse_nodes(&chunk, model, false)) return false; break;
            case FOURCC('P','R','E','2'): if (!parse_particle_emitters(&chunk, model)) return false; break;
            case FOURCC('P','I','V','T'): assign_pivots(&chunk, model); break;
            default: break; /* Known-but-unexported chunks remain bounded by the top-level chunk size. */
        }
    }
    if (model->version != 800 || !model->geoset_count || !model->node_count) return false;
    return match_geoset_anims(model) && resolve_geoset_skins(model);
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
    uint32_t geosets_offset, materials_offset, layers_offset, textures_offset, sequences_offset;
    uint32_t nodes_offset, global_sequences_offset, emitters_offset;
    bzTTAsset_t *asset;
    bzTTGeosetRecord_t *records;
    bzTTNodeRecord_t *node_records;
    bzTTEmitterRecord_t *emitter_records;
    if (!payload_add(&payload, src->geoset_count, sizeof(bzTTGeosetRecord_t),
                     _Alignof(bzTTGeosetRecord_t), &geosets_offset) ||
        !payload_add(&payload, src->material_count, sizeof(*src->materials),
                     _Alignof(bzTTMaterialInfo_t), &materials_offset) ||
        !payload_add(&payload, src->layer_count, sizeof(*src->layers),
                     _Alignof(bzTTMaterialLayerInfo_t), &layers_offset) ||
        !payload_add(&payload, src->texture_count, sizeof(*src->textures),
                     _Alignof(bzTTModelTextureInfo_t), &textures_offset) ||
        !payload_add(&payload, src->sequence_count, sizeof(*src->sequences),
                     _Alignof(bzTTSequenceInfo_t), &sequences_offset) ||
        !payload_add(&payload, src->node_count, sizeof(bzTTNodeRecord_t),
                     _Alignof(bzTTNodeRecord_t), &nodes_offset) ||
        !payload_add(&payload, src->global_sequence_count, sizeof(uint32_t),
                     _Alignof(uint32_t), &global_sequences_offset) ||
        !payload_add(&payload, src->emitter_count, sizeof(bzTTEmitterRecord_t),
                     _Alignof(bzTTEmitterRecord_t), &emitters_offset)) {
        *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL;
    }
    for (uint32_t i = 0; i < src->geoset_count; i++) {
        const mdxTempGeoset_t *geo = src->geosets + i;
        uint32_t ignored;
        uint32_t alpha_keys = geo->has_anim && geo->alpha.key_count ? geo->alpha.key_count : 0;
        if (!payload_add(&payload, geo->info.vertex_count, sizeof(bzTTVec3_t), _Alignof(bzTTVec3_t), &ignored) ||
            !payload_add(&payload, geo->info.normal_count, sizeof(bzTTVec3_t), _Alignof(bzTTVec3_t), &ignored) ||
            !payload_add(&payload, geo->info.uv_count, sizeof(bzTTVec2_t), _Alignof(bzTTVec2_t), &ignored) ||
            !payload_add(&payload, geo->info.index_count, sizeof(uint16_t), _Alignof(uint16_t), &ignored) ||
            !payload_add(&payload, geo->info.vertex_count, sizeof(bzTTVertexSkin_t),
                         _Alignof(bzTTVertexSkin_t), &ignored) ||
            !payload_add(&payload, geo->palette_count, sizeof(uint32_t), _Alignof(uint32_t), &ignored) ||
            !payload_add(&payload, alpha_keys, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored)) {
            *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL;
        }
    }
    for (uint32_t i = 0; i < src->node_count; i++) {
        const mdxTempNode_t *node = src->nodes + i;
        uint32_t ignored;
        if (!payload_add(&payload, node->translation.key_count, sizeof(bzTTVec3Key_t),
                         _Alignof(bzTTVec3Key_t), &ignored) ||
            !payload_add(&payload, node->rotation.key_count, sizeof(bzTTQuatKey_t),
                         _Alignof(bzTTQuatKey_t), &ignored) ||
            !payload_add(&payload, node->scale.key_count, sizeof(bzTTVec3Key_t),
                         _Alignof(bzTTVec3Key_t), &ignored)) {
            *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL;
        }
    }
    for (uint32_t i = 0; i < src->emitter_count; i++) {
        const mdxTempEmitter_t *e = src->emitters + i;
        uint32_t ignored;
        if (!payload_add(&payload, e->visibility.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored) ||
            !payload_add(&payload, e->emission_rate.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored) ||
            !payload_add(&payload, e->width.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored) ||
            !payload_add(&payload, e->length.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored) ||
            !payload_add(&payload, e->speed.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored) ||
            !payload_add(&payload, e->latitude.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored) ||
            !payload_add(&payload, e->gravity.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored) ||
            !payload_add(&payload, e->variation.key_count, sizeof(bzTTFloatKey_t), _Alignof(bzTTFloatKey_t), &ignored)) {
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
        .global_sequence_count = src->global_sequence_count,
        .emitter_count = src->emitter_count,
    };
    asset->u.model.geosets_offset = geosets_offset; asset->u.model.materials_offset = materials_offset;
    asset->u.model.layers_offset = layers_offset; asset->u.model.textures_offset = textures_offset;
    asset->u.model.sequences_offset = sequences_offset; asset->u.model.nodes_offset = nodes_offset;
    asset->u.model.global_sequences_offset = global_sequences_offset;
    asset->u.model.emitters_offset = emitters_offset;
    records = BZ_TTA_AssetData(asset, geosets_offset, (size_t)src->geoset_count * sizeof(*records));
    node_records = BZ_TTA_AssetData(asset, nodes_offset, (size_t)src->node_count * sizeof(*node_records));
    emitter_records = BZ_TTA_AssetData(asset, emitters_offset, (size_t)src->emitter_count * sizeof(*emitter_records));
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
    if (src->global_sequence_count)
        memcpy(BZ_TTA_AssetData(asset, global_sequences_offset,
              (size_t)src->global_sequence_count * sizeof(uint32_t)),
              src->global_sequences, (size_t)src->global_sequence_count * sizeof(uint32_t));
    /* Resume the running cursor at the end of the fixed-size top-level arrays (the LAST of
     * which is now the emitters struct array, appended for ABI v4 - see this function's
     * payload_add() sizing pass above) before the variable-length per-geoset/per-node/
     * per-emitter arrays below. Forgetting to advance past a newly-appended fixed-size array
     * here would let the very next variable-length write silently overlap it. */
    payload = emitters_offset + (size_t)src->emitter_count * sizeof(bzTTEmitterRecord_t);
    for (uint32_t i = 0; i < src->geoset_count; i++) {
        const mdxTempGeoset_t *geo = src->geosets + i;
        bool has_track = geo->has_anim && geo->alpha.key_count > 0;
        uint32_t alpha_keys = has_track ? geo->alpha.key_count : 0;
        records[i].info = geo->info;
        records[i].info.matrix_palette_count = geo->palette_count;
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
        payload = mdx_align_size(payload, _Alignof(bzTTVertexSkin_t));
        records[i].skin_offset = (uint32_t)payload;
        if (geo->info.vertex_count)
            memcpy(BZ_TTA_AssetData(asset, records[i].skin_offset,
                  (size_t)geo->info.vertex_count * sizeof(bzTTVertexSkin_t)),
                  geo->skin, (size_t)geo->info.vertex_count * sizeof(bzTTVertexSkin_t));
        payload += (size_t)geo->info.vertex_count * sizeof(bzTTVertexSkin_t);
        payload = mdx_align_size(payload, _Alignof(uint32_t));
        records[i].palette_offset = (uint32_t)payload;
        if (geo->palette_count)
            memcpy(BZ_TTA_AssetData(asset, records[i].palette_offset,
                  (size_t)geo->palette_count * sizeof(uint32_t)),
                  geo->palette, (size_t)geo->palette_count * sizeof(uint32_t));
        payload += (size_t)geo->palette_count * sizeof(uint32_t);
        records[i].anim.has_alpha_track = has_track;
        records[i].anim.static_alpha = geo->has_anim ? geo->static_alpha : 1.0f;
        records[i].anim.alpha_track.key_count = alpha_keys;
        records[i].anim.alpha_track.interp = has_track ? geo->alpha.interp : BZ_TTA_INTERP_NONE;
        records[i].anim.alpha_track.global_sequence =
            has_track ? geo->alpha.global_sequence : BZ_TTA_NO_GLOBAL_SEQUENCE;
        payload = mdx_align_size(payload, _Alignof(bzTTFloatKey_t));
        records[i].alpha_keys_offset = (uint32_t)payload;
        if (alpha_keys)
            memcpy(BZ_TTA_AssetData(asset, records[i].alpha_keys_offset,
                  (size_t)alpha_keys * sizeof(bzTTFloatKey_t)),
                  geo->alpha.keys, (size_t)alpha_keys * sizeof(bzTTFloatKey_t));
        payload += (size_t)alpha_keys * sizeof(bzTTFloatKey_t);
    }
    for (uint32_t i = 0; i < src->node_count; i++) {
        const mdxTempNode_t *node = src->nodes + i;
        node_records[i].info = node->info;
#define COPY_TRACK(FIELD, TRACK, TYPE) \
        payload = mdx_align_size(payload, _Alignof(TYPE)); \
        node_records[i].FIELD.key_count = node->TRACK.key_count; \
        node_records[i].FIELD.interp = node->TRACK.interp; \
        node_records[i].FIELD.global_sequence = node->TRACK.global_sequence; \
        node_records[i].FIELD.keys_offset = (uint32_t)payload; \
        if (node->TRACK.key_count) memcpy(BZ_TTA_AssetData(asset, node_records[i].FIELD.keys_offset, \
            (size_t)node->TRACK.key_count * sizeof(TYPE)), node->TRACK.keys, \
            (size_t)node->TRACK.key_count * sizeof(TYPE)); \
        payload += (size_t)node->TRACK.key_count * sizeof(TYPE)
        COPY_TRACK(translation, translation, bzTTVec3Key_t);
        COPY_TRACK(rotation, rotation, bzTTQuatKey_t);
        COPY_TRACK(scale, scale, bzTTVec3Key_t);
#undef COPY_TRACK
    }
    for (uint32_t i = 0; i < src->emitter_count; i++) {
        const mdxTempEmitter_t *e = src->emitters + i;
        emitter_records[i].info = e->info;
#define COPY_EMITTER_TRACK(FIELD, TRACK) \
        payload = mdx_align_size(payload, _Alignof(bzTTFloatKey_t)); \
        emitter_records[i].FIELD.key_count = e->TRACK.key_count; \
        emitter_records[i].FIELD.interp = e->TRACK.interp; \
        emitter_records[i].FIELD.global_sequence = e->TRACK.global_sequence; \
        emitter_records[i].FIELD.keys_offset = (uint32_t)payload; \
        if (e->TRACK.key_count) memcpy(BZ_TTA_AssetData(asset, emitter_records[i].FIELD.keys_offset, \
            (size_t)e->TRACK.key_count * sizeof(bzTTFloatKey_t)), e->TRACK.keys, \
            (size_t)e->TRACK.key_count * sizeof(bzTTFloatKey_t)); \
        payload += (size_t)e->TRACK.key_count * sizeof(bzTTFloatKey_t)
        COPY_EMITTER_TRACK(visibility, visibility);
        COPY_EMITTER_TRACK(emission_rate, emission_rate);
        COPY_EMITTER_TRACK(width, width);
        COPY_EMITTER_TRACK(length, length);
        COPY_EMITTER_TRACK(speed, speed);
        COPY_EMITTER_TRACK(latitude, latitude);
        COPY_EMITTER_TRACK(gravity, gravity);
        COPY_EMITTER_TRACK(variation, variation);
#undef COPY_EMITTER_TRACK
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

#include "sc2_m3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    struct ReferenceEntry ent;
    DWORD readcount, length;
    BOOL valid;
    BOOL *model_valid;
    void *data;
} m3Reader_t;

typedef DWORD (*m3StrideFunc_t)(DWORD version);
typedef struct {
    char id[4];
    DWORD min_version, max_version, stride;
    m3StrideFunc_t version_stride;
} m3Schema_t;

#define M3_READ(BUFFER, VAR, VERSION) \
if ((BUFFER)->ent.version > VERSION || VERSION == 0) m3_read(BUFFER, &VAR, sizeof(VAR));

#define M3_READER(TYPE) \
static void m3_read_##TYPE(m3Model_t *model, m3Reader_t *sb, m3##TYPE##_t *data)

#define M3_READ_REFERENCE(MODEL, TARGET, REF, TYPE) do { \
    m3Reader_t reader = { 0 }; \
    DWORD stride = 0; \
    TARGET##Num = 0; TARGET = NULL; \
    if (!(REF).nEntries) break; \
    if (!m3_schema_reader(MODEL, REF, &m3_schema_##TYPE, &reader, &stride) || (REF).nEntries == UINT32_MAX) { \
        (MODEL)->valid = false; break; \
    } \
    TARGET##Num = (REF).nEntries; \
    TARGET = TARGET##Num ? calloc(TARGET##Num + 1, sizeof(m3##TYPE##_t)) : NULL; \
    if (TARGET##Num && !TARGET) { TARGET##Num = 0; (MODEL)->valid = false; } \
    FOR_LOOP(n, TARGET##Num) m3_read_##TYPE(MODEL, &reader, &TARGET[n]); \
    if (reader.readcount != TARGET##Num * stride) (MODEL)->valid = false; \
} while (0)

#define M3_REFR(BUFFER, TARGET, TYPE, VERSION) do { \
    if ((BUFFER)->ent.version > VERSION || VERSION == 0) { \
        Reference ref; \
        m3_read(BUFFER, &ref, sizeof(ref)); \
        M3_READ_REFERENCE(model, TARGET, ref, TYPE); \
    } \
} while (0)

static void m3_read(m3Reader_t *reader, void *dst, DWORD bytes) {
    if (!dst || !bytes) return;
    if (!reader || !reader->valid || !reader->data || reader->readcount > reader->length ||
        bytes > reader->length - reader->readcount) {
        memset(dst, 0, bytes);
        if (reader) { reader->valid = false; if (reader->model_valid) *reader->model_valid = false; }
        return;
    }
    memcpy(dst, (LPBYTE)reader->data + reader->readcount, bytes);
    reader->readcount += bytes;
}

static DWORD m3_stride_layer(DWORD version) { return version > 24 ? 468 : version > 23 ? 436 : 356; }
static DWORD m3_stride_material(DWORD version) { return version > 18 ? 340 : version > 15 ? 280 : 268; }
static DWORD m3_stride_region(DWORD version) {
    switch (version) { case 2: return 28; case 3: return 36; case 4: return 40; case 5: return 48; }
    return 0;
}
static DWORD m3_stride_sequence(DWORD version) { return version < 2 ? 96 : 92; }
static DWORD m3_stride_model(DWORD version) {
    switch (version) {
    case 20: return 748; case 21: return 760; case 23: return 784; case 25: return 808;
    case 26: return 820; case 28: return 844; case 29: return 856;
    }
    return 0;
}

#define M3_SCHEMA_FIXED(TYPE, ID, MIN_VERSION, MAX_VERSION, STRIDE) \
static const m3Schema_t m3_schema_##TYPE = { ID, MIN_VERSION, MAX_VERSION, STRIDE, NULL }
#define M3_SCHEMA_VERSIONED(TYPE, ID, MIN_VERSION, MAX_VERSION, FUNC) \
static const m3Schema_t m3_schema_##TYPE = { ID, MIN_VERSION, MAX_VERSION, 0, FUNC }
M3_SCHEMA_FIXED(Char, "RAHC", 0, 0, 1);
M3_SCHEMA_FIXED(Uint16, "_61U", 0, 0, 2);
M3_SCHEMA_FIXED(Uint32, "_23U", 0, 0, 4);
M3_SCHEMA_FIXED(Matrix4, "FERI", 0, 0, 64);
M3_SCHEMA_FIXED(Face, "_61U", 0, 0, 2);
M3_SCHEMA_FIXED(MaterialReference, "MTAM", 0, 0, 8);
M3_SCHEMA_FIXED(CompositeMaterialSection, "_SMC", 0, 0, 24);
M3_SCHEMA_FIXED(CompositeMaterial, "_PMC", 0, 2, 28);
M3_SCHEMA_VERSIONED(Layer, "RYAL", 0, 25, m3_stride_layer);
M3_SCHEMA_VERSIONED(Material, "_TAM", 0, 19, m3_stride_material);
M3_SCHEMA_VERSIONED(Region, "NGER", 2, 5, m3_stride_region);
M3_SCHEMA_FIXED(Batch, "_TAB", 1, 1, 14);
M3_SCHEMA_FIXED(Divisions, "_VID", 2, 2, 52);
M3_SCHEMA_VERSIONED(Sequence, "SQES", 0, 2, m3_stride_sequence);
M3_SCHEMA_FIXED(SequenceTimeline, "_CTS", 0, 4, 204);
M3_SCHEMA_FIXED(SequenceValidator, "_STS", 0, 0, 28);
M3_SCHEMA_FIXED(SequenceGetter, "_GTS", 0, 0, 24);
M3_SCHEMA_FIXED(Bone, "ENOB", 0, 1, 160);
M3_SCHEMA_VERSIONED(Model, "LDOM", 20, 29, m3_stride_model);
#undef M3_SCHEMA_FIXED
#undef M3_SCHEMA_VERSIONED

static DWORD m3_schema_stride(m3Schema_t const *schema, DWORD version) {
    return schema->version_stride ? schema->version_stride(version) : schema->stride;
}

static m3Reader_t m3_make_reader(m3Model_t const *model, Reference ref) {
    DWORD section_end;
    if (!model || !model->buffer || !model->refs || !model->head || !ref.nEntries)
        return (m3Reader_t){ 0 };
    if (ref.ref >= model->head->nRefs || model->refs[ref.ref].offset >= model->size ||
        model->refs[ref.ref].offset >= model->head->ofsRefs)
        return (m3Reader_t){ 0 };
    section_end = model->head->ofsRefs;
    if (section_end <= model->refs[ref.ref].offset || section_end > model->size) section_end = model->size;
    FOR_LOOP(i, model->head->nRefs)
        if (model->refs[i].offset > model->refs[ref.ref].offset &&
            model->refs[i].offset < section_end)
            section_end = model->refs[i].offset;
    return (m3Reader_t){
        .ent = model->refs[ref.ref],
        .length = section_end - model->refs[ref.ref].offset,
        .valid = true,
        .model_valid = &((m3Model_t *)model)->valid,
        .data = (LPBYTE)model->buffer + model->refs[ref.ref].offset,
    };
}

static BOOL m3_schema_reader(m3Model_t const *model, Reference ref, m3Schema_t const *schema,
                             m3Reader_t *reader, LPDWORD stride) {
    if (!model || !schema || !reader || !stride || !ref.nEntries) return false;
    *reader = m3_make_reader(model, ref);
    if (!reader->valid || memcmp(reader->ent.id, schema->id, 4) ||
        reader->ent.version < schema->min_version || reader->ent.version > schema->max_version ||
        reader->ent.nEntries != ref.nEntries)
        return false;
    *stride = m3_schema_stride(schema, reader->ent.version);
    return *stride && ref.nEntries <= reader->length / *stride;
}

static BOOL m3_reference_reader(m3Model_t const *model, m3ReferenceRead_t const *read,
                                m3Reader_t *reader) {
    static const char no_id[4] = { 0 };
    if (!model || !model->head || !model->valid || !read || !reader || !read->element_size)
        return false;
    if (!read->reference.nEntries) {
        *reader = (m3Reader_t){ .valid = true };
        return true;
    }
    *reader = m3_make_reader(model, read->reference);
    if (!reader->valid || reader->ent.nEntries != read->reference.nEntries ||
        (memcmp(read->section_id, no_id, 4) && memcmp(reader->ent.id, read->section_id, 4)) ||
        read->reference.nEntries > reader->length / read->element_size)
        return false;
    return true;
}

BOOL SC2_M3ReferenceCount(m3Model_t const *model, m3ReferenceRead_t const *read, LPDWORD count) {
    m3Reader_t reader;
    if (!count || !m3_reference_reader(model, read, &reader)) return false;
    *count = read->reference.nEntries;
    return true;
}

BOOL SC2_M3ReferenceElement(m3Model_t const *model, m3ReferenceRead_t const *read, HANDLE out) {
    m3Reader_t reader;
    if (!out || !m3_reference_reader(model, read, &reader) ||
        read->element_index >= read->reference.nEntries)
        return false;
    memcpy(out, (LPBYTE)reader.data + read->element_index * read->element_size, read->element_size);
    return true;
}

DWORD SC2_M3VertexUVCount(DWORD flags) {
    DWORD count;
    if (flags & 0x100000) return 4;
    if (flags & 0x80000) count = 3;
    else if (flags & 0x40000) count = 2;
    else count = 1;
    if (flags & 0x40000000) count++;
    return MIN(count, 4);
}

DWORD SC2_M3VertexDiskSize(DWORD flags) {
    return 28 + SC2_M3VertexUVCount(flags) * sizeof(SHORT) * 2 + ((flags & 0x200) ? sizeof(COLOR32) : 0);
}

M3_READER(Int16) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Uint16) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Int32) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Uint32) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Float32) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Vector2) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Vector3) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Vector4) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Matrix4) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Face) { m3_read(sb, data, sizeof(*data)); }
M3_READER(Pixel) { m3_read(sb, data, sizeof(*data)); }

M3_READER(Char) {
    m3_read(sb, data, sizeof(*data));
    if (*data == '/') *data = '\\';
}

M3_READER(Vertex) {
    DWORD uv_count = SC2_M3VertexUVCount(model->vertexFlags);
    M3_READ(sb, data->pos, 0);
    M3_READ(sb, data->boneWeight, 0);
    M3_READ(sb, data->boneIndex, 0);
    M3_READ(sb, data->normal, 0);
    data->color = COLOR32_WHITE;
    if (model->vertexFlags & 0x200) M3_READ(sb, data->color, 0);
    FOR_LOOP(i, uv_count) M3_READ(sb, data->uv[i], 0);
    M3_READ(sb, data->tangent, 0);
}

static void m3_read_vertex_reference(m3Model_t *model, m3Reader_t *sb) {
    Reference ref;
    m3Reader_t reader;
    DWORD stride, count;
    m3_read(sb, &ref, sizeof(ref));
    stride = SC2_M3VertexDiskSize(model->vertexFlags);
    reader = m3_make_reader(model, ref);
    if (!ref.nEntries) return;
    if (!reader.valid || memcmp(reader.ent.id, "__8U", 4) || reader.ent.version ||
        reader.ent.nEntries != ref.nEntries || !stride || ref.nEntries % stride ||
        ref.nEntries > reader.length) {
        model->valid = false;
        return;
    }
    count = ref.nEntries / stride;
    model->vertices = count ? calloc(count + 1, sizeof(*model->vertices)) : NULL;
    model->verticesNum = model->vertices ? count : 0;
    if (count && !model->vertices) model->valid = false;
    FOR_LOOP(n, model->verticesNum) m3_read_Vertex(model, &reader, &model->vertices[n]);
}

M3_READER(MaterialReference) {
    M3_READ(sb, data->materialType, 0);
    M3_READ(sb, data->materialIndex, 0);
}

M3_READER(CompositeMaterialSection) {
    M3_READ(sb, data->materialReferenceIndex, 0);
    M3_READ(sb, data->alphaFactor, 0);
}

M3_READER(CompositeMaterial) {
    M3_REFR(sb, data->name, Char, 0);
    M3_READ(sb, data->unknown, 0);
    M3_REFR(sb, data->sections, CompositeMaterialSection, 0);
}

M3_READER(Layer) {
    M3_READ(sb, data->unknown0, 0);
    M3_REFR(sb, data->imagePath, Char, 0);
    M3_READ(sb, data->color, 0);
    M3_READ(sb, data->flags, 0);
    M3_READ(sb, data->uvSource1, 0);
    M3_READ(sb, data->colorChannelSetting, 0);
    M3_READ(sb, data->brightMult, 0);
    M3_READ(sb, data->midtoneOffset, 0);
    M3_READ(sb, data->unknown1, 0);
    M3_READ(sb, data->noise, 23);
    M3_READ(sb, data->rttChannel, 0);
    M3_READ(sb, data->video, 0);
    M3_READ(sb, data->flipBook, 0);
    M3_READ(sb, data->uv, 0);
    M3_READ(sb, data->brightness, 0);
    M3_READ(sb, data->triPlanarOffset, 23);
    M3_READ(sb, data->triPlanarScale, 23);
    M3_READ(sb, data->unknown4, 0);
    M3_READ(sb, data->fresnel, 0);
    M3_READ(sb, data->fresnel2, 24);
}

M3_READER(Material) {
    M3_REFR(sb, data->name, Char, 0);
    M3_READ(sb, data->additionalFlags, 0);
    M3_READ(sb, data->flags, 0);
    M3_READ(sb, data->blendMode, 0);
    M3_READ(sb, data->priority, 0);
    M3_READ(sb, data->usedRTTChannels, 0);
    M3_READ(sb, data->specularity, 0);
    M3_READ(sb, data->depthBlendFalloff, 0);
    M3_READ(sb, data->cutoutThreshold, 0);
    M3_READ(sb, data->specMult, 0);
    M3_READ(sb, data->emisMult, 0);
    M3_REFR(sb, data->diffuseLayer, Layer, 0);
    M3_REFR(sb, data->decalLayer, Layer, 0);
    M3_REFR(sb, data->specularLayer, Layer, 0);
    M3_REFR(sb, data->glossLayer, Layer, 15);
    M3_REFR(sb, data->emissiveLayer, Layer, 0);
    M3_REFR(sb, data->emissive2Layer, Layer, 0);
    M3_REFR(sb, data->evioLayer, Layer, 0);
    M3_REFR(sb, data->evioMaskLayer, Layer, 0);
    M3_REFR(sb, data->alphaMaskLayer, Layer, 0);
    M3_REFR(sb, data->alphaMask2Layer, Layer, 0);
    M3_REFR(sb, data->normalLayer, Layer, 0);
    M3_REFR(sb, data->heightLayer, Layer, 0);
    M3_REFR(sb, data->lightMapLayer, Layer, 0);
    M3_REFR(sb, data->ambientOcclusionLayer, Layer, 0);
    M3_READ(sb, data->unknown4, 18);
    M3_READ(sb, data->unknown8, 0);
    M3_READ(sb, data->layerBlendType, 0);
    M3_READ(sb, data->emisBlendType, 0);
    M3_READ(sb, data->emisMode, 0);
    M3_READ(sb, data->specType, 0);
    M3_READ(sb, data->unknown9, 0);
    M3_READ(sb, data->unknown10, 0);
    M3_READ(sb, data->unknown11, 18);
}

M3_READER(Region) {
    M3_READ(sb, data->unknown0, 0);
    if (sb->ent.version >= 3) {
        M3_READ(sb, data->unknown1, 0);
        M3_READ(sb, data->firstVertexIndex, 0);
        M3_READ(sb, data->verticesCount, 0);
    } else {
        USHORT first_vertex, vertex_count;
        M3_READ(sb, first_vertex, 0); M3_READ(sb, vertex_count, 0);
        data->firstVertexIndex = first_vertex; data->verticesCount = vertex_count;
    }
    M3_READ(sb, data->firstTriangleIndex, 0);
    M3_READ(sb, data->triangleIndicesCount, 0);
    M3_READ(sb, data->bonesCount, 0);
    M3_READ(sb, data->firstBoneLookupIndex, 0);
    M3_READ(sb, data->boneLookupIndicesCount, 0);
    M3_READ(sb, data->unknown2, 0);
    M3_READ(sb, data->boneWeightPairsCount, 0);
    M3_READ(sb, data->unknown3, 0);
    M3_READ(sb, data->rootBoneIndex, 0);
    M3_READ(sb, data->unknown4, 3);
    M3_READ(sb, data->unknown5, 4);
}

M3_READER(Batch) {
    M3_READ(sb, data->unknown0, 0);
    M3_READ(sb, data->regionIndex, 0);
    M3_READ(sb, data->unknown1, 0);
    M3_READ(sb, data->materialReferenceIndex, 0);
    M3_READ(sb, data->unknown2, 0);
}

M3_READER(Divisions) {
    M3_REFR(sb, data->faces, Face, 0);
    M3_REFR(sb, data->regions, Region, 0);
    M3_REFR(sb, data->batches, Batch, 0);
    M3_READ(sb, data->MSEC, 0);
    M3_READ(sb, data->unknown, 0);
}

M3_READER(Sequence) {
    M3_READ(sb, data->unknown, 0);
    M3_REFR(sb, data->name, Char, 0);
    M3_READ(sb, data->interval, 0);
    M3_READ(sb, data->movementSpeed, 0);
    M3_READ(sb, data->flags, 0);
    M3_READ(sb, data->frequency, 0);
    M3_READ(sb, data->unk, 0);
    if (sb->ent.version < 2) M3_READ(sb, data->unk2, 0);
    M3_READ(sb, data->boundingSphere, 0);
    M3_READ(sb, data->d5, 0);
}

M3_READER(SequenceTimeline) {
    M3_REFR(sb, data->name, Char, 0);
    M3_READ(sb, data->runsConcurrent, 0);
    M3_READ(sb, data->priority, 0);
    M3_READ(sb, data->stsIndex, 0);
    M3_READ(sb, data->stsIndexCopy, 0);
    M3_REFR(sb, data->animIds, Uint32, 0);
    M3_REFR(sb, data->animRefs, Uint32, 0);
    M3_READ(sb, data->d3, 0);
    FOR_LOOP(i, 13) M3_READ(sb, data->sd[i], 0);
}

M3_READER(SequenceValidator) {
    M3_REFR(sb, data->animIds, Uint32, 0);
    M3_READ(sb, data->unk, 0);
}

M3_READER(SequenceGetter) {
    M3_REFR(sb, data->name, Char, 0);
    M3_REFR(sb, data->stcID, Uint32, 0);
}

M3_READER(Bone) {
    M3_READ(sb, data->unknown0, 0);
    M3_REFR(sb, data->name, Char, 0);
    M3_READ(sb, data->flags, 0);
    M3_READ(sb, data->parent, 0);
    M3_READ(sb, data->unknown1, 0);
    M3_READ(sb, data->position, 0);
    M3_READ(sb, data->rotation, 0);
    M3_READ(sb, data->scale, 0);
    M3_READ(sb, data->visibility, 0);
}

static void m3_init_model(m3Model_t *model, m3Reader_t *sb) {
    M3_REFR(sb, model->modelName, Char, 0);
    M3_READ(sb, model->flags, 0);
    M3_REFR(sb, model->sequences, Sequence, 0);
    M3_REFR(sb, model->stc, SequenceTimeline, 0);
    M3_REFR(sb, model->stg, SequenceGetter, 0);
    M3_READ(sb, model->unknown0, 0);
    M3_REFR(sb, model->sts, SequenceValidator, 0);
    M3_REFR(sb, model->bones, Bone, 0);
    M3_READ(sb, model->numberOfBonesToCheckForSkin, 0);
    M3_READ(sb, model->vertexFlags, 0);
    m3_read_vertex_reference(model, sb);
    M3_REFR(sb, model->divisions, Divisions, 0);
    M3_REFR(sb, model->boneLookup, Uint16, 0);
    M3_READ(sb, model->boundings, 0);
    M3_READ(sb, model->unknown4, 0);
    M3_READ(sb, model->attachmentPoints, 0);
    M3_READ(sb, model->attachmentPointAddons, 0);
    M3_READ(sb, model->ligts, 0);
    M3_READ(sb, model->shbxData, 20);
    M3_READ(sb, model->cameras, 0);
    M3_READ(sb, model->unknown21, 0);
    M3_REFR(sb, model->materialReferences, MaterialReference, 0);
    M3_REFR(sb, model->materialStandard, Material, 0);
    M3_READ(sb, model->materialDisplacement, 0);
    M3_REFR(sb, model->materialComposite, CompositeMaterial, 0);
    M3_READ(sb, model->materialTerrain, 0);
    M3_READ(sb, model->materialVolume, 0);
    M3_READ(sb, model->materialUnknown1, 0);
    M3_READ(sb, model->materialCreep, 0);
    M3_READ(sb, model->materialVolumeNoise, 24);
    M3_READ(sb, model->materialSplatTerrainBake, 25);
    M3_READ(sb, model->materialUnknown2, 27);
    M3_READ(sb, model->materialLensFlare, 28);
    M3_READ(sb, model->particleEmitters, 0);
    M3_READ(sb, model->particleEmitterCopies, 0);
    M3_READ(sb, model->ribbonEmitters, 0);
    M3_READ(sb, model->projections, 0);
    M3_READ(sb, model->forces, 0);
    M3_READ(sb, model->warps, 0);
    M3_READ(sb, model->unknown22, 0);
    M3_READ(sb, model->rigidBodies, 0);
    M3_READ(sb, model->unknown23, 0);
    M3_READ(sb, model->physicsJoints, 0);
    M3_READ(sb, model->clothBehavior, 27);
    M3_READ(sb, model->unknown24, 0);
    M3_READ(sb, model->ikjtData, 0);
    M3_READ(sb, model->unknown25, 0);
    M3_READ(sb, model->unknown26, 24);
    M3_READ(sb, model->partsOfTurrentBehaviors, 0);
    M3_READ(sb, model->turrentBehaviors, 0);
    M3_REFR(sb, model->absoluteInverseBoneRestPositions, Matrix4, 0);
    M3_READ(sb, model->tightHitTest, 0);
    M3_READ(sb, model->fuzzyHitTestObjects, 0);
    M3_READ(sb, model->attachmentVolumes, 0);
    M3_READ(sb, model->attachmentVolumesAddon0, 22);
    M3_READ(sb, model->attachmentVolumesAddon1, 22);
    M3_READ(sb, model->billboardBehaviors, 0);
    M3_READ(sb, model->tmdData, 0);
    M3_READ(sb, model->unknown27, 0);
    M3_READ(sb, model->unknown28, 0);
}

static DWORD m3_animation_value_stride(m3Model_t const *model, Reference ref) {
    static const struct { char id[4]; DWORD stride; } values[] = {
        { "_23I", 4 }, { "_23U", 4 }, { "_61I", 2 }, { "_61U", 2 }, { "LAER", 4 },
        { "LOC\0", 4 }, { "2CEV", 8 }, { "3CEV", 12 }, { "TAUQ", 16 }, { "4CEV", 16 },
        { "GALF", 4 }, { "SDNB", 28 },
    };
    if (!ref.nEntries || ref.ref >= model->head->nRefs) return ref.nEntries ? 0 : 1;
    struct ReferenceEntry const *entry = &model->refs[ref.ref];
    if (!memcmp(entry->id, "TNVE", 4))
        return entry->version <= 2 ? 100 + entry->version * 4 : 0;
    FOR_LOOP(i, sizeof(values) / sizeof(values[0]))
        if (!memcmp(entry->id, values[i].id, 4)) return entry->version == 0 ? values[i].stride : 0;
    return 0;
}

/* Animation remains renderer-owned, but every raw declaration must be safe before publication. */
static BOOL m3_validate_animations(m3Model_t const *model) {
    FOR_LOOP(i, model->stcNum) {
        m3SequenceTimeline_t const *timeline = &model->stc[i];
        if (timeline->animIdsNum != timeline->animRefsNum) return false;
        FOR_LOOP(sd_ref, 13) {
            m3ReferenceRead_t sd_read = { .reference = timeline->sd[sd_ref],
                .element_size = sizeof(m3SequenceData_t) };
            DWORD sd_count;
            if (!SC2_M3ReferenceCount(model, &sd_read, &sd_count)) return false;
            FOR_LOOP(sd_index, sd_count) {
                m3SequenceData_t sd;
                DWORD key_count, value_count, value_stride;
                sd_read.element_index = sd_index;
                if (!SC2_M3ReferenceElement(model, &sd_read, &sd)) return false;
                m3ReferenceRead_t key_read = { .reference = sd.keys, .element_size = sizeof(m3Uint32_t),
                    .section_id = "_23I" };
                if (!SC2_M3ReferenceCount(model, &key_read, &key_count)) return false;
                value_stride = m3_animation_value_stride(model, sd.values);
                if (!value_stride) return false;
                m3ReferenceRead_t value_read = { .reference = sd.values, .element_size = value_stride };
                if (!SC2_M3ReferenceCount(model, &value_read, &value_count) || key_count != value_count)
                    return false;
            }
        }
    }
    return true;
}

/* Both renderer paths consume region-local faces, so validate once before either can publish/upload them. */
BOOL SC2_M3ValidateGeometry(m3Model_t const *model) {
    if (!model || !model->head || !model->valid) return false;
    FOR_LOOP(d, model->divisionsNum) {
        m3Divisions_t const *division = &model->divisions[d];
        if (division->facesNum % 3) return false;
        FOR_LOOP(r, division->regionsNum) {
            m3Region_t const *region = &division->regions[r];
            uint64_t vertex_end = (uint64_t)region->firstVertexIndex + region->verticesCount;
            uint64_t index_end = (uint64_t)region->firstTriangleIndex + region->triangleIndicesCount;
            uint64_t bone_end = (uint64_t)region->firstBoneLookupIndex + region->boneLookupIndicesCount;
            if (vertex_end > model->verticesNum || index_end > division->facesNum ||
                bone_end > model->boneLookupNum || region->triangleIndicesCount % 3)
                return false;
            FOR_LOOP(i, region->triangleIndicesCount)
                if (division->faces[region->firstTriangleIndex + i] >= region->verticesCount) return false;
            /* Region-local skin slots must not let either renderer read another palette. */
            FOR_LOOP(v, region->verticesCount)
                FOR_LOOP(i, 4)
                    if (model->vertices[region->firstVertexIndex + v].boneIndex[i] >=
                        region->boneLookupIndicesCount)
                        return false;
        }
        FOR_LOOP(b, division->batchesNum)
            if (division->batches[b].regionIndex >= division->regionsNum ||
                division->batches[b].materialReferenceIndex >= model->materialReferencesNum)
                return false;
    }
    return true;
}

m3Model_t *SC2_M3Parse(void const *data, DWORD size) {
    m3Model_t *model = calloc(1, sizeof(*model));
    m3Reader_t root;
    DWORD root_stride;
    if (!model || !data || size < sizeof(struct MD33)) return model;
    model->buffer = malloc(size);
    if (!model->buffer) return model;
    model->size = size;
    memcpy(model->buffer, data, size);
    model->head = model->buffer;
    if (memcmp(model->head->id, "43DM", 4) || model->head->ofsRefs < sizeof(struct MD33) ||
        model->head->ofsRefs >= model->size ||
        model->head->nRefs > (model->size - model->head->ofsRefs) / sizeof(struct ReferenceEntry) ||
        model->head->MODL.nEntries != 1 || model->head->MODL.ref >= model->head->nRefs) {
        fprintf(stderr, "SC2_M3Parse: invalid header\n");
        /* Invalid MD34 bytes are not a parsed model; the old non-NULL head made callers upload zeros. */
        model->head = NULL;
        return model;
    }
    model->refs = (struct ReferenceEntry *)((LPBYTE)model->buffer + model->head->ofsRefs);
    FOR_LOOP(i, model->head->nRefs)
        if (model->refs[i].offset >= model->size || model->refs[i].offset >= model->head->ofsRefs) {
            fprintf(stderr, "SC2_M3Parse: reference %u offset is out of bounds\n", i);
            model->head = NULL;
            return model;
        }
    model->type = model->refs[model->head->MODL.ref].version;
    model->valid = true;
        if (!m3_schema_reader(model, model->head->MODL, &m3_schema_Model, &root, &root_stride)) {
            fprintf(stderr, "SC2_M3Parse: invalid MODL root declaration\n");
            if (!memcmp(model->refs[model->head->MODL.ref].id, "LDOM", 4) &&
                model->refs[model->head->MODL.ref].nEntries == 1 &&
                !m3_stride_model(model->refs[model->head->MODL.ref].version))
                model->unsupported = true;
            model->head = NULL;
            return model;
        }
        m3_init_model(model, &root);
        if (!model->valid || root.readcount != root_stride || !m3_validate_animations(model) ||
            !SC2_M3ValidateGeometry(model)) {
            fprintf(stderr, "SC2_M3Parse: truncated or malformed referenced section\n");
            model->head = NULL;
    }
    return model;
}

static void m3_free_layers(m3Layer_t *layers, DWORD count) {
    FOR_LOOP(i, count) free(layers[i].imagePath);
    free(layers);
}

static void m3_free_material(m3Material_t *material) {
    free(material->name);
#define M3_FREE_LAYER(NAME) m3_free_layers(material->NAME, material->NAME##Num)
    M3_FREE_LAYER(diffuseLayer);
    M3_FREE_LAYER(decalLayer);
    M3_FREE_LAYER(specularLayer);
    M3_FREE_LAYER(glossLayer);
    M3_FREE_LAYER(emissiveLayer);
    M3_FREE_LAYER(emissive2Layer);
    M3_FREE_LAYER(evioLayer);
    M3_FREE_LAYER(evioMaskLayer);
    M3_FREE_LAYER(alphaMaskLayer);
    M3_FREE_LAYER(alphaMask2Layer);
    M3_FREE_LAYER(normalLayer);
    M3_FREE_LAYER(heightLayer);
    M3_FREE_LAYER(lightMapLayer);
    M3_FREE_LAYER(ambientOcclusionLayer);
#undef M3_FREE_LAYER
}

void SC2_M3Free(m3Model_t *model) {
    if (!model) return;
    free(model->modelName);
    FOR_LOOP(i, model->sequencesNum) free(model->sequences[i].name);
    free(model->sequences);
    FOR_LOOP(i, model->stcNum) {
        free(model->stc[i].name);
        free(model->stc[i].animIds);
        free(model->stc[i].animRefs);
    }
    free(model->stc);
    FOR_LOOP(i, model->stgNum) { free(model->stg[i].name); free(model->stg[i].stcID); }
    free(model->stg);
    FOR_LOOP(i, model->stsNum) free(model->sts[i].animIds);
    free(model->sts);
    FOR_LOOP(i, model->bonesNum) free(model->bones[i].name);
    free(model->bones);
    free(model->vertices);
    FOR_LOOP(i, model->divisionsNum) {
        free(model->divisions[i].faces);
        free(model->divisions[i].regions);
        free(model->divisions[i].batches);
    }
    free(model->divisions);
    free(model->boneLookup);
    free(model->materialReferences);
    FOR_LOOP(i, model->materialStandardNum) m3_free_material(&model->materialStandard[i]);
    free(model->materialStandard);
    FOR_LOOP(i, model->materialCompositeNum) {
        free(model->materialComposite[i].name);
        free(model->materialComposite[i].sections);
    }
    free(model->materialComposite);
    free(model->absoluteInverseBoneRestPositions);
    free(model->buffer);
    free(model);
}

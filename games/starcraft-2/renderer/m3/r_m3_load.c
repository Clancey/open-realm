#include "renderer/r_local.h"
#include "games/starcraft-2/common/sc2_map.h"
#include "r_m3.h"
#include "r_m3_utils.h"

#define M3_FOR_EACH(TYPE, VAR, LIST) \
for (m3##TYPE##_t const *VAR = LIST; VAR && VAR < LIST + LIST##Num; VAR++)

static MATRIX4 bonemats[BZ_M3_RENDERER_MAX_BONES];
static MATRIX4 tmp[BZ_M3_RENDERER_MAX_BONES];

#ifdef USE_SHADOWMAPS
extern bool is_rendering_lights;
#endif

static struct {
    LPSHADER shader;
    DWORD uDiffuseMap;
} m3 = { 0 };

void
R_EvalKeyframeValue(void const *left,
                    void const *right,
                    float t,
                    MODELKEYTRACKDATATYPE datatype,
                    MODELKEYTRACKTYPE linetype,
                    HANDLE out);

/* Map SC2 multi-directional lights onto the unified single directional + ambient.
   Use directional[0] as the primary light; ambient comes from the map ambient. */
static void M3_SetLightUniforms(LPSHADER shader) {
    sc2Map_t const *map = SC2_MapCurrent();
    sc2MapLighting_t const *lighting = map ? &map->lighting : NULL;
    VECTOR3 ambient = lighting && lighting->enabled ? lighting->ambient_color : (VECTOR3){ 1.0f, 1.0f, 1.0f };
    sc2DirectionalLight_t const *light0 = lighting && lighting->enabled ? &lighting->directional[0] : NULL;
    FLOAT enabled = light0 && light0->enabled ? 1.0f : 0.0f;
    VECTOR3 direction = enabled ? (VECTOR3){ -light0->direction.x, -light0->direction.y, -light0->direction.z } : (VECTOR3){ 0.0f, 0.0f, 1.0f };
    VECTOR3 color = (enabled && light0) ? light0->color : (VECTOR3){ 0.0f, 0.0f, 0.0f };
    FLOAT multiplier = (enabled && light0) ? light0->color_multiplier : 0.0f;

    R_Call(glUniform3f, shader->uLightAmbient, ambient.x, ambient.y, ambient.z);
    R_Call(glUniform3f, shader->uLightDir, direction.x, direction.y, direction.z);
    R_Call(glUniform3f, shader->uLightColor, color.x * multiplier, color.y * multiplier, color.z * multiplier);
}

enum {
    kMaterialStandard = 1,
    kMaterialDisplacement,
    kMaterialComposite,
    kMaterialTerrain,
    kMaterialVolume,
    kMaterialUnknown1,
    kMaterialCreep,
    kMaterialVolumeNoise,
    kMaterialSplatTerrainBake,
    kMaterialUnknown2,
    kMaterialLensFlare,
};

/* Converts M3 vertex data (SHORT UV ÷ 2048, ubyte normal ×2−1) to the unified
   float layout expected by the shared model shader. Uploads a VERTEX array so
   M3 uses the same VAO layout as MDX/M2. */
void M3_MakeBuffer(m3Model_t *model) {
    VERTEX *verts = model->verticesNum ? ri.MemAlloc(model->verticesNum * sizeof(VERTEX)) : NULL;
    model->renbuf = ri.MemAlloc(sizeof(BUFFER));

    FOR_LOOP(i, model->verticesNum) {
        m3Vertex_t const *src = &model->vertices[i];
        VERTEX *dst = &verts[i];
        dst->position = src->pos;
        dst->texcoord = (VECTOR2){ src->uv[0][0] / 2048.0f, src->uv[0][1] / 2048.0f };
        dst->normal = (VECTOR3){
            src->normal[0] / 127.5f - 1.0f,
            src->normal[1] / 127.5f - 1.0f,
            src->normal[2] / 127.5f - 1.0f,
        };
        dst->color = src->color;
        memcpy(dst->skin, src->boneIndex, 4);
        memcpy(dst->boneWeight, src->boneWeight, 4);
    }

    R_Call(glGenVertexArrays, 1, &model->renbuf->vao);
    R_Call(glBindVertexArray, model->renbuf->vao);
    R_Call(glGenBuffers, 1, &model->renbuf->vbo);
    R_Call(glBindBuffer, GL_ARRAY_BUFFER, model->renbuf->vbo);

    R_Call(glEnableVertexAttribArray, attrib_position);
    R_Call(glEnableVertexAttribArray, attrib_texcoord);
    R_Call(glEnableVertexAttribArray, attrib_skin1);
    R_Call(glEnableVertexAttribArray, attrib_boneWeight1);
    R_Call(glEnableVertexAttribArray, attrib_normal);
    R_Call(glEnableVertexAttribArray, attrib_color);

    R_Call(glVertexAttribPointer, attrib_position, 3, GL_FLOAT, GL_FALSE, sizeof(VERTEX), FOFS(vertex, position));
    R_Call(glVertexAttribPointer, attrib_texcoord, 2, GL_FLOAT, GL_FALSE, sizeof(VERTEX), FOFS(vertex, texcoord));
    R_Call(glVertexAttribPointer, attrib_skin1, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(VERTEX), FOFS(vertex, skin[0]));
    R_Call(glVertexAttribPointer, attrib_boneWeight1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VERTEX), FOFS(vertex, boneWeight[0]));
    R_Call(glVertexAttribPointer, attrib_normal, 3, GL_FLOAT, GL_FALSE, sizeof(VERTEX), FOFS(vertex, normal));
    R_Call(glVertexAttribPointer, attrib_color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(VERTEX), FOFS(vertex, color));

    R_Call(glBufferData, GL_ARRAY_BUFFER, model->verticesNum * sizeof(VERTEX), verts, GL_STATIC_DRAW);
    ri.MemFree(verts);
}

m3Uint32_t M3_FindAnimRef(m3SequenceTimeline_t const *timeline, m3Uint32_t animID) {
    if (!timeline || timeline->animIdsNum != timeline->animRefsNum)
        return 0;
    M3_FOR_EACH(Uint32, it, timeline->animIds) {
        if (animID == *it) {
            return timeline->animRefs[it - timeline->animIds];
        }
    }
    return 0;
}

#define M3_GET_ANIM_VALUE(ANIMREF, DATATYPE) \
M3_Get##ANIMREF##AnimValue(m3Model_t const *model, \
                           m3SequenceTimeline_t const *timeline, \
                           m3##ANIMREF##AnimRef_t const *animref, \
                           DWORD time) { \
    if (!model || !timeline || !animref) return animref ? animref->initValue : (m3##ANIMREF##_t){ 0 }; \
    m3Uint32_t const anim = M3_FindAnimRef(timeline, animref->animId); \
    if (anim == 0) return animref->initValue; \
    DWORD const sdref = anim >> 16; \
    DWORD const sdindex = anim & 0xffff; \
    m3SequenceData_t sd; \
    m3##ANIMREF##_t output = animref->initValue; \
    m3##ANIMREF##_t values[2]; \
    m3KeySpan_t span; \
    DWORD key_count, value_count; \
    if (sdref >= 13) return animref->initValue; \
    m3ReferenceRead_t sd_read = { .reference = timeline->sd[sdref], \
        .element_size = sizeof(m3SequenceData_t), .element_index = sdindex }; \
    if (!SC2_M3ReferenceElement(model, &sd_read, &sd)) return animref->initValue; \
    m3ReferenceRead_t key_read = { .reference = sd.keys, .element_size = sizeof(m3Uint32_t), \
        .section_id = "_23I" }; \
    m3ReferenceRead_t value_read = { .reference = sd.values, .element_size = sizeof(m3##ANIMREF##_t) }; \
    if (!SC2_M3ReferenceCount(model, &key_read, &key_count) || \
        !SC2_M3ReferenceCount(model, &value_read, &value_count) || key_count != value_count) \
        return animref->initValue; \
    if (!m3_find_key_span(model, sd.keys, time, &span)) return animref->initValue; \
    value_read.element_index = span.left; \
    if (!SC2_M3ReferenceElement(model, &value_read, &values[0])) return animref->initValue; \
    if (span.left == span.right) return values[0]; \
    value_read.element_index = span.right; \
    if (!SC2_M3ReferenceElement(model, &value_read, &values[1])) return animref->initValue; \
    R_EvalKeyframeValue(&values[0], &values[1], span.fraction, DATATYPE, TRACK_LINEAR, &output); \
    return output; \
}

DWORD   M3_GET_ANIM_VALUE(Uint32,  TDATA_INT1);
float   M3_GET_ANIM_VALUE(Float32, TDATA_FLOAT1);
VECTOR3 M3_GET_ANIM_VALUE(Vector3, TDATA_FLOAT3);
VECTOR4 M3_GET_ANIM_VALUE(Vector4, TDATA_FLOAT4);

static BOOL M3_MaterialIsBlended(m3Material_t const *material) {
    return material && material->blendMode >= BLEND_MODE_BLEND;
}

static BOOL M3_MaterialHasAlphaMask(m3Material_t const *material) {
    return material &&
        material->alphaMaskLayer &&
        material->alphaMaskLayer->texture;
}

static FLOAT M3_MaterialAlphaCutoff(m3Material_t const *material) {
    if (!material) {
        return 1.0f;
    }
    if (material->cutoutThreshold > 0) {
        return (FLOAT)material->cutoutThreshold / 255.0f;
    }
    if (!M3_MaterialIsBlended(material) && M3_MaterialHasAlphaMask(material)) {
        return 0.5f;
    }
    return -1.0f;
}

static COLOR32 M3_LayerColor(m3Layer_t const *layer) {
    COLOR32 color;

    if (!layer)
        return COLOR32_WHITE;
    color = layer->color.initValue;
    if (!color.r && !color.g && !color.b && !color.a)
        return COLOR32_WHITE;
    return color;
}

static BOOL M3_SetMaterialBlendMode(m3Material_t const *material) {
    switch (material ? material->blendMode : BLEND_MODE_NONE) {
        case BLEND_MODE_NONE:
        case BLEND_MODE_ALPHAKEY:
            R_Call(glDisable, GL_BLEND);
            R_Call(glBlendFunc, GL_ONE, GL_ZERO);
            R_Call(glDepthMask, GL_TRUE);
            break;
        case BLEND_MODE_BLEND:
#ifdef USE_SHADOWMAPS
            if (is_rendering_lights)
                return false;
#endif
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_ADD:
#ifdef USE_SHADOWMAPS
            if (is_rendering_lights)
                return false;
#endif
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_ONE, GL_ONE);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_ADDALPHA:
#ifdef USE_SHADOWMAPS
            if (is_rendering_lights)
                return false;
#endif
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_SRC_ALPHA, GL_ONE);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_MODULATE:
#ifdef USE_SHADOWMAPS
            if (is_rendering_lights)
                return false;
#endif
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_DST_COLOR, GL_ZERO);
            R_Call(glDepthMask, GL_FALSE);
            break;
        case BLEND_MODE_MODULATE_2X:
#ifdef USE_SHADOWMAPS
            if (is_rendering_lights)
                return false;
#endif
            R_Call(glEnable, GL_BLEND);
            R_Call(glBlendFunc, GL_DST_COLOR, GL_SRC_COLOR);
            R_Call(glDepthMask, GL_FALSE);
            break;
        default:
            R_Call(glDisable, GL_BLEND);
            R_Call(glBlendFunc, GL_ONE, GL_ZERO);
            R_Call(glDepthMask, GL_TRUE);
            break;
    }
    return true;
}

static void M3_LoadLayers(m3Layer_t *layers, DWORD count) {
    FOR_LOOP(i, count)
        if (layers[i].imagePath && *layers[i].imagePath)
            layers[i].texture = R_LoadTexture(layers[i].imagePath);
}

/* Layer textures are renderer-owned just like the VAO/VBO/EBO resources released below. */
static void M3_ReleaseLayers(m3Layer_t *layers, DWORD count) {
    FOR_LOOP(i, count)
        if (layers[i].texture) R_ReleaseTexture(layers[i].texture);
}

/* Texture registration and GL upload are renderer policy, never parser side effects. */
static void M3_UploadModel(m3Model_t *model) {
    FOR_LOOP(i, model->materialStandardNum) {
        m3Material_t *material = &model->materialStandard[i];
#define M3_LOAD_LAYER(NAME) M3_LoadLayers(material->NAME, material->NAME##Num)
        M3_LOAD_LAYER(diffuseLayer);
        M3_LOAD_LAYER(decalLayer);
        M3_LOAD_LAYER(specularLayer);
        M3_LOAD_LAYER(glossLayer);
        M3_LOAD_LAYER(emissiveLayer);
        M3_LOAD_LAYER(emissive2Layer);
        M3_LOAD_LAYER(evioLayer);
        M3_LOAD_LAYER(evioMaskLayer);
        M3_LOAD_LAYER(alphaMaskLayer);
        M3_LOAD_LAYER(alphaMask2Layer);
        M3_LOAD_LAYER(normalLayer);
        M3_LOAD_LAYER(heightLayer);
        M3_LOAD_LAYER(lightMapLayer);
        M3_LOAD_LAYER(ambientOcclusionLayer);
#undef M3_LOAD_LAYER
    }
    FOR_LOOP(i, model->divisionsNum) {
        m3Divisions_t *division = &model->divisions[i];
        R_Call(glGenBuffers, 1, &division->indicesBuffer);
        R_Call(glBindBuffer, GL_ELEMENT_ARRAY_BUFFER, division->indicesBuffer);
        R_Call(glBufferData, GL_ELEMENT_ARRAY_BUFFER, division->facesNum * sizeof(USHORT),
               division->faces, GL_STATIC_DRAW);
    }
    M3_MakeBuffer(model);
}

m3Model_t *R_LoadModelM3(void *data, DWORD size) {
    m3Model_t *model = SC2_M3Parse(data, size);
    if (model && model->head && !m3_renderer_model_supported(model)) {
        /* The old renderer wrote past tmp[128]; reject before any texture or GL resource upload. */
        fprintf(stderr, "R_LoadModelM3: model has %u bones; desktop limit is %u\n",
                model->bonesNum, BZ_M3_RENDERER_MAX_BONES);
        SC2_M3Free(model);
        return NULL;
    }
    if (model && model->head) M3_UploadModel(model);
    return model;
}

void R_FreeModelM3(m3Model_t *model) {
    if (!model) return;
    FOR_LOOP(i, model->materialStandardNum) {
        m3Material_t *material = &model->materialStandard[i];
#define M3_RELEASE_LAYER(NAME) M3_ReleaseLayers(material->NAME, material->NAME##Num)
        M3_RELEASE_LAYER(diffuseLayer);
        M3_RELEASE_LAYER(decalLayer);
        M3_RELEASE_LAYER(specularLayer);
        M3_RELEASE_LAYER(glossLayer);
        M3_RELEASE_LAYER(emissiveLayer);
        M3_RELEASE_LAYER(emissive2Layer);
        M3_RELEASE_LAYER(evioLayer);
        M3_RELEASE_LAYER(evioMaskLayer);
        M3_RELEASE_LAYER(alphaMaskLayer);
        M3_RELEASE_LAYER(alphaMask2Layer);
        M3_RELEASE_LAYER(normalLayer);
        M3_RELEASE_LAYER(heightLayer);
        M3_RELEASE_LAYER(lightMapLayer);
        M3_RELEASE_LAYER(ambientOcclusionLayer);
#undef M3_RELEASE_LAYER
    }
    FOR_LOOP(i, model->divisionsNum)
        if (model->divisions[i].indicesBuffer)
            R_Call(glDeleteBuffers, 1, &model->divisions[i].indicesBuffer);
    if (model->renbuf) {
        if (model->renbuf->vbo) R_Call(glDeleteBuffers, 1, &model->renbuf->vbo);
        if (model->renbuf->vao) R_Call(glDeleteVertexArrays, 1, &model->renbuf->vao);
        ri.MemFree(model->renbuf);
        model->renbuf = NULL;
    }
    SC2_M3Free(model);
}

static void M3_DrawRegionMaterial(m3Region_t const *region, m3Material_t const *material, FLOAT alpha) {
    LPCTEXTURE diffuse = material->diffuseLayer && material->diffuseLayer->texture ? material->diffuseLayer->texture : tr.texture[TEX_WHITE];
    COLOR32 diffuse_color = M3_LayerColor(material->diffuseLayer);
#ifndef __linux__
    DWORD const num_indices = region->triangleIndicesCount;
    DWORD const first_vertex = region->firstVertexIndex;
    HANDLE const indices = (HANDLE)(sizeof(USHORT) * region->firstTriangleIndex);
#endif

    if (!M3_SetMaterialBlendMode(material)) {
        return;
    }
    R_Call(glUniform4f, m3.shader->uGeosetColor,
           diffuse_color.r / 255.0f,
           diffuse_color.g / 255.0f,
           diffuse_color.b / 255.0f,
           diffuse_color.a / 255.0f * alpha);
    {
        FLOAT cutoff = M3_MaterialAlphaCutoff(material);
        R_Call(glUniform1i, m3.shader->uUseDiscard, cutoff >= 0.0f ? 1 : 0);
        R_Call(glUniform1f, m3.shader->uAlphaCutoff, cutoff >= 0.0f ? cutoff : 0.5f);
    }
    R_Call(glUniform1f, m3.shader->uFirstBoneLookupIndex, (FLOAT)region->firstBoneLookupIndex);

    R_Call(glActiveTexture, GL_TEXTURE0);
    R_Call(glBindTexture, GL_TEXTURE_2D, diffuse->texid);

    M3_FOR_EACH(Layer, layer, material->diffuseLayer) {
        if (!layer->texture)
            continue;
#ifndef __linux__
        R_Call(glDrawElementsBaseVertex, GL_TRIANGLES, num_indices, GL_UNSIGNED_SHORT, indices, first_vertex);
#endif
    }
}

static void M3_DrawRegionMaterialReference(m3Model_t const *model,
                                           m3Region_t const *region,
                                           m3MaterialReference_t const *mref,
                                           FLOAT alpha,
                                           BOOL blendedPass,
                                           DWORD depth) {
    if (!model || !region || !mref || depth > 4)
        return;
    switch (mref->materialType) {
        case kMaterialStandard:
            if (mref->materialIndex < model->materialStandardNum) {
                m3Material_t const *material = model->materialStandard+mref->materialIndex;
                if (M3_MaterialIsBlended(material) == blendedPass)
                    M3_DrawRegionMaterial(region, material, alpha);
            }
            break;
        case kMaterialComposite:
            if (mref->materialIndex >= model->materialCompositeNum)
                break;
            m3CompositeMaterial_t const *composite = model->materialComposite+mref->materialIndex;
            M3_FOR_EACH(CompositeMaterialSection, section, composite->sections) {
                if (section->materialReferenceIndex >= model->materialReferencesNum)
                    continue;
                M3_DrawRegionMaterialReference(model,
                                               region,
                                               model->materialReferences+section->materialReferenceIndex,
                                               alpha * section->alphaFactor.initValue,
                                               blendedPass,
                                               depth + 1);
            }
            break;
    }
}

void M3_DrawDivisions(m3Model_t const *model, m3Divisions_t const *divisions, BOOL blendedPass) {
    if (!model || !divisions || !divisions->indicesBuffer)
        return;
    R_Call(glBindBuffer, GL_ELEMENT_ARRAY_BUFFER, divisions->indicesBuffer);
    M3_FOR_EACH(Batch, batch, divisions->batches) {
        if (batch->regionIndex >= divisions->regionsNum ||
            batch->materialReferenceIndex >= model->materialReferencesNum)
            continue;
        M3_DrawRegionMaterialReference(model,
                                       divisions->regions+batch->regionIndex,
                                       model->materialReferences+batch->materialReferenceIndex,
                                       1.0f,
                                       blendedPass,
                                       0);
    }
}

void M3_MakeBoneMatrix(LPCVECTOR3 p, LPCVECTOR4 r, LPCVECTOR3 s, LPCMATRIX4 par, LPMATRIX4 m) {
    MATRIX4 matrix;
    Matrix4_identity(&matrix);
    Matrix4_translate(&matrix, p);
    Matrix4_rotate4(&matrix, r);
    Matrix4_scale(&matrix, s);
    Matrix4_multiply(par, &matrix, m);
}

//m3Sequence_t const *
//M3_FindSequenceByName(m3Model_t const *model,
//                      LPCSTR name)
//{
//    M3_FOR_EACH(Sequence, seq, model->sequences) {
//        if (!strcmp(seq->name, name)) {
//            return seq;
//        }
//    }
//    return NULL;
//}

m3SequenceTimeline_t const *
M3_FindSequenceTimeline(m3Model_t const *model,
                        m3Sequence_t const *seq)
{
    M3_FOR_EACH(SequenceTimeline, stc, model->stc) {
        if (stc->stsIndex == seq - model->sequences) {
            return stc;
        }
    }
    return NULL;
}

m3SequenceTimeline_t const *
M3_FindAnimationAtTime(m3Model_t const *model,
                       DWORD time,
                       DWORD *localtime)
{
    M3_FOR_EACH(Sequence, seq, model->sequences) {
        if (time < seq->interval[1]) {
            *localtime = time;
            return M3_FindSequenceTimeline(model, seq);
        } else {
            time -= seq->interval[1];
        }
    }
    return NULL;
}

void M3_RenderModel(renderEntity_t const *entity, m3Model_t const *model, LPCMATRIX4 transform) {
    MATRIX4 identity;
    if (!entity || !model || !model->renbuf || !model->bones || !model->absoluteInverseBoneRestPositions)
        return;
    Matrix4_identity(&identity);
    
    struct {
        m3SequenceTimeline_t const *stc;
        DWORD time;
    } a, b;
    
    a.stc = M3_FindAnimationAtTime(model, entity->oldframe, &a.time);
    b.stc = M3_FindAnimationAtTime(model, entity->frame, &b.time);

    M3_FOR_EACH(Bone, bone, model->bones) {
        LPCMATRIX4 parent = bone->parent >= 0 && bone->parent < (SHORT)model->bonesNum ? tmp+bone->parent : &identity;
        VECTOR3 a_p = M3_GetVector3AnimValue(model, a.stc, &bone->position, a.time);
        VECTOR4 a_r = M3_GetVector4AnimValue(model, a.stc, &bone->rotation, a.time);
        VECTOR3 a_s = M3_GetVector3AnimValue(model, a.stc, &bone->scale, a.time);
        VECTOR3 b_p = M3_GetVector3AnimValue(model, b.stc, &bone->position, b.time);
        VECTOR4 b_r = M3_GetVector4AnimValue(model, b.stc, &bone->rotation, b.time);
        VECTOR3 b_s = M3_GetVector3AnimValue(model, b.stc, &bone->scale, b.time);
        VECTOR3 p = Vector3_lerp(&a_p, &b_p, tr.viewDef.lerpfrac);
        QUATERNION r = Quaternion_slerp((LPCQUATERNION)&a_r, (LPCQUATERNION)&b_r, tr.viewDef.lerpfrac);
        VECTOR3 s = Vector3_lerp(&a_s, &b_s, tr.viewDef.lerpfrac);
//        float v = M3_GetUint32AnimValue(model, a.stc, &bone->visibility, a.time);
        M3_MakeBoneMatrix(&p, (LPCVECTOR4)&r, &s, parent, tmp+(bone-model->bones));
    }

    /* Build a full 128-entry bone palette indexed by boneLookup[i].
       Vertex boneIndex values are region-relative; uFirstBoneLookupIndex
       selects the corresponding pre-multiplied inverse-rest palette slot. */
    memset(bonemats, 0, sizeof(bonemats));
    FOR_LOOP(j, BZ_M3_RENDERER_MAX_BONES) {
        MATRIX4 ident; Matrix4_identity(&ident); bonemats[j] = ident;
    }
    M3_FOR_EACH(Uint16, boneLookup, model->boneLookup) {
        m3Uint16_t boneIndex = *boneLookup;
        DWORD paletteIndex = (DWORD)(boneLookup - model->boneLookup);
        if (boneIndex >= model->bonesNum || boneIndex >= model->absoluteInverseBoneRestPositionsNum ||
            paletteIndex >= BZ_M3_RENDERER_MAX_BONES) {
            continue;
        }
        Matrix4_multiply(tmp + boneIndex, model->absoluteInverseBoneRestPositions + boneIndex,
                         bonemats + paletteIndex);
    }

    MATRIX4 mScaledMatrix;
    MATRIX3 mNormalMatrix;

    memcpy(&mScaledMatrix, transform, sizeof(MATRIX4));
    // SC2 placed-object rotations are already in the entity matrix; the old global M3 +90 made bridges and doodads quarter-turn too far.
//    Matrix4_rotate(&mScaledMatrix, &(VECTOR3){0,0,90/*tr.viewDef.time*0.05*/}, ROTATE_ZYX);
    // SC2 entity scale is already applied by R_GetEntityMatrix; the old 100x loader scale put the camera inside units.
//    Matrix4_scale(&mScaledMatrix, &(VECTOR3){100,100,100});
    Matrix3_normal(&mNormalMatrix, &mScaledMatrix);

    R_Call(glDisable, GL_BLEND);
    R_Call(glEnable, GL_DEPTH_TEST);
    R_Call(glDepthMask, GL_TRUE);
    R_Call(glUseProgram, m3.shader->progid);
    R_Call(glUniform1i, m3.shader->uLightCount, 0);
#ifdef USE_SHADOWMAPS
    extern bool is_rendering_lights;
    if (is_rendering_lights) {
        R_Call(glUniformMatrix4fv, m3.shader->uViewProjectionMatrix, 1, GL_FALSE, tr.viewDef.lightMatrix.v);
    } else {
        R_Call(glUniformMatrix4fv, m3.shader->uViewProjectionMatrix, 1, GL_FALSE, tr.viewDef.viewProjectionMatrix.v);
    }
#else
    R_Call(glUniformMatrix4fv, m3.shader->uViewProjectionMatrix, 1, GL_FALSE, tr.viewDef.viewProjectionMatrix.v);
#endif
    R_Call(glUniformMatrix4fv, m3.shader->uLightMatrix, 1, GL_FALSE, tr.viewDef.lightMatrix.v);
    R_Call(glUniformMatrix4fv, m3.shader->uTextureMatrix, 1, GL_FALSE, tr.viewDef.textureMatrix.v);
    R_Call(glUniformMatrix4fv, m3.shader->uModelMatrix, 1, GL_FALSE, mScaledMatrix.v);
    R_Call(glUniformMatrix3fv, m3.shader->uNormalMatrix, 1, GL_TRUE, mNormalMatrix.v);
    R_Call(glUniformMatrix4fv, m3.shader->uBones,
           MIN(model->boneLookupNum, BZ_M3_RENDERER_MAX_BONES), GL_FALSE, bonemats->v);
    M3_SetLightUniforms(m3.shader);
    /* The unified model shader requires identity defaults for uniforms that
       M3 does not animate (texture UV transform, layer alpha, geoset colour). */
    R_Call(glUniform4f, m3.shader->uGeosetColor, 1.0f, 1.0f, 1.0f, 1.0f);
    R_Call(glUniform1f, m3.shader->uLayerAlpha, 1.0f);
    R_Call(glUniform2f, m3.shader->uUvTrans, 0.0f, 0.0f);
    R_Call(glUniform2f, m3.shader->uUvRot, 0.0f, 1.0f);
    R_Call(glUniform2f, m3.shader->uUvScale, 1.0f, 1.0f);
    R_Call(glUniform1i, m3.shader->uUseDiscard, 0);
    R_Call(glUniform1f, m3.shader->uAlphaCutoff, 0.5f);
    R_Call(glUniform1i, m3.shader->uUnshaded, 0);
    R_Call(glUniform1f, m3.shader->uFogEnable, 0);
    R_Call(glUniform1f, m3.shader->uFirstBoneLookupIndex, 0.0f);
    R_Call(glBindVertexArray, model->renbuf->vao);
    R_Call(glBindBuffer, GL_ARRAY_BUFFER, model->renbuf->vbo);
    
    R_BindTexture(tr.texture[TEX_WHITE], 0);
    
    R_Call(glDisable, GL_CULL_FACE);
    
    M3_FOR_EACH(Divisions, div, model->divisions) {
        M3_DrawDivisions(model, div, false);
    }
    M3_FOR_EACH(Divisions, div, model->divisions) {
        M3_DrawDivisions(model, div, true);
    }
    
    R_Call(glActiveTexture, GL_TEXTURE0);
    R_Call(glDepthMask, GL_TRUE);
    R_Call(glEnable, GL_BLEND);
}

void M3_Init(void) {
    m3.shader = R_ModelShader();
    /* uTexture (unit 0) is wired up by R_InitShader; diffuse texture binds to unit 0. */
}

void M3_Shutdown(void) {
    /* m3.shader is the shared model shader, released by the renderer. */
}

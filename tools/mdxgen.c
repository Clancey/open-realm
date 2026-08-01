/*
 * mdxgen - deterministic MDX (MDLX) binary model fixture generator
 *
 * Generates minimal binary MDX files for use as test fixtures in tests.mpq.
 * Output is byte-compatible with the renderer's r_mdx_load.c binary reader.
 * No third-party dependencies.
 *
 * Usage:
 *   mdxgen quad_sprite   <tex_path> <out.mdx>
 *   mdxgen panel_sprite  <tex_path> <out.mdx>
 *   mdxgen ui_panel      <tex_path> <out.mdx>
 *   mdxgen anim_pulse    <tex_path> <out.mdx>
 *   mdxgen rigged_anim   <tex_path> <out.mdx>
 *
 * quad_sprite  - 1x1 unit flat quad in XY-plane, 1 sequence "Stand"
 * panel_sprite - 2x1.5 unit flat quad, suitable for orthographic UI panels, 1 sequence "Stand"
 * ui_panel     - 0.6x0.4 unit flat quad from origin, matching Warcraft glue panels
 * anim_pulse   - 1x1 quad with 2 sequences "Stand" (0-1000) and "Birth" (1001-2000)
 * rigged_anim  - 1x1 quad, 2-bone hierarchy (root with a global-sequence translation
 *                track, child with a sequence-relative rotation track), a geoset alpha
 *                animation, and a 3-matrix-group vertex skin (single-bone + blended).
 *                Exercises slice 5C's skeletal/vertex/geoset animation decode path.
 *
 * All models include:
 *   - one texture (path provided as argument)
 *   - one opaque material with one layer
 *   - one geoset (4 vertices, 2 triangles)
 *   - one bone at the origin
 *   - one pivot
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

/* =========================================================================
 * Write buffer
 * =========================================================================*/
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} wbuf_t;

static void wb_grow(wbuf_t *b, size_t need) {
    if (b->size + need <= b->cap) return;
    size_t newcap = b->cap ? b->cap * 2 : 4096;
    while (newcap < b->size + need) newcap *= 2;
    b->data = realloc(b->data, newcap);
    if (!b->data) { fprintf(stderr, "mdxgen: out of memory\n"); exit(1); }
    b->cap = newcap;
}

static void wb_write(wbuf_t *b, const void *src, size_t n) {
    wb_grow(b, n);
    memcpy(b->data + b->size, src, n);
    b->size += n;
}

static void wb_u32(wbuf_t *b, uint32_t v) { wb_write(b, &v, 4); }
static void wb_i32(wbuf_t *b, int32_t  v) { wb_write(b, &v, 4); }
static void wb_f32(wbuf_t *b, float    v) { wb_write(b, &v, 4); }

static void wb_str(wbuf_t *b, const char *s, size_t fixed_len) {
    wb_grow(b, fixed_len);
    size_t len = s ? strlen(s) : 0;
    if (len > fixed_len) len = fixed_len;
    if (len) memcpy(b->data + b->size, s, len);
    if (fixed_len > len) memset(b->data + b->size + len, 0, fixed_len - len);
    b->size += fixed_len;
}

/* Reserve space for a uint32 size field; returns the offset so we can patch
 * it once we know the content size. */
static size_t wb_reserve_u32(wbuf_t *b) {
    size_t off = b->size;
    wb_u32(b, 0);
    return off;
}

/* Patch a previously reserved uint32 with the number of bytes written since
 * the byte AFTER the size field. */
static void wb_patch_size(wbuf_t *b, size_t off) {
    uint32_t sz = (uint32_t)(b->size - off - 4);
    memcpy(b->data + off, &sz, 4);
}

/* MDX object records include their own size DWORD; top-level chunk sizes do not. */
static void wb_patch_record_size(wbuf_t *b, size_t off) {
    uint32_t sz = (uint32_t)(b->size - off);
    memcpy(b->data + off, &sz, 4);
}

static void wb_tag(wbuf_t *b, const char tag[4]) {
    wb_write(b, tag, 4);
}

static void wb_free(wbuf_t *b) { free(b->data); b->data = NULL; b->size = b->cap = 0; }

/* =========================================================================
 * On-disk constants (matching r_mdx.h and r_mdx_load.c)
 * =========================================================================*/
#define MDX_MAGIC          "MDLX"
#define MDX_VERSION        800u

/*
 * Texture record on disk:
 *   replaceableID (4) + path[260] (260) + nWrapping (4) = 268 bytes
 */
#define MDX_TEX_PATH_LEN   260
#define MDX_TEX_RECORD     268u

/*
 * mdxSequence_t on disk = 132 bytes:
 *   name[80] + interval[2]*4 + movespeed(4) + flags(4) + rarity(4) + syncpoint(4) + bounds(28) = 132
 *
 * mdxBounds_t = float radius(4) + BOX3 min/max (2*3*4=24) = 28 bytes
 */
#define MDX_SEQ_RECORD     132u
#define MDX_BOUNDS_SIZE    28u

/* Node fixed content: name[80] + objectId(4) + parentId(4) + flags(4) = 92 bytes.
 * FileReadBlock size field stores node content size = 92+4=96? No:
 * ReadNode blockSize = cursize-readcount_at_entry = stored_size - 4.
 * After fixed reads: 80+4+4+4=92 bytes.  For while to not run: 92 = stored_size-4 → stored_size=96.
 * So node size field in file = 96. Then geoset_id(4) + geoset_anim_id(4) follow as overflow.
 */
#define MDX_NODE_NAME_LEN  80
#define MDX_NODE_SIZE_FIELD 96u   /* value stored in bone's size prefix (node content) */

/* Bone record total on disk: 4(size=96) + 80(name) + 4(objectId) + 4(parentId) + 4(flags)
 *                             + 4(geoset_id) + 4(geoset_anim_id) = 104 bytes */

/* Material layer fixed fields: blendMode(4)+flags(4)+textureId(4)+transformId(4)+coordId(4)+staticAlpha(4) = 24 */
#define MDX_LAYER_FIXED    24u

/* Camera fixed fields in ReadCamera: name[80] + pivot(12) + fov(4) + farClip(4) + nearClip(4) + targetPivot(12) = 116 */
#define MDX_CAM_FIXED      116u

/* MDLXNODE flags */
#define MDLXNODE_Bone      256u

/* =========================================================================
 * Geometry helpers
 * =========================================================================*/
static void wb_vec3(wbuf_t *b, float x, float y, float z) {
    wb_f32(b, x); wb_f32(b, y); wb_f32(b, z);
}

static void wb_vec2(wbuf_t *b, float u, float v) {
    wb_f32(b, u); wb_f32(b, v);
}

/* Write an mdxBounds_t: radius + BOX3 min + BOX3 max */
static void wb_bounds(wbuf_t *b, float radius,
                      float minx, float miny, float minz,
                      float maxx, float maxy, float maxz)
{
    wb_f32(b, radius);
    wb_vec3(b, minx, miny, minz);
    wb_vec3(b, maxx, maxy, maxz);
}

/* =========================================================================
 * MDX chunk emitters
 * =========================================================================*/

/* VERS chunk: 4 bytes content */
static void emit_VERS(wbuf_t *b) {
    wb_tag(b, "VERS");
    wb_u32(b, 4);
    wb_u32(b, MDX_VERSION);
}

/* MODL chunk: mdxInfo_t = name[80] + animFile[260] + bounds(28) + blendTime(4) = 372 bytes */
static void emit_MODL(wbuf_t *b, const char *model_name) {
    wb_tag(b, "MODL");
    wb_u32(b, 372);
    wb_str(b, model_name, 80);     /* name */
    wb_str(b, "", 260);            /* animationFile */
    wb_bounds(b, 1.0f,             /* bounds */
              -0.5f, -0.5f, 0.0f,
               0.5f,  0.5f, 0.0f);
    wb_u32(b, 150);                /* blendTime */
}

/* SEQS chunk: array of mdxSequence_t (132 bytes each) */
static void emit_SEQS(wbuf_t *b, const char **names,
                      uint32_t const *starts, uint32_t const *ends,
                      int n)
{
    wb_tag(b, "SEQS");
    wb_u32(b, (uint32_t)(n * MDX_SEQ_RECORD));
    for (int i = 0; i < n; i++) {
        wb_str(b, names[i], 80);   /* name */
        wb_u32(b, starts[i]);      /* interval[0] */
        wb_u32(b, ends[i]);        /* interval[1] */
        wb_f32(b, 0.0f);           /* movespeed */
        wb_u32(b, 0);              /* flags (0=looping) */
        wb_f32(b, 0.0f);           /* rarity */
        wb_i32(b, 0);              /* syncpoint */
        wb_bounds(b, 1.0f,         /* bounds */
                  -0.5f,-0.5f,0.0f,
                   0.5f, 0.5f,0.0f);
    }
}

/* TEXS chunk: on-disk texture records, 268 bytes each. */
static void emit_TEXS(wbuf_t *b, const char *tex_path) {
    wb_tag(b, "TEXS");
    wb_u32(b, MDX_TEX_RECORD);
    wb_u32(b, 0);                  /* replaceableID = TEXREPL_NONE */
    wb_str(b, tex_path, MDX_TEX_PATH_LEN);
    wb_i32(b, 0);                  /* nWrapping */
}

/* MTLS chunk: one material with one opaque layer referencing texture 0 */
static void emit_MTLS(wbuf_t *b) {
    wb_tag(b, "MTLS");
    size_t chunk_size_off = wb_reserve_u32(b);
    {
        /* One material entry: 4-byte size prefix then content */
        size_t mat_size_off = wb_reserve_u32(b);
        wb_i32(b, 0);              /* priority */
        wb_i32(b, 0);              /* flags */
        /* LAYS sub-chunk */
        wb_tag(b, "LAYS");
        wb_u32(b, 1);              /* num_layers */
        /* Layer entry: 4-byte size prefix then 24 bytes of fixed fields */
        wb_u32(b, MDX_LAYER_FIXED + 4);
        wb_u32(b, 4);              /* blendMode = 4 (AddAlpha, works for sprites) */
        wb_u32(b, 0x10);           /* flags = TwoSided */
        wb_u32(b, 0);              /* textureId = 0 */
        wb_u32(b, 0xFFFFFFFF);     /* transformId = none */
        wb_i32(b, 0);              /* coordId */
        wb_f32(b, 1.0f);           /* staticAlpha */
        wb_patch_record_size(b, mat_size_off);
    }
    wb_patch_size(b, chunk_size_off);
}

/* Write a quad geoset (4 verts, 2 triangles) */
static void emit_GEOS_RECT(wbuf_t *b, float minx, float miny, float maxx, float maxy) {
    wb_tag(b, "GEOS");
    size_t chunk_size_off = wb_reserve_u32(b);
    {
        /* One geoset entry: 4-byte size prefix then sub-chunks */
        size_t geo_size_off = wb_reserve_u32(b);

        /* VRTX: 4 vertices */
        wb_tag(b, "VRTX"); wb_u32(b, 4);
        wb_vec3(b, minx, miny, 0.0f);
        wb_vec3(b, maxx, miny, 0.0f);
        wb_vec3(b, maxx, maxy, 0.0f);
        wb_vec3(b, minx, maxy, 0.0f);

        /* NRMS: 4 normals (all pointing +Z) */
        wb_tag(b, "NRMS"); wb_u32(b, 4);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);

        /* PTYP: 1 primitive group of type 4 (triangles) */
        wb_tag(b, "PTYP"); wb_u32(b, 1); wb_u32(b, 4);

        /* PCNT: 1 primitive group with 2 triangles */
        wb_tag(b, "PCNT"); wb_u32(b, 1); wb_u32(b, 2);

        /* PVTX: 6 uint16 triangle indices */
        uint16_t tris[6] = { 0, 1, 2,  0, 2, 3 };
        wb_tag(b, "PVTX"); wb_u32(b, 6); wb_write(b, tris, 12);

        /* GNDX: 4 vertex group indices (all vertex group 0) */
        uint8_t gndx[4] = { 0, 0, 0, 0 };
        wb_tag(b, "GNDX"); wb_u32(b, 4); wb_write(b, gndx, 4);

        /* MTGC: 1 matrix group of size 1 (one bone) */
        wb_tag(b, "MTGC"); wb_u32(b, 1); wb_u32(b, 1);

        /* MATS: matrices + geoset metadata */
        wb_tag(b, "MATS");
        wb_u32(b, 1);              /* num_matrices */
        wb_u32(b, 0);              /* matrix[0] = bone 0 */
        wb_u32(b, 0);              /* materialID = 0 */
        wb_u32(b, 0);              /* group = 0 */
        wb_u32(b, 0);              /* selectable = 0 */
        /* default_bounds */
        wb_bounds(b, fmaxf(fabsf(maxx - minx), fabsf(maxy - miny)),
                  minx, miny, 0.0f,
                  maxx, maxy, 0.0f);
        wb_u32(b, 0);              /* num_bounds (per-sequence) = 0 */

        /* Retail MDX 800 writes UV channels after the fixed geoset metadata. */
        wb_tag(b, "UVAS"); wb_u32(b, 1);
        wb_tag(b, "UVBS"); wb_u32(b, 4);
        wb_vec2(b, 0.125f, 0.875f);
        wb_vec2(b, 0.75f, 0.625f);
        wb_vec2(b, 0.9f, 0.2f);
        wb_vec2(b, 0.3f, 0.1f);

        wb_patch_record_size(b, geo_size_off);
    }
    wb_patch_size(b, chunk_size_off);
}

/* Write a centered quad geoset (4 verts, 2 triangles, half_w x half_h footprint) */
static void emit_GEOS(wbuf_t *b, float hw, float hh) {
    emit_GEOS_RECT(b, -hw, -hh, hw, hh);
}

/* BONE chunk: one static bone */
static void emit_BONE(wbuf_t *b) {
    wb_tag(b, "BONE");
    size_t chunk_size_off = wb_reserve_u32(b);
    {
        /*
         * Bone record layout (per ReadBone / ReadNode analysis):
         *   [size=96][name(80)][objectId(4)][parentId(4)][flags(4)]
         *   [geoset_id(4, overflow)][geoset_anim_id(4, overflow)]
         *
         * The 4-byte size field stores 96 (=80+4+4+4+4_for_extra_loop_read).
         * Wait: ReadNode blockSize = stored_size - 4 (because readcount starts at 4).
         * After reading name(80)+objectId(4)+parentId(4)+flags(4)=92 bytes,
         * readcount = 4+92=96.  blockEnd = 4 + (stored_size-4) = stored_size.
         * For while to not fire: 96 >= stored_size → stored_size = 96.
         */
        wb_u32(b, MDX_NODE_SIZE_FIELD);   /* = 96 */
        wb_str(b, "Bone_Root", MDX_NODE_NAME_LEN);
        wb_u32(b, 0);                     /* objectId = 0 */
        wb_u32(b, 0xFFFFFFFF);            /* parentId = none */
        wb_u32(b, MDLXNODE_Bone);         /* flags */
        /* overflow fields (beyond node size, per MSG_ReadOverflow) */
        wb_u32(b, 0);                     /* geoset_id = 0 */
        wb_u32(b, 0xFFFFFFFF);            /* geoset_animation_id = none */
    }
    wb_patch_size(b, chunk_size_off);
}

/* PIVT chunk: one pivot (float[3]) per node, at origin */
static void emit_PIVT(wbuf_t *b) {
    wb_tag(b, "PIVT");
    wb_u32(b, 12);         /* 1 pivot = 3 floats */
    wb_vec3(b, 0.0f, 0.0f, 0.0f);
}

/* =========================================================================
 * Rigged/animated fixture (bone hierarchy, keyframes, global sequences,
 * geoset alpha animation, multi-bone vertex skin) for slice 5C tests.
 * =========================================================================*/

/* GLBS chunk: flat uint32 durations, no per-entry size prefix. */
static void emit_GLBS(wbuf_t *b, uint32_t duration_msec) {
    wb_tag(b, "GLBS");
    wb_u32(b, 4);
    wb_u32(b, duration_msec);
}

/* Writes one KGxx keytrack: keyCount + lineType(1=LINEAR, no tangents) + globalSeqId,
 * followed by keyCount keys of time(4) + n_values floats. Matches ReadKeyTrack exactly. */
static void wb_track(wbuf_t *b, const char tag[4], uint32_t global_seq,
                     const uint32_t *times, const float *values, int n_keys, int n_values) {
    wb_tag(b, tag);
    wb_u32(b, (uint32_t)n_keys);
    wb_u32(b, 1);              /* lineType = LINEAR */
    wb_u32(b, global_seq);
    for (int i = 0; i < n_keys; i++) {
        wb_u32(b, times[i]);
        for (int j = 0; j < n_values; j++) wb_f32(b, values[i * n_values + j]);
    }
}

/* PIVT for the 2-bone rigged fixture: Bone_Root at origin, Bone_Child offset +1 on Z. */
static void emit_PIVT_RIGGED(wbuf_t *b) {
    wb_tag(b, "PIVT");
    wb_u32(b, 24);
    wb_vec3(b, 0.0f, 0.0f, 0.0f);
    wb_vec3(b, 0.0f, 0.0f, 1.0f);
}

/* BONE chunk: Bone_Root (object 0, no parent) with a globally-looping KGTR translation
 * track (0,0,0)->(0,0,2) over the full 500ms global sequence), and Bone_Child (object 1,
 * parented to Bone_Root) with a sequence-relative KGRT rotation track, identity->90 degrees
 * about Z over the full "Stand" [0,2000) interval. Exercises hierarchy, pivots, global
 * sequences, and ordinary sequence-relative sampling in one fixture. */
static void emit_BONE_RIGGED(wbuf_t *b) {
    wb_tag(b, "BONE");
    size_t chunk_size_off = wb_reserve_u32(b);
    {
        size_t node_size_off = wb_reserve_u32(b);
        wb_str(b, "Bone_Root", MDX_NODE_NAME_LEN);
        wb_u32(b, 0);                  /* objectId */
        wb_u32(b, 0xFFFFFFFF);         /* parentId = none */
        wb_u32(b, MDLXNODE_Bone);      /* flags */
        {
            uint32_t times[2] = { 0, 500 };
            float values[6] = { 0,0,0,  0,0,2 };
            wb_track(b, "KGTR", 0, times, values, 2, 3);
        }
        wb_patch_record_size(b, node_size_off);
        wb_u32(b, 0);                  /* geoset_id overflow field: none */
        wb_u32(b, 0xFFFFFFFF);         /* geoset_animation_id overflow field: none */
    }
    {
        size_t node_size_off = wb_reserve_u32(b);
        wb_str(b, "Bone_Child", MDX_NODE_NAME_LEN);
        wb_u32(b, 1);                  /* objectId */
        wb_u32(b, 0);                  /* parentId = Bone_Root */
        wb_u32(b, MDLXNODE_Bone);      /* flags */
        {
            uint32_t times[2] = { 0, 2000 };
            float values[8] = { 0,0,0,1,  0,0,0.70710678f,0.70710678f };
            wb_track(b, "KGRT", 0xFFFFFFFF, times, values, 2, 4);
        }
        wb_patch_record_size(b, node_size_off);
        wb_u32(b, 0xFFFFFFFF);         /* geoset_id overflow field: none */
        wb_u32(b, 0xFFFFFFFF);         /* geoset_animation_id overflow field: none */
    }
    wb_patch_size(b, chunk_size_off);
}

/* GEOA chunk: one entry targeting geoset 0, with a KGAO alpha track fading 1.0 -> 0.25
 * over the "Stand" sequence. staticColor/KGAC color animation are out of slice 5C scope. */
static void emit_GEOA(wbuf_t *b) {
    wb_tag(b, "GEOA");
    size_t chunk_size_off = wb_reserve_u32(b);
    {
        size_t rec_size_off = wb_reserve_u32(b);
        wb_f32(b, 1.0f);               /* staticAlpha (unused: a KGAO track follows) */
        wb_u32(b, 0);                  /* flags */
        wb_vec3(b, 1.0f, 1.0f, 1.0f);  /* staticColor (color anim out of scope) */
        wb_u32(b, 0);                  /* geosetId = 0 */
        {
            uint32_t times[2] = { 0, 2000 };
            float values[2] = { 1.0f, 0.25f };
            wb_track(b, "KGAO", 0xFFFFFFFF, times, values, 2, 1);
        }
        wb_patch_record_size(b, rec_size_off);
    }
    wb_patch_size(b, chunk_size_off);
}

/* Quad geoset with a 3-matrix-group vertex skin: v0,v1 -> group0 (bone0 only, weight 255);
 * v2 -> group1 (bone1 only, weight 255); v3 -> group2 (blended bone0+bone1, weights 128/127).
 * Exercises matrix-palette dedup and the top-weighted vertex skin resolution/renormalization. */
static void emit_GEOS_RIGGED(wbuf_t *b, float hw, float hh) {
    wb_tag(b, "GEOS");
    size_t chunk_size_off = wb_reserve_u32(b);
    {
        size_t geo_size_off = wb_reserve_u32(b);

        wb_tag(b, "VRTX"); wb_u32(b, 4);
        wb_vec3(b, -hw, -hh, 0.0f);
        wb_vec3(b,  hw, -hh, 0.0f);
        wb_vec3(b,  hw,  hh, 0.0f);
        wb_vec3(b, -hw,  hh, 0.0f);

        wb_tag(b, "NRMS"); wb_u32(b, 4);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);
        wb_vec3(b, 0.0f, 0.0f, 1.0f);

        wb_tag(b, "PTYP"); wb_u32(b, 1); wb_u32(b, 4);
        wb_tag(b, "PCNT"); wb_u32(b, 1); wb_u32(b, 2);
        {
            uint16_t tris[6] = { 0, 1, 2,  0, 2, 3 };
            wb_tag(b, "PVTX"); wb_u32(b, 6); wb_write(b, tris, 12);
        }
        {
            uint8_t gndx[4] = { 0, 0, 1, 2 };
            wb_tag(b, "GNDX"); wb_u32(b, 4); wb_write(b, gndx, 4);
        }
        {
            uint32_t sizes[3] = { 1, 1, 2 };
            wb_tag(b, "MTGC"); wb_u32(b, 3); wb_write(b, sizes, 12);
        }
        wb_tag(b, "MATS");
        {
            uint32_t matrices[4] = { 0, 1, 0, 1 }; /* group0=[bone0] group1=[bone1] group2=[bone0,bone1] */
            wb_u32(b, 4);
            wb_write(b, matrices, 16);
        }
        wb_u32(b, 0);              /* materialID = 0 */
        wb_u32(b, 0);              /* group = 0 */
        wb_u32(b, 0);              /* selectable = 0 */
        wb_bounds(b, fmaxf(hw, hh) * 1.5f, -hw, -hh, 0.0f, hw, hh, 0.0f);
        wb_u32(b, 0);              /* num_bounds (per-sequence) = 0 */

        wb_tag(b, "UVAS"); wb_u32(b, 1);
        wb_tag(b, "UVBS"); wb_u32(b, 4);
        wb_vec2(b, 0.0f, 1.0f);
        wb_vec2(b, 1.0f, 1.0f);
        wb_vec2(b, 1.0f, 0.0f);
        wb_vec2(b, 0.0f, 0.0f);

        wb_patch_record_size(b, geo_size_off);
    }
    wb_patch_size(b, chunk_size_off);
}

/* =========================================================================
 * Model builders
 * =========================================================================*/

static int build_model(const char *tex_path, const char *out_path,
                       const char *model_name,
                       float hw, float hh,
                       const char **seq_names,
                       const uint32_t *seq_starts,
                       const uint32_t *seq_ends,
                       int n_seqs)
{
    wbuf_t b = { 0 };

    /* MDX magic (4 bytes, no size field) */
    wb_tag(&b, MDX_MAGIC);

    emit_VERS(&b);
    emit_MODL(&b, model_name);
    emit_SEQS(&b, seq_names, seq_starts, seq_ends, n_seqs);
    emit_TEXS(&b, tex_path);
    emit_MTLS(&b);
    emit_GEOS(&b, hw, hh);
    emit_BONE(&b);
    emit_PIVT(&b);

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "mdxgen: cannot open '%s' for writing\n", out_path);
        wb_free(&b);
        return 0;
    }
    fwrite(b.data, 1, b.size, f);
    fclose(f);
    fprintf(stderr, "mdxgen: wrote %s (%zu bytes)\n", out_path, b.size);
    wb_free(&b);
    return 1;
}

static int build_model_rect(const char *tex_path, const char *out_path,
                            const char *model_name,
                            float minx, float miny,
                            float maxx, float maxy,
                            const char **seq_names,
                            const uint32_t *seq_starts,
                            const uint32_t *seq_ends,
                            int n_seqs)
{
    wbuf_t b = { 0 };

    wb_tag(&b, MDX_MAGIC);

    emit_VERS(&b);
    emit_MODL(&b, model_name);
    emit_SEQS(&b, seq_names, seq_starts, seq_ends, n_seqs);
    emit_TEXS(&b, tex_path);
    emit_MTLS(&b);
    emit_GEOS_RECT(&b, minx, miny, maxx, maxy);
    emit_BONE(&b);
    emit_PIVT(&b);

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "mdxgen: failed to open output %s\n", out_path);
        wb_free(&b);
        return 0;
    }
    if (fwrite(b.data, 1, b.size, f) != b.size) {
        fprintf(stderr, "mdxgen: short write %s\n", out_path);
        fclose(f);
        wb_free(&b);
        return 0;
    }
    fclose(f);
    fprintf(stderr, "mdxgen: wrote %s (%zu bytes)\n", out_path, b.size);
    wb_free(&b);
    return 1;
}

/* Rigged 2-bone model: 1 sequence "Stand" [0,2000), 1 global sequence (500ms), a geoset
 * alpha animation, and a 3-matrix-group vertex skin. See emit_BONE_RIGGED/emit_GEOA/
 * emit_GEOS_RIGGED for the exact per-track/per-vertex values this exercises. */
static int build_model_rigged(const char *tex_path, const char *out_path,
                              const char *model_name, float hw, float hh)
{
    wbuf_t b = { 0 };
    const char *names[] = { "Stand" };
    uint32_t starts[] = { 0 };
    uint32_t ends[] = { 2000 };

    wb_tag(&b, MDX_MAGIC);
    emit_VERS(&b);
    emit_MODL(&b, model_name);
    emit_SEQS(&b, names, starts, ends, 1);
    emit_GLBS(&b, 500);
    emit_TEXS(&b, tex_path);
    emit_MTLS(&b);
    emit_GEOS_RIGGED(&b, hw, hh);
    emit_GEOA(&b);
    emit_BONE_RIGGED(&b);
    emit_PIVT_RIGGED(&b);

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "mdxgen: cannot open '%s' for writing\n", out_path);
        wb_free(&b);
        return 0;
    }
    if (fwrite(b.data, 1, b.size, f) != b.size) {
        fprintf(stderr, "mdxgen: short write %s\n", out_path);
        fclose(f);
        wb_free(&b);
        return 0;
    }
    fclose(f);
    fprintf(stderr, "mdxgen: wrote %s (%zu bytes)\n", out_path, b.size);
    wb_free(&b);
    return 1;
}

/* =========================================================================
 * Presets
 * =========================================================================*/

static int gen_quad_sprite(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mdxgen quad_sprite <tex_path> <out.mdx>\n");
        return 1;
    }
    const char *tex  = argv[1];
    const char *out  = argv[2];
    const char *names[]  = { "Stand" };
    uint32_t starts[] = { 0 };
    uint32_t ends[]   = { 1000 };
    return build_model(tex, out, "QuadSprite",
                       0.5f, 0.5f,
                       names, starts, ends, 1) ? 0 : 1;
}

static int gen_panel_sprite(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mdxgen panel_sprite <tex_path> <out.mdx>\n");
        return 1;
    }
    const char *tex  = argv[1];
    const char *out  = argv[2];
    const char *names[]  = { "Stand" };
    uint32_t starts[] = { 0 };
    uint32_t ends[]   = { 1000 };
    return build_model(tex, out, "PanelSprite",
                       1.0f, 0.75f,
                       names, starts, ends, 1) ? 0 : 1;
}

static int gen_ui_panel(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mdxgen ui_panel <tex_path> <out.mdx>\n");
        return 1;
    }
    const char *tex  = argv[1];
    const char *out  = argv[2];
    const char *names[]  = { "Stand" };
    uint32_t starts[] = { 0 };
    uint32_t ends[]   = { 1000 };
    return build_model_rect(tex, out, "UIPanel",
                            0.0f, 0.0f,
                            0.6f, 0.4f,
                            names, starts, ends, 1) ? 0 : 1;
}

static int gen_anim_pulse(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mdxgen anim_pulse <tex_path> <out.mdx>\n");
        return 1;
    }
    const char *tex  = argv[1];
    const char *out  = argv[2];
    const char *names[]  = { "Stand", "Birth" };
    uint32_t starts[] = { 0,    1001 };
    uint32_t ends[]   = { 1000, 2000 };
    return build_model(tex, out, "AnimPulse",
                       0.5f, 0.5f,
                       names, starts, ends, 2) ? 0 : 1;
}

static int gen_rigged_anim(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mdxgen rigged_anim <tex_path> <out.mdx>\n");
        return 1;
    }
    const char *tex = argv[1];
    const char *out = argv[2];
    return build_model_rigged(tex, out, "RiggedAnim", 0.5f, 0.5f) ? 0 : 1;
}

/* =========================================================================
 * main
 * =========================================================================*/
static void usage(void) {
    fprintf(stderr,
        "mdxgen - MDX binary model fixture generator\n"
        "\n"
        "Usage:\n"
        "  mdxgen quad_sprite   <tex_path> <out.mdx>\n"
        "  mdxgen panel_sprite  <tex_path> <out.mdx>\n"
        "  mdxgen ui_panel      <tex_path> <out.mdx>\n"
        "  mdxgen anim_pulse    <tex_path> <out.mdx>\n"
        "  mdxgen rigged_anim   <tex_path> <out.mdx>\n"
        "\n"
        "Examples:\n"
        "  mdxgen quad_sprite   TestUI/Textures/checker_8x8.blp  quad_sprite.mdx\n"
        "  mdxgen panel_sprite  TestUI/Textures/solid_white.blp  panel_sprite.mdx\n"
        "  mdxgen ui_panel      TestUI/Textures/solid_white.blp  ui_panel.mdx\n"
        "  mdxgen anim_pulse    TestUI/Textures/alpha_ring_16x16.blp  anim_pulse.mdx\n"
        "  mdxgen rigged_anim   TestUI/Textures/checker_8x8.blp  rigged_anim.mdx\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    const char *cmd   = argv[1];
    int   sub_argc    = argc - 1;
    char **sub_argv   = argv + 1;

    if (strcmp(cmd, "quad_sprite")  == 0) return gen_quad_sprite(sub_argc, sub_argv);
    if (strcmp(cmd, "panel_sprite") == 0) return gen_panel_sprite(sub_argc, sub_argv);
    if (strcmp(cmd, "ui_panel")     == 0) return gen_ui_panel(sub_argc, sub_argv);
    if (strcmp(cmd, "anim_pulse")   == 0) return gen_anim_pulse(sub_argc, sub_argv);
    if (strcmp(cmd, "rigged_anim")  == 0) return gen_rigged_anim(sub_argc, sub_argv);

    fprintf(stderr, "mdxgen: unknown command '%s'\n\n", cmd);
    usage();
    return 1;
}

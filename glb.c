/* glb.c — glTF binary container + accessor decode. See glb.h. */

#include "glb.h"
#include "json.h"
#include "rhi.h"
#include "image.h"
#include "sol_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>                /* sqrtf — matrix-joint decomposition (item 9) */

/* glTF is little-endian; read bytes explicitly so we don't depend on host order. */
static sol_u32 read_u32(const unsigned char *p) {
    return (sol_u32)p[0] | ((sol_u32)p[1] << 8) | ((sol_u32)p[2] << 16) | ((sol_u32)p[3] << 24);
}

static float read_f32(const unsigned char *p) {
    sol_u32 u = read_u32(p);
    float   f;
    memcpy(&f, &u, 4);                 /* portable type-pun (no strict-aliasing UB) */
    return f;
}

static sol_u32 read_index(const unsigned char *p, int comp) {
    if (comp == 5121) return (sol_u32)p[0];                            /* unsigned byte  */
    if (comp == 5123) return (sol_u32)p[0] | ((sol_u32)p[1] << 8);     /* unsigned short */
    return read_u32(p);                                               /* 5125 unsigned int */
}

static int type_ncomp(const char *t) {
    if (!t) return 0;
    if (strcmp(t, "SCALAR") == 0) return 1;
    if (strcmp(t, "VEC2") == 0)   return 2;
    if (strcmp(t, "VEC3") == 0)   return 3;
    if (strcmp(t, "VEC4") == 0)   return 4;
    if (strcmp(t, "MAT4") == 0)   return 16;
    return 0;
}

static int comp_size(int comp) {
    if (comp == 5120 || comp == 5121) return 1;   /* (u)byte  */
    if (comp == 5122 || comp == 5123) return 2;   /* (u)short */
    if (comp == 5125 || comp == 5126) return 4;   /* uint / float */
    return 0;
}

/* A resolved accessor: where element 0 lives in the BIN blob, plus how to step. */
typedef struct {
    const unsigned char *base;
    sol_u32 count;
    int     comp;     /* componentType */
    int     ncomp;    /* components per element */
    int     stride;   /* bytes between elements */
    int     ok;
} Accessor;

/* accessor[i] -> bufferView -> a slice of the BIN blob, with bounds checking. */
static Accessor get_accessor(JsonValue *g, const unsigned char *bin, sol_u32 bin_len, int idx) {
    Accessor   a;
    JsonValue *acc, *bv;
    int        bv_idx, comp, ncomp, csize, elem_size, stride;
    sol_u32    count;
    size_t     acc_off, bv_off, base_off;

    a.base = (const unsigned char *)0;
    a.count = 0; a.comp = 0; a.ncomp = 0; a.stride = 0; a.ok = 0;
    if (idx < 0) return a;

    acc = json_index(json_member(g, "accessors"), (sol_u32)idx);
    if (!acc) return a;
    bv_idx = (int)json_number(json_member(acc, "bufferView"), -1.0);
    if (bv_idx < 0) return a;                  /* sparse / no bufferView: unsupported */
    comp    = (int)json_number(json_member(acc, "componentType"), 0.0);
    count   = (sol_u32)json_number(json_member(acc, "count"), 0.0);
    ncomp   = type_ncomp(json_string(json_member(acc, "type")));
    acc_off = (size_t)json_number(json_member(acc, "byteOffset"), 0.0);
    csize   = comp_size(comp);
    if (!ncomp || !csize || !count) return a;
    elem_size = ncomp * csize;

    bv = json_index(json_member(g, "bufferViews"), (sol_u32)bv_idx);
    if (!bv) return a;
    bv_off = (size_t)json_number(json_member(bv, "byteOffset"), 0.0);
    stride = (int)json_number(json_member(bv, "byteStride"), 0.0);
    if (stride == 0) stride = elem_size;       /* 0 -> tightly packed */

    base_off = bv_off + acc_off;
    if (base_off + (size_t)(count - 1) * (size_t)stride + (size_t)elem_size > (size_t)bin_len) {
        return a;                              /* would read past the blob */
    }

    a.base   = bin + base_off;
    a.count  = count;
    a.comp   = comp;
    a.ncomp  = ncomp;
    a.stride = stride;
    a.ok     = 1;
    return a;
}

static float acc_float(const Accessor *a, sol_u32 i, int c) {
    return read_f32(a->base + (size_t)i * (size_t)a->stride + (size_t)c * (size_t)comp_size(a->comp));
}

static sol_u32 acc_index(const Accessor *a, sol_u32 i) {
    return read_index(a->base + (size_t)i * (size_t)a->stride, a->comp);
}

/* Build one primitive's geometry into a Mesh, baking the node `world` matrix
   into positions and normals. Pushes vertices in POSITION-accessor order (so
   glTF indices map straight to builder indices), then the triangles. */
static int build_primitive(JsonValue *g, const unsigned char *bin, sol_u32 bin_len,
                           JsonValue *prim, mat4 world, Mesh *out) {
    JsonValue  *attrs;
    Accessor    pos, nrm, uv, idx;
    int         has_nrm, has_uv, has_idx;
    int         pos_i, nrm_i, uv_i, idx_i;
    MeshBuilder mb;
    sol_u32     i;

    attrs = json_member(prim, "attributes");
    pos_i = (int)json_number(json_member(attrs, "POSITION"), -1.0);
    nrm_i = (int)json_number(json_member(attrs, "NORMAL"), -1.0);
    uv_i  = (int)json_number(json_member(attrs, "TEXCOORD_0"), -1.0);
    idx_i = (int)json_number(json_member(prim, "indices"), -1.0);
    if (pos_i < 0) return 0;

    pos = get_accessor(g, bin, bin_len, pos_i);
    if (!pos.ok) return 0;
    has_nrm = (nrm_i >= 0); if (has_nrm) { nrm = get_accessor(g, bin, bin_len, nrm_i); has_nrm = nrm.ok; }
    has_uv  = (uv_i  >= 0); if (has_uv)  { uv  = get_accessor(g, bin, bin_len, uv_i);  has_uv  = uv.ok; }
    has_idx = (idx_i >= 0); if (has_idx) { idx = get_accessor(g, bin, bin_len, idx_i); has_idx = idx.ok; }

    mb_init(&mb);
    for (i = 0; i < pos.count; i++) {
        vec3  lp, ln, wp, wn;
        float u = 0.0f, v = 0.0f;
        lp = vec3_make(acc_float(&pos, i, 0), acc_float(&pos, i, 1), acc_float(&pos, i, 2));
        ln = vec3_make(0.0f, 0.0f, 0.0f);
        if (has_nrm) ln = vec3_make(acc_float(&nrm, i, 0), acc_float(&nrm, i, 1), acc_float(&nrm, i, 2));
        if (has_uv)  { u = acc_float(&uv, i, 0); v = acc_float(&uv, i, 1); }
        wp = mat4_mul_point(world, lp);                            /* bake node transform */
        wn = has_nrm ? vec3_normalize(mat4_mul_dir(world, ln)) : ln;
        mb_push_vertex(&mb, wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, u, 1.0f - v);  /* glTF UV top-left -> flip V */
    }

    if (has_idx) {
        for (i = 0; i + 2 < idx.count; i += 3) {
            mb_push_triangle(&mb, acc_index(&idx, i), acc_index(&idx, i + 1), acc_index(&idx, i + 2));
        }
    } else {
        for (i = 0; i + 2 < pos.count; i += 3) {       /* non-indexed: sequential */
            mb_push_triangle(&mb, i, i + 1, i + 2);
        }
    }

    *out = mesh_from_builder(&mb);
    mb_free(&mb);
    return 1;
}

/* Decode the image referenced by a texture-reference object (a material's
   baseColorTexture / metallicRoughnessTexture / ...) and upload it in the given
   color space. Cached by glTF image index — which assumes an image isn't reused
   across color spaces (true for color-vs-data maps; refine if an asset breaks
   it). Returns a 0 handle when absent or the image can't be reached. */
static RhiTexture decode_texture(JsonValue *g, const unsigned char *bin, sol_u32 bin_len,
                                 JsonValue *tex_ref, RhiTextureFormat fmt,
                                 RhiTexture *cache, sol_u32 img_count) {
    RhiTexture none;
    JsonValue *tex, *img, *bv;
    int        tex_idx, img_idx, bv_idx;
    size_t     bvoff, bvlen;
    Image      decoded;

    none.id = 0;
    tex_idx = (int)json_number(json_member(tex_ref, "index"), -1.0);
    if (tex_idx < 0) return none;

    tex = json_index(json_member(g, "textures"), (sol_u32)tex_idx);
    img_idx = (int)json_number(json_member(tex, "source"), -1.0);
    if (img_idx < 0 || (sol_u32)img_idx >= img_count) return none;
    if (cache[img_idx].id) return cache[img_idx];          /* decoded earlier */

    img = json_index(json_member(g, "images"), (sol_u32)img_idx);
    bv_idx = (int)json_number(json_member(img, "bufferView"), -1.0);
    if (bv_idx < 0) return none;                            /* external/data-uri: unsupported */
    bv = json_index(json_member(g, "bufferViews"), (sol_u32)bv_idx);
    bvoff = (size_t)json_number(json_member(bv, "byteOffset"), 0.0);
    bvlen = (size_t)json_number(json_member(bv, "byteLength"), 0.0);
    if (bvoff + bvlen > (size_t)bin_len) return none;

    if (image_load_from_memory(bin + bvoff, (int)bvlen, &decoded)) {
        RhiTexture t = rhi_create_texture(decoded.pixels, decoded.w, decoded.h, fmt);
        image_free(&decoded);
        cache[img_idx] = t;
        return t;
    }
    return none;
}

/* Resolve a full PBR material: the texture maps + scalar factors. Absent factors
   fall back to the glTF spec defaults (white, metal 1, rough 1). Albedo decodes
   sRGB; the MR map decodes linear (data, not color). 8c-8d add AO / normal. */
static Material get_material(JsonValue *g, const unsigned char *bin, sol_u32 bin_len,
                             int mat_idx, RhiTexture *cache, sol_u32 img_count) {
    Material   out;
    JsonValue *mat, *pbr, *bcf;

    out.albedo_tex.id = 0;
    out.mr_tex.id     = 0;
    out.ao_tex.id     = 0;
    out.normal_tex.id = 0;
    out.base_color    = vec3_make(1.0f, 1.0f, 1.0f);
    out.emissive      = vec3_make(0.0f, 0.0f, 0.0f);
    out.metallic      = 1.0f;     /* glTF spec defaults when a factor is absent */
    out.roughness     = 1.0f;
    out.ao_strength   = 1.0f;
    out.normal_scale  = 1.0f;
    if (mat_idx < 0) return out;

    mat = json_index(json_member(g, "materials"), (sol_u32)mat_idx);
    pbr = json_member(mat, "pbrMetallicRoughness");

    out.metallic  = (float)json_number(json_member(pbr, "metallicFactor"),  1.0);
    out.roughness = (float)json_number(json_member(pbr, "roughnessFactor"), 1.0);

    bcf = json_member(pbr, "baseColorFactor");
    if (json_count(bcf) >= 3) {                  /* RGBA array; take RGB (linear) */
        out.base_color = vec3_make((float)json_number(json_index(bcf, 0), 1.0),
                                   (float)json_number(json_index(bcf, 1), 1.0),
                                   (float)json_number(json_index(bcf, 2), 1.0));
    }
    {   /* emissiveFactor: material-level, linear RGB (P4 item 5) */
        JsonValue *emf = json_member(mat, "emissiveFactor");
        if (json_count(emf) >= 3)
            out.emissive = vec3_make((float)json_number(json_index(emf, 0), 0.0),
                                     (float)json_number(json_index(emf, 1), 0.0),
                                     (float)json_number(json_index(emf, 2), 0.0));
    }

    out.albedo_tex = decode_texture(g, bin, bin_len,
                       json_member(pbr, "baseColorTexture"),         RHI_TEX_SRGB8, cache, img_count);
    out.mr_tex     = decode_texture(g, bin, bin_len,
                       json_member(pbr, "metallicRoughnessTexture"), RHI_TEX_RGBA8, cache, img_count);

    {   /* occlusionTexture: material-level (not under pbr); R = AO. Often the
           same image as the MR map (ORM) -> decode_texture returns the cache. */
        JsonValue *occ = json_member(mat, "occlusionTexture");
        out.ao_tex      = decode_texture(g, bin, bin_len, occ, RHI_TEX_RGBA8, cache, img_count);
        out.ao_strength = (float)json_number(json_member(occ, "strength"), 1.0);
    }
    {   /* normalTexture: material-level; tangent-space normals (linear) + a scale */
        JsonValue *nrm = json_member(mat, "normalTexture");
        out.normal_tex   = decode_texture(g, bin, bin_len, nrm, RHI_TEX_RGBA8, cache, img_count);
        out.normal_scale = (float)json_number(json_member(nrm, "scale"), 1.0);
    }
    return out;
}

/* A node's local transform: a "matrix" (16 column-major floats, same order as
   our mat4) if present, else composed from translation/rotation/scale (defaults
   identity). */
static void node_trs(JsonValue *node, vec3 *t, quat *q, vec3 *s) {
    JsonValue *jt = json_member(node, "translation");
    JsonValue *jr = json_member(node, "rotation");
    JsonValue *js = json_member(node, "scale");
    *t = vec3_make((float)json_number(json_index(jt, 0), 0.0),
                   (float)json_number(json_index(jt, 1), 0.0),
                   (float)json_number(json_index(jt, 2), 0.0));
    q->x = (float)json_number(json_index(jr, 0), 0.0);
    q->y = (float)json_number(json_index(jr, 1), 0.0);
    q->z = (float)json_number(json_index(jr, 2), 0.0);
    q->w = (float)json_number(json_index(jr, 3), 1.0);
    *s = vec3_make((float)json_number(json_index(js, 0), 1.0),
                   (float)json_number(json_index(js, 1), 1.0),
                   (float)json_number(json_index(js, 2), 1.0));
}

static mat4 node_local(JsonValue *node) {
    JsonValue *jm = json_member(node, "matrix");
    if (jm && json_count(jm) == 16) {
        mat4 r;
        int  i;
        for (i = 0; i < 16; i++) r.m[i] = (float)json_number(json_index(jm, (sol_u32)i), 0.0);
        return r;
    }
    {
        vec3 t, s;
        quat q;
        node_trs(node, &t, &q, &s);
        return mat4_from_trs(t, q, s);
    }
}

/* a node's local transform as TRS COMPONENTS: straight from the TRS
   members, or a matrix decomposed — T = column 3, S = column lengths,
   R = the normalized columns via quat_from_mat4 (the item-6d deferral,
   cashed for item 9). */
static void node_decompose(JsonValue *node, vec3 *t, quat *q, vec3 *s) {
    JsonValue *jm = json_member(node, "matrix");
    if (jm && json_count(jm) == 16) {
        mat4  m  = node_local(node);
        float sx = sqrtf(m.m[0]*m.m[0] + m.m[1]*m.m[1] + m.m[2]*m.m[2]);
        float sy = sqrtf(m.m[4]*m.m[4] + m.m[5]*m.m[5] + m.m[6]*m.m[6]);
        float sz = sqrtf(m.m[8]*m.m[8] + m.m[9]*m.m[9] + m.m[10]*m.m[10]);
        int   c;
        if (sx < 1e-8f || sy < 1e-8f || sz < 1e-8f) sx = sy = sz = 1.0f;
        for (c = 0; c < 3; c++) {
            m.m[c]     /= sx;
            m.m[4 + c] /= sy;
            m.m[8 + c] /= sz;
        }
        *t = vec3_make(m.m[12], m.m[13], m.m[14]);
        *q = quat_from_mat4(m);
        *s = vec3_make(sx, sy, sz);
        return;
    }
    node_trs(node, t, q, s);
}

/* the inverse of one node's local: inv(T R S) = S^-1 R^-1 T^-1 —
   composable up a chain, so no general matrix inverse is ever needed */
static mat4 node_local_inv(JsonValue *node) {
    vec3 t, s, si;
    quat q;
    node_decompose(node, &t, &q, &s);
    si = vec3_make(s.x != 0.0f ? 1.0f / s.x : 0.0f,
                   s.y != 0.0f ? 1.0f / s.y : 0.0f,
                   s.z != 0.0f ? 1.0f / s.z : 0.0f);
    return mat4_mul(mat4_scale(si),
           mat4_mul(quat_to_mat4(quat_conjugate(q)),
                    mat4_translate(vec3_make(-t.x, -t.y, -t.z))));
}

/* Traversal context: shared inputs + the growable output parts. */
typedef struct {
    JsonValue           *g;
    const unsigned char *bin;
    sol_u32              bin_len;
    RhiTexture          *cache;
    sol_u32              img_count;
    GlbPart             *parts;
    sol_u32              count;
    sol_u32              cap;
} GlbCtx;

static void ctx_add(GlbCtx *c, Mesh mesh, Material material) {
    if (c->count == c->cap) {
        sol_u32  cap   = c->cap ? c->cap * 2 : 8;
        GlbPart *grown = (GlbPart *)realloc(c->parts, (size_t)cap * sizeof(GlbPart));
        if (!grown) return;            /* drop this part rather than crash */
        c->parts = grown;
        c->cap   = cap;
    }
    c->parts[c->count].mesh     = mesh;
    c->parts[c->count].material = material;
    c->count++;
}

/* Walk a node and its children, composing world transforms and emitting a part
   per primitive of any referenced mesh. */
static void process_node(GlbCtx *c, int node_idx, mat4 parent, int depth) {
    JsonValue *node, *children;
    mat4       world;
    int        mesh_idx;
    sol_u32    i;

    if (depth > 256) return;           /* cycle guard */
    node = json_index(json_member(c->g, "nodes"), (sol_u32)node_idx);
    if (!node) return;
    world = mat4_mul(parent, node_local(node));

    mesh_idx = (int)json_number(json_member(node, "mesh"), -1.0);
    if (mesh_idx >= 0 && json_member(node, "skin") != (JsonValue *)0)
        mesh_idx = -1;       /* THE IMPORT FORK (item 9): a skinned mesh must
                                NOT be baked — its vertices stay in mesh-local
                                bind space for the skeleton to pose at runtime
                                (glb_load_skinned is its door) */
    if (mesh_idx >= 0) {
        JsonValue *prims = json_member(json_index(json_member(c->g, "meshes"), (sol_u32)mesh_idx),
                                       "primitives");
        sol_u32 np = json_count(prims), p;
        for (p = 0; p < np; p++) {
            JsonValue *prim = json_index(prims, p);
            Mesh       m;
            if (build_primitive(c->g, c->bin, c->bin_len, prim, world, &m)) {
                int mat_idx = (int)json_number(json_member(prim, "material"), -1.0);
                Material mat = get_material(c->g, c->bin, c->bin_len, mat_idx, c->cache, c->img_count);
                ctx_add(c, m, mat);
            }
        }
    }

    children = json_member(node, "children");
    for (i = 0; i < json_count(children); i++) {
        process_node(c, (int)json_number(json_index(children, i), -1.0), world, depth + 1);
    }
}

static unsigned char *slurp_file(const char *path, long *out_size) {
    FILE          *f;
    long           size;
    unsigned char *buf;
    size_t         got;

    f = fopen(path, "rb");
    if (!f) return (unsigned char *)0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return (unsigned char *)0; }
    size = ftell(f);
    if (size < 0) { fclose(f); return (unsigned char *)0; }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { fclose(f); return (unsigned char *)0; }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    *out_size = (long)got;
    return buf;
}

/* open a GLB container: slurp, validate magic/version, locate the JSON +
   BIN chunks, parse the JSON. Returns the raw file buffer (the BIN pointer
   ALIASES into it — caller frees the buffer AND json_free's *out_g), or
   NULL on any failure. Shared by glb_load and glb_load_skeleton. */
static unsigned char *glb_open(const char *path, JsonValue **out_g,
                               const unsigned char **out_bin,
                               sol_u32 *out_bin_len) {
    unsigned char *buf;
    long           size;
    char          *json_str;
    size_t         off;

    *out_g       = (JsonValue *)0;
    *out_bin     = (const unsigned char *)0;
    *out_bin_len = 0;

    buf = slurp_file(path, &size);
    if (!buf) return (unsigned char *)0;
    if (size < 12 || read_u32(buf) != 0x46546C67u || read_u32(buf + 4) != 2u) {  /* "glTF", v2 */
        free(buf);
        return (unsigned char *)0;
    }

    json_str = (char *)0;
    off = 12;
    while (off + 8 <= (size_t)size) {                  /* walk chunks */
        sol_u32              clen  = read_u32(buf + off);
        sol_u32              ctype = read_u32(buf + off + 4);
        const unsigned char *cdata = buf + off + 8;
        if (off + 8 + (size_t)clen > (size_t)size) break;            /* truncated */
        if (ctype == 0x4E4F534Au && !json_str) {                     /* "JSON" */
            json_str = (char *)malloc((size_t)clen + 1);
            if (json_str) { memcpy(json_str, cdata, (size_t)clen); json_str[clen] = '\0'; }
        } else if (ctype == 0x004E4942u && !*out_bin) {              /* "BIN\0" */
            *out_bin     = cdata;
            *out_bin_len = clen;
        }
        off += 8 + (size_t)clen;
    }
    if (!json_str) { free(buf); return (unsigned char *)0; }

    *out_g = json_parse(json_str);
    free(json_str);
    if (!*out_g) { free(buf); return (unsigned char *)0; }
    return buf;
}

sol_bool glb_load(const char *path, GlbModel *out) {
    unsigned char       *buf;
    const unsigned char *bin;
    sol_u32              bin_len;
    JsonValue           *g;

    out->parts = (GlbPart *)0;
    out->count = 0;

    buf = glb_open(path, &g, &bin, &bin_len);
    if (!buf) return SOL_FALSE;

    /* traverse the default scene's node tree, composing + baking transforms */
    {
        GlbCtx     ctx;
        JsonValue *scene, *roots;
        int        scene_idx;
        sol_u32    i;

        ctx.g         = g;
        ctx.bin       = bin;
        ctx.bin_len   = bin_len;
        ctx.img_count = json_count(json_member(g, "images"));
        ctx.cache     = ctx.img_count ? (RhiTexture *)calloc(ctx.img_count, sizeof(RhiTexture)) : (RhiTexture *)0;
        ctx.parts     = (GlbPart *)0;
        ctx.count     = 0;
        ctx.cap       = 0;

        scene_idx = (int)json_number(json_member(g, "scene"), 0.0);
        scene     = json_index(json_member(g, "scenes"), (sol_u32)scene_idx);
        roots     = json_member(scene, "nodes");
        for (i = 0; i < json_count(roots); i++) {
            process_node(&ctx, (int)json_number(json_index(roots, i), -1.0), mat4_identity(), 0);
        }
        free(ctx.cache);                      /* CPU bookkeeping; the GPU textures persist */

        out->parts = ctx.parts;
        out->count = ctx.count;
    }

    json_free(g);
    free(buf);
    return (out->count > 0) ? SOL_TRUE : SOL_FALSE;
}

void glb_free(GlbModel *out) {
    free(out->parts);
    out->parts = (GlbPart *)0;
    out->count = 0;
}

/* ---- skeletal animation (P4 item 9 piece 1): skins + animations ----
   Parses skin 0 and every animation into compact SkelData. Channels
   target NODES in the file; here they are remapped to JOINT indices
   (channels aimed at non-joint nodes are skipped). Static non-joint
   ANCESTOR transforms are folded into each root joint's root_pre, since
   glTF joint worlds compose in scene space. All of it is accessor
   surface — get_accessor/acc_float carry the load, as promised. */
/* P4 item 9 piece 2: the skinned primitive itself — UNBAKED (vertices in
   mesh-local bind space; the palette poses them, the object's transform
   places them), interleaved into the 20-float layout, uploaded. Handles
   the sample assets' quirks honestly: non-indexed primitives (the fox)
   get indices synthesized; absent NORMAL (the fox again) gets per-face
   normals computed; absent TEXCOORD reads zero; JOINTS_0 may be
   ubyte/ushort/uint, WEIGHTS_0 must be float. One primitive in v1 (both
   test assets have exactly one; more warns and takes the first). */
sol_bool glb_load_skinned(const char *path, SkinnedModel *out) {
    unsigned char       *buf;
    const unsigned char *bin;
    sol_u32              bin_len;
    JsonValue           *g, *nodes, *prim;
    float               *verts = (float *)0;
    sol_u32             *idx   = (sol_u32 *)0;
    sol_u32              vn, in_count, i;
    sol_bool             ok = SOL_FALSE;

    memset(out, 0, sizeof *out);
    buf = glb_open(path, &g, &bin, &bin_len);
    if (!buf) return SOL_FALSE;

    nodes = json_member(g, "nodes");
    prim  = (JsonValue *)0;
    for (i = 0; i < json_count(nodes); i++) {
        JsonValue *n = json_index(nodes, i);
        if ((int)json_number(json_member(n, "skin"), -1.0) == 0 &&
            json_member(n, "mesh") != (JsonValue *)0) {
            JsonValue *prims = json_member(
                json_index(json_member(g, "meshes"),
                    (sol_u32)(int)json_number(json_member(n, "mesh"), -1.0)),
                "primitives");
            if (json_count(prims) > 1)
                fprintf(stderr, "glb: %s has %u skinned primitives; "
                        "taking the first\n", path, (unsigned)json_count(prims));
            prim = json_index(prims, 0);
            break;
        }
    }
    if (prim == (JsonValue *)0) goto done;

    {
        JsonValue *attrs = json_member(prim, "attributes");
        Accessor   apos  = get_accessor(g, bin, bin_len,
            (int)json_number(json_member(attrs, "POSITION"), -1.0));
        Accessor   anrm  = get_accessor(g, bin, bin_len,
            (int)json_number(json_member(attrs, "NORMAL"), -1.0));
        Accessor   auv   = get_accessor(g, bin, bin_len,
            (int)json_number(json_member(attrs, "TEXCOORD_0"), -1.0));
        Accessor   ajnt  = get_accessor(g, bin, bin_len,
            (int)json_number(json_member(attrs, "JOINTS_0"), -1.0));
        Accessor   awgt  = get_accessor(g, bin, bin_len,
            (int)json_number(json_member(attrs, "WEIGHTS_0"), -1.0));
        Accessor   aidx  = get_accessor(g, bin, bin_len,
            (int)json_number(json_member(prim, "indices"), -1.0));
        int        c;

        if (!apos.ok || apos.comp != 5126 || !ajnt.ok || !awgt.ok ||
            awgt.comp != 5126) {
            fprintf(stderr, "glb: %s skinned attributes unsupported\n", path);
            goto done;
        }
        vn = apos.count;
        verts = (float *)calloc((size_t)vn * 20, sizeof(float));
        if (!verts) goto done;
        for (i = 0; i < vn; i++) {
            float *v = verts + (size_t)i * 20;
            for (c = 0; c < 3; c++) v[c] = acc_float(&apos, i, c);
            if (anrm.ok && anrm.comp == 5126) {
                for (c = 0; c < 3; c++) v[3 + c] = acc_float(&anrm, i, c);
            }
            if (auv.ok && auv.comp == 5126) {
                v[6] = acc_float(&auv, i, 0);
                v[7] = 1.0f - acc_float(&auv, i, 1);   /* the standing V flip */
            }
            /* tangents stay zero (no normal maps on the v1 menagerie) */
            for (c = 0; c < 4; c++) {
                v[12 + c] = (float)read_index(
                    ajnt.base + (size_t)i * (size_t)ajnt.stride
                              + (size_t)c * (size_t)comp_size(ajnt.comp),
                    ajnt.comp);
                v[16 + c] = acc_float(&awgt, i, c);
            }
        }

        if (aidx.ok) {
            in_count = aidx.count;
            idx = (sol_u32 *)malloc((size_t)in_count * sizeof(sol_u32));
            if (!idx) goto done;
            for (i = 0; i < in_count; i++) idx[i] = acc_index(&aidx, i);
        } else {                       /* non-indexed (the fox): 0,1,2,... */
            in_count = vn;
            idx = (sol_u32 *)malloc((size_t)in_count * sizeof(sol_u32));
            if (!idx) goto done;
            for (i = 0; i < in_count; i++) idx[i] = i;
        }

        if (!anrm.ok) {                /* no normals (the fox): per-face,
                                          accumulated then normalized */
            for (i = 0; i + 2 < in_count; i += 3) {
                float *p0 = verts + (size_t)idx[i]     * 20;
                float *p1 = verts + (size_t)idx[i + 1] * 20;
                float *p2 = verts + (size_t)idx[i + 2] * 20;
                float  e1x = p1[0]-p0[0], e1y = p1[1]-p0[1], e1z = p1[2]-p0[2];
                float  e2x = p2[0]-p0[0], e2y = p2[1]-p0[1], e2z = p2[2]-p0[2];
                float  nx = e1y*e2z - e1z*e2y;
                float  ny = e1z*e2x - e1x*e2z;
                float  nz = e1x*e2y - e1y*e2x;
                int    k;
                for (k = 0; k < 3; k++) {
                    float *v = verts + (size_t)idx[i + k] * 20;
                    v[3] += nx; v[4] += ny; v[5] += nz;
                }
            }
            for (i = 0; i < vn; i++) {
                float *v = verts + (size_t)i * 20;
                float  l = sqrtf(v[3]*v[3] + v[4]*v[4] + v[5]*v[5]);
                if (l > 1e-12f) { v[3] /= l; v[4] /= l; v[5] /= l; }
                else            { v[4] = 1.0f; }
            }
        }

        out->mesh = mesh_from_skinned(verts, vn, idx, in_count);

        /* the material, through the same door as static parts */
        {
            sol_u32     img_count = json_count(json_member(g, "images"));
            RhiTexture *cache     = img_count
                ? (RhiTexture *)calloc(img_count, sizeof(RhiTexture))
                : (RhiTexture *)0;
            out->material = get_material(g, bin, bin_len,
                (int)json_number(json_member(prim, "material"), -1.0),
                cache, img_count);
            free(cache);
        }
    }
    ok = SOL_TRUE;

done:
    free(verts);
    free(idx);
    json_free(g);
    free(buf);
    if (ok)                             /* the skeleton, through its own door
                                           (a second open of a small file,
                                           once per session — honest cost) */
        ok = glb_load_skeleton(path, &out->rig);
    if (!ok) memset(&out->mesh, 0, sizeof out->mesh);
    return ok;
}

sol_bool glb_load_skeleton(const char *path, SkelData *out) {
    unsigned char       *buf;
    const unsigned char *bin;
    sol_u32              bin_len;
    JsonValue           *g, *skin, *joints, *nodes, *anims;
    int                 *node_parent = (int *)0;
    int                 *node_joint  = (int *)0;
    int                  nn, jn, i, j;
    sol_bool             ok = SOL_FALSE;

    memset(out, 0, sizeof *out);
    buf = glb_open(path, &g, &bin, &bin_len);
    if (!buf) return SOL_FALSE;

    skin   = json_index(json_member(g, "skins"), 0);
    joints = skin ? json_member(skin, "joints") : (JsonValue *)0;
    nodes  = json_member(g, "nodes");
    jn = (int)json_count(joints);
    nn = (int)json_count(nodes);
    if (skin == (JsonValue *)0 || jn <= 0 || nn <= 0) goto done;
    if (jn > SKEL_MAX_JOINTS) {
        fprintf(stderr, "glb: %s has %d joints (cap %d)\n",
                path, jn, SKEL_MAX_JOINTS);
        goto done;
    }

    /* node-space maps: who is whose parent, who is a joint */
    node_parent = (int *)malloc((size_t)nn * sizeof(int));
    node_joint  = (int *)malloc((size_t)nn * sizeof(int));
    if (!node_parent || !node_joint) goto done;
    for (i = 0; i < nn; i++) { node_parent[i] = -1; node_joint[i] = -1; }
    for (i = 0; i < nn; i++) {
        JsonValue *ch = json_member(json_index(nodes, (sol_u32)i), "children");
        sol_u32    c;
        for (c = 0; c < json_count(ch); c++) {
            int id = (int)json_number(json_index(ch, c), -1.0);
            if (id >= 0 && id < nn) node_parent[id] = i;
        }
    }
    for (j = 0; j < jn; j++) {
        int id = (int)json_number(json_index(joints, (sol_u32)j), -1.0);
        if (id < 0 || id >= nn) goto done;
        node_joint[id] = j;
    }

    /* the skeleton: rest TRS, joint-space parents, folded ancestors */
    out->skel.joint_count = jn;
    for (j = 0; j < jn; j++) {
        int        id   = (int)json_number(json_index(joints, (sol_u32)j), -1.0);
        JsonValue *node = json_index(nodes, (sol_u32)id);
        int        p    = node_parent[id];
        /* matrix-form joints are legal when un-animated (RiggedSimple's
           root is one): node_decompose splits either form into TRS */
        node_decompose(node, &out->skel.rest_t[j], &out->skel.rest_r[j],
                       &out->skel.rest_s[j]);
        out->skel.parent[j]   = (p >= 0 && node_joint[p] >= 0) ? node_joint[p]
                                                               : -1;
        out->skel.root_pre[j] = mat4_identity();
        if (out->skel.parent[j] < 0) {
            int a;
            for (a = p; a >= 0; a = node_parent[a]) {
                out->skel.root_pre[j] =
                    mat4_mul(node_local(json_index(nodes, (sol_u32)a)),
                             out->skel.root_pre[j]);
            }
        }
    }

    /* glTF's skinning equation carries inverse(global(meshNode)) on its
       LEFT: skinned vertices are authored in the MESH NODE's local space,
       and the palette must return them there — the renderer adds only the
       object's own transform on top. Fold that inverse into every root's
       prefix; with it, the bind pose yields identity palettes UNIVERSALLY
       (RiggedSimple's cylinder hangs under two rotated parents — the
       assets where the naive form "happens to work" just have this factor
       at identity). */
    {
        int mesh_node = -1;
        for (i = 0; i < nn; i++) {
            JsonValue *n2 = json_index(nodes, (sol_u32)i);
            if ((int)json_number(json_member(n2, "skin"), -1.0) == 0 &&
                json_member(n2, "mesh") != (JsonValue *)0) {
                mesh_node = i;
                break;
            }
        }
        if (mesh_node >= 0) {
            mat4 inv = mat4_identity();
            int  a;
            for (a = mesh_node; a >= 0; a = node_parent[a])
                inv = mat4_mul(inv,
                               node_local_inv(json_index(nodes, (sol_u32)a)));
            for (j = 0; j < jn; j++) {
                if (out->skel.parent[j] < 0)
                    out->skel.root_pre[j] =
                        mat4_mul(inv, out->skel.root_pre[j]);
            }
        }
    }

    /* inverse binds: a MAT4 accessor, column-major like our mat4;
       absent = identity (the spec's default) */
    {
        Accessor ib = get_accessor(g, bin, bin_len,
            (int)json_number(json_member(skin, "inverseBindMatrices"), -1.0));
        for (j = 0; j < jn; j++) {
            int c;
            if (ib.ok && ib.ncomp == 16 && ib.comp == 5126 &&
                (int)ib.count >= jn) {
                for (c = 0; c < 16; c++)
                    out->skel.inverse_bind[j].m[c] =
                        acc_float(&ib, (sol_u32)j, c);
            } else {
                out->skel.inverse_bind[j] = mat4_identity();
            }
        }
    }

    /* evaluation order: parents before children, computed once (the
       bvh-refit lesson) — a cycle means a corrupt file */
    {
        int placed[SKEL_MAX_JOINTS];
        int done_n = 0;
        memset(placed, 0, sizeof placed);
        while (done_n < jn) {
            int progressed = 0;
            for (j = 0; j < jn; j++) {
                if (placed[j]) continue;
                if (out->skel.parent[j] < 0 || placed[out->skel.parent[j]]) {
                    out->skel.order[done_n++] = j;
                    placed[j]  = 1;
                    progressed = 1;
                }
            }
            if (!progressed) goto done;
        }
    }

    /* the clips: channels resolved, remapped, copied out of the blob */
    anims = json_member(g, "animations");
    {
        sol_u32 an = json_count(anims), a;
        if (an > 0) {
            out->clips = (SkelClip *)calloc((size_t)an, sizeof(SkelClip));
            if (!out->clips) goto done;
            out->clip_count = (int)an;
            for (a = 0; a < an; a++) {
                JsonValue  *anim = json_index(anims, a);
                JsonValue  *jch  = json_member(anim, "channels");
                JsonValue  *jsm  = json_member(anim, "samplers");
                const char *nm   = json_string(json_member(anim, "name"));
                SkelClip   *cl   = &out->clips[a];
                sol_u32     cn   = json_count(jch), c;
                char        fallback[8];
                if (!nm || !nm[0]) {                   /* unnamed: "clip0".. */
                    strcpy(fallback, "clip");
                    fallback[4] = (char)('0' + (int)(a % 10u));
                    fallback[5] = '\0';
                    nm = fallback;
                }
                cl->name = (char *)malloc(strlen(nm) + 1);
                if (cl->name) strcpy(cl->name, nm);
                cl->channels = (SkelChannel *)calloc(cn ? cn : 1,
                                                     sizeof(SkelChannel));
                if (!cl->channels) continue;
                for (c = 0; c < cn; c++) {
                    JsonValue   *chan  = json_index(jch, c);
                    JsonValue   *tgt   = json_member(chan, "target");
                    int          tnode = (int)json_number(json_member(tgt, "node"), -1.0);
                    const char  *pth   = json_string(json_member(tgt, "path"));
                    JsonValue   *smp   = json_index(jsm,
                        (sol_u32)(int)json_number(json_member(chan, "sampler"), -1.0));
                    const char  *itp   = smp ? json_string(json_member(smp, "interpolation"))
                                             : (const char *)0;
                    int          cpath, interp, nc, ki;
                    Accessor     tin, tout;
                    SkelChannel *sc;
                    if (tnode < 0 || tnode >= nn || node_joint[tnode] < 0 ||
                        pth == (const char *)0 || smp == (JsonValue *)0)
                        continue;                      /* non-joint target etc. */
                    if      (strcmp(pth, "translation") == 0) cpath = SKEL_PATH_T;
                    else if (strcmp(pth, "rotation")    == 0) cpath = SKEL_PATH_R;
                    else if (strcmp(pth, "scale")       == 0) cpath = SKEL_PATH_S;
                    else continue;                     /* "weights": morphs, deferred */
                    interp = SKEL_INTERP_LINEAR;
                    if (itp && strcmp(itp, "STEP") == 0) {
                        interp = SKEL_INTERP_STEP;
                    } else if (itp && strcmp(itp, "LINEAR") != 0) {
                        fprintf(stderr, "glb: %s channel skipped (%s)\n",
                                path, itp);            /* CUBICSPLINE: deferred */
                        continue;
                    }
                    tin  = get_accessor(g, bin, bin_len,
                        (int)json_number(json_member(smp, "input"), -1.0));
                    tout = get_accessor(g, bin, bin_len,
                        (int)json_number(json_member(smp, "output"), -1.0));
                    nc = (cpath == SKEL_PATH_R) ? 4 : 3;
                    if (!tin.ok || !tout.ok || tin.comp != 5126 ||
                        tout.comp != 5126 || tin.ncomp != 1 ||
                        tout.ncomp != nc || tin.count == 0 ||
                        tout.count < tin.count)
                        continue;
                    sc = &cl->channels[cl->channel_count];
                    sc->joint     = node_joint[tnode];
                    sc->path      = cpath;
                    sc->interp    = interp;
                    sc->key_count = (int)tin.count;
                    sc->times  = (float *)malloc((size_t)tin.count * sizeof(float));
                    sc->values = (float *)malloc((size_t)tin.count *
                                                 (size_t)nc * sizeof(float));
                    if (!sc->times || !sc->values) {
                        free(sc->times);
                        free(sc->values);
                        continue;
                    }
                    for (ki = 0; ki < (int)tin.count; ki++) {
                        int cc;
                        sc->times[ki] = acc_float(&tin, (sol_u32)ki, 0);
                        for (cc = 0; cc < nc; cc++)
                            sc->values[ki * nc + cc] =
                                acc_float(&tout, (sol_u32)ki, cc);
                    }
                    if (sc->times[sc->key_count - 1] > cl->duration)
                        cl->duration = sc->times[sc->key_count - 1];
                    cl->channel_count++;
                }
            }
        }
    }
    ok = SOL_TRUE;

done:
    free(node_parent);
    free(node_joint);
    if (!ok) skel_data_free(out);
    json_free(g);
    free(buf);
    return ok;
}

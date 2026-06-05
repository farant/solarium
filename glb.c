/* glb.c — glTF binary container + accessor decode. See glb.h. */

#include "glb.h"
#include "json.h"
#include "rhi.h"
#include "image.h"
#include "sol_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    out.base_color    = vec3_make(1.0f, 1.0f, 1.0f);
    out.metallic      = 1.0f;     /* glTF spec defaults when a factor is absent */
    out.roughness     = 1.0f;
    out.ao_strength   = 1.0f;
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
    return out;
}

/* A node's local transform: a "matrix" (16 column-major floats, same order as
   our mat4) if present, else composed from translation/rotation/scale (defaults
   identity). */
static mat4 node_local(JsonValue *node) {
    JsonValue *jm = json_member(node, "matrix");
    if (jm && json_count(jm) == 16) {
        mat4 r;
        int  i;
        for (i = 0; i < 16; i++) r.m[i] = (float)json_number(json_index(jm, (sol_u32)i), 0.0);
        return r;
    }
    {
        JsonValue *jt = json_member(node, "translation");
        JsonValue *jr = json_member(node, "rotation");
        JsonValue *js = json_member(node, "scale");
        vec3 t, s;
        quat q;
        t = vec3_make((float)json_number(json_index(jt, 0), 0.0),
                      (float)json_number(json_index(jt, 1), 0.0),
                      (float)json_number(json_index(jt, 2), 0.0));
        q.x = (float)json_number(json_index(jr, 0), 0.0);
        q.y = (float)json_number(json_index(jr, 1), 0.0);
        q.z = (float)json_number(json_index(jr, 2), 0.0);
        q.w = (float)json_number(json_index(jr, 3), 1.0);
        s = vec3_make((float)json_number(json_index(js, 0), 1.0),
                      (float)json_number(json_index(js, 1), 1.0),
                      (float)json_number(json_index(js, 2), 1.0));
        return mat4_from_trs(t, q, s);
    }
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

sol_bool glb_load(const char *path, GlbModel *out) {
    unsigned char       *buf;
    long                 size;
    char                *json_str;
    const unsigned char *bin;
    sol_u32              bin_len;
    size_t               off;
    JsonValue           *g;

    out->parts = (GlbPart *)0;
    out->count = 0;

    buf = slurp_file(path, &size);
    if (!buf) return SOL_FALSE;
    if (size < 12 || read_u32(buf) != 0x46546C67u || read_u32(buf + 4) != 2u) {  /* "glTF", v2 */
        free(buf);
        return SOL_FALSE;
    }

    json_str = (char *)0;
    bin = (const unsigned char *)0;
    bin_len = 0;
    off = 12;
    while (off + 8 <= (size_t)size) {                  /* walk chunks */
        sol_u32              clen  = read_u32(buf + off);
        sol_u32              ctype = read_u32(buf + off + 4);
        const unsigned char *cdata = buf + off + 8;
        if (off + 8 + (size_t)clen > (size_t)size) break;            /* truncated */
        if (ctype == 0x4E4F534Au && !json_str) {                     /* "JSON" */
            json_str = (char *)malloc((size_t)clen + 1);
            if (json_str) { memcpy(json_str, cdata, (size_t)clen); json_str[clen] = '\0'; }
        } else if (ctype == 0x004E4942u && !bin) {                   /* "BIN\0" */
            bin = cdata;
            bin_len = clen;
        }
        off += 8 + (size_t)clen;
    }
    if (!json_str) { free(buf); return SOL_FALSE; }

    g = json_parse(json_str);
    free(json_str);
    if (!g) { free(buf); return SOL_FALSE; }

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

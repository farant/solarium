/* glb.c — glTF binary container + accessor decode. See glb.h. */

#include "glb.h"
#include "json.h"

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

/* Build one primitive's geometry into a Mesh. Pushes vertices in POSITION-accessor
   order (so glTF indices map straight to builder indices), then the triangles. */
static int build_primitive(JsonValue *g, const unsigned char *bin, sol_u32 bin_len,
                           JsonValue *prim, Mesh *out) {
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
        float px = acc_float(&pos, i, 0), py = acc_float(&pos, i, 1), pz = acc_float(&pos, i, 2);
        float nx = 0.0f, ny = 0.0f, nz = 0.0f, u = 0.0f, v = 0.0f;
        if (has_nrm) { nx = acc_float(&nrm, i, 0); ny = acc_float(&nrm, i, 1); nz = acc_float(&nrm, i, 2); }
        if (has_uv)  { u  = acc_float(&uv,  i, 0); v  = acc_float(&uv,  i, 1); }
        mb_push_vertex(&mb, px, py, pz, nx, ny, nz, u, 1.0f - v);   /* glTF UV is top-left -> flip V */
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
    JsonValue           *g, *meshes;
    sol_u32              prim_total, written;

    out->meshes = (Mesh *)0;
    out->mesh_count = 0;

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

    meshes = json_member(g, "meshes");
    prim_total = 0;
    {
        sol_u32 n = json_count(meshes), k;
        for (k = 0; k < n; k++) {
            prim_total += json_count(json_member(json_index(meshes, k), "primitives"));
        }
    }
    if (prim_total == 0) { json_free(g); free(buf); return SOL_FALSE; }

    out->meshes = (Mesh *)malloc((size_t)prim_total * sizeof(Mesh));
    if (!out->meshes) { json_free(g); free(buf); return SOL_FALSE; }

    written = 0;
    {
        sol_u32 n = json_count(meshes), k, p, np;
        for (k = 0; k < n; k++) {
            JsonValue *prims = json_member(json_index(meshes, k), "primitives");
            np = json_count(prims);
            for (p = 0; p < np; p++) {
                Mesh m;
                if (build_primitive(g, bin, bin_len, json_index(prims, p), &m)) {
                    out->meshes[written++] = m;
                }
            }
        }
    }
    out->mesh_count = written;

    json_free(g);
    free(buf);
    return (written > 0) ? SOL_TRUE : SOL_FALSE;
}

void glb_free(GlbModel *out) {
    free(out->meshes);
    out->meshes = (Mesh *)0;
    out->mesh_count = 0;
}

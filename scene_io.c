/* scene_io.c — scene persistence: STML serialization (save and load).
   Above the seam: writes text via stdio, knows the scene schema, never GL.
   References (parent, rel target) are written as the *referenced object's nid*,
   resolved from its runtime handle at write time. Floats use %.9g so a 32-bit
   float round-trips exactly. The writer is a FORM SELECTOR: per value it picks
   the cheapest representation that round-trips (quote choice for attributes,
   capture vs raw for meta text) — STML-native, no character entities, and `&`
   is never touched. See SCENE_FORMAT.md. */

#include "scene.h"
#include "stml.h"
#include "mesh.h"      /* mesh_ref_schema: the registry names the param attrs (item 5) */
#include "texgen.h"    /* texgen_schema: synthesized materials name their knobs the same way */
#include "component.h" /* component_schema: behavior attrs by name (P4 item 6) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ObjectKind <-> its serialized name. Indexed by the enum; KIND_PLAIN is
   never written (absent attribute = plain, which keeps old files byte-stable
   and old readers forward-compatible). */
static const char *KIND_NAMES[] = {
    "plain", "file", "folder", "alias", "note", "tombstone", "portal"
};
#define KIND_COUNT (sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]))

static ObjectKind kind_from_name(const char *s) {
    sol_u32 i;
    for (i = 1; i < KIND_COUNT; i++) {
        if (strcmp(s, KIND_NAMES[i]) == 0) return (ObjectKind)i;
    }
    return KIND_PLAIN;          /* unknown kinds degrade to props, not errors */
}

static void write_vec3(FILE *f, const char *tag, vec3 v) {
    fprintf(f, "    <%s x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" />\n",
            tag, (double)v.x, (double)v.y, (double)v.z);
}

/* Write one attribute as ` name="value"`, selecting the quote that avoids
   escaping: `"` normally, `'` when the value contains `"`, and only when it
   contains BOTH quote characters fall back to `"` with `\"`. A literal
   backslash is always written `\\` (the reader's escape decoding is always
   on — see stml.c). NEVER touches `&` (reserved for &name; references).
   Deterministic — a pure function of the value — which is what keeps
   save -> load -> save byte-identical. */
static void write_attr(FILE *f, const char *name, const char *value) {
    const char *p;
    char        q;

    q = strchr(value, '"') ? (strchr(value, '\'') ? '"' : '\'') : '"';
    fprintf(f, " %s=%c", name, q);
    for (p = value; *p; p++) {
        if (*p == '\\' || *p == q) fputc('\\', f);
        fputc(*p, f);
    }
    fputc(q, f);
}

/* Can `v` ride the normal capture form (<meta key (>v), whose read path trims
   surrounding whitespace and dedents? Only when that read gives back exactly
   `v`: one line, no '<' (the capture terminator), and nothing for the trim to
   eat at either end. Everything else escalates to a raw form. */
static int capture_safe(const char *v) {
    size_t      n = strlen(v);
    const char *p;
    if (n == 0) return 1;
    if (v[0] == ' ' || v[0] == '\t') return 0;
    if (v[n - 1] == ' ' || v[n - 1] == '\t') return 0;
    for (p = v; *p; p++) {
        if (*p == '<' || *p == '\n' || *p == '\r') return 0;
    }
    return 1;
}

/* One meta entry, in the cheapest form that round-trips it (see
   SCENE_FORMAT.md "serialization rules"). Returns 0 only for the raw blind
   spot: a multiline value containing the literal terminator "</". */
static int write_meta(FILE *f, const char *key, const char *value) {
    if (capture_safe(value)) {                    /* trim/dedent is identity */
        fprintf(f, "    <meta");
        write_attr(f, "key", key);
        fprintf(f, " (>%s\n", value);
    } else if (strchr(value, '\n') == NULL) {     /* raw line: verbatim to \n */
        fprintf(f, "    <meta!");
        write_attr(f, "key", key);
        fprintf(f, " (>%s\n", value);
    } else if (strstr(value, "</") == NULL) {     /* raw block, written TIGHT:
                            inside raw every byte is content, so the value butts
                            directly against '>' and '</' — no pretty-printing */
        fprintf(f, "    <meta!");
        write_attr(f, "key", key);
        fprintf(f, ">%s</meta>\n", value);
    } else {
        return 0;
    }
    return 1;
}

sol_bool scene_save(Scene *s, const char *path) {
    FILE   *f;
    sol_u32 i, j;

    f = fopen(path, "w");
    if (!f) return SOL_FALSE;

    fprintf(f, "<scene version=\"1\">\n");
    for (i = 0; i < s->count; i++) {
        SceneObject *o    = &s->objects[i];
        const char  *pnid = "";                       /* root -> parent="" */
        const char  *derived = scene_meta_get(s, o->handle, "derived");
        if (derived && strcmp(derived, "1") == 0) continue;   /* glb parts (6e):
                  regenerated from the anchor's ref on load, never persisted —
                  the file stores arrangement and references, not geometry */
        if (o->parent != 0) {
            SceneObject *par = scene_get(s, o->parent);
            if (par && par->nid) pnid = par->nid;     /* reference BY nid, not handle */
        }
        fprintf(f, "  <object");
        write_attr(f, "nid", o->nid ? o->nid : "");
        write_attr(f, "parent", pnid);
        if (o->kind != KIND_PLAIN && (sol_u32)o->kind < KIND_COUNT)
            write_attr(f, "kind", KIND_NAMES[o->kind]);
        fprintf(f, ">\n");

        write_vec3(f, "pos", o->pos);
        fprintf(f, "    <rot x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" w=\"%.9g\" />\n",
                (double)o->rot.x, (double)o->rot.y, (double)o->rot.z, (double)o->rot.w);
        write_vec3(f, "scale", o->scale);

        if (o->mesh_ref) {
            fprintf(f, "    <mesh");
            write_attr(f, "ref", o->mesh_ref);
            /* parametric ref (item 5): the registry's schema names the
               attributes — <mesh ref="room" w="6" d="4" h="3"/> — so the
               file stays self-describing and the loader fills absent params
               from the same table's defaults. Schema order keeps re-saves
               byte-identical. An unknown ref's params can't be named, so
               they are not written (a documented forward-compat boundary). */
            if (o->mesh_param_count > 0) {
                const char *const *names;
                const float       *defs;
                int n = mesh_ref_schema(o->mesh_ref, &names, &defs);
                int k;
                for (k = 0; k < n && k < o->mesh_param_count; k++)
                    fprintf(f, " %s=\"%.9g\"", names[k], (double)o->mesh_params[k]);
            }
            fprintf(f, " />\n");
        }

        {   /* material (6e): the scalar PBR factors, written when they differ
               from the default. Texture handles are runtime-only — glb parts
               re-derive theirs on re-import; the page rebinds at load. */
            Material d = material_default();
            if (o->material.base_color.x != d.base_color.x ||
                o->material.base_color.y != d.base_color.y ||
                o->material.base_color.z != d.base_color.z ||
                o->material.metallic  != d.metallic ||
                o->material.roughness != d.roughness ||
                o->material.emissive.x != 0.0f ||
                o->material.emissive.y != 0.0f ||
                o->material.emissive.z != 0.0f) {
                fprintf(f, "    <mat r=\"%.9g\" g=\"%.9g\" b=\"%.9g\""
                           " metal=\"%.9g\" rough=\"%.9g\"",
                        (double)o->material.base_color.x,
                        (double)o->material.base_color.y,
                        (double)o->material.base_color.z,
                        (double)o->material.metallic,
                        (double)o->material.roughness);
                /* emissive attrs only when something glows (P4 item 5):
                   absent = zero, so old files stay byte-identical */
                if (o->material.emissive.x != 0.0f ||
                    o->material.emissive.y != 0.0f ||
                    o->material.emissive.z != 0.0f)
                    fprintf(f, " er=\"%.9g\" eg=\"%.9g\" eb=\"%.9g\"",
                            (double)o->material.emissive.x,
                            (double)o->material.emissive.y,
                            (double)o->material.emissive.z);
                fprintf(f, " />\n");
            }
        }

        if (o->tex_ref) {   /* synthesized material (texture side-quest):
               its OWN element, the <mesh ref=...> pattern — the knob names
               must not share <mat>'s attr namespace ("rough" collides).
               The file's knob PREFIX only — absent knobs stay the kind's,
               so old files survive a schema growing. An unknown kind's
               knobs can't be named (the documented component edge). */
            int kind = texgen_kind(o->tex_ref);
            fprintf(f, "    <tex");
            write_attr(f, "kind", o->tex_ref);
            if (kind >= 0) {
                const char *const *tn;
                int tnum = texgen_schema(kind, &tn, (const float **)0);
                int k;
                for (k = 0; k < o->tex_param_count && k < tnum; k++)
                    fprintf(f, " %s=\"%.9g\"", tn[k], (double)o->tex_params[k]);
            }
            fprintf(f, " />\n");
        }

        for (j = 0; j < o->comp_count; j++) {  /* components (P4 item 6): the
                                                  behavior, as data — named
                                                  attrs from the schema, the
                                                  file's own param PREFIX only.
                                                  An unknown type writes bare
                                                  (its params have no names we
                                                  know — the documented edge). */
            const Component   *c = &o->components[j];
            const char *const *names;
            int n, k;
            n = component_schema(c->type, &names, (const float **)0);
            fprintf(f, "    <component");
            write_attr(f, "type", c->type);
            if (n > 0) {
                for (k = 0; k < c->param_count && k < n; k++)
                    fprintf(f, " %s=\"%.9g\"", names[k], (double)c->params[k]);
            }
            fprintf(f, " />\n");
        }

        for (j = 0; j < o->meta_count; j++) {
            if (!write_meta(f, o->meta[j].key, o->meta[j].value)) {
                fclose(f);                    /* unrepresentable value: fail loud */
                remove(path);                 /* and leave no truncated file behind */
                return SOL_FALSE;
            }
        }

        for (j = 0; j < o->rel_count; j++) {
            SceneObject *tgt = scene_get(s, o->relations[j].target);
            fprintf(f, "    <rel");
            write_attr(f, "type", o->relations[j].type);
            write_attr(f, "target", (tgt && tgt->nid) ? tgt->nid : "");
            fprintf(f, " />\n");
        }

        if (o->content) {
            fprintf(f, "    <content");
            write_attr(f, "path", o->content);
            fprintf(f, " />\n");
        }

        fprintf(f, "  </object>\n");
    }
    fprintf(f, "</scene>\n");

    fclose(f);
    return SOL_TRUE;
}

/* ----------------------------------------------------------------- loading */

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1;
    char  *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Read the whole file into a NUL-terminated buffer (binary mode: an exact byte
   count, no newline translation). Caller frees. The file I/O deferred from the
   parser lives here, where load actually needs it. */
static char *slurp_file(const char *path) {
    FILE  *f;
    long   size;
    char  *buf;
    size_t got;

    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    rewind(f);

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* One float attribute of a child element, or the default if the element or
   attribute is absent (gives identity-transform defaults for missing parts). */
static float attr_f(StmlNode *n, const char *key, float dflt) {
    const char *s;
    if (!n) return dflt;
    s = stml_attr(n, key);
    return s ? (float)strtod(s, (char **)0) : dflt;
}

static vec3 read_vec3(StmlNode *obj, const char *tag, float dflt) {
    StmlNode *n = stml_child(obj, tag);
    vec3      v;
    v.x = attr_f(n, "x", dflt);
    v.y = attr_f(n, "y", dflt);
    v.z = attr_f(n, "z", dflt);
    return v;
}

static quat read_quat(StmlNode *obj, const char *tag) {
    StmlNode *n = stml_child(obj, tag);
    quat      q;
    q.x = attr_f(n, "x", 0.0f);
    q.y = attr_f(n, "y", 0.0f);
    q.z = attr_f(n, "z", 0.0f);
    q.w = attr_f(n, "w", 1.0f);          /* absent rotation -> identity */
    return q;
}

sol_bool scene_load(Scene *s, const char *path) {
    char     *buf;
    StmlNode *root, *sn;
    sol_u32   i;
    Mesh      empty;

    buf = slurp_file(path);
    if (!buf) return SOL_FALSE;
    root = stml_parse(buf);
    free(buf);
    if (!root) return SOL_FALSE;

    sn = stml_child(root, "scene");
    if (!sn) { stml_free(root); return SOL_FALSE; }

    scene_init(s);
    empty.vbuffer.id = 0; empty.ibuffer.id = 0; empty.index_count = 0;

    /* Pass 1: create every object with its file nid + self-contained data. */
    for (i = 0; i < sn->child_count; i++) {
        StmlNode   *on = sn->children[i];
        StmlNode   *mn;
        const char *nid, *mref;
        sol_u32     h, j;

        if (!on->tag || strcmp(on->tag, "object") != 0) continue;   /* skip foreign tags */

        h = scene_add(s, 0, empty,
                      read_vec3(on, "pos",   0.0f),
                      read_quat(on, "rot"),
                      read_vec3(on, "scale", 1.0f));

        nid = stml_attr(on, "nid");
        if (nid) {
            SceneObject *o = scene_get(s, h);
            free(o->nid);                       /* drop the freshly-minted one */
            o->nid = dup_cstr(nid);             /* keep the file's identity */
        }
        {
            const char *ks = stml_attr(on, "kind");
            if (ks) scene_kind_set(s, h, kind_from_name(ks));
        }

        mn   = stml_child(on, "mesh");
        mref = mn ? stml_attr(mn, "ref") : (const char *)0;
        if (mref) {
            scene_mesh_ref_set(s, h, mref);
            {   /* parametric ref: read the schema-named attrs, defaults for
                   absent ones. Store only the file's own PREFIX length (the
                   last present attr) — storing the full schema count would
                   make a re-save write attrs the file never had, breaking
                   byte-identity whenever a schema grows. */
                const char *const *names;
                const float       *defs;
                int n = mesh_ref_schema(mref, &names, &defs);
                if (n > 0) {
                    float p[MESH_REF_MAX_PARAMS];
                    int   k, last = -1;
                    for (k = 0; k < n; k++) {
                        p[k] = attr_f(mn, names[k], defs[k]);
                        if (stml_attr(mn, names[k])) last = k;
                    }
                    if (last >= 0) scene_mesh_params_set(s, h, p, last + 1);
                }
            }
        }

        for (j = 0; j < on->child_count; j++) {                     /* meta */
            StmlNode *c = on->children[j];
            if (c->tag && strcmp(c->tag, "meta") == 0) {
                const char *k = stml_attr(c, "key");
                if (k) scene_meta_set(s, h, k, c->text ? c->text : "");
            }
        }

        {                                                          /* content */
            StmlNode   *cn = stml_child(on, "content");
            const char *p  = cn ? stml_attr(cn, "path") : (const char *)0;
            if (p) scene_content_set(s, h, p);
        }

        {                                                          /* material (6e) */
            StmlNode *mat = stml_child(on, "mat");
            if (mat) {
                Material mm  = material_default();
                mm.base_color.x = attr_f(mat, "r", mm.base_color.x);
                mm.base_color.y = attr_f(mat, "g", mm.base_color.y);
                mm.base_color.z = attr_f(mat, "b", mm.base_color.z);
                mm.metallic     = attr_f(mat, "metal", mm.metallic);
                mm.roughness    = attr_f(mat, "rough", mm.roughness);
                mm.emissive.x   = attr_f(mat, "er", 0.0f);
                mm.emissive.y   = attr_f(mat, "eg", 0.0f);
                mm.emissive.z   = attr_f(mat, "eb", 0.0f);
                scene_material_set(s, h, mm);
            }
        }

        {   /* synthesized material (texture side-quest): kind + knob
               PREFIX in its own element — the mesh-ref reader's shape */
            StmlNode   *tn0  = stml_child(on, "tex");
            const char *tref = tn0 ? stml_attr(tn0, "kind") : (const char *)0;
            if (tref) {
                int kind = texgen_kind(tref);
                scene_tex_ref_set(s, h, tref);
                if (kind >= 0) {
                    const char *const *tn;
                    const float       *td;
                    int tnum = texgen_schema(kind, &tn, &td);
                    float tp[TEX_REF_MAX_PARAMS];
                    int   k, last = -1;
                    for (k = 0; k < tnum && k < TEX_REF_MAX_PARAMS; k++) {
                        tp[k] = attr_f(tn0, tn[k], td[k]);
                        if (stml_attr(tn0, tn[k])) last = k;
                    }
                    if (last >= 0)
                        scene_tex_params_set(s, h, tp, last + 1);
                }
            }
        }

        for (j = 0; j < on->child_count; j++) {        /* components (P4 item 6) */
            StmlNode   *c = on->children[j];
            const char *ty;
            if (!c->tag || strcmp(c->tag, "component") != 0) continue;
            ty = stml_attr(c, "type");
            if (!ty) continue;
            {
                const char *const *names;
                int   n = component_schema(ty, &names, (const float **)0);
                float ps[COMPONENT_MAX_PARAMS];
                int   k, cnt = 0;
                if (n > 0) {
                    for (k = 0; k < n; k++) {          /* the PREFIX rule: stop
                                                          at the first absent
                                                          attr, defaults fill
                                                          the rest at update */
                        const char *v = stml_attr(c, names[k]);
                        if (!v) break;
                        ps[k] = (float)atof(v);
                        cnt = k + 1;
                    }
                }
                scene_component_add(s, h, ty, ps, cnt);
            }
        }
    }

    /* Pass 2: now every nid exists, resolve parent + rel target references. */
    for (i = 0; i < sn->child_count; i++) {
        StmlNode   *on = sn->children[i];
        const char *nid, *pnid;
        sol_u32     h, j;

        if (!on->tag || strcmp(on->tag, "object") != 0) continue;
        nid = stml_attr(on, "nid");
        h   = nid ? scene_handle_for_nid(s, nid) : 0;
        if (!h) continue;

        pnid = stml_attr(on, "parent");
        if (pnid && pnid[0] != '\0') {
            scene_get(s, h)->parent = scene_handle_for_nid(s, pnid);   /* 0 if absent -> root */
        }

        for (j = 0; j < on->child_count; j++) {                    /* rel */
            StmlNode *c = on->children[j];
            if (c->tag && strcmp(c->tag, "rel") == 0) {
                const char *ty = stml_attr(c, "type");
                const char *tn = stml_attr(c, "target");
                if (ty) scene_rel_add(s, h, ty, tn ? scene_handle_for_nid(s, tn) : 0);
            }
        }
    }

    stml_free(root);
    return SOL_TRUE;
}

/* scene_io.c — scene persistence: STML serialization (save here; load next).
   Above the seam: writes text via stdio, knows the scene schema, never GL.
   References (parent, rel target) are written as the *referenced object's nid*,
   resolved from its runtime handle at write time. Floats use %.9g so a 32-bit
   float round-trips exactly. See SCENE_FORMAT.md. */

#include "scene.h"
#include "stml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_vec3(FILE *f, const char *tag, vec3 v) {
    fprintf(f, "    <%s x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" />\n",
            tag, (double)v.x, (double)v.y, (double)v.z);
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
        if (o->parent != 0) {
            SceneObject *par = scene_get(s, o->parent);
            if (par && par->nid) pnid = par->nid;     /* reference BY nid, not handle */
        }
        fprintf(f, "  <object nid=\"%s\" parent=\"%s\">\n",
                o->nid ? o->nid : "", pnid);

        write_vec3(f, "pos", o->pos);
        fprintf(f, "    <rot x=\"%.9g\" y=\"%.9g\" z=\"%.9g\" w=\"%.9g\" />\n",
                (double)o->rot.x, (double)o->rot.y, (double)o->rot.z, (double)o->rot.w);
        write_vec3(f, "scale", o->scale);

        if (o->mesh_ref) fprintf(f, "    <mesh ref=\"%s\" />\n", o->mesh_ref);

        for (j = 0; j < o->meta_count; j++)            /* capture form: value after (> */
            fprintf(f, "    <meta key=\"%s\" (>%s\n", o->meta[j].key, o->meta[j].value);

        for (j = 0; j < o->rel_count; j++) {
            SceneObject *tgt = scene_get(s, o->relations[j].target);
            fprintf(f, "    <rel type=\"%s\" target=\"%s\" />\n",
                    o->relations[j].type, (tgt && tgt->nid) ? tgt->nid : "");
        }

        if (o->content) fprintf(f, "    <content path=\"%s\" />\n", o->content);

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

        mn   = stml_child(on, "mesh");
        mref = mn ? stml_attr(mn, "ref") : (const char *)0;
        if (mref) scene_mesh_ref_set(s, h, mref);

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

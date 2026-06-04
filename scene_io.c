/* scene_io.c — scene persistence: STML serialization (save here; load next).
   Above the seam: writes text via stdio, knows the scene schema, never GL.
   References (parent, rel target) are written as the *referenced object's nid*,
   resolved from its runtime handle at write time. Floats use %.9g so a 32-bit
   float round-trips exactly. See SCENE_FORMAT.md. */

#include "scene.h"

#include <stdio.h>

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

/* obj.c — minimal OBJ loader (extracted from main.c). Pure CPU, no GL.
   Emits the canonical 8-float vertex layout (uv = 0; OBJ here has no texcoords). */

#include "obj.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- a growable float array (doubles capacity on demand) --- */
typedef struct { float *data; size_t count, capacity; } FloatArray;

static void fa_push(FloatArray *a, float v) {
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;   /* double */
        a->data = realloc(a->data, a->capacity * sizeof(float));
    }
    a->data[a->count++] = v;
}

/* Append one OBJ corner (1-based position p, normal n) as 8 floats:
   position, normal, then uv = (0,0). */
static void push_vertex(FloatArray *out, const FloatArray *pos, const FloatArray *nrm,
                        int p, int n) {
    int pi = (p - 1) * 3;                       /* 1-based -> 0-based */
    fa_push(out, pos->data[pi+0]);
    fa_push(out, pos->data[pi+1]);
    fa_push(out, pos->data[pi+2]);
    if (n > 0) {
        int ni = (n - 1) * 3;
        fa_push(out, nrm->data[ni+0]);
        fa_push(out, nrm->data[ni+1]);
        fa_push(out, nrm->data[ni+2]);
    } else {                                    /* no normal -> zero */
        fa_push(out, 0); fa_push(out, 0); fa_push(out, 0);
    }
    fa_push(out, 0.0f);   /* u */
    fa_push(out, 0.0f);   /* v */
}

float *obj_load(const char *path, int *out_count) {
    FILE *f = fopen(path, "r");
    FloatArray positions = {0}, normals = {0}, out = {0};
    char line[256];

    if (!f) { fprintf(stderr, "obj_load: cannot open %s\n", path); return NULL; }

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == 'v' && line[1] == ' ') {                 /* position */
            float x, y, z;
            sscanf(line, "v %f %f %f", &x, &y, &z);
            fa_push(&positions, x); fa_push(&positions, y); fa_push(&positions, z);
        } else if (line[0] == 'v' && line[1] == 'n') {          /* normal */
            float x, y, z;
            sscanf(line, "vn %f %f %f", &x, &y, &z);
            fa_push(&normals, x); fa_push(&normals, y); fa_push(&normals, z);
        } else if (line[0] == 'f' && line[1] == ' ') {   /* face: tri / quad / n-gon */
            int cp[16], cn[16], nc = 0;                  /* per-corner pos/normal idx */
            char *tok = strtok(line + 1, " \t\r\n");
            int i;
            while (tok && nc < 16) {
                int p = 0, t = 0, n = 0;
                if      (sscanf(tok, "%d/%d/%d", &p, &t, &n) == 3) {}   /* p/t/n */
                else if (sscanf(tok, "%d//%d",  &p, &n)      == 2) {}   /* p//n  */
                else if (sscanf(tok, "%d/%d",   &p, &t)      == 2) {}   /* p/t   */
                else     sscanf(tok, "%d", &p);                        /* p     */
                cp[nc] = p; cn[nc] = n; nc++;
                tok = strtok(NULL, " \t\r\n");
            }
            /* fan-triangulate: (0,1,2), (0,2,3), ... handles tris and quads */
            for (i = 2; i < nc; i++) {
                push_vertex(&out, &positions, &normals, cp[0],   cn[0]);
                push_vertex(&out, &positions, &normals, cp[i-1], cn[i-1]);
                push_vertex(&out, &positions, &normals, cp[i],   cn[i]);
            }
        }
        /* comments / blank / o,g,usemtl,s -> skipped */
    }
    fclose(f);

    free(positions.data);   /* scaffolding — done with it */
    free(normals.data);

    /* recenter on the bounding-box center so the mesh rotates in place,
       regardless of where it was authored in model space (8 floats/vertex) */
    if (out.count >= 8) {
        float lo[3], hi[3], center[3];
        size_t v;
        int k;
        for (k = 0; k < 3; k++) { lo[k] = out.data[k]; hi[k] = out.data[k]; }
        for (v = 0; v < out.count; v += 8) {     /* positions are first 3 of 8 */
            for (k = 0; k < 3; k++) {
                float c = out.data[v + k];
                if (c < lo[k]) lo[k] = c;
                if (c > hi[k]) hi[k] = c;
            }
        }
        center[0] = (lo[0]+hi[0])*0.5f;
        center[1] = (lo[1]+hi[1])*0.5f;
        center[2] = (lo[2]+hi[2])*0.5f;
        for (v = 0; v < out.count; v += 8)
            for (k = 0; k < 3; k++) out.data[v + k] -= center[k];
    }

    *out_count = (int)(out.count / 8);   /* 8 floats per vertex */
    return out.data;
}

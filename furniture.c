/* furniture.c — see furniture.h. */
#include "furniture.h"
#include <string.h>
#include <math.h>

static const char *const FURN_CATALOG[] = { "table", "bookshelf" };
#define FURN_COUNT ((int)(sizeof FURN_CATALOG / sizeof FURN_CATALOG[0]))

int furniture_catalog_count(void) { return FURN_COUNT; }

const char *furniture_catalog_name(int i) {
    if (i < 0 || i >= FURN_COUNT) return (const char *)0;
    return FURN_CATALOG[i];
}

int furniture_catalog_cycle(int i, int dir) {
    int n = FURN_COUNT;
    return ((i + dir) % n + n) % n;
}

sol_bool furniture_is_table(const char *mesh_ref) {
    return (sol_bool)(mesh_ref && strcmp(mesh_ref, "table") == 0);
}

sol_bool furniture_is_shelf(const char *mesh_ref) {
    return (sol_bool)(mesh_ref && strcmp(mesh_ref, "bookshelf") == 0);
}

#define FURN_SPINE_PITCH  0.06f
#define FURN_SHELF_MARGIN 0.06f
#define FURN_PANEL_T      0.04f

/* columns that fit across a shelf of width w */
static int furn_shelf_cols(float w) {
    int c = (int)((w - 2.0f * FURN_SHELF_MARGIN) / FURN_SPINE_PITCH);
    return c < 1 ? 1 : c;
}

vec3 furniture_shelf_slot(const float *params, int count, int i) {
    float w  = (count > 0) ? params[0] : 1.0f;
    float h  = (count > 1) ? params[1] : 1.8f;
    float d  = (count > 2) ? params[2] : 0.3f;
    int   sh = (count > 3) ? (int)(params[3] + 0.5f) : 4;
    int   cols = furn_shelf_cols(w);
    int   col, row;
    float x0 = -w * 0.5f + FURN_SHELF_MARGIN + FURN_SPINE_PITCH * 0.5f;
    vec3  s;
    if (sh < 1) sh = 1;
    if (i < 0) i = 0;
    col = i % cols;
    row = (i / cols) % sh;                 /* wrap within the shelf rows (cap) */
    s.x = x0 + (float)col * FURN_SPINE_PITCH;
    /* shelf board k=row+1 sits at this y (matches emit_bookshelf); a spine
       stands ON it. Top shelf (row 0) gets the HIGHEST y, so indices fill the
       top shelf first then descend (sh - row). */
    s.y = (h - FURN_PANEL_T) * (float)(sh - row) / (float)(sh + 1) + FURN_PANEL_T;
    s.z = d * 0.5f - 0.04f;                 /* just inside the front opening */
    return s;
}

/* furniture_table_point / furniture_surface_aim land in Tasks 4-5; leave them
   out until then (their tests arrive with them). */

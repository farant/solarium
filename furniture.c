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

int furniture_shelf_capacity(const float *params, int count) {
    float w  = (count > 0) ? params[0] : 1.0f;
    int   sh = (count > 3) ? (int)(params[3] + 0.5f) : 4;
    if (sh < 1) sh = 1;
    return furn_shelf_cols(w) * sh;
}

vec3 furniture_table_point(const float *params, int count, vec3 local_hit) {
    float w  = (count > 0) ? params[0] : 1.4f;
    float d  = (count > 1) ? params[1] : 0.9f;
    float h  = (count > 2) ? params[2] : 0.75f;
    float hw = w * 0.5f - 0.05f, hd = d * 0.5f - 0.05f;   /* keep a small inset */
    vec3  q  = local_hit;
    if (hw < 0.0f) hw = 0.0f;
    if (hd < 0.0f) hd = 0.0f;
    if (q.x >  hw) q.x =  hw;  if (q.x < -hw) q.x = -hw;
    if (q.z >  hd) q.z =  hd;  if (q.z < -hd) q.z = -hd;
    q.y = h;                                              /* on the top */
    return q;
}

/* rotate a vector by -yaw about +Y, i.e. world->local for a furniture placed
   with quat_from_axis_angle(+Y, yaw): the matrix [c -s; s c] with c=cos(yaw),
   s=sin(yaw) is exactly R_y(yaw)^-1 = R_y(-yaw). */
static vec3 furn_unrotate_y(vec3 v, float yaw) {
    float c = (float)cos((double)yaw), s = (float)sin((double)yaw);
    vec3  r;
    r.x = v.x * c - v.z * s;
    r.y = v.y;
    r.z = v.x * s + v.z * c;
    return r;
}

sol_bool furniture_surface_aim(const char *mesh_ref, const float *params, int count,
                               vec3 fpos, float fyaw, Ray ray, vec3 *out_local) {
    Ray   lr;
    float t;
    vec3  pt, pnorm, hit;
    /* world ray -> furniture local (translate then un-yaw) */
    lr.origin = furn_unrotate_y(vec3_sub(ray.origin, fpos), fyaw);
    lr.dir    = furn_unrotate_y(ray.dir, fyaw);
    if (furniture_is_table(mesh_ref)) {
        float w = (count > 0) ? params[0] : 1.4f;
        float d = (count > 1) ? params[1] : 0.9f;
        float h = (count > 2) ? params[2] : 0.75f;
        pt    = vec3_make(0.0f, h, 0.0f);
        pnorm = vec3_make(0.0f, 1.0f, 0.0f);
        if (!ray_vs_plane(lr, pt, pnorm, &t) || t <= 0.0f) return SOL_FALSE;
        hit = vec3_add(lr.origin, vec3_scale(lr.dir, t));
        if (hit.x < -w*0.5f || hit.x > w*0.5f || hit.z < -d*0.5f || hit.z > d*0.5f)
            return SOL_FALSE;
        *out_local = hit;
        return SOL_TRUE;
    }
    if (furniture_is_shelf(mesh_ref)) {
        float w = (count > 0) ? params[0] : 1.0f;
        float h = (count > 1) ? params[1] : 1.8f;
        float d = (count > 2) ? params[2] : 0.3f;
        pt    = vec3_make(0.0f, 0.0f, d * 0.5f);
        pnorm = vec3_make(0.0f, 0.0f, 1.0f);
        if (!ray_vs_plane(lr, pt, pnorm, &t) || t <= 0.0f) return SOL_FALSE;
        hit = vec3_add(lr.origin, vec3_scale(lr.dir, t));
        if (hit.x < -w*0.5f || hit.x > w*0.5f || hit.y < 0.0f || hit.y > h)
            return SOL_FALSE;
        *out_local = hit;
        return SOL_TRUE;
    }
    return SOL_FALSE;
}

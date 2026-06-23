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

#define FURN_SHELF_MARGIN 0.06f
#define FURN_PANEL_T      0.04f
#define FURN_SHELF_GAP    0.012f   /* the breathing room between packed spines */

int furniture_shelf_layout(const float *params, int count,
                           const float *widths, int n,
                           float *out_x, int *out_row) {
    float w      = (count > 0) ? params[0] : 1.0f;
    int   sh     = (count > 3) ? (int)(params[3] + 0.5f) : 2;
    float usable = w - 2.0f * FURN_SHELF_MARGIN;
    float left   = -w * 0.5f + FURN_SHELF_MARGIN;
    float cursor = 0.0f;                    /* distance from `left` along the row */
    int   row = 0, rows, i;
    if (sh < 1) sh = 1;
    rows = sh + 1;
    for (i = 0; i < n; i++) {
        float bw = (widths[i] > 0.0f) ? widths[i] : 0.0f;
        if (cursor > 0.0f && cursor + bw > usable) {   /* won't fit: next row */
            cursor = 0.0f;
            if (row < rows - 1) row++;                 /* clamp; overflow piles on last */
        }
        out_x[i]   = left + cursor + bw * 0.5f;
        out_row[i] = row;
        cursor += bw + FURN_SHELF_GAP;
    }
    return (n > 0) ? row + 1 : 0;
}

float furniture_shelf_row_y(const float *params, int count, int row) {
    float h  = (count > 1) ? params[1] : 1.8f;
    int   sh = (count > 3) ? (int)(params[3] + 0.5f) : 2;
    if (sh < 1) sh = 1;
    if (row < 0)  row = 0;
    if (row > sh) row = sh;
    return (h - FURN_PANEL_T) * (float)(sh - row) / (float)(sh + 1) + FURN_PANEL_T;
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

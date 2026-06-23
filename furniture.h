/* furniture.h — placeable furniture geometry (bookshelves + tables). SCENE-FREE
   pure math: the catalog, kind predicates, shelf-slot layout, table point, and
   the surface-aim hit test. No GL, no scene graph — main.c owns the glue. */
#ifndef SOL_FURNITURE_H
#define SOL_FURNITURE_H

#include "sol_base.h"   /* sol_bool */
#include "sol_math.h"   /* vec3, Ray */

/* the placeable catalog (v1: table, bookshelf), index-ordered for cycling. */
int         furniture_catalog_count(void);
const char *furniture_catalog_name(int i);          /* NULL if out of range */
int         furniture_catalog_cycle(int i, int dir);/* +/-1, wraps */

sol_bool    furniture_is_table(const char *mesh_ref);
sol_bool    furniture_is_shelf(const char *mesh_ref);

/* the i-th spine's LOCAL position on a bookshelf (fills a shelf left-to-right,
   then the next shelf down). `params` = the bookshelf mesh_params. */
vec3 furniture_shelf_slot(const float *params, int count, int i);

/* how many distinct spine slots a bookshelf holds (cols * shelves) before
   furniture_shelf_slot wraps and re-uses positions. Caller scans 0..capacity-1
   to find the lowest free slot when filing. */
int  furniture_shelf_capacity(const float *params, int count);

/* Pack n items of along-shelf widths widths[0..n) left-to-right across the
   bookshelf, each followed by a small gap, wrapping to the next row (0 = top
   board ... sh = floor board) when an item won't fit the remaining row width.
   Fills out_x[i] (LOCAL x of the item's CENTRE) and out_row[i]. An item wider
   than a whole row still gets placed (its own row). Returns the rows used. */
int   furniture_shelf_layout(const float *params, int count,
                             const float *widths, int n,
                             float *out_x, int *out_row);

/* the LOCAL y of a shelf row's board top (row 0 = top ... sh = floor), so a
   filed item's base can sit on it. */
float furniture_shelf_row_y(const float *params, int count, int row);

/* a tablet's LOCAL resting position on a table top given a LOCAL hit point on
   the top surface (clamped inside the top; y = top height). */
vec3 furniture_table_point(const float *params, int count, vec3 local_hit);

/* camera `ray` (WORLD) vs the furniture's filing surface (table top / shelf
   front), given the furniture's world position `fpos` + yaw `fyaw` + params.
   SOL_TRUE on hit; *out_local = the hit point in furniture LOCAL space. */
sol_bool furniture_surface_aim(const char *mesh_ref, const float *params, int count,
                               vec3 fpos, float fyaw, Ray ray, vec3 *out_local);

#endif /* SOL_FURNITURE_H */

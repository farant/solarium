#ifndef SOL_MULTISELECT_H
#define SOL_MULTISELECT_H
#include "sol_base.h"

#define MULTISEL_CAP 256            /* max cards in one selection */

/* AABB overlap (Finder "touch"): true if rect A and rect B overlap, edges
   inclusive. Corners may be given in any order (normalized internally). */
sol_bool msel_rect_overlap(float ax0, float ay0, float ax1, float ay1,
                           float bx0, float by0, float bx1, float by1);

/* Set ops on a handle array (len entries, cap capacity), stable order. */
sol_bool msel_contains(const sol_u32 *set, int len, sol_u32 h);
void     msel_add(sol_u32 *set, int *len, int cap, sol_u32 h);    /* no-op if present/full */
void     msel_remove(sol_u32 *set, int *len, sol_u32 h);          /* compacts; no-op if absent */
/* returns new present-state; if at cap and h absent, SOL_FALSE also means 'couldn't add' */
sol_bool msel_toggle(sol_u32 *set, int *len, int cap, sol_u32 h);

#endif

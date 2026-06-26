#include "multiselect.h"

sol_bool msel_rect_overlap(float ax0, float ay0, float ax1, float ay1,
                           float bx0, float by0, float bx1, float by1) {
    float axlo = ax0 < ax1 ? ax0 : ax1, axhi = ax0 < ax1 ? ax1 : ax0;
    float aylo = ay0 < ay1 ? ay0 : ay1, ayhi = ay0 < ay1 ? ay1 : ay0;
    float bxlo = bx0 < bx1 ? bx0 : bx1, bxhi = bx0 < bx1 ? bx1 : bx0;
    float bylo = by0 < by1 ? by0 : by1, byhi = by0 < by1 ? by1 : by0;
    if (axhi < bxlo || bxhi < axlo) return SOL_FALSE;
    if (ayhi < bylo || byhi < aylo) return SOL_FALSE;
    return SOL_TRUE;
}

sol_bool msel_contains(const sol_u32 *set, int len, sol_u32 h) {
    int i;
    for (i = 0; i < len; i++) if (set[i] == h) return SOL_TRUE;
    return SOL_FALSE;
}

void msel_add(sol_u32 *set, int *len, int cap, sol_u32 h) {
    if (msel_contains(set, *len, h) || *len >= cap) return;
    set[(*len)++] = h;
}

void msel_remove(sol_u32 *set, int *len, sol_u32 h) {
    int i, j = 0;
    for (i = 0; i < *len; i++) if (set[i] != h) set[j++] = set[i];
    *len = j;
}

sol_bool msel_toggle(sol_u32 *set, int *len, int cap, sol_u32 h) {
    if (msel_contains(set, *len, h)) { msel_remove(set, len, h); return SOL_FALSE; }
    msel_add(set, len, cap, h);
    return msel_contains(set, *len, h);
}

/* inventory.c — see inventory.h. Pure layout math, framebuffer pixels. */
#include "inventory.h"

/* the grid occupies a centred region inset from the screen edges; cells are
   evenly spaced with a small gap. These fractions are the only "look" knobs. */
#define INV_MX_FRAC   0.12f   /* left/right outer margin, fraction of width  */
#define INV_MY_FRAC   0.14f   /* top/bottom outer margin, fraction of height */
#define INV_GAP_FRAC  0.015f  /* gap between cells, fraction of width        */

int inv_page_count(int n_items, int per_page) {
    if (per_page < 1) per_page = 1;
    if (n_items <= 0) return 1;
    return (n_items + per_page - 1) / per_page;
}

int inv_clamp_page(int page, int n_items, int per_page) {
    int last = inv_page_count(n_items, per_page) - 1;
    if (page < 0)    page = 0;
    if (page > last) page = last;
    return page;
}

void inv_cell_rect(int slot, int cols, int rows, int sw, int sh,
                   float *x, float *y, float *w, float *h) {
    float fw = (float)sw, fh = (float)sh;
    float mx = fw * INV_MX_FRAC, my = fh * INV_MY_FRAC;
    float gap = fw * INV_GAP_FRAC;
    float gw = fw - 2.0f * mx, gh = fh - 2.0f * my;
    float cw = (cols > 0) ? (gw - (float)(cols - 1) * gap) / (float)cols : gw;
    float ch = (rows > 0) ? (gh - (float)(rows - 1) * gap) / (float)rows : gh;
    int   col, row;
    if (slot < 0 || cols < 1 || rows < 1 || slot >= cols * rows) {
        *x = *y = *w = *h = 0.0f;
        return;
    }
    col = slot % cols;
    row = slot / cols;
    *x = mx + (float)col * (cw + gap);
    *y = my + (float)row * (ch + gap);
    *w = cw;
    *h = ch;
}

int inv_hit_slot(float px, float py, int cols, int rows, int sw, int sh) {
    int slot, n = cols * rows;
    for (slot = 0; slot < n; slot++) {
        float x, y, w, h;
        inv_cell_rect(slot, cols, rows, sw, sh, &x, &y, &w, &h);
        if (px >= x && px <= x + w && py >= y && py <= y + h)
            return slot;
    }
    return -1;
}

void inv_prev_rect(int sw, int sh, float *x, float *y, float *w, float *h) {
    float fw = (float)sw, fh = (float)sh;
    *w = fw * 0.04f; *h = fh * 0.04f;
    *x = fw * 0.42f - *w * 0.5f;
    *y = fh * 0.93f - *h * 0.5f;
}

void inv_next_rect(int sw, int sh, float *x, float *y, float *w, float *h) {
    float fw = (float)sw, fh = (float)sh;
    *w = fw * 0.04f; *h = fh * 0.04f;
    *x = fw * 0.58f - *w * 0.5f;
    *y = fh * 0.93f - *h * 0.5f;
}

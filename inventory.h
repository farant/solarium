/* inventory.h — the inventory grid's pure layout math. SCENE-FREE: page
   counts, cell rectangles, click hit-testing, the page-arrow rects. No GL,
   no scene graph — main.c owns the bag, the items, and the drawing. All
   coordinates are FRAMEBUFFER PIXELS, origin top-left, y-down (the ui.h
   convention). */
#ifndef SOL_INVENTORY_H
#define SOL_INVENTORY_H

#define INV_COLS     4
#define INV_ROWS     3
#define INV_PER_PAGE (INV_COLS * INV_ROWS)

/* number of pages for n items (always at least 1). */
int inv_page_count(int n_items, int per_page);

/* clamp a page index into [0, page_count-1]. */
int inv_clamp_page(int page, int n_items, int per_page);

/* the pixel rect of grid cell `slot` (0..INV_PER_PAGE-1) on a screen of
   sw x sh, laid out cols x rows. Fills x,y,w,h. Out-of-range slot -> a zero
   rect. */
void inv_cell_rect(int slot, int cols, int rows, int sw, int sh,
                   float *x, float *y, float *w, float *h);

/* which slot (0..cols*rows-1) does pixel (px,py) fall in? -1 if none (a gap
   or a margin). The caller maps slot -> item via page*per_page + slot. */
int inv_hit_slot(float px, float py, int cols, int rows, int sw, int sh);

/* the previous / next page-arrow rects (for click hit-testing). */
void inv_prev_rect(int sw, int sh, float *x, float *y, float *w, float *h);
void inv_next_rect(int sw, int sh, float *x, float *y, float *w, float *h);

#endif /* SOL_INVENTORY_H */

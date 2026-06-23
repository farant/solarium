#include "inventory.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* page counts: empty still shows one page; exact fills don't spill. */
    CHECK(inv_page_count(0, 12)  == 1);
    CHECK(inv_page_count(12, 12) == 1);
    CHECK(inv_page_count(13, 12) == 2);
    CHECK(inv_page_count(24, 12) == 2);
    CHECK(inv_page_count(25, 12) == 3);

    /* clamp into [0, pages-1]. */
    CHECK(inv_clamp_page(5,  13, 12) == 1);   /* 2 pages -> max index 1 */
    CHECK(inv_clamp_page(-1, 13, 12) == 0);
    CHECK(inv_clamp_page(0,   0, 12) == 0);

    /* cell rects: grid order, on-screen, non-overlapping. */
    {
        float x0,y0,w0,h0, x3,y3,w3,h3, x4,y4,w4,h4;
        inv_cell_rect(0, INV_COLS, INV_ROWS, 1920, 1080, &x0,&y0,&w0,&h0);
        inv_cell_rect(3, INV_COLS, INV_ROWS, 1920, 1080, &x3,&y3,&w3,&h3);
        inv_cell_rect(4, INV_COLS, INV_ROWS, 1920, 1080, &x4,&y4,&w4,&h4);
        CHECK(w0 > 0.0f && h0 > 0.0f);
        CHECK(x0 >= 0.0f && y0 >= 0.0f && x0 + w0 <= 1920.0f && y0 + h0 <= 1080.0f);
        CHECK(x3 > x0 && fabs((double)(y3 - y0)) < 1e-3);   /* slot 3 = same row, further right */
        CHECK(y4 > y0 && fabs((double)(x4 - x0)) < 1e-3);   /* slot 4 = next row, back to left col */
        CHECK(x0 + w0 <= x3 + 1e-3f);                       /* no horizontal overlap within a row */
    }

    /* hit-test: a point at a cell centre returns that slot; a margin point misses. */
    {
        float x,y,w,h;
        int s;
        inv_cell_rect(5, INV_COLS, INV_ROWS, 1920, 1080, &x,&y,&w,&h);
        s = inv_hit_slot(x + w*0.5f, y + h*0.5f, INV_COLS, INV_ROWS, 1920, 1080);
        CHECK(s == 5);
        CHECK(inv_hit_slot(1.0f, 1.0f, INV_COLS, INV_ROWS, 1920, 1080) == -1);  /* top-left margin */
    }

    /* page arrows live on screen and don't overlap each other. */
    {
        float px,py,pw,ph, nx,ny,nw,nh;
        inv_prev_rect(1920, 1080, &px,&py,&pw,&ph);
        inv_next_rect(1920, 1080, &nx,&ny,&nw,&nh);
        CHECK(pw > 0.0f && ph > 0.0f && nw > 0.0f && nh > 0.0f);
        CHECK(px + pw <= 1920.0f && nx + nw <= 1920.0f);
        CHECK(px + pw <= nx + 1e-3f);   /* prev is left of next */
    }

    if (fails == 0) printf("inventory_test: OK\n");
    return fails ? 1 : 0;
}

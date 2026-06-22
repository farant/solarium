/* image_test.c — the pure aspect-fit math (image_fit_box). No GL, no stb call
   (image.c is linked only for the symbol; the decoder is never run). */
#include "image.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a,b) (fabs((double)((a) - (b))) < 1e-4)

int main(void) {
    float w, h;
    /* a WIDE image (2:1) in a square field is width-bound: w fills, h is half */
    image_fit_box(200, 100, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 1.0f)); CHECK(NEAR(h, 0.5f));
    /* a TALL image (1:2) in a square field is height-bound */
    image_fit_box(100, 200, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(h, 1.0f)); CHECK(NEAR(w, 0.5f));
    /* a SQUARE image in a WIDE field fits the smaller (height) dimension */
    image_fit_box(100, 100, 2.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 1.0f)); CHECK(NEAR(h, 1.0f));
    /* aspect preserved + stays inside the field */
    image_fit_box(300, 200, 1.4f, 0.8f, &w, &h);
    CHECK(NEAR(w / h, 1.5f));
    CHECK(w <= 1.4f + 1e-4f && h <= 0.8f + 1e-4f);
    /* degenerate dims -> 0,0, no divide-by-zero */
    image_fit_box(0, 100, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 0.0f) && NEAR(h, 0.0f));
    image_fit_box(100, 0, 1.0f, 1.0f, &w, &h);
    CHECK(NEAR(w, 0.0f) && NEAR(h, 0.0f));
    if (fails == 0) printf("image_test: OK\n");
    return fails ? 1 : 0;
}

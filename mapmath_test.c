/* mapmath_test.c — pure-logic test for mapmath.c (equirect map math). GL-free,
   ASan/UBSan via `build.sh mapmathtest`. */
#include "mapmath.h"
#include <stdio.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static int near(double a, double b) { double d = a - b; if (d < 0) d = -d; return d < 1e-9; }

static void test_lonlat_corners(void) {
    double u, v;
    mapmath_lonlat_to_uv(-180.0, -90.0, &u, &v);
    CHECK(near(u, 0.0) && near(v, 0.0), "SW corner -> (0,0)");
    mapmath_lonlat_to_uv(180.0, 90.0, &u, &v);
    CHECK(near(u, 1.0) && near(v, 1.0), "NE corner -> (1,1)");
    mapmath_lonlat_to_uv(0.0, 0.0, &u, &v);
    CHECK(near(u, 0.5) && near(v, 0.5), "origin -> (0.5,0.5)");
}

static void test_roundtrip(void) {
    double u, v, lon, lat;
    mapmath_lonlat_to_uv(2.35, 48.85, &u, &v);       /* Paris */
    mapmath_uv_to_lonlat(u, v, &lon, &lat);
    CHECK(near(lon, 2.35) && near(lat, 48.85), "roundtrip Paris");
}

static void test_clamp_inputs(void) {
    double u, v;
    mapmath_lonlat_to_uv(999.0, -999.0, &u, &v);
    CHECK(near(u, 1.0) && near(v, 0.0), "out-of-range clamps to edges");
}

static void test_window_center(void) {
    double u0, v0, u1, v1;
    /* z=4, aspect 2, centered at origin: du = 1/16, dv = du. */
    mapmath_window(0.0, 0.0, 4, 2.0, &u0, &v0, &u1, &v1);
    CHECK(near(u1 - u0, 1.0 / 16.0), "z4 width = 1/16");
    CHECK(near(v1 - v0, 1.0 / 16.0), "z4 height = 1/16 at aspect 2");
    CHECK(near((u0 + u1) * 0.5, 0.5) && near((v0 + v1) * 0.5, 0.5), "centered at origin");
}

static void test_window_aspect(void) {
    double u0, v0, u1, v1;
    mapmath_window(0.0, 0.0, 4, 1.0, &u0, &v0, &u1, &v1);  /* square board */
    CHECK(near(u1 - u0, 1.0 / 16.0), "aspect1 width unchanged");
    CHECK(near(v1 - v0, 2.0 / 16.0), "aspect1 height = 2*du");
}

static void test_window_z0_full(void) {
    double u0, v0, u1, v1;
    mapmath_window(0.0, 0.0, 0, 2.0, &u0, &v0, &u1, &v1);
    CHECK(near(u0, 0.0) && near(u1, 1.0) && near(v0, 0.0) && near(v1, 1.0), "z0 = whole world");
}

static void test_window_edge_shift(void) {
    double u0, v0, u1, v1;
    /* near the north pole / antimeridian, the window must stay inside [0,1]
       at full size (center shifted, not cropped). */
    mapmath_window(179.0, 89.0, 4, 2.0, &u0, &v0, &u1, &v1);
    CHECK(near(u1 - u0, 1.0 / 16.0) && near(v1 - v0, 1.0 / 16.0), "edge window keeps size");
    CHECK(u0 >= 0.0 && u1 <= 1.0 && v0 >= 0.0 && v1 <= 1.0, "edge window inside [0,1]");
}

static void test_window_guards(void) {
    double u0, v0, u1, v1;
    mapmath_window(0.0, 0.0, 40, 2.0, &u0, &v0, &u1, &v1);   /* huge z -> clamped, still valid */
    CHECK(u0 < u1 && v0 < v1, "huge z stays valid (no overflow)");
    mapmath_window(0.0, 0.0, 4, 0.0, &u0, &v0, &u1, &v1);    /* aspect<=0 -> default 2.0 */
    CHECK(u0 < u1 && v0 < v1, "aspect<=0 defaults ok");
    mapmath_window(0.0, 80.0, 2, 0.01, &u0, &v0, &u1, &v1);  /* tiny aspect -> dv clamps to full v */
    CHECK(near(v0, 0.0) && near(v1, 1.0), "tiny aspect -> full v span");
}

static void test_pin_local(void) {
    double u0, v0, u1, v1;
    double clon, clat, lx, ly;
    const double w = 1.6, h = 0.8;   /* the MAP_BOARD_W/H board (2:1) */
    int    in;
    mapmath_window(0.0, 0.0, 4, 2.0, &u0, &v0, &u1, &v1);   /* z4 window, centered at (0,0) */

    /* (a) a pin at the window CENTRE lon/lat -> quad centre (lx=0, ly=h/2), in */
    mapmath_uv_to_lonlat((u0 + u1) * 0.5, (v0 + v1) * 0.5, &clon, &clat);
    in = map_pin_local(u0, v0, u1, v1, w, h, clon, clat, &lx, &ly);
    CHECK(in == 1, "centre pin is in-bounds");
    CHECK(near(lx, 0.0), "centre pin lx = 0");
    CHECK(near(ly, h * 0.5), "centre pin ly = h/2");

    /* (b) the window's corner uv maps to the quad corners */
    {
        double lon, lat;
        mapmath_uv_to_lonlat(u0, v0, &lon, &lat);
        in = map_pin_local(u0, v0, u1, v1, w, h, lon, lat, &lx, &ly);
        CHECK(in == 1 && near(lx, -w * 0.5) && near(ly, 0.0), "SW corner -> (-w/2, 0)");
        mapmath_uv_to_lonlat(u1, v1, &lon, &lat);
        in = map_pin_local(u0, v0, u1, v1, w, h, lon, lat, &lx, &ly);
        CHECK(in == 1 && near(lx, w * 0.5) && near(ly, h), "NE corner -> (w/2, h)");
    }

    /* (c) a pin well outside the window reports out-of-bounds and leaves lx/ly */
    lx = -123.0; ly = -123.0;
    in = map_pin_local(u0, v0, u1, v1, w, h, 170.0, 80.0, &lx, &ly);
    CHECK(in == 0, "far pin is out-of-bounds");
    CHECK(near(lx, -123.0) && near(ly, -123.0), "out-of-bounds leaves lx/ly untouched");

    /* (d) round-trip: a local point -> uv -> lonlat -> map_pin_local = same local */
    {
        double lx0 = 0.3, ly0 = 0.5, pu, pv, lon, lat, lx2, ly2;
        pu = u0 + (lx0 + w * 0.5) / w * (u1 - u0);
        pv = v0 + ly0 / h * (v1 - v0);
        mapmath_uv_to_lonlat(pu, pv, &lon, &lat);
        in = map_pin_local(u0, v0, u1, v1, w, h, lon, lat, &lx2, &ly2);
        CHECK(in == 1 && near(lx2, lx0) && near(ly2, ly0), "round-trip local point");
    }
}

int main(void) {
    test_lonlat_corners();
    test_roundtrip();
    test_clamp_inputs();
    test_window_center();
    test_window_aspect();
    test_window_z0_full();
    test_window_edge_shift();
    test_window_guards();
    test_pin_local();
    if (fails == 0) printf("mapmath_test: all passed\n");
    return fails ? 1 : 0;
}

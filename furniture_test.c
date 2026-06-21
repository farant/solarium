#include "furniture.h"
#include "mesh.h"
#include "sol_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* catalog: two kinds, names correct, cycling wraps both ways */
    {
        CHECK(furniture_catalog_count() == 2);
        CHECK(strcmp(furniture_catalog_name(0), "table") == 0);
        CHECK(strcmp(furniture_catalog_name(1), "bookshelf") == 0);
        CHECK(furniture_catalog_name(2) == (const char *)0);   /* out of range */
        CHECK(furniture_catalog_cycle(0,  1) == 1);
        CHECK(furniture_catalog_cycle(1,  1) == 0);            /* wrap forward */
        CHECK(furniture_catalog_cycle(0, -1) == 1);            /* wrap back */
    }
    /* kind predicates */
    {
        CHECK(furniture_is_table("table"));
        CHECK(!furniture_is_table("bookshelf"));
        CHECK(furniture_is_shelf("bookshelf"));
        CHECK(!furniture_is_shelf("table"));
        CHECK(!furniture_is_table((const char *)0));
        CHECK(!furniture_is_shelf("card"));
    }
    /* both furniture meshes build with geometry */
    {
        MeshBuilder b;
        mb_init(&b);
        CHECK(mesh_ref_build("table", (const float *)0, 0, &b) == SOL_TRUE);
        CHECK(b.vertex_count > 0 && b.index_count > 0);
        mb_free(&b);
        mb_init(&b);
        CHECK(mesh_ref_build("bookshelf", (const float *)0, 0, &b) == SOL_TRUE);
        CHECK(b.vertex_count > 0 && b.index_count > 0);
        mb_free(&b);
    }
    /* shelf slots: fill a shelf left-to-right, wrap to the next shelf down */
    {
        float p[4]; vec3 s0, s1, sN;
        p[0] = 1.0f; p[1] = 1.8f; p[2] = 0.3f; p[3] = 4.0f;   /* w h d shelves */
        s0 = furniture_shelf_slot(p, 4, 0);
        s1 = furniture_shelf_slot(p, 4, 1);
        CHECK(s1.x > s0.x);                       /* second spine is to the right */
        CHECK(fabs((double)(s1.y - s0.y)) < 1e-4);/* same shelf -> same height */
        CHECK(fabs((double)s0.x) <= 0.5);         /* inside the width */
        sN = furniture_shelf_slot(p, 4, 100);     /* far index: still finite + in-bounds-ish */
        CHECK(fabs((double)sN.x) <= 0.5 && sN.y >= 0.0f && sN.y <= 1.8f);
        /* once a shelf fills, the next index drops to a lower shelf */
        {
            int cols = (int)((1.0f - 2.0f * 0.06f) / 0.06f);   /* (w-2*margin)/pitch */
            vec3 a = furniture_shelf_slot(p, 4, 0);
            vec3 b = furniture_shelf_slot(p, 4, cols);          /* first of next shelf */
            CHECK(b.y < a.y - 1e-4f);
        }
    }
    if (fails == 0) printf("furniture_test: OK\n");
    return fails ? 1 : 0;
}

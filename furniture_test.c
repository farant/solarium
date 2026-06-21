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
    /* table point: clamps inside the top, sits at the top height */
    {
        float p[3]; vec3 q;
        p[0] = 1.4f; p[1] = 0.9f; p[2] = 0.75f;   /* w d h */
        q = furniture_table_point(p, 3, vec3_make(0.3f, 0.75f, 0.2f));
        CHECK(fabs((double)(q.y - 0.75f)) < 1e-4); /* on the top surface (y=h) */
        CHECK(fabs((double)(q.x - 0.3f)) < 1e-4);  /* in-bounds: unchanged */
        q = furniture_table_point(p, 3, vec3_make(5.0f, 0.0f, 5.0f));  /* way off */
        CHECK(q.x <= 0.7f + 1e-4f && q.x >= -0.7f - 1e-4f);            /* clamped to +/-w/2 */
        CHECK(q.z <= 0.45f + 1e-4f && q.z >= -0.45f - 1e-4f);          /* clamped to +/-d/2 */
    }
    /* surface-aim: a downward ray onto a table top hits; aiming away misses */
    {
        float p[3]; Ray ray; vec3 loc;
        p[0] = 1.4f; p[1] = 0.9f; p[2] = 0.75f;
        ray.origin = vec3_make(0.0f, 2.0f, 0.0f);          /* above a table at origin */
        ray.dir    = vec3_make(0.0f, -1.0f, 0.0f);
        CHECK(furniture_surface_aim("table", p, 3, vec3_make(0,0,0), 0.0f, ray, &loc));
        CHECK(fabs((double)(loc.y - 0.75f)) < 1e-3);       /* hit at the top height */
        ray.dir = vec3_make(0.0f, 1.0f, 0.0f);             /* aim up: miss */
        CHECK(!furniture_surface_aim("table", p, 3, vec3_make(0,0,0), 0.0f, ray, &loc));
    }
    /* surface-aim: a forward ray into a bookshelf's front face hits */
    {
        float p[4]; Ray ray; vec3 loc;
        p[0]=1.0f; p[1]=1.8f; p[2]=0.3f; p[3]=4.0f;
        ray.origin = vec3_make(0.0f, 0.9f, 2.0f);          /* in front (+Z) of a shelf */
        ray.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        CHECK(furniture_surface_aim("bookshelf", p, 4, vec3_make(0,0,0), 0.0f, ray, &loc));
        ray.origin = vec3_make(0.0f, 0.9f, -2.0f);         /* behind: ray points away */
        ray.dir    = vec3_make(0.0f, 0.0f, -1.0f);
        CHECK(!furniture_surface_aim("bookshelf", p, 4, vec3_make(0,0,0), 0.0f, ray, &loc));
    }
    /* surface-aim with a ROTATED table/shelf (catches a yaw-inverse bug) */
    {
        float pt_[3], ps[4]; Ray ray; vec3 loc;
        pt_[0] = 1.4f; pt_[1] = 0.9f; pt_[2] = 0.75f;
        /* table yawed 90deg: a downward off-center ray -> local hit un-rotated correctly.
           world (0.3,*,0) maps to local (0,*,0.3) at yaw 90. */
        ray.origin = vec3_make(0.3f, 2.0f, 0.0f);
        ray.dir    = vec3_make(0.0f, -1.0f, 0.0f);
        CHECK(furniture_surface_aim("table", pt_, 3, vec3_make(0,0,0), sol_radians(90.0f), ray, &loc));
        CHECK(fabs((double)(loc.z - 0.3f)) < 1e-3);
        /* shelf yawed 90deg: its front (+Z local) now faces world +X; aim from +X hits */
        ps[0]=1.0f; ps[1]=1.8f; ps[2]=0.3f; ps[3]=4.0f;
        ray.origin = vec3_make(2.0f, 0.9f, 0.0f);
        ray.dir    = vec3_make(-1.0f, 0.0f, 0.0f);
        CHECK(furniture_surface_aim("bookshelf", ps, 4, vec3_make(0,0,0), sol_radians(90.0f), ray, &loc));
    }
    if (fails == 0) printf("furniture_test: OK\n");
    return fails ? 1 : 0;
}

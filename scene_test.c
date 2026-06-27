/* scene_test.c — characterization + regression test for scene_get. Pins its
   contract (handle -> the object whose ->handle matches; NULL for 0, removed,
   or never-issued handles) across add / realloc / remove / churn, so the O(1)
   handle->index refactor stays behavior-preserving. Built by
   `build.sh scenetest` under ASan/UBSan; GL-free (empties only, zero Mesh). */

#include "scene.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static vec3 v3(float x, float y, float z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }
static quat qid(void) { quat q; q.x = 0.0f; q.y = 0.0f; q.z = 0.0f; q.w = 1.0f; return q; }

static sol_u32 add_empty(Scene *s) {
    Mesh m;
    memset(&m, 0, sizeof m);
    return scene_add(s, 0, m, v3(0.0f, 0.0f, 0.0f), qid(), v3(1.0f, 1.0f, 1.0f));
}

static void test_add_get(void) {
    Scene   s;
    sol_u32 h[5];
    int     i;
    scene_init(&s);
    for (i = 0; i < 5; i++) h[i] = add_empty(&s);
    for (i = 0; i < 5; i++) {
        SceneObject *o = scene_get(&s, h[i]);
        CHECK(o != NULL && o->handle == h[i], "add/get: each handle resolves to its object");
    }
    scene_free(&s);
}

static void test_realloc(void) {
    Scene   s;
    sol_u32 h[40];
    int     i;
    scene_init(&s);
    for (i = 0; i < 40; i++) h[i] = add_empty(&s);   /* past the initial cap (16) */
    for (i = 0; i < 40; i++) {
        SceneObject *o = scene_get(&s, h[i]);
        CHECK(o != NULL && o->handle == h[i], "realloc: handles survive objects[] growth");
    }
    scene_free(&s);
}

static void test_remove_middle(void) {
    Scene   s;
    sol_u32 h[5];
    int     i;
    scene_init(&s);
    for (i = 0; i < 5; i++) h[i] = add_empty(&s);    /* handles 1..5 */
    scene_remove(&s, h[2]);                          /* drop the middle one */
    CHECK(scene_get(&s, h[2]) == NULL, "remove: removed handle -> NULL");
    for (i = 0; i < 5; i++) {
        SceneObject *o;
        if (i == 2) continue;
        o = scene_get(&s, h[i]);
        CHECK(o != NULL && o->handle == h[i], "remove: survivors still resolve correctly");
    }
    {
        sol_u32      hn = add_empty(&s);             /* add after a remove */
        SceneObject *o  = scene_get(&s, hn);
        CHECK(o != NULL && o->handle == hn, "remove+add: new handle resolves");
    }
    scene_free(&s);
}

static void test_zero_and_oob(void) {
    Scene s;
    scene_init(&s);
    (void)add_empty(&s);
    CHECK(scene_get(&s, 0) == NULL, "get(0) -> NULL (root/none)");
    CHECK(scene_get(&s, 99999u) == NULL, "get(never-issued) -> NULL");
    scene_free(&s);
}

/* a linear-scan oracle: does the scene contain `handle`, and at what index? */
static int oracle_present(Scene *s, sol_u32 handle, sol_u32 *out_index) {
    sol_u32 i;
    for (i = 0; i < s->count; i++)
        if (s->objects[i].handle == handle) {
            if (out_index) *out_index = i;
            return 1;
        }
    return 0;
}

/* churn: an interleave of add + remove; after every step, scene_get must agree
   with the oracle for EVERY handle ever issued (present -> exact pointer; absent
   -> NULL). This is the strong guard on the index map's correctness. */
static void test_churn_oracle(void) {
    Scene   s;
    sol_u32 hs[64];
    int     n = 0, step;
    scene_init(&s);
    for (step = 0; step < 200; step++) {
        if ((step % 3) != 0 || n == 0) {             /* mostly add, sometimes remove */
            if (n < 64) hs[n++] = add_empty(&s);
        } else {
            int k = step % n;                        /* deterministic victim */
            scene_remove(&s, hs[k]);
            hs[k] = hs[--n];                         /* compact the shadow list */
        }
        {
            sol_u32 h;
            for (h = 1; h < s.next_handle; h++) {
                sol_u32      oi = 0;
                int          present = oracle_present(&s, h, &oi);
                SceneObject *o = scene_get(&s, h);
                if (present)
                    CHECK(o == &s.objects[oi] && o->handle == h,
                          "churn: get matches oracle (present)");
                else
                    CHECK(o == NULL, "churn: get matches oracle (absent)");
            }
        }
    }
    scene_free(&s);
}

int main(void) {
    test_add_get();
    test_realloc();
    test_remove_middle();
    test_zero_and_oob();
    test_churn_oracle();
    if (fails == 0) printf("scene_test: all passed\n");
    return fails ? 1 : 0;
}

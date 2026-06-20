#include "workspace.h"
#include "scene.h"
#include "sol_math.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* a top-level object tagged into a workspace */
static sol_u32 add_tagged(Scene *s, const char *ws) {
    Mesh empty; sol_u32 h;
    memset(&empty, 0, sizeof empty);
    h = scene_add(s, 0, empty, vec3_make(0,0,0), quat_identity(), vec3_make(1,1,1));
    if (ws) scene_meta_set(s, h, "workspace", ws);
    return h;
}

int main(void) {
    /* workspace_of: tagged top-level -> its name; child inherits; untagged -> home */
    {
        Scene s; sol_u32 room, card, bare; Mesh empty;
        memset(&empty, 0, sizeof empty);
        scene_init(&s);
        room = add_tagged(&s, "photos");
        card = scene_add(&s, room, empty, vec3_make(0,0,0), quat_identity(), vec3_make(1,1,1));
        bare = add_tagged(&s, NULL);
        CHECK(strcmp(workspace_of(&s, room), "photos") == 0);
        CHECK(strcmp(workspace_of(&s, card), "photos") == 0);   /* inherited */
        CHECK(strcmp(workspace_of(&s, bare), "home") == 0);     /* absent => home */
        scene_free(&s);
    }
    /* scene_object_active: "" filter => everything active; set => only matching */
    {
        Scene s; sol_u32 a, b;
        scene_init(&s);
        a = add_tagged(&s, "home");
        b = add_tagged(&s, "photos");
        s.active_ws[0] = '\0';
        CHECK(scene_object_active(&s, a));
        CHECK(scene_object_active(&s, b));      /* unfiltered: all visible */
        strcpy(s.active_ws, "home");
        CHECK(scene_object_active(&s, a));
        CHECK(!scene_object_active(&s, b));      /* photos hidden while home active */
        scene_free(&s);
    }
    if (fails == 0) printf("workspace_test: OK\n");
    return fails ? 1 : 0;
}

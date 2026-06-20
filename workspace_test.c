#include "workspace.h"
#include "scene.h"
#include "mesh.h"
#include "sol_math.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

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
    /* id scheme: namespaced per workspace, monotonic */
    {
        Scene s; sol_u32 g;
        scene_init(&s);
        CHECK(workspace_gate_count(&s, "home") == 0);
        g = workspace_add_gate(&s, "home", vec3_make(0,0,0), 0.0f,
                               "home-1", "photos", "photos-1", "photos");
        CHECK(g != 0);
        CHECK(workspace_gate_count(&s, "home") == 1);
        CHECK(strcmp(scene_meta_get(&s, g, "portal_id"), "home-1") == 0);
        CHECK(strcmp(scene_meta_get(&s, g, "target_ws"), "photos") == 0);
        CHECK(strcmp(scene_meta_get(&s, g, "target_portal_id"), "photos-1") == 0);
        CHECK(strcmp(scene_meta_get(&s, g, "workspace"), "home") == 0);
        CHECK(scene_get(&s, g)->kind == KIND_PORTAL);
        scene_free(&s);
    }
    /* link: a pair cross-references, each gate tagged to its own side */
    {
        Scene s; sol_u32 out; sol_u32 i, ret = 0;
        const char *out_id, *ret_id;
        scene_init(&s);
        out = workspace_link(&s, "home",   vec3_make(0,0,0),  0.0f,
                                 "photos", vec3_make(5,0,0),  0.0f);
        CHECK(out != 0);
        out_id = scene_meta_get(&s, out, "portal_id");
        ret_id = scene_meta_get(&s, out, "target_portal_id");
        CHECK(strcmp(scene_meta_get(&s, out, "workspace"), "home") == 0);
        CHECK(strcmp(scene_meta_get(&s, out, "target_ws"), "photos") == 0);
        for (i = 0; i < s.count; i++) {                       /* find the partner */
            const char *pid = scene_meta_get(&s, s.objects[i].handle, "portal_id");
            if (pid && strcmp(pid, ret_id) == 0) { ret = s.objects[i].handle; break; }
        }
        CHECK(ret != 0);
        CHECK(strcmp(scene_meta_get(&s, ret, "workspace"), "photos") == 0);
        CHECK(strcmp(scene_meta_get(&s, ret, "target_ws"), "home") == 0);
        CHECK(strcmp(scene_meta_get(&s, ret, "target_portal_id"), out_id) == 0);
        scene_free(&s);
    }
    /* home room is tagged and is a real room (a "room" shell child) */
    {
        Scene s; sol_u32 room, i; int shell = 0;
        scene_init(&s);
        room = workspace_add_home_room(&s, "photos", vec3_make(0, 12, 0));
        CHECK(strcmp(scene_meta_get(&s, room, "workspace"), "photos") == 0);
        CHECK(strcmp(scene_meta_get(&s, room, "room_type"), "home") == 0);
        for (i = 0; i < s.count; i++) {
            SceneObject *o = &s.objects[i];
            if (o->parent == room && o->mesh_ref && strcmp(o->mesh_ref, "room") == 0) shell = 1;
        }
        CHECK(shell);
        scene_free(&s);
    }
    /* anchor find-or-create is idempotent */
    {
        Scene s; sol_u32 a1, a2;
        scene_init(&s);
        a1 = workspace_anchor_add(&s, "photos");
        a2 = workspace_anchor_add(&s, "photos");
        CHECK(a1 != 0 && a1 == a2);
        CHECK(workspace_anchor_find(&s, "photos") == a1);
        CHECK(workspace_anchor_find(&s, "nope") == 0);
        scene_free(&s);
    }
    /* arrival: find by id, and spawn stands in front along the gate's yaw */
    {
        Scene s; sol_u32 g; vec3 p; float yaw;
        scene_init(&s);
        /* gate at x=10, facing yaw = +pi/2 */
        g = workspace_add_gate(&s, "home", vec3_make(10,5,0), 1.5707963f,
                               "home-1", "b", "b-1", "b");
        CHECK(workspace_find_gate_by_id(&s, "home-1") == g);
        CHECK(workspace_find_gate_by_id(&s, "nope") == 0);
        workspace_spawn_at_gate(&s, g, 1.5f, 1.65f, &p, &yaw);
        CHECK(fabs((double)(p.y - (5.0f + 1.65f))) < 1e-3);   /* raised by eye */
        CHECK(fabs((double)(yaw - 1.5707963f)) < 1e-3);        /* faces the way the gate points */
        /* stands 1.5 in front: horizontal displacement magnitude ~= 1.5 */
        {
            double dx = p.x - 10.0, dz = p.z - 0.0;
            CHECK(fabs(sqrt(dx*dx + dz*dz) - 1.5) < 1e-2);
        }
        scene_free(&s);
    }
    /* the gate mesh builds with geometry */
    {
        MeshBuilder b;
        mb_init(&b);
        CHECK(mesh_ref_build("gate", (const float *)0, 0, &b) == SOL_TRUE);
        CHECK(b.vertex_count > 0 && b.index_count > 0);
        mb_free(&b);
    }
    if (fails == 0) printf("workspace_test: OK\n");
    return fails ? 1 : 0;
}

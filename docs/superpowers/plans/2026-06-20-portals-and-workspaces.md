# Portals & Workspaces Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hold many self-contained worlds ("workspaces") in the one `scene.stml`, navigated by walk-through portal gates that swap the active workspace in memory and set you down at the paired return gate.

**Architecture:** Workspaces are `meta["workspace"]` tags on top-level objects (absent ⇒ `"home"`, so the current scene migrates for free). Exactly one workspace is *active*; a runtime `Scene.active_ws` field + a `scene_object_active()` predicate gate every scene reader (render, collide, route, pick, editor). Portals are `KIND_PORTAL` objects carrying a stable-id link to their paired return gate; walking through one saves, swaps `active_ws`, rebuilds the derived geometry, and respawns you at the partner gate.

**Tech Stack:** C89 (`build.sh c89check` = `-std=c89 -pedantic-errors -Werror -Wall -Wextra`); the scene graph (`scene.c`/`scene_io.c`), `mesh.c` procedural registry, command palette (`command.h`/`palette.c`), `collide.c`, `route.c`, `editor.c`. No new shader → no MSL twin.

**Spec:** `docs/superpowers/specs/2026-06-20-portals-and-workspaces-design.md`

---

## Conventions for every task

- **Branch:** work on a feature branch `portals-workspaces` off `main` (do NOT commit to `main` directly). Create it once before Task 1: `git switch -c portals-workspaces`.
- **NEVER stage `NOTES.stml` or `paper-picture.png`** — they always show as modified; they are Fran's. Stage only the exact files each task names.
- **C89 only:** declarations at the top of each block; `/* */` comments only; no `//`, no C99 mixed declarations, no VLAs, no `for (int i...)`.
- **Read-only git:** `git diff`/`log`/`show` are fine; never `checkout`/`reset`/`stash`/`switch`/`branch` other than the one feature-branch creation above and your own task commits.
- **Build gauntlet** (run before every commit that touches compiled code): `./build.sh c89check && ./build.sh debug && ./build.sh metal`. Plus the task's own headless suite.
- Each commit message ends with the line:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

---

## File Structure

| File | Responsibility |
|------|----------------|
| `scene.h` | + `KIND_PORTAL` enum value; + `SOL_WS_NAME_CAP`; + runtime `char active_ws[]` field on `Scene` |
| `scene.c` | `scene_init` zero-inits `active_ws` |
| `scene_io.c` | + `"portal"` in `KIND_NAMES` (index-aligned) |
| `workspace.h` / `workspace.c` (NEW) | the headless workspace module: `workspace_of`, `scene_object_active`, anchor find/add, portal-pair creation + id scheme, `add_home_room`, arrival lookup + spawn math. Uses only public scene/mesh API — no GL. |
| `workspace_test.c` (NEW) | headless suite for the module (the `descend_test.c` mold) |
| `mesh.c` | + `emit_gate` + the `"gate"` registry row |
| `collide.c` | `collide_rebuild` honors `scene_object_active` |
| `route.c` | room collection honors `scene_object_active` |
| `editor.c` | room enumeration honors `scene_object_active` |
| `main.c` | render/BVH/connections filters; `Scene.active_ws` set at init; `world_rebuild` factored from `load_palace`; two palette commands; per-frame gate trigger + travel + debounce; `KIND_PORTAL` material |
| `collide_test.c` / `route_test.c` / `editor_test.c` | + a "hidden workspace is filtered" assertion each |
| `build.sh` | add `workspace.c` to the 4 main builds (`c89check`/`metal`/`asan`/`debug`) [Task 2]; + `workspacetest` mode [Task 2]; add `workspace.c` to `coltest`/`routetest` [Task 6] + `editortest`/`descendtest` [Task 8] test links |
| `.gitignore` | + `/workspace_test` |

---

## Task 1: `KIND_PORTAL` kind + serialization

**Files:**
- Modify: `scene.h` (the `ObjectKind` enum)
- Modify: `scene_io.c:23` (`KIND_NAMES`)
- Test: `scene_io_test.c` (a portal round-trip)

- [ ] **Step 1: Write the failing test** — append inside `main()` of `scene_io_test.c`, just before the final `printf("scene_io_test: OK\n");`. The file's idiom is inline `if (!cond) { printf("FAIL: ...\n"); return 1; }` (it does NOT use `assert`); already-included headers cover everything used here:

```c
    /* a KIND_PORTAL object survives save+load with its kind + meta intact */
    {
        Scene s, r;
        sol_u32 h, i;
        Mesh empty;
        int found = 0;
        memset(&empty, 0, sizeof empty);
        scene_init(&s);
        h = scene_add(&s, 0, empty, vec3_make(1.0f, 2.0f, 3.0f),
                      quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_kind_set(&s, h, KIND_PORTAL);
        scene_mesh_ref_set(&s, h, "gate");
        scene_meta_set(&s, h, "portal_id", "home-1");
        if (!scene_save(&s, "scene_io_test_portal.stml")) {
            printf("FAIL: could not save portal scene\n"); return 1;
        }
        if (!scene_load(&r, "scene_io_test_portal.stml")) {
            printf("FAIL: could not load portal scene\n"); return 1;
        }
        for (i = 0; i < r.count; i++) {
            if (r.objects[i].kind == KIND_PORTAL) {
                const char *pid = scene_meta_get(&r, r.objects[i].handle, "portal_id");
                found = 1;
                if (!pid || strcmp(pid, "home-1") != 0) {
                    printf("FAIL: portal lost its portal_id\n"); return 1;
                }
            }
        }
        if (!found) { printf("FAIL: KIND_PORTAL did not round-trip\n"); return 1; }
        scene_free(&s); scene_free(&r);
        printf("portal kind round-trip: OK\n");
    }
```

- [ ] **Step 2: Run it to confirm it fails** — `./build.sh iotest && ./scene_io_test`. Expected: a compile error (`KIND_PORTAL` undeclared) — that IS the red.

- [ ] **Step 3: Add the enum value** — in `scene.h`, append to `ObjectKind` after `KIND_TOMBSTONE`:

```c
typedef enum {
    KIND_PLAIN = 0,
    KIND_FILE,
    KIND_FOLDER,
    KIND_ALIAS,
    KIND_NOTE,
    KIND_TOMBSTONE,
    KIND_PORTAL          /* a workspace travel gate (Portals & Workspaces) */
} ObjectKind;
```

- [ ] **Step 4: Add the serialized name** — in `scene_io.c:23`, append `"portal"` so its index (6) matches `KIND_PORTAL`:

```c
static const char *KIND_NAMES[] = {
    "plain", "file", "folder", "alias", "note", "tombstone", "portal"
};
```

- [ ] **Step 5: Run the test to confirm it passes** — `./build.sh iotest && ./scene_io_test`. Expected: `portal kind round-trip: OK` and the suite's overall OK line. Then `./build.sh c89check`. Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add scene.h scene_io.c scene_io_test.c
git commit -m "$(printf 'Portals: add KIND_PORTAL kind + serialization\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 2: workspace module foundation — `workspace_of` + `scene_object_active`

**Files:**
- Modify: `scene.h` (the `Scene` struct + a cap macro)
- Modify: `scene.c` (`scene_init`)
- Create: `workspace.h`, `workspace.c`
- Create: `workspace_test.c`
- Modify: `build.sh` (new `workspacetest` mode)
- Modify: `.gitignore`

- [ ] **Step 1: Add the cap + the runtime field.** In `scene.h`, near the top (after the includes, before the `Scene` struct), add:

```c
#define SOL_WS_NAME_CAP 64   /* max workspace-name length + NUL */
```

Find the `Scene` struct definition in `scene.h` (the one with `objects`, `count`, `capacity`). Add a field at its end:

```c
    char     active_ws[SOL_WS_NAME_CAP];  /* runtime view filter: the workspace
                          currently shown; "" = unfiltered (show all). NEVER
                          serialized — reset on load, set by the app. */
```

- [ ] **Step 2: Zero-init it.** In `scene.c`, in `scene_init`, set the field empty (add after the existing zeroing of count/capacity):

```c
    s->active_ws[0] = '\0';
```

- [ ] **Step 3: Write the failing test.** Create `workspace_test.c`:

```c
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
```

- [ ] **Step 4: Create the header.** Create `workspace.h`:

```c
/* workspace.h — workspaces are tagged partitions of the one scene; exactly
   one is ACTIVE. This module owns the membership predicate every scene reader
   consults, plus the portal-pair authoring used by the palette commands.
   Headless: only public scene/mesh API, no GL. */
#ifndef SOL_WORKSPACE_H
#define SOL_WORKSPACE_H

#include "scene.h"

/* The workspace an object belongs to: walk the parent chain to the first
   meta["workspace"]; absent before parent 0 => "home". Never NULL. */
const char *workspace_of(Scene *s, sol_u32 handle);

/* Visible/live under the current filter? s->active_ws == "" => always true. */
sol_bool    scene_object_active(Scene *s, sol_u32 handle);

#endif /* SOL_WORKSPACE_H */
```

- [ ] **Step 5: Create the implementation.** Create `workspace.c`:

```c
/* workspace.c — see workspace.h. */
#include "workspace.h"
#include <string.h>

const char *workspace_of(Scene *s, sol_u32 handle) {
    sol_u32 h = handle;
    int     guard = 0;                 /* parent-chain runaway bound */
    while (h != 0 && guard++ < 64) {
        const char  *w = scene_meta_get(s, h, "workspace");
        SceneObject *o;
        if (w) return w;
        o = scene_get(s, h);
        if (!o) break;
        h = o->parent;
    }
    return "home";
}

sol_bool scene_object_active(Scene *s, sol_u32 handle) {
    if (s->active_ws[0] == '\0') return SOL_TRUE;     /* unfiltered */
    return (sol_bool)(strcmp(workspace_of(s, handle), s->active_ws) == 0);
}
```

- [ ] **Step 6: Wire `workspace.c` into the builds.** Two parts:

  **(a)** Add `workspace.c` to the FOUR main-app source lists in `build.sh` — every one ends in `... route.c editor.c descend.c`; append ` workspace.c` after `descend.c` in each:
  - the `c89check` block (line ~16, the `-fsyntax-only` list) — so the new TU is C89-checked;
  - the `metal` block;
  - the `asan` block;
  - the default/`debug` build at the bottom of the file.

  (Compiling it everywhere now is harmless — its functions go unused until `collide.c`/`route.c`/`editor.c`/`main.c` call them in later tasks; this keeps every build linking from here on.)

  **(b)** Add the headless suite, after the `descendtest` block (mirroring its link set; `workspace.c` + the scene spine, no GL):

```sh
# workspacetest: the workspace module — membership + portal-pair authoring
# (no GL). Links the scene spine + mesh.c the gates reference.
if [ "$MODE" = "workspacetest" ]; then
    set -x
    clang -std=c89 -pedantic-errors -Werror -g -fsanitize=address,undefined \
        workspace.c workspace_test.c scene.c material.c scene_io.c mesh.c flora.c rock.c gothic.c sweep.c nid.c stml.c sol_math.c \
        -o workspace_test
    echo "built ./workspace_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 7: Run the test.** `./build.sh workspacetest && ./workspace_test`. Expected: `workspace_test: OK`. Then `./build.sh c89check`. Expected: PASS.

- [ ] **Step 8: Ignore the test binary.** Add to `.gitignore`:

```
/workspace_test
```

- [ ] **Step 9: Commit**

```bash
git add scene.h scene.c workspace.h workspace.c workspace_test.c build.sh .gitignore
git commit -m "$(printf 'Portals: workspace membership predicate + active_ws filter\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 3: portal-pair authoring + id scheme + `add_home_room`

**Files:**
- Modify: `workspace.h`, `workspace.c`
- Modify: `workspace_test.c`

This task adds the headless authoring the palette commands will call. All in-memory; no file or GL.

- [ ] **Step 1: Declare the API.** Add to `workspace.h` (before `#endif`):

```c
/* a WorkspaceAnchor: a parent-0 empty carrying meta["workspace_name"]. The
   identity + display handle of a workspace, and what "Portal to..." lists. */
sol_u32 workspace_anchor_find(Scene *s, const char *name);   /* 0 if none */
sol_u32 workspace_anchor_add(Scene *s, const char *name);    /* find-or-create */

/* count of KIND_PORTAL gates tagged into workspace `ws` (for id minting) */
int     workspace_gate_count(Scene *s, const char *ws);

/* a fresh open-topped home room tagged into `ws`, at `pos`. Returns the room
   anchor handle. (Factored to mirror populate_home_scene's room.) */
sol_u32 workspace_add_home_room(Scene *s, const char *ws, vec3 pos);

/* Add one gate: a KIND_PORTAL object, mesh_ref "gate", tagged into `ws`, with
   the full link meta. `yaw` is its facing (radians about world up). Returns
   the gate handle. */
sol_u32 workspace_add_gate(Scene *s, const char *ws, vec3 pos, float yaw,
                           const char *self_id, const char *target_ws,
                           const char *target_id, const char *label);

/* Create a LINKED PAIR: an outbound gate in `from_ws` at (from_pos,from_yaw)
   and a return gate in `to_ws` at (to_pos,to_yaw), cross-referenced by id.
   `label_out`/`label_ret` are the display names (usually the OTHER ws's name).
   Returns the outbound gate handle. */
sol_u32 workspace_link(Scene *s,
                       const char *from_ws, vec3 from_pos, float from_yaw,
                       const char *to_ws,   vec3 to_pos,   float to_yaw);
```

- [ ] **Step 2: Write the failing tests.** Append to `workspace_test.c`'s `main()` (before the summary):

```c
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
```

- [ ] **Step 3: Run to confirm it fails.** `./build.sh workspacetest`. Expected: link errors (`workspace_add_gate` etc. undefined).

- [ ] **Step 4: Implement.** Add to `workspace.c` (after the existing functions). Note the id buffers are local `char[SOL_WS_NAME_CAP+16]`; format with `sprintf` of an `int`:

```c
#include <stdio.h>    /* sprintf for id minting */

sol_u32 workspace_anchor_find(Scene *s, const char *name) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        const char *n = scene_meta_get(s, s->objects[i].handle, "workspace_name");
        if (n && strcmp(n, name) == 0) return s->objects[i].handle;
    }
    return 0;
}

sol_u32 workspace_anchor_add(Scene *s, const char *name) {
    Mesh    empty;
    sol_u32 h = workspace_anchor_find(s, name);
    vec3    one, zero;
    quat    qid;
    if (h != 0) return h;
    memset(&empty, 0, sizeof empty);
    one.x = one.y = one.z = 1.0f; zero.x = zero.y = zero.z = 0.0f;
    qid.x = qid.y = qid.z = 0.0f; qid.w = 1.0f;
    h = scene_add(s, 0, empty, zero, qid, one);
    scene_meta_set(s, h, "workspace_name", name);
    return h;
}

int workspace_gate_count(Scene *s, const char *ws) {
    sol_u32 i; int n = 0;
    for (i = 0; i < s->count; i++) {
        if (s->objects[i].kind != KIND_PORTAL) continue;
        if (strcmp(workspace_of(s, s->objects[i].handle), ws) == 0) n++;
    }
    return n;
}

sol_u32 workspace_add_gate(Scene *s, const char *ws, vec3 pos, float yaw,
                           const char *self_id, const char *target_ws,
                           const char *target_id, const char *label) {
    Mesh    empty;
    sol_u32 h;
    vec3    one;
    quat    rot;
    memset(&empty, 0, sizeof empty);
    one.x = one.y = one.z = 1.0f;
    rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw);
    h = scene_add(s, 0, empty, pos, rot, one);
    scene_kind_set(s, h, KIND_PORTAL);
    scene_mesh_ref_set(s, h, "gate");
    scene_meta_set(s, h, "workspace", ws);
    scene_meta_set(s, h, "portal_id", self_id);
    scene_meta_set(s, h, "target_ws", target_ws);
    scene_meta_set(s, h, "target_portal_id", target_id);
    if (label) scene_meta_set(s, h, "name", label);
    return h;
}

sol_u32 workspace_add_home_room(Scene *s, const char *ws, vec3 pos) {
    Mesh    empty;
    sol_u32 room, shell;
    float   p[8];
    vec3    one, zero;
    quat    qid;
    memset(&empty, 0, sizeof empty);
    one.x = one.y = one.z = 1.0f; zero.x = zero.y = zero.z = 0.0f;
    qid.x = qid.y = qid.z = 0.0f; qid.w = 1.0f;
    room = scene_add(s, 0, empty, pos, qid, one);
    scene_meta_set(s, room, "room_type", "home");
    scene_meta_set(s, room, "name", ws);
    scene_meta_set(s, room, "workspace", ws);
    shell = scene_add(s, room, empty, zero, qid, one);
    scene_mesh_ref_set(s, shell, "room");
    p[0] = 8.0f; p[1] = 8.0f; p[2] = 3.0f; p[3] = 1.0f;
    p[4] = 1.0f; p[5] = 1.0f; p[6] = 1.0f; p[7] = 0.0f;
    scene_mesh_params_set(s, shell, p, 8);
    return room;
}

sol_u32 workspace_link(Scene *s,
                       const char *from_ws, vec3 from_pos, float from_yaw,
                       const char *to_ws,   vec3 to_pos,   float to_yaw) {
    char id_from[SOL_WS_NAME_CAP + 16];
    char id_to[SOL_WS_NAME_CAP + 16];
    sol_u32 out;
    sprintf(id_from, "%s-%d", from_ws, workspace_gate_count(s, from_ws) + 1);
    sprintf(id_to,   "%s-%d", to_ws,   workspace_gate_count(s, to_ws)   + 1);
    out = workspace_add_gate(s, from_ws, from_pos, from_yaw,
                             id_from, to_ws, id_to, to_ws);
    workspace_add_gate(s, to_ws, to_pos, to_yaw,
                       id_to, from_ws, id_from, from_ws);
    return out;
}
```

(`quat_from_axis_angle(vec3 axis, float angle)` is declared in `sol_math.h:45`; for a Y axis it yields the pure-yaw quaternion the Task 4 spawn math decodes.)

- [ ] **Step 5: Run the tests.** `./build.sh workspacetest && ./workspace_test`. Expected: `workspace_test: OK`. Then `./build.sh c89check`. Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add workspace.h workspace.c workspace_test.c
git commit -m "$(printf 'Portals: workspace anchors + portal-pair authoring + home room\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 4: arrival lookup + spawn math

**Files:**
- Modify: `workspace.h`, `workspace.c`
- Modify: `workspace_test.c`

- [ ] **Step 1: Declare.** Add to `workspace.h`:

```c
/* the gate carrying meta["portal_id"] == id, or 0. */
sol_u32 workspace_find_gate_by_id(Scene *s, const char *portal_id);

/* Where a traveller arriving at `gate` stands: `stand` meters in front of the
   gate along its facing, raised by `eye`; *out_yaw is the gate's facing (you
   arrive looking the way it points, into the world). */
void    workspace_spawn_at_gate(Scene *s, sol_u32 gate, float stand, float eye,
                                vec3 *out_pos, float *out_yaw);
```

- [ ] **Step 2: Write the failing test.** Append to `workspace_test.c` `main()`:

```c
/* arrival: find by id, and spawn stands in front along the gate's yaw */
{
    Scene s; sol_u32 g; vec3 p; float yaw;
    scene_init(&s);
    /* gate at x=10, facing +X (yaw = +pi/2 points world forward toward +X
       under the engine's yaw convention used below) */
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
```

Add `#include <math.h>` at the top of `workspace_test.c` if not already present.

- [ ] **Step 3: Implement.** Add `#include <math.h>` to the top of `workspace.c` (for `atan2`/`cos`/`sin`), then add the functions. The gate's world position is the translation of its world matrix (`mat4` is flat column-major — `m.m[12..14]`; the render loop reads world Y as `model.m[13]` at `main.c:9652`, confirming the layout). The gate's facing is recovered from its rotation quaternion (gates are built as pure-yaw quaternions in Task 3, `q = (0, sin(yaw/2), 0, cos(yaw/2))`, so `yaw = 2*atan2(q.y, q.w)`). The horizontal forward is then `(cos yaw, 0, sin yaw)` — matching `camera_forward` at pitch 0:

```c
sol_u32 workspace_find_gate_by_id(Scene *s, const char *portal_id) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        const char *pid;
        if (s->objects[i].kind != KIND_PORTAL) continue;
        pid = scene_meta_get(s, s->objects[i].handle, "portal_id");
        if (pid && strcmp(pid, portal_id) == 0) return s->objects[i].handle;
    }
    return 0;
}

void workspace_spawn_at_gate(Scene *s, sol_u32 gate, float stand, float eye,
                             vec3 *out_pos, float *out_yaw) {
    SceneObject *o = scene_get(s, gate);
    mat4  m;
    vec3  gp, fwd;
    quat  q;
    float yaw;
    if (!o) { out_pos->x = out_pos->y = out_pos->z = 0.0f; *out_yaw = 0.0f; return; }
    m   = scene_world_matrix(s, o);
    gp  = vec3_make(m.m[12], m.m[13], m.m[14]);             /* translation column */
    q   = o->rot;                                           /* pure-yaw quaternion */
    yaw = 2.0f * (float)atan2((double)q.y, (double)q.w);
    fwd = vec3_make((float)cos((double)yaw), 0.0f, (float)sin((double)yaw));
    out_pos->x = gp.x + fwd.x * stand;
    out_pos->y = gp.y + eye;
    out_pos->z = gp.z + fwd.z * stand;
    *out_yaw   = yaw;
}
```

- [ ] **Step 4: Run.** `./build.sh workspacetest && ./workspace_test`. Expected: `workspace_test: OK` (all prior + new). `./build.sh c89check`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add workspace.h workspace.c workspace_test.c
git commit -m "$(printf 'Portals: arrival gate lookup + spawn-in-front math\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 5: the `"gate"` mesh + `KIND_PORTAL` material

**Files:**
- Modify: `mesh.c` (a new `emit_gate` + registry row)
- Modify: `main.c:8065` (`apply_kind_materials`)
- Modify: `workspace_test.c` (mesh build assertion)

- [ ] **Step 1: Write the failing test.** Append to `workspace_test.c` `main()` (it links `mesh.c`):

```c
/* the gate mesh builds with geometry */
{
    MeshBuilder b;
    mb_init(&b);
    CHECK(mesh_ref_build("gate", (const float *)0, 0, &b) == SOL_TRUE);
    CHECK(b.vertex_count > 0 && b.index_count > 0);
    mb_free(&b);
}
```

Add `#include "mesh.h"` to `workspace_test.c` top.

- [ ] **Step 2: Run to confirm fail.** `./build.sh workspacetest`. Expected: `mesh_ref_build("gate", ...)` returns `SOL_FALSE` → CHECK fails (`registry_find` returns NULL for an unknown ref).

- [ ] **Step 3: Implement the mesh.** In `mesh.c`, add `emit_gate` near the other `emit_*` functions (above the registry table). It builds a freestanding doorframe with `aabb_box(b, x0,x1, y0,y1, z0,z1)` — two posts, a lintel, and a thin glowing pane in the opening; centered on origin, opening through local ±Z, base at y=0:

```c
/* a freestanding portal gate (Portals & Workspaces): two posts + a lintel
   framing a thin central pane. opening faces local +/-Z; base at y=0.
   params: w (opening width), h (height), t (depth), post (post/lintel width). */
static void emit_gate(MeshBuilder *b, const float *p) {
    float w = p[0], h = p[1], t = p[2], pw = p[3];
    float hw = w * 0.5f, hz = t * 0.5f;
    /* left + right posts */
    aabb_box(b, -hw - pw, -hw,     0.0f, h,      -hz, hz);
    aabb_box(b,  hw,       hw + pw, 0.0f, h,      -hz, hz);
    /* lintel across the top */
    aabb_box(b, -hw - pw,  hw + pw, h - pw, h,    -hz, hz);
    /* the shimmer pane: thin slab filling the opening */
    aabb_box(b, -hw, hw, 0.0f, h - pw, -0.02f, 0.02f);
}
```

Add the registry row in the `MeshRefEntry` table (near the `"card"`/`"walkway"` rows — NOT near `"portal"`, which is the gothic arch):

```c
    { "gate", 4, { "w", "h", "t", "post" }, { 1.6f, 2.4f, 0.18f, 0.16f }, emit_gate },
```

(Gates are intentionally **non-solid**: `collide_rebuild` dispatches only on known mesh-refs — `"room"`/`"wall"`/`"walkway"`/… — and `"gate"` matches none, so no collider is emitted and you walk straight through. This is the spec §6 "trigger only" behavior, achieved for free.)

- [ ] **Step 4: Give `KIND_PORTAL` its glow.** In `main.c:8065`, in `apply_kind_materials`'s `switch (o->kind)`, add a case before `default:`:

```c
            case KIND_PORTAL:    m.base_color = vec3_make(0.20f, 0.32f, 0.55f); m.roughness = 0.30f;
                                 m.emissive   = vec3_make(0.25f, 0.55f, 0.95f); break;
```

- [ ] **Step 5: Run.** `./build.sh workspacetest && ./workspace_test`. Expected: `workspace_test: OK`. Then the gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal`. Expected: all PASS/build.

- [ ] **Step 6: Commit**

```bash
git add mesh.c main.c workspace_test.c
git commit -m "$(printf 'Portals: the gate mesh + KIND_PORTAL emissive material\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 6: the active filter — collide + route (headless, tested)

**Files:**
- Modify: `collide.c` (`collide_rebuild`), `route.c` (`collect_rooms`)
- Modify: `collide_test.c`, `route_test.c`
- Modify: `build.sh` (`coltest`, `routetest` link lines += `workspace.c`)

- [ ] **Step 1: Wire the route filter.** In `route.c`, add `#include "workspace.h"` with the other includes. In `collect_rooms` (`route.c:33`), right after the room-type accept check (`if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;` near `route.c:40`), add:

```c
        if (!scene_object_active(s, o->handle)) continue;   /* hidden workspace */
```

- [ ] **Step 2: Wire the collide filter.** In `collide.c`, add `#include "workspace.h"`. In `collide_rebuild` (`collide.c:309`), right after `if (o->mesh_ref == NULL) continue;`, add:

```c
        if (!scene_object_active(s, o->handle)) continue;   /* hidden workspace */
```

- [ ] **Step 3: Add `workspace.c` to the test links.** In `build.sh`, append ` workspace.c` to the source list of BOTH the `coltest` and `routetest` blocks (right after `route.c`).

- [ ] **Step 4: Write the failing route test.** `route_test.c` already has `add_room(Scene*, x, y, z, w, d)`, `add_walkway(Scene*, a, b)`, the `CHECK`/`fails` macro, and `<string.h>`. Add this block in `main()` before the `if (fails == 0) printf("route_test: OK\n");` line. `Route` exposes `room_lo`/`room_hi` (route.h:21):

```c
    /* a room in a non-active workspace is not routed to */
    {
        Scene s; Route routes[ROUTE_MAX]; int i, n, touches = 0;
        sol_u32 home, other;
        scene_init(&s);
        home  = add_room(&s, 0.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        other = add_room(&s, 14.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        add_walkway(&s, home, other);                  /* would otherwise route */
        scene_meta_set(&s, other, "workspace", "hidden");
        strcpy(s.active_ws, "home");                   /* ...but other is hidden */
        n = route_all(&s, routes, ROUTE_MAX);
        for (i = 0; i < n; i++)
            if (routes[i].room_lo == other || routes[i].room_hi == other) touches = 1;
        CHECK(!touches);
        scene_free(&s);
    }
```

- [ ] **Step 5: Write the failing collide test.** `collide_test.c` is `-std=c11`, uses inline `printf("FAIL: ...")` + `return 1`, and has helpers `v3(x,y,z)` and `q(x,y,z)` (Euler→quat). Add `#include "scene.h"` to its includes (it currently has only `collide.h`/`flora.h`/`gothic.h`/`sol_math.h`). Add this block in `main()` before the final success `return 0;`. `ColliderSet` has an `int count` (collide.h:51):

```c
    /* a room tagged into a hidden workspace contributes no colliders */
    {
        Scene s; ColliderSet cs; Mesh empty;
        sol_u32 hr, sh, orr, osh; float p[8]; int base, after;
        memset(&empty, 0, sizeof empty);
        p[0]=8;p[1]=8;p[2]=3;p[3]=1;p[4]=1;p[5]=1;p[6]=1;p[7]=0;
        scene_init(&s); collide_set_init(&cs);
        hr = scene_add(&s, 0, empty, v3(0,12,0), q(0,0,0), v3(1,1,1));
        scene_meta_set(&s, hr, "room_type", "home");
        sh = scene_add(&s, hr, empty, v3(0,0,0), q(0,0,0), v3(1,1,1));
        scene_mesh_ref_set(&s, sh, "room"); scene_mesh_params_set(&s, sh, p, 8);
        collide_rebuild(&cs, &s); base = cs.count;
        orr = scene_add(&s, 0, empty, v3(40,12,0), q(0,0,0), v3(1,1,1));
        scene_meta_set(&s, orr, "room_type", "home");
        scene_meta_set(&s, orr, "workspace", "hidden");
        osh = scene_add(&s, orr, empty, v3(0,0,0), q(0,0,0), v3(1,1,1));
        scene_mesh_ref_set(&s, osh, "room"); scene_mesh_params_set(&s, osh, p, 8);
        strcpy(s.active_ws, "home");
        collide_rebuild(&cs, &s); after = cs.count;
        if (after != base) { printf("FAIL: a hidden-workspace room must not collide\n"); return 1; }
        printf("hidden-workspace collide filter: OK\n");
        collide_set_free(&cs); scene_free(&s);
    }
```

- [ ] **Step 6: Run both suites.** `./build.sh routetest && ./route_test` → `route_test: OK`. `./build.sh coltest && ./collide_test` → prints `hidden-workspace collide filter: OK` among its lines and exits 0. Then `./build.sh c89check` → PASS. (Until the filters were wired, both would have failed to link on `scene_object_active` — an acceptable RED.)

- [ ] **Step 7: Commit**

```bash
git add collide.c route.c collide_test.c route_test.c build.sh
git commit -m "$(printf 'Portals: active-workspace filter in collide + route\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 7: the active filter — render, BVH, connections (main.c)

**Files:**
- Modify: `main.c`

These loops have no headless test (they need the window); they are verified by the gauntlet building clean and by the human live-verify at the end. Add `#include "workspace.h"` to `main.c`'s includes.

- [ ] **Step 1: Filter the BVH build (pick + cull).** In `bvh_refresh` (`main.c:2967`), in the gather loop (`main.c:2982`), right after `if (o->mesh.index_count == 0) continue;`, add:

```c
        if (!scene_object_active(s, o->handle)) continue;   /* hidden workspace */
```

- [ ] **Step 2: Filter the visible draw pass.** In the per-object draw loop in `render()` (`main.c:9627`, the `for (i = 0; i < state->scene.count; i++)` whose body increments `state->draws_total`), right after `if (o->mesh.index_count == 0) continue;`, add:

```c
        if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
```

- [ ] **Step 3: Filter the shadow pass.** In `emit_shadow_casters` (`main.c:9337`), in the static-caster loop (`main.c:9341`), right after `if (o->mesh.index_count == 0) continue;` (`main.c:9344`), add the same guard:

```c
        if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
```

Also add the identical guard to the two skinned loops that iterate `state->scene` (the skinned visible loop at `main.c:9673` and the skinned shadow loop at `main.c:9358`), right after each obtains its `o`, so a skinned prop in a hidden workspace can't leak. (Instanced FIELD ornament derives from source objects already covered; no extra guard needed.)

- [ ] **Step 4: Filter the room-shell rebuild.** In `connections_rebuild` (`main.c:4100`), in the room loop (the `for (j ...)` that checks `room_type`), after the `room_type` checks (`if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;`), add:

```c
        if (!scene_object_active(s, room->handle)) continue;
```

(Walkways already follow `route_all`, which Task 6 filtered, so they need no extra guard.)

- [ ] **Step 5: Set the active workspace at startup + register home.** Find where the scene is first brought up in `init_scene`/`main` (after `load_palace` succeeds or `populate_home_scene` runs). Set the default filter and register the home anchor so "Portal to" can list it:

```c
    strcpy(state->scene.active_ws, "home");
    workspace_anchor_add(&state->scene, "home");   /* enumerable home (idempotent) */
```

Also: in `load_palace` (`main.c:8190`), the swap installs a `fresh` scene whose `active_ws` is `""` (the reload would un-filter you). Preserve the current workspace across the swap — declare `keep_ws` with the function's other locals (C89), capture before the swap, restore after the rebuilds:

```c
    char keep_ws[SOL_WS_NAME_CAP];
    /* before `old = st->scene;` */
    strcpy(keep_ws, st->scene.active_ws[0] ? st->scene.active_ws : "home");
    /* after `st->scene = fresh;` and the rebuilds, before return */
    strcpy(st->scene.active_ws, keep_ws);
```

- [ ] **Step 5: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal`. Expected: all clean. (Behavior is verified at the end; here we only ensure it compiles and the existing world still renders — nothing is tagged yet, so `active_ws="home"` shows everything, unchanged.)

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "$(printf 'Portals: active-workspace filter in render, BVH, connections\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 8: the active filter — editor (tested)

**Files:**
- Modify: `editor.c`
- Modify: `editor_test.c`
- Modify: `build.sh` (`editortest`, `descendtest` link lines += `workspace.c`)

- [ ] **Step 1: Write the failing test.** `editor_test.c` has `add_room(Scene*, x, y, z, w, d)` and the `CHECK`/`fails` macro + `<string.h>`. The public, headless seam for the filter is `editor_can_connect` (you cannot wire a walkway to a hidden room). Add this block in `main()` before `if (fails == 0) printf("editor_test: OK\n");`:

```c
    /* the editor refuses to connect a room in a non-active workspace */
    {
        Scene s; sol_u32 a, b;
        scene_init(&s);
        a = add_room(&s, 0.0f,  12.0f, 0.0f, 8.0f, 8.0f);
        b = add_room(&s, 14.0f, 12.0f, 0.0f, 8.0f, 8.0f);
        CHECK(editor_can_connect(&s, a, b));         /* both visible: connectable */
        scene_meta_set(&s, b, "workspace", "hidden");
        strcpy(s.active_ws, "home");
        CHECK(!editor_can_connect(&s, a, b));        /* b hidden: refused */
        scene_free(&s);
    }
```

- [ ] **Step 2: Wire it (two spots).** In `editor.c`, add `#include "workspace.h"`. (a) In `editor_can_connect` (`editor.c:68`), after the two `room_type` checks (`editor.c:71-72`), add:

```c
    if (!scene_object_active(s, a) || !scene_object_active(s, b)) return SOL_FALSE;
```

(b) In the room hit-test loop (`editor.c:184`), after `if (!scene_meta_get(s, o->handle, "room_type")) continue;`, add:

```c
        if (!scene_object_active(s, o->handle)) continue;   /* hidden workspace */
```

- [ ] **Step 3: Update build link lines.** In `build.sh`, append `workspace.c` to the `editortest` AND `descendtest` blocks (descend_test links `editor.c`, which now references `workspace_of`).

- [ ] **Step 4: Run.** `./build.sh editortest && ./editor_test` and `./build.sh descendtest && ./descend_test`. Expected: both `OK`. Then `./build.sh c89check`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add editor.c editor_test.c build.sh
git commit -m "$(printf 'Portals: active-workspace filter in the editor\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 9: creation commands — "New workspace…" + "Portal to…"

**Files:**
- Modify: `main.c`

Both palette-only commands. They author gates via the Task 3 module, spawn the outbound gate in front of the player, rebuild, and save. GUI-verified.

- [ ] **Step 1: Add the gate-spawn helper + the two callbacks.** Near `create_root_from_path`/`cmd_new_root` (`main.c:6685`), add. The outbound gate sits in front of the player, FACING the player (so you walk into it). The player's eye is `st->camera.pos`; floor is `pos.y - CAMERA_EYE_HEIGHT`. The gate's facing is the camera yaw + π (toward the player). Note `workspace_add_gate` writes `meta["yaw"]` (Task 4); pass the gate's own facing:

```c
/* the world position + facing for an outbound gate placed in front of the
   player. The gate faces the player (yaw + PI) so you walk INTO it. */
static void outbound_gate_placement(AppState *st, vec3 *pos, float *yaw) {
    vec3  f = camera_forward(&st->camera);
    float fl = (float)sqrt((double)(f.x * f.x + f.z * f.z));
    vec3  fwd = (fl > 1e-4f) ? vec3_make(f.x / fl, 0.0f, f.z / fl)
                             : vec3_make(0.0f, 0.0f, 1.0f);
    pos->x = st->camera.pos.x + fwd.x * 3.0f;
    pos->z = st->camera.pos.z + fwd.z * 3.0f;
    pos->y = st->camera.pos.y - CAMERA_EYE_HEIGHT;     /* floor level */
    *yaw   = st->camera.yaw + (float)SOL_PI;            /* face the player */
}

/* "New workspace <name>": a fresh empty home world + a linked gate pair. */
static void create_workspace_from_name(AppState *st, const char *name) {
    vec3  gpos, hroom;
    float gyaw;
    const char *cur = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    if (!name || name[0] == '\0') { printf("workspace: empty name\n"); return; }
    if (workspace_anchor_find(&st->scene, name) != 0 ||
        strcmp(name, cur) == 0) { printf("workspace '%s' already exists\n", name); return; }
    workspace_anchor_add(&st->scene, name);
    /* the new world's home room, placed far from home so the two never overlap
       even if both somehow shown; hidden while inactive anyway. */
    hroom = vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    workspace_add_home_room(&st->scene, name, hroom);
    outbound_gate_placement(st, &gpos, &gyaw);
    /* return gate sits just inside the new home room, facing into it */
    workspace_link(&st->scene, cur, gpos, gyaw,
                   name, vec3_make(hroom.x, hroom.y, hroom.z + 3.0f), 0.0f);
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    scene_save(&st->scene, "scene.stml");
    printf("new workspace '%s' — step through the gate\n", name);
}

static void cmd_new_workspace(AppState *st) {
    palette_prompt(&st->palette, "new workspace name", create_workspace_from_name);
}

/* "Portal to <name>": a gate to an EXISTING workspace. */
static void portal_to_named(AppState *st, const char *name) {
    vec3  gpos, hroom; float gyaw;
    const char *cur = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    if (!name || name[0] == '\0') return;
    if (workspace_anchor_find(&st->scene, name) == 0 && strcmp(name, "home") != 0) {
        printf("no workspace '%s'\n", name); return;
    }
    if (strcmp(name, cur) == 0) { printf("already in '%s'\n", name); return; }
    outbound_gate_placement(st, &gpos, &gyaw);
    /* the return gate lands near the target's home room (or origin if unknown) */
    {
        sol_u32 hr = 0, i;
        for (i = 0; i < st->scene.count; i++) {
            if (strcmp(workspace_of(&st->scene, st->scene.objects[i].handle), name) == 0 &&
                scene_meta_get(&st->scene, st->scene.objects[i].handle, "room_type")) {
                hr = st->scene.objects[i].handle; break;
            }
        }
        hroom = hr ? object_world_pos(&st->scene, hr) : vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    }
    workspace_link(&st->scene, cur, gpos, gyaw,
                   name, vec3_make(hroom.x, hroom.y, hroom.z + 3.0f), 0.0f);
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    scene_save(&st->scene, "scene.stml");
    printf("portal to '%s' opened\n", name);
}

static void cmd_portal_to(AppState *st) {
    palette_prompt(&st->palette, "portal to workspace", portal_to_named);
}
```

(Confirm `SOL_PI` exists — `grep -n "SOL_PI" sol_math.h`; if it's `SOL_PI` use it, else use `3.14159265358979f`.)

- [ ] **Step 2: Register the commands.** In `g_commands[]` (`main.c:6816`), add two palette-only rows (key 0, hint NULL) near the other "Mint …" palette-only rows:

```c
    { "New workspace",               NULL, 0, cmd_new_workspace,    NULL,                  SOL_FALSE },
    { "Portal to",                   NULL, 0, cmd_portal_to,        NULL,                  SOL_FALSE },
```

- [ ] **Step 3: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal`. Expected: all clean.

- [ ] **Step 4: Commit**

```bash
git add main.c
git commit -m "$(printf 'Portals: New-workspace + Portal-to palette commands\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Task 10: travel — per-frame trigger, switch, spawn, debounce

**Files:**
- Modify: `main.c`

GUI-verified. This is the payoff: walking into a gate moves you to its paired world.

- [ ] **Step 1: Factor `world_rebuild`.** In `main.c`, just above `load_palace` (`main.c:8190`), add a helper that runs the derived-geometry rebuild tail WITHOUT loading a file (the subset of `load_palace` that re-derives meshes/colliders/scenery/materials):

```c
/* Re-derive everything that hangs off the scene spine after the ACTIVE set
   changes (a workspace switch) — no file load, no glb reimport. The shared
   tail of load_palace. */
static void world_rebuild(AppState *st) {
    scene_resolve_meshes(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    meadow_rebuild(st);
    forest_rebuild(st);
    apply_kind_materials(&st->scene);
}
```

(If `meadow_rebuild`/`forest_rebuild` are declared later than this point, add forward declarations near the other forward decls around `main.c:4390`, or place `world_rebuild` after their definitions — pick whichever keeps it compiling.)

- [ ] **Step 2: Add the debounce field.** In the `AppState` struct (search `struct AppState`/`current_room;`), add:

```c
    sol_u32     portal_debounce;   /* gate just arrived at: ignore until you leave it */
```

Initialize it to 0 where the other handles are nulled (near `st->current_room = 0;`, `main.c:8141`).

- [ ] **Step 3: Add the travel function.** Near `world_rebuild`/`load_palace`, add:

```c
#define PORTAL_TRIGGER_R 1.1f    /* walk within this of a gate's mouth to travel */

/* Switch to the workspace gate `g` leads to and set the camera at its partner. */
static void portal_travel(AppState *st, sol_u32 g) {
    const char *target = scene_meta_get(&st->scene, g, "target_ws");
    const char *retid  = scene_meta_get(&st->scene, g, "target_portal_id");
    sol_u32     ret;
    vec3        pos; float yaw;
    if (!target || !retid) return;
    scene_save(&st->scene, "scene.stml");           /* persist the world you leave */
    strcpy(st->scene.active_ws, target);
    world_rebuild(st);
    ret = workspace_find_gate_by_id(&st->scene, retid);
    if (ret != 0) {
        workspace_spawn_at_gate(&st->scene, ret, 1.5f, CAMERA_EYE_HEIGHT, &pos, &yaw);
        st->camera.pos = pos;
        st->camera.yaw = yaw;
        st->portal_debounce = ret;                  /* don't bounce back through it */
    } else {
        st->portal_debounce = 0;                    /* fallback: stay put */
    }
    st->current_room = room_containing(&st->scene, st->camera.pos);
    printf("entered workspace '%s'\n", target);
}

/* Per-frame: if not carrying, fire the first active gate whose mouth you're in
   (skipping the one you just arrived at until you step away from it). */
static void portal_update(AppState *st) {
    sol_u32 i;
    if (st->carried != 0) return;                   /* hands full: no travel */
    /* clear the debounce once you've stepped clear of the arrival gate */
    if (st->portal_debounce != 0) {
        vec3 gp = object_world_pos(&st->scene, st->portal_debounce);
        float dx = st->camera.pos.x - gp.x, dz = st->camera.pos.z - gp.z;
        if ((float)sqrt((double)(dx*dx + dz*dz)) > PORTAL_TRIGGER_R * 1.6f)
            st->portal_debounce = 0;
    }
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        vec3  gp; float dx, dz, dy;
        if (o->kind != KIND_PORTAL) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;
        if (o->handle == st->portal_debounce) continue;
        gp = object_world_pos(&st->scene, o->handle);
        dx = st->camera.pos.x - gp.x; dz = st->camera.pos.z - gp.z;
        dy = st->camera.pos.y - (gp.y + CAMERA_EYE_HEIGHT);   /* gate base vs eye */
        if ((float)sqrt((double)(dx*dx + dz*dz)) < PORTAL_TRIGGER_R &&
            dy > -1.0f && dy < 2.5f) {               /* roughly at the mouth's height */
            portal_travel(st, o->handle);
            return;                                  /* one switch per frame */
        }
    }
}
```

- [ ] **Step 4: Call it each frame.** In the main loop, after camera movement is applied and `st->current_room` is refreshed (`grep -n "room_containing" main.c` — find the per-frame `st->current_room = room_containing(...)` call in the loop body; the init at `main.c:8832` is NOT it), add right after:

```c
        portal_update(st);
```

- [ ] **Step 5: Build the gauntlet.** `./build.sh c89check && ./build.sh debug && ./build.sh metal`. Expected: all clean.

- [ ] **Step 6: Commit**

```bash
git add main.c
git commit -m "$(printf 'Portals: walk-through travel, paired-gate spawn, debounce\n\nCo-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>')"
```

---

## Final verification (human live-verify — NOT a subagent step)

Subagents cannot drive the window; after Task 10, Fran verifies on BOTH backends (`./build.sh debug` then run; `./build.sh metal` then run):

1. `:` → "New workspace" → type a name → a glowing gate appears in front of you.
2. Walk through it → you arrive in an empty home room, a return gate beside you; the previous world is gone (no ghost rooms/walls/cards).
3. Walk back through the return gate → you're home again, where you started.
4. `:` → "Portal to" → an existing workspace name → a gate to it appears; it travels there.
5. Pick up a card (`E`) and try to walk through a gate → travel is refused until you put it down.
6. Reload (`L`) keeps you in your current workspace; save/quit/relaunch restores it.
7. The editor (top-down) and pathing show only the active workspace's rooms.

Then: `superpowers:finishing-a-development-branch` to merge `portals-workspaces` → `main` (ff-merge), and update memory (`spatial-filesystem-direction.md` + `MEMORY.md`).

---

## Notes carried from spec §11 (resolved here)

- **How the active name reaches `Scene*`-only readers:** a runtime `Scene.active_ws` field + the `scene_object_active` predicate (Task 2) — zero signature churn, lowest chance a reader "forgets the filter."
- **`world_rebuild` factoring:** Task 10 Step 1 — the `load_palace` tail minus `scene_load`/`scene_reimport_glbs`/`bind_runtime_handles`.
- **Trigger dims & stand-off:** `PORTAL_TRIGGER_R = 1.1` horizontal, mouth height band `[-1.0, 2.5]`; arrival stand-off `1.5` m (Task 10).
- **Gate mesh name is `"gate"`** (not `"portal"` — that's the gothic arch at `mesh.c:981`).

# Timber Halls — Stage 1: Taller Walls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make all rooms 50% taller (3.0 → 4.5 m) — the foundation for the timber-frame halls — via an idempotent migration of existing rooms plus new-room defaults, with the headroom heuristics following.

**Architecture:** A one-time idempotent migration in `load_palace` bumps any room still at the old 3.0 m default to 4.5 m (a room's height is `mesh_params[2]` in the `{w,d,h,…}` schema). The three new-room creation sites default to 4.5; the hardcoded headroom fallbacks/heuristics bump to match. No new geometry — `make_room_doored`, collision, doorways, and the wall-plank overlay all read the room height and auto-adapt.

**Tech Stack:** C89; the existing room schema + `load_palace` + collision.

**Branch:** `timber-halls-stage1` (create at start; ff-merge to `main` at the end).

**C89 reminders:** decls at top of block; `/* */`; `fabs((double)x)` not `fabsf`; c89check is `-Wall -Wextra -Werror`. **Never commit** `NOTES.stml`/`paper-picture.png`. Commit trailer: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**No unit test:** this is a scene-data migration + constants — verification is the build gauntlet + human live-verify (rooms visibly taller, collision headroom correct, reload idempotent). The room height is `mesh_params[2]` because the `"room"` mesh schema (mesh.c:1004) is `{ "w", "d", "h", "wn", "we", "ws", "ww", "ceil" }`.

---

### Task 0: Branch

- [ ] **Step 1:** `git checkout -b timber-halls-stage1`

---

### Task 1: The height migration + defaults + headroom

**Files:** Modify `main.c`.

- [ ] **Step 1: Add the migration helper**

Add this static function immediately ABOVE `static sol_bool load_palace(AppState *st) {` (grep it; ~main.c:10342):

```c
/* timber halls (stage 1): one-time idempotent room-height migration. A room
   still at the old 3.0m default becomes 4.5m; a deliberately-resized height is
   left alone, and re-running on a 4.5m room is a no-op (4.5 != 3.0). Height is
   mesh_params[2] in the room schema {w,d,h,...}. */
static void migrate_room_heights(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->mesh_ref && strcmp(o->mesh_ref, "room") == 0 &&
            o->mesh_param_count >= 3 &&
            fabs((double)(o->mesh_params[2] - 3.0f)) < 0.01) {
            o->mesh_params[2] = 4.5f;
        }
    }
}
```

- [ ] **Step 2: Call it in `load_palace` before the derives**

In `load_palace`, after `strcpy(st->scene.active_ws, keep_ws);` (~main.c:10362) and BEFORE `scene_reimport_glbs(st);`, insert:

```c
    migrate_room_heights(&st->scene);   /* timber halls: 3.0m rooms -> 4.5m (idempotent) */
```

(This runs after the scene swap + filter restore, before `scene_resolve_meshes`/`connections_rebuild`/`collide_rebuild`, so every derived mesh + collider is built at the new height. The bumped `h` persists on the next `scene_save` — idempotent thereafter.)

- [ ] **Step 3: Bump the three new-room height defaults to 4.5**

Each of these is a room's `h` (the `[2]` of a `{w,d,h,...}` param array). Change the `3.0f` to `4.5f` at each (confirm each line is a room param block by reading it):
- `home_p[2] = 3.0f;` → `home_p[2] = 4.5f;` (~main.c:10410, the home build)
- `room_p[2] = 3.0f;` → `room_p[2] = 4.5f;` (~main.c:8073, where `room_p[0]/[1]` are `10.0f`)
- `p[2] = 3.0f;` → `p[2] = 4.5f;` (~main.c:8321, where `p[0]/[1]` are `8.0f` and `p[3] = 1.0f` follows — the add-room free room)

Do NOT touch `lp[2] = 3.0f` (main.c:3635 — that's not a room) or the `z + 3.0f` position offsets (8192/8227).

- [ ] **Step 4: Bump the headroom heuristic + fallbacks**

(a) The load-bearing one — the "clear in Y" placement filter at ~main.c:8009:
```c
        if ((c.y > p.y ? c.y - p.y : p.y - c.y) >= 3.5f) continue;   /* clear in Y (room height 3.0 + 0.5 gap) */
```
→
```c
        if ((c.y > p.y ? c.y - p.y : p.y - c.y) >= 5.0f) continue;   /* clear in Y (room height 4.5 + 0.5 gap) */
```

(b) The two `3.0f` fallbacks (used only when a room has no `h` param — belt-and-suspenders so a height-less room still reads tall):
- In `room_interior_height` (grep `static float room_interior_height`, ~main.c:7728) the fallback return value `3.0f` (the comment says "default 3.0 if none") → `4.5f`.
- The `rh = 3.0f;` at ~main.c:7955 (comment `/* room interior height (default) */`) → `rh = 4.5f;`.

- [ ] **Step 5: Scan for any other room-height assumption**

Run: `grep -n "3\.0f\|3\.5f" main.c | grep -iE "room|ceil|height|headroom|interior"` and read each hit. Confirm there is no OTHER hardcoded room-height constant left at 3.0/3.5 that a taller room would break (the known ones are handled in Steps 3-4). If you find one that's clearly a room-height assumption, bump it and note it in your report; if it's unrelated, leave it.

- [ ] **Step 6: Build both backends**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal`
Expected: all PASS. (No new geometry; `make_room_doored`/collision read the height. If a build fails, fix minimally or report BLOCKED with the exact error.)

- [ ] **Step 7: Commit**

```bash
git add main.c
git commit -m "$(cat <<'EOF'
Timber halls stage 1: taller walls (rooms 3.0m -> 4.5m)

Idempotent migration in load_palace bumps existing default-height rooms to 4.5m;
the three new-room defaults and the headroom heuristic/fallbacks follow. No new
geometry -- doored walls, collision, doorways, and the wall-plank overlay all
read the room height and adapt. Foundation for the timber-frame halls.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Live-verify + finish

- [ ] **Step 1: Full gauntlet**

```bash
./build.sh c89check && ./build.sh debug && ./build.sh metal
```
Expected: all PASS.

- [ ] **Step 2: Human live-verify (Fran)** — on `./solarium` and `./solarium-metal`: every room is visibly ~50% taller; the wall planks tile up the taller walls; doorways are intact; you don't bonk your head early (collision headroom matches the new height); jumping/movement feels right. Mint a fresh room (`:` Add room / descend a folder) → it's also tall. Quit + relaunch → rooms stay 4.5 m (migration persisted, idempotent — no compounding to 6.75 m).

- [ ] **Step 3: Finish** — use superpowers:finishing-a-development-branch; ff-merge `timber-halls-stage1` to `main` (or per Fran's call). Do NOT stage `NOTES.stml`/`paper-picture.png`.

---

## Plan self-review

**Spec coverage (Stage 1 section of the timber-frame-halls spec):** idempotent 3.0→4.5 migration on load ✓ (Task 1 Steps 1-2); new-room defaults → 4.5 ✓ (Step 3); headroom heuristic + fallbacks follow ✓ (Step 4); flat ceiling left through stages 1-3 (untouched here) ✓; collision auto-follows (reads room height) — the one hardcoded heuristic bumped ✓; verify rooms tall + idempotent reload ✓ (Task 2). No frame geometry (correctly out of Stage 1).

**Placeholder scan:** none — exact anchors + the grep-and-confirm step for the two approximate fallbacks.

**Type consistency:** `migrate_room_heights(Scene *)` uses `o->mesh_ref`/`o->mesh_params[2]`/`o->mesh_param_count` (the real `SceneObject` fields, scene.h:60-61). `mesh_params[2]` = room height is consistent throughout. The `4.5f` target and the `< 0.01` idempotency epsilon are used consistently.

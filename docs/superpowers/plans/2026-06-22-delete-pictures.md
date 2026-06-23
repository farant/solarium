# Delete Pictures Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user delete a selected image (a "picture", wall-mounted or pinned to a whiteboard) by pressing Delete or Backspace in first-person.

**Architecture:** Extend the existing selected-object delete handler in `read_input` (`main.c:8382`–`8406`), which already removes a selected tombstone or arrow. Broaden its trigger from Backspace-only to Delete-or-Backspace, and add one `else if` branch that removes a selected object whose `mesh_ref == "picture"`: release its mesh asset, clear any transient drag/resize references, `scene_remove`, and save. No new files, no new shader, no new test target.

**Tech Stack:** C89, GLFW key polling, the project's RHI/asset/scene layers. Build gauntlet: `./build.sh c89check && ./build.sh debug && ./build.sh metal`.

---

## Context the implementer needs

- **This is a GUI input feature.** There is no pure function to unit-test (consistent with every prior carry/place/file feature in this repo). "Verification" = the build gauntlet passes on both backends, then a human live-verifies. Do **not** invent a `*_test` target.
- **The block you are editing** is in `read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st)`. `read_input` early-returns at `main.c:7398` when `st->edit_handle != 0 || st->palette.open`, so this block already cannot fire while typing note text or with the palette open. You are reusing that protection, not adding to it.
- **A picture is** `KIND_PLAIN` with `o->mesh_ref` (a `char *`, NULL = none) equal to `"picture"`. Its parent is a room (wall-mounted) or a board (pinned); both are covered by the single `mesh_ref` check. Pictures are the only objects created with that ref.
- **Asset cleanup mirrors the tombstone branch:** `mesh_asset_key(o, key)` fills `key` and returns true when the mesh is keyed; then `asset_release(&g_mesh_assets, key)` drops the GPU buffer refcount (identical shared pictures survive). Do **not** free the texture — `load_texture` returns a path-cached, refcounted, hot-reload-watched asset that is session-lived in this codebase.
- **Transient references to clear** so a stale drag/resize cannot dereference a removed object: `st->selected_handle`, and (if equal to the deleted handle) `st->resize_board` and `st->move_board`. Field declarations: `selected_handle`, `resize_board` (`main.c:2688`), `move_board` (`main.c:2699`).
- **No `arrows_rebuild` and no `scene_resolve_meshes`/`apply_kind_materials`** are needed: nothing connects to a picture, and removal doesn't change any other object's geometry or material.

---

### Task 1: Delete a selected picture on Delete/Backspace

**Files:**
- Modify: `main.c` — the delete block currently at lines `8382`–`8406` (inside `read_input`).

- [ ] **Step 1: Re-read the exact current block before editing**

Open `main.c` and confirm the block reads exactly as below (line numbers may have drifted; match on the text). This is the current code:

```c
    /* Backspace dismisses a selected TOMBSTONE — manual, deliberate (the 6c
       decision): the system never throws away the marker for you. Item 8
       extends it to ARROWS: deleting the edge object deletes the relation. */
    {
        sol_bool bs_now = glfwGetKey(w, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
        if (bs_now && !st->bs_was_down && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_TOMBSTONE) {
                char        lbuf[16];
                char        akey[160];
                const char *label = object_label(&st->scene, st->selected_handle, lbuf);
                printf("dismissed tombstone: %s\n", label);
                if (mesh_asset_key(o, akey))           /* its shape goes back (P4 i4) */
                    asset_release(&g_mesh_assets, akey);
                scene_remove(&st->scene, st->selected_handle);
                st->selected_handle = 0;
                arrows_rebuild(st);            /* edges to the dead card go dormant */
                scene_save(&st->scene, "scene.stml");
            } else if (o && object_is_arrow(&st->scene, st->selected_handle)) {
                mesh_destroy(&o->mesh);        /* derived GPU buffers, freed now */
                scene_remove(&st->scene, st->selected_handle);
                st->selected_handle = 0;
                scene_save(&st->scene, "scene.stml");
                printf("removed arrow — the connection is gone\n");
            }
        }
        st->bs_was_down = bs_now;
    }
```

- [ ] **Step 2: Broaden the trigger to Delete OR Backspace**

Replace this line:

```c
        sol_bool bs_now = glfwGetKey(w, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
```

with:

```c
        sol_bool bs_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_BACKSPACE) == GLFW_PRESS ||
                                     glfwGetKey(w, GLFW_KEY_DELETE)    == GLFW_PRESS);
```

(The edge-tracker `st->bs_was_down` and the comment above the block stay as-is — the comment still describes the tombstone/arrow intent, which is unchanged.)

- [ ] **Step 3: Add the picture-delete branch**

Insert a new `else if` branch after the arrow branch — i.e. between the arrow branch's closing `}` and the `}` that closes the `if (bs_now ...)`. The result reads:

```c
            } else if (o && object_is_arrow(&st->scene, st->selected_handle)) {
                mesh_destroy(&o->mesh);        /* derived GPU buffers, freed now */
                scene_remove(&st->scene, st->selected_handle);
                st->selected_handle = 0;
                scene_save(&st->scene, "scene.stml");
                printf("removed arrow — the connection is gone\n");
            } else if (o && o->mesh_ref != NULL &&
                       strcmp(o->mesh_ref, "picture") == 0) {
                /* a placed image (on a wall or pinned to a whiteboard): remove
                   the display copy. The file card already returned to its shelf
                   when the picture was planted, and the texture is a shared,
                   session-lived asset — only the per-object mesh buffer goes. */
                char akey[160];
                sol_u32 doomed = st->selected_handle;
                if (mesh_asset_key(o, akey))
                    asset_release(&g_mesh_assets, akey);
                st->selected_handle = 0;
                if (st->resize_board == doomed) st->resize_board = 0;
                if (st->move_board   == doomed) st->move_board   = 0;
                scene_remove(&st->scene, doomed);
                scene_save(&st->scene, "scene.stml");
                printf("deleted picture #%u\n", (unsigned)doomed);
            }
```

Note: `o` is captured via `scene_get` at the top of the block and must not be dereferenced after `scene_remove`. The branch above reads everything it needs from `o` (`mesh_ref`, the asset key) **before** the `scene_remove`, and uses the saved `doomed` handle afterward.

- [ ] **Step 4: c89 conformance check**

Run: `./build.sh c89check`
Expected: PASS (no declaration-after-statement, no `//` comments, no warnings-as-errors failures). The new branch declares `char akey[160];` and `sol_u32 doomed` at the top of its block before any statement — verify that ordering held after your edit.

- [ ] **Step 5: Debug (GL) build**

Run: `./build.sh debug`
Expected: PASS, clean link.

- [ ] **Step 6: Metal build**

Run: `./build.sh metal`
Expected: PASS. (No shader changed, so there is no MSL twin to worry about; this just confirms the C/ObjC compiles under the Metal backend.)

- [ ] **Step 7: Human live-verify (hand off to Fran)**

Subagents cannot GUI-test. Hand these steps to Fran and wait for confirmation before committing:
1. Place an image on a wall; tap it to select (selection blip + console "picked" line); press **Delete** → the picture disappears, console prints "deleted picture #N", `scene.stml` is saved, and the image's file card is still on its shelf.
2. Reload the scene → the deleted picture stays gone.
3. Pin an image to a whiteboard; select it; press **Delete** → same result.
4. Press **Backspace** on a selected picture → also deletes (parity with Delete).
5. Regression: a selected tombstone and a selected arrow still delete (via both keys); typing in a note's text and the RTS editor's walkway-delete are unaffected.

- [ ] **Step 8: Commit**

Only after Fran confirms live-verify passes. Stage `main.c` only — never `NOTES.stml` or `paper-picture.png`.

```bash
git add main.c
git commit -m "$(cat <<'EOF'
delete-pictures: Delete/Backspace removes a selected image

Extends the tombstone/arrow delete block in read_input: a selected object
with mesh_ref "picture" (wall-mounted or pinned to a whiteboard) is removed
on Delete or Backspace. Releases the mesh asset, clears resize/move refs,
scene_remove + save. Non-destructive: the file card and the shared,
session-lived texture are untouched. Images only; no new shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

- **Spec coverage:**
  - "Select + Delete/Backspace removes a picture" → Steps 2–3.
  - "Both wall-mounted and board-pinned" → single `mesh_ref == "picture"` guard (Step 3); parent is irrelevant to the check.
  - "Release mesh asset, not the texture" → Step 3 (`asset_release`; texture left resident, per Context).
  - "Clear transient drag/resize state" → Step 3 (`resize_board`/`move_board`/`selected_handle`).
  - "Non-destructive / no arrows_rebuild / no re-resolve" → Context + Step 3 (omitted deliberately).
  - "Fire on Delete or Backspace" → Step 2.
  - "Inherits mode-guards" → Context (the `read_input` early-return at `main.c:7398`).
  - "Build gauntlet both backends; no new test target" → Steps 4–6.
  - "Human live-verify; never commit NOTES.stml/paper-picture.png; Co-Authored-By line" → Steps 7–8.
- **Placeholder scan:** none — every code step shows complete code and exact commands.
- **Type/name consistency:** `mesh_asset_key(const SceneObject*, char*)` → `akey[160]` matches the tombstone branch's `akey[160]`; `asset_release(&g_mesh_assets, akey)`, `scene_remove(Scene*, sol_u32)`, `o->mesh_ref` (`char *`), `st->resize_board`/`st->move_board` (`sol_u32`) all match their declarations.

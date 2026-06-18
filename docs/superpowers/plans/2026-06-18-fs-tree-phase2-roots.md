# Spatial Filesystem Tree — Phase 2: Roots + a Directory's Room Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `New root…` palette command prompts for a directory path, then creates a floating room for that directory (near home) filled with its files/folders as cards.

**Architecture:** Add a reusable **prompt mode** to the palette (collect one typed line, fire a callback on Enter — reuses the palette's text field + routing). A `cmd_new_root` opens that prompt; its callback `create_root_from_path` builds a `room_type="mirror"` room (the existing `"room"` shell), fills it with `room_mirror_scan` (the existing file-card scan), authors a `connects` edge home→root (for the Phase-3 walkway), and resolves/collides/saves.

**Tech Stack:** Strict C89, the command palette (just built), `room_mirror_scan` (existing mirror file-scan), the parametric `"room"` mesh + scene builder helpers.

**Spec:** `docs/superpowers/specs/2026-06-18-spatial-filesystem-tree-design.md` (Phase 2 of the phasing). Reaching the new room is by **fly (`F`) for now** — real walkways are **Phase 3**; the carry-able door-seed *tablet* + descent are **Phase 4**. Phase 2 = roots + a dir-room of cards, end to end.

**Conventions:** strict C89 (the c89check gate compiles with `-std=c89 -pedantic-errors -Werror`): declarations at top of block, `/* */` only, no mixed decl/code, **ASCII-only source**. Commit on a branch, end messages with the `Co-Authored-By: Claude Opus 4.8 (1M context)` line. Never stage `NOTES.stml` or `paper-picture.png`.

**Testing note:** palette/scene interaction with no isolatable pure unit → verification is the **build gauntlet + live-verify** (deferred to the human). No new headless test.

**Key facts (verified):**
- Palette: `Palette { open, query[128], len, sel, eat_char }` (palette.h:14); `palette_open_now`/`palette_input_char`/`palette_input_key`/`palette_draw` (palette.c). ENTER runs the selected command at palette.c:80-88. `AppState.palette` is a direct member (main.c:2676); `cmd_*(AppState*)` reach it as `&st->palette`.
- `int room_mirror_scan(Scene *s, sol_u32 room, const char *dirpath)` (mirror.h:24): adds FILE/FOLDER cards (kind, `content`=path, name=basename, `mesh_ref="card"`) as children of the room's tray; returns #changes or -1 (dir won't open). Caller must then `scene_resolve_meshes` + `apply_kind_materials`.
- Room builder pattern (main.c `populate_home_scene`): `scene_add` anchor (+`room_type`/meta) → `scene_add` shell child (`mesh_ref="room"`, params `[w,d,h, wn,we,ws,ww, ceil]`) → material → resolve. A mirror room also sets `source_path`.
- `object_world_pos(Scene*, handle)` (main.c:2812); no find-room-by-type helper (iterate `scene.objects[i].handle` + `scene_meta_get(...,"room_type")`). `scene_rel_add(Scene*, handle, type, target)`. Basename = `strrchr(path,'/')+1` else path. `apply_kind_materials(Scene*)` + `collide_rebuild(&st->colliders,&st->scene)` exist.
- on_char/on_key already route to `palette_input_char`/`palette_input_key` while `palette.open` (main.c:10085/10103) — prompt mode reuses this.

---

## Task 1: Palette prompt mode

A reusable text-prompt: open the palette to collect one typed line and call a callback on Enter. All in `palette.h` / `palette.c`. A subagent can't GUI-test; verification is `./build.sh c89check && ./build.sh debug && ./build.sh metal`.

**Files:** Modify `palette.h`, `palette.c`.

- [ ] **Step 1: Extend the `Palette` struct + declare `palette_prompt` (palette.h)**

Change the struct (palette.h:14-20) to add three fields:
```c
typedef struct {
    sol_bool open;
    char     query[PALETTE_QUERY_CAP];
    int      len;
    int      sel;       /* highlighted result row */
    sol_bool eat_char;  /* swallow the leading ':' that opened the palette */
    sol_bool prompt;    /* prompt mode: collect a typed line, fire prompt_cb on Enter */
    const char *prompt_label;                         /* shown before the typed text */
    void      (*prompt_cb)(struct AppState *, const char *);
} Palette;
```
Add a declaration after `palette_open_now` (palette.h:31):
```c
/* Open the palette as a text PROMPT: collect one typed line shown after `label`,
   and call cb(st, line) on Enter (Esc cancels). Reuses the palette text field. */
void     palette_prompt(Palette *p, const char *label,
                        void (*cb)(struct AppState *, const char *));
```

- [ ] **Step 2: `palette_open_now` clears prompt mode (palette.c:14-20)**

Add `p->prompt = SOL_FALSE;` so opening via `:` is always command mode:
```c
void palette_open_now(Palette *p) {
    p->open     = SOL_TRUE;
    p->query[0] = '\0';
    p->len      = 0;
    p->sel      = 0;
    p->eat_char = SOL_TRUE;   /* the ':' that opened us arrives next as a char */
    p->prompt   = SOL_FALSE;  /* command mode */
}
```

- [ ] **Step 3: Add `palette_prompt` (palette.c, after `palette_open_now`)**
```c
void palette_prompt(Palette *p, const char *label,
                    void (*cb)(struct AppState *, const char *)) {
    p->open         = SOL_TRUE;
    p->query[0]     = '\0';
    p->len          = 0;
    p->sel          = 0;
    p->eat_char     = SOL_FALSE;   /* no ':' to swallow — a command opened us */
    p->prompt       = SOL_TRUE;
    p->prompt_label = label;
    p->prompt_cb    = cb;
}
```

- [ ] **Step 4: Handle prompt mode in `palette_input_key` (palette.c:63-90)**

Replace the body from the `CANCEL` line through the command `ENTER` block so prompt mode branches first:
```c
sol_bool palette_input_key(Palette *p, PaletteKey k, struct AppState *st,
                           const Command *cmds, int ncmds) {
    int order[PALETTE_MAX_COMMANDS];
    int n;

    if (!p->open) return SOL_FALSE;

    if (k == PALETTE_KEY_CANCEL) {
        p->open = SOL_FALSE; p->prompt = SOL_FALSE;
        return SOL_TRUE;
    }

    if (p->prompt) {                                 /* text-prompt mode */
        if (k == PALETTE_KEY_BACKSPACE) {
            if (p->len > 0) { p->len--; p->query[p->len] = '\0'; }
        } else if (k == PALETTE_KEY_ENTER) {
            p->open = SOL_FALSE; p->prompt = SOL_FALSE;
            if (p->prompt_cb) p->prompt_cb(st, p->query);
        }
        return SOL_TRUE;                             /* ignore UP/DOWN in a prompt */
    }

    n = palette_rank(p, cmds, ncmds, order, PALETTE_MAX_COMMANDS);

    if (k == PALETTE_KEY_DOWN) { if (p->sel + 1 < n) p->sel++; return SOL_TRUE; }
    if (k == PALETTE_KEY_UP)   { if (p->sel > 0)     p->sel--; return SOL_TRUE; }
    if (k == PALETTE_KEY_BACKSPACE) {
        if (p->len > 0) { p->len--; p->query[p->len] = '\0'; p->sel = 0; }
        return SOL_TRUE;
    }
    if (k == PALETTE_KEY_ENTER) {
        p->open = SOL_FALSE;                         /* close first */
        if (n > 0 && p->sel < n) {
            const Command *cmd = &cmds[order[p->sel]];
            if (cmd->can_run == NULL || cmd->can_run(st))
                cmd->run(st);
        }
        return SOL_TRUE;
    }
    return SOL_TRUE;                                  /* swallow anything else */
}
```
(`palette_input_char` needs no change — in prompt mode `eat_char` is FALSE, so typed chars append to `query` exactly as in command mode.)

- [ ] **Step 5: Render prompt mode in `palette_draw` (palette.c:92-154)**

Two changes: skip the command list in prompt mode, and prefix the query row with the label. Replace the `n`/`shown` computation (palette.c:105-106) and the query-row block (palette.c:116-125):

The `n`/`shown` lines become:
```c
    if (p->prompt) {
        n = 0; shown = 0;
    } else {
        n     = palette_rank(p, cmds, ncmds, order, PALETTE_MAX_COMMANDS);
        shown = (n < PALETTE_MAX_ROWS) ? n : PALETTE_MAX_ROWS;
    }
```
The query-row block becomes (handles both modes; the result loop below is skipped when `shown == 0`):
```c
    {   /* query row: command mode ":<typed>_", prompt mode "<label>: <typed>_" */
        char  line[PALETTE_QUERY_CAP + 40];
        float qy = box_y + pad + font_ascent(font) * ts;
        int   ll = 0, q;
        if (p->prompt) {
            const char *lbl = p->prompt_label ? p->prompt_label : "input";
            while (*lbl && ll < (int)sizeof line - 4) line[ll++] = *lbl++;
            line[ll++] = ':';
            line[ll++] = ' ';
        } else {
            line[ll++] = ':';
        }
        for (q = 0; q < p->len && ll < (int)sizeof line - 2; q++)
            line[ll++] = p->query[q];
        line[ll++] = '_';
        line[ll]   = '\0';
        ui_text(font, line, box_x + pad, qy, ts, 0.95f, 0.92f, 0.80f, 1.0f);
    }
```

- [ ] **Step 6: Build gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all must pass. (Prompt mode is unexercised until Task 2; this just confirms it compiles.)

- [ ] **Step 7: Commit**
```bash
git add palette.h palette.c
git commit -m "feat: fs-tree phase 2a — palette text-prompt mode" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: The `New root...` command

`cmd_new_root` opens the prompt; `create_root_from_path` builds the floating mirror room and fills it. All in `main.c`.

**Files:** Modify `main.c`.

- [ ] **Step 1: Add `create_root_from_path` + `cmd_new_root`, just before `static Command g_commands[]`**

Place this with the other `cmd_*` functions (every helper it calls — `scene_add`, `scene_meta_set`, `scene_mesh_ref_set`, `scene_mesh_params_set`, `scene_material_set`, `material_default`, `object_world_pos`, `room_mirror_scan`, `scene_rel_add`, `scene_resolve_meshes`, `apply_kind_materials`, `collide_rebuild`, `scene_save`, `vec3_make`, `vec3_add`, `quat_identity`, `strrchr`, `HOME_FLOOR_Y` — is defined above this point):

```c
/* The palette-prompt callback for "New root...": build a floating mirror room
   for `path`, east of home, and fill it with that directory's file/folder cards.
   Reaching it is by fly (F) for now; Phase 3 generates the walkway. */
static void create_root_from_path(AppState *st, const char *path) {
    Mesh       empty = {0};
    sol_u32    home = 0, root, shell, i;
    int        mirror_count = 0, changed;
    float      room_p[8];
    vec3       home_pos, pos;
    Material   stone = material_default();
    const char *slash, *name;

    if (path == NULL || path[0] == '\0') return;

    /* find the home room + count existing mirror rooms (placement spreads east) */
    for (i = 0; i < st->scene.count; i++) {
        sol_u32     h  = st->scene.objects[i].handle;
        const char *rt = scene_meta_get(&st->scene, h, "room_type");
        if (!rt) continue;
        if      (strcmp(rt, "home")   == 0) home = h;
        else if (strcmp(rt, "mirror") == 0) mirror_count++;
    }
    home_pos = (home != 0) ? object_world_pos(&st->scene, home)
                           : vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    pos = vec3_add(home_pos,
                   vec3_make(14.0f + 14.0f * (float)mirror_count, 0.0f, 0.0f));

    slash = strrchr(path, '/');
    name  = (slash && slash[1]) ? slash + 1 : path;

    /* the root: a floating mirror room (open-topped 10x10) */
    root = scene_add(&st->scene, 0, empty, pos, quat_identity(),
                     vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(&st->scene, root, "room_type",   "mirror");
    scene_meta_set(&st->scene, root, "source_path", path);
    scene_meta_set(&st->scene, root, "name",        name);

    shell = scene_add(&st->scene, root, empty, vec3_make(0.0f, 0.0f, 0.0f),
                      quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, shell, "room");
    room_p[0] = 10.0f; room_p[1] = 10.0f; room_p[2] = 3.0f;
    room_p[3] = 1.0f;  room_p[4] = 1.0f;  room_p[5] = 1.0f;  room_p[6] = 1.0f;
    room_p[7] = 0.0f;
    scene_mesh_params_set(&st->scene, shell, room_p, 8);
    stone.base_color = vec3_make(0.55f, 0.53f, 0.50f);
    stone.roughness  = 0.92f;
    scene_material_set(&st->scene, shell, stone);

    changed = room_mirror_scan(&st->scene, root, path);   /* files -> cards */

    if (home != 0) scene_rel_add(&st->scene, home, "connects", root); /* Phase 3 reads this */

    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    collide_rebuild(&st->colliders, &st->scene);
    scene_save(&st->scene, "scene.stml");

    if (changed < 0)
        printf("new root: couldn't open '%s' (empty room created)\n", path);
    else
        printf("new root '%s': %d item(s)\n", name, changed);
}

/* Palette command: prompt for a directory path, then build a root room for it. */
static void cmd_new_root(AppState *st) {
    palette_prompt(&st->palette, "root path", create_root_from_path);
}
```

- [ ] **Step 2: Register the command in `g_commands[]`**

Add a row (palette-only — no hotkey; you reach it as `:new root`):
```c
    { "New root...",                 NULL, 0,         cmd_new_root,          NULL,                  SOL_FALSE },
```

- [ ] **Step 3: Build gauntlet**

Run: `./build.sh c89check && ./build.sh debug && ./build.sh metal` — all must pass. If c89check flags a name, verify against mirror.h / scene.h / main.c (everything used was verified to exist). If it flags a sign-compare on the loop, note `scene.count` is `sol_u32` so `sol_u32 i` matches (as `rescan_mirrors` does).

- [ ] **Step 4: (Interactive verify — DEFERRED TO HUMAN.)**

- [ ] **Step 5: Commit**
```bash
git add main.c
git commit -m "feat: fs-tree phase 2b — New root command builds a directory's room" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (human, live)

Run `./solarium` (you spawn in the home room):
- [ ] Open the palette (`:`), type `new root`, press Enter → the palette shows a `root path:` prompt.
- [ ] Type a real directory path (e.g. `/Users/francisarant/Documents/projects/solarium` or your home dir) and press Enter.
- [ ] A floating room appears **east of home**; switch to fly mode (`F`) and fly over to it.
- [ ] It's filled with that directory's files/folders as **cards** (in the tray); the console prints `new root '<name>': N item(s)`.
- [ ] A bad path prints `couldn't open ... (empty room created)` and doesn't crash.
- [ ] Create a **second** root → it appears further east (doesn't overlap the first).
- [ ] Quit and relaunch → the root room(s) and their cards persist (loaded from `scene.stml`).
- [ ] `./solarium-metal` → same (no new shader, so parity expected).

## Self-review (writing-plans)

- **Spec coverage (Phase 2 slice):** typed-path root creation (Task 1 prompt mode + Task 2 `cmd_new_root`/`create_root_from_path`), a floating dir-room (Task 2, the `"room"` shell near home), files-as-cards (Task 2 `room_mirror_scan`), home→root `connects` edge for Phase 3 (Task 2). Walkways (Phase 3), tablets/descent (Phase 4) explicitly out of scope.
- **Placeholders:** none — all code concrete; every helper verified to exist.
- **Consistency:** `prompt`/`prompt_label`/`prompt_cb` added in Task 1 and used by `palette_prompt`/`palette_input_key`/`palette_draw`; `cmd_new_root` calls `palette_prompt` with `create_root_from_path` whose signature matches the `prompt_cb` type `void(*)(struct AppState*, const char*)`; the `g_commands` row references `cmd_new_root`.
- **C89/ASCII:** all declarations at block top (incl. the `for`-body `sol_u32 h` and the draw block's `const char *lbl`); `"New root..."` uses ASCII dots, not an ellipsis glyph.

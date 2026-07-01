# Map Pins — Iteration 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to execute this plan task-by-task. Fresh implementer per task, then spec + code-quality review, then human live-verify. Steps use checkbox (`- [ ]`).

**Goal:** Three live-verify refinements to map pins: a pin selects individually (not its whole map), the marker becomes a tip-anchored map-pin teardrop, and "Add place…" gains a "+ New place" row that opens a lat/lon/label form creating a catalog place + pin.

**Architecture:** Item 1 is a one-line `selection_root` exception. Item 2 reshapes `pin_marker_mesh` (tip at local origin, so `resolve_pin`'s existing pos anchors the tip) and bumps the label offset. Item 3 adds a reusable palette **form mode** (labeled fields, Tab/↑↓ nav) plus the Add-place sentinel + a shared `add_pin_to_map` helper.

**Tech Stack:** C89 engine sources (`main.c`, `palette.c/.h`); build via `build.sh`; OpenGL + Metal (no shader change).

---

## Context the implementer needs

Reference the spec: `docs/superpowers/specs/2026-07-01-map-pins-iteration-design.md`.

- **C89** for all `.c`/`.h`: declarations at the TOP of every block, no `//`, no C99/C11.
- **No new shader** → no MSL twin (marker is a scene mesh, label is `wtext_block`, form is `palette.c`).
- **Do NOT run the engine binary** (`./solarium`). Build only.
- **Commit discipline:** `git add` ONLY the named files; NEVER `git add -A`/`.`; NEVER stage `NOTES.stml` or `paper-picture.png`; commit body ends EXACTLY with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- **Gauntlet per task:** `./build.sh`, `./build.sh c89check`, `./build.sh asan`, `./build.sh metal`.
- Facts (verified): `resolve_pin` sets `o->pos = (lx, ly, PIN_Z_OFFSET)` — so a marker whose geometry has its point at local `(0,0)` is automatically tip-anchored on the lat/lon; no `resolve_pin` change is needed for item 2. The pin-label pass runs each frame over `"pin"` objects with a built mesh; a pin's parent is its map.

## File Structure

```
main.c        (Tasks 1,2) selection_root exception; pin_marker_mesh teardrop;
                          label offset; add_pin_to_map + new_place_form_cb +
                          add_place_pin_cb rewrite + cmd_add_place sentinel;
                          GLFW_KEY_TAB -> PALETTE_KEY_TAB routing
palette.h     (Task 2)    PALETTE_KEY_TAB; form fields + palette_form decl
palette.c     (Task 2)    palette_form + form input/draw + clear-on-open/cancel
```

---

## Task 1 — pin polish (select individually + teardrop marker)

**Files:** `main.c`.

### Step 1.1 — `selection_root` gains a `"pin"` exception

Find:
```c
    if (o->mesh_ref && strcmp(o->mesh_ref, "arrow") == 0)
        return handle;            /* an edge is its own thing: select it, not
                                     the board group it hangs on (item 8) */
    return group_root(s, handle);
```
Replace with:
```c
    if (o->mesh_ref && strcmp(o->mesh_ref, "arrow") == 0)
        return handle;            /* an edge is its own thing: select it, not
                                     the board group it hangs on (item 8) */
    if (o->mesh_ref && strcmp(o->mesh_ref, "pin") == 0)
        return handle;            /* a map pin selects individually, not its map */
    return group_root(s, handle);
```

### Step 1.2 — reshape `pin_marker_mesh` into a tip-anchored teardrop

Find the whole current definition:
```c
#define PIN_SEGMENTS  16
#define PIN_Z_OFFSET  0.010f    /* proud of the map face, toward the viewer */
static Mesh pin_marker_mesh(float mw) {
    MeshBuilder mb;
    Mesh        m;
    float       r = 0.03f * mw;
    sol_u32     center, prev = 0;
    int         i;
    mb_init(&mb);
    center = mb_push_vertex(&mb, 0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.5f, 0.5f);
    for (i = 0; i <= PIN_SEGMENTS; i++) {
        float   a = 6.2831853f * (float)i / (float)PIN_SEGMENTS;
        sol_u32 v = mb_push_vertex(&mb, r * cosf(a), r * sinf(a), 0.0f,
                                   0.0f, 0.0f, 1.0f,
                                   0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a));
        if (i > 0) mb_push_triangle(&mb, center, prev, v);
        prev = v;
    }
    m = mesh_from_builder(&mb);
    mb_free(&mb);
    return m;
}
```
Replace with:
```c
#define PIN_SEGMENTS  16
#define PIN_Z_OFFSET  0.010f    /* proud of the map face, toward the viewer */
#define PIN_HEAD_R    0.020f    /* head radius, x map width */
#define PIN_HEAD_CY   0.055f    /* head centre above the tip, x map width */
/* A classic map marker: a circle HEAD centred at (0, cy) with a downward TRIANGLE
   whose tip is the local origin (0,0). resolve_pin seats local (0,0) on the
   projected lat/lon, so the tip rests exactly on the point and the head rises
   above it. Co-planar +Z, OWNED per pin (excluded from mesh_asset_key). */
static Mesh pin_marker_mesh(float mw) {
    MeshBuilder mb;
    Mesh        m;
    float       r  = PIN_HEAD_R  * mw;
    float       cy = PIN_HEAD_CY * mw;
    float       bx = r * 0.6f;              /* triangle half-width at the head base */
    sol_u32     center, prev = 0, tip, bl, br;
    int         i;
    mb_init(&mb);
    center = mb_push_vertex(&mb, 0.0f, cy, 0.0f,  0.0f, 0.0f, 1.0f,  0.5f, 0.5f);
    for (i = 0; i <= PIN_SEGMENTS; i++) {
        float   a = 6.2831853f * (float)i / (float)PIN_SEGMENTS;
        sol_u32 v = mb_push_vertex(&mb, r * cosf(a), cy + r * sinf(a), 0.0f,
                                   0.0f, 0.0f, 1.0f,
                                   0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a));
        if (i > 0) mb_push_triangle(&mb, center, prev, v);
        prev = v;
    }
    tip = mb_push_vertex(&mb, 0.0f, 0.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.5f, 0.0f);
    bl  = mb_push_vertex(&mb, -bx,  cy,   0.0f,  0.0f, 0.0f, 1.0f,  0.2f, 0.5f);
    br  = mb_push_vertex(&mb,  bx,  cy,   0.0f,  0.0f, 0.0f, 1.0f,  0.8f, 0.5f);
    mb_push_triangle(&mb, tip, br, bl);
    m = mesh_from_builder(&mb);
    mb_free(&mb);
    return m;
}
```

### Step 1.3 — raise the label so it clears the taller marker

The label pass currently offsets by a fixed `0.06f` (sized for the old disc). The teardrop is taller and scales with the map, so derive the clearance from the pin's parent map width. Find:
```c
            nm = scene_meta_get(&state->scene, o->handle, "name");
            if (!nm || !nm[0]) continue;                    /* unnamed: no label */
            px2m  = 0.030f / lh;                            /* ~3cm line */
            text_measure_cached(uf, nm, 1.0f, &nw, (float *)0);
            top_y = 0.06f + lh * px2m;                      /* clear the marker, hang above it */
```
Replace with:
```c
            nm = scene_meta_get(&state->scene, o->handle, "name");
            if (!nm || !nm[0]) continue;                    /* unnamed: no label */
            px2m  = 0.030f / lh;                            /* ~3cm line */
            text_measure_cached(uf, nm, 1.0f, &nw, (float *)0);
            {   /* clear the marker head: its top is (PIN_HEAD_CY+PIN_HEAD_R)*mw
                   above the tip, scaled by the parent map's width. */
                SceneObject *pm = scene_get(&state->scene, o->parent);
                float mw2 = pm ? mesh_ref_param("map", pm->mesh_params,
                                                pm->mesh_param_count, "w") : MAP_BOARD_W;
                if (mw2 <= 0.0f) mw2 = MAP_BOARD_W;
                top_y = (PIN_HEAD_CY + PIN_HEAD_R) * mw2 + 0.02f + lh * px2m;
            }
```

### Step 1.4 — gauntlet build

- [ ] `./build.sh` — expect `built ./solarium (debug)`.
- [ ] `./build.sh c89check` — expect `c89check: PASS — all sources are C89-pedantic clean`.
- [ ] `./build.sh asan` — links (pre-existing `sprintf` deprecation warning at an unrelated line is OK).
- [ ] `./build.sh metal` — links clean.

### Step 1.5 — commit

- [ ] `git add main.c`
- [ ] Commit:
```
Map pins: select a pin individually + tip-anchored teardrop marker

selection_root gives "pin" the same self-selecting exception as "arrow"
(a clicked pin highlights alone, not its whole map). pin_marker_mesh
becomes a circle head + downward triangle with the tip at local origin,
so resolve_pin seats the tip on the lat/lon; the label lifts to clear
the taller head (scaled by the parent map width).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## Task 2 — "New place" via a palette form mode

**Files:** `palette.h`, `palette.c`, `main.c`.

### Step 2.1 — `palette.h`: TAB key, form fields, `palette_form` decl

**(a)** the caps. Find:
```c
#define PALETTE_QUERY_CAP 128
#define PALETTE_PICK_CAP  64
```
Replace with:
```c
#define PALETTE_QUERY_CAP 128
#define PALETTE_PICK_CAP  64
#define PALETTE_FORM_FIELDS 4
#define PALETTE_FIELD_CAP   64
```

**(b)** the struct. Find:
```c
    sol_bool pick;      /* pick mode: fuzzy-choose one of a copied name/ref list */
    char     pick_names[PALETTE_PICK_CAP][48];        /* shown + fuzzy-filtered */
    char     pick_refs[PALETTE_PICK_CAP][24];         /* returned to the callback */
    int      pick_n;
    void     (*pick_cb)(struct AppState *, const char *);
} Palette;
```
Replace with:
```c
    sol_bool pick;      /* pick mode: fuzzy-choose one of a copied name/ref list */
    char     pick_names[PALETTE_PICK_CAP][48];        /* shown + fuzzy-filtered */
    char     pick_refs[PALETTE_PICK_CAP][24];         /* returned to the callback */
    int      pick_n;
    void     (*pick_cb)(struct AppState *, const char *);
    sol_bool form;      /* form mode: N labeled fields, submit all on Enter */
    char     form_labels[PALETTE_FORM_FIELDS][32];
    char     form_vals[PALETTE_FORM_FIELDS][PALETTE_FIELD_CAP];
    int      form_n;
    int      form_field;   /* the field being edited */
    void     (*form_cb)(struct AppState *, const char *const *vals, int n);
} Palette;
```

**(c)** the enum. Find:
```c
    PALETTE_KEY_BACKSPACE,
    PALETTE_KEY_CANCEL
} PaletteKey;
```
Replace with:
```c
    PALETTE_KEY_BACKSPACE,
    PALETTE_KEY_CANCEL,
    PALETTE_KEY_TAB
} PaletteKey;
```

**(d)** the declaration. Find:
```c
void     palette_pick(Palette *p, const char *label,
                      const char *const *names, const char *const *refs, int n,
                      void (*cb)(struct AppState *, const char *ref));
```
Replace with:
```c
void     palette_pick(Palette *p, const char *label,
                      const char *const *names, const char *const *refs, int n,
                      void (*cb)(struct AppState *, const char *ref));
/* Open the palette as a multi-field FORM: `nfields` labeled text fields (Tab/
   arrows move between them, Enter submits all values to cb, Esc cancels). Labels
   are copied in; the `vals` pointers passed to cb are valid only during the call. */
void     palette_form(Palette *p, const char *const *labels, int nfields,
                      void (*cb)(struct AppState *, const char *const *vals, int n));
```

### Step 2.2 — `palette.c`: clear `form` in the other opens + add `palette_form`

**(a)** clear `form` in the three existing opens. In `palette_open_now`, find `    p->pick     = SOL_FALSE;\n}` (its last line) and add `    p->form     = SOL_FALSE;` before the `}`. Do the same in `palette_prompt` (after its `p->pick = SOL_FALSE;`) and `palette_pick` (after its `p->pick = SOL_TRUE;` line, add `    p->form = SOL_FALSE;`). Concretely:

In `palette_open_now`, find:
```c
    p->eat_char = SOL_TRUE;   /* the ':' that opened us arrives next as a char */
    p->prompt   = SOL_FALSE;
    p->pick     = SOL_FALSE;
}
```
Replace with:
```c
    p->eat_char = SOL_TRUE;   /* the ':' that opened us arrives next as a char */
    p->prompt   = SOL_FALSE;
    p->pick     = SOL_FALSE;
    p->form     = SOL_FALSE;
}
```

In `palette_prompt`, find:
```c
    p->prompt       = SOL_TRUE;
    p->pick         = SOL_FALSE;
    p->prompt_label = label;
    p->prompt_cb    = cb;
}
```
Replace with:
```c
    p->prompt       = SOL_TRUE;
    p->pick         = SOL_FALSE;
    p->form         = SOL_FALSE;
    p->prompt_label = label;
    p->prompt_cb    = cb;
}
```

In `palette_pick`, find:
```c
    p->prompt       = SOL_FALSE;
    p->pick         = SOL_TRUE;
    p->prompt_label = label;
    p->pick_cb      = cb;
```
Replace with:
```c
    p->prompt       = SOL_FALSE;
    p->pick         = SOL_TRUE;
    p->form         = SOL_FALSE;
    p->prompt_label = label;
    p->pick_cb      = cb;
```

**(b)** add `palette_form` immediately AFTER `palette_pick`'s closing `}` (before `palette_input_char`). Find:
```c
        strncpy(p->pick_refs[i], refs[i] ? refs[i] : "",
                sizeof p->pick_refs[i] - 1);
        p->pick_refs[i][sizeof p->pick_refs[i] - 1] = '\0';
    }
}

void palette_input_char(Palette *p, unsigned int cp) {
```
Replace with:
```c
        strncpy(p->pick_refs[i], refs[i] ? refs[i] : "",
                sizeof p->pick_refs[i] - 1);
        p->pick_refs[i][sizeof p->pick_refs[i] - 1] = '\0';
    }
}

void palette_form(Palette *p, const char *const *labels, int nfields,
                  void (*cb)(struct AppState *, const char *const *vals, int n)) {
    int i;
    p->open       = SOL_TRUE;
    p->query[0]   = '\0';
    p->len        = 0;
    p->sel        = 0;
    p->eat_char   = SOL_FALSE;   /* a command opened us — no ':' to swallow */
    p->prompt     = SOL_FALSE;
    p->pick       = SOL_FALSE;
    p->form       = SOL_TRUE;
    p->form_cb    = cb;
    p->form_field = 0;
    if (nfields < 0) nfields = 0;
    if (nfields > PALETTE_FORM_FIELDS) nfields = PALETTE_FORM_FIELDS;
    p->form_n = nfields;
    for (i = 0; i < nfields; i++) {
        strncpy(p->form_labels[i], labels[i] ? labels[i] : "",
                sizeof p->form_labels[i] - 1);
        p->form_labels[i][sizeof p->form_labels[i] - 1] = '\0';
        p->form_vals[i][0] = '\0';
    }
}

void palette_input_char(Palette *p, unsigned int cp) {
```

### Step 2.3 — `palette.c`: type into the current field in form mode

Find:
```c
void palette_input_char(Palette *p, unsigned int cp) {
    if (!p->open) return;
    if (p->eat_char) { p->eat_char = SOL_FALSE; return; }
    if (cp < 0x20 || cp > 0x7e) return;             /* v1: printable ASCII only */
    if (p->len + 1 >= PALETTE_QUERY_CAP) return;
    p->query[p->len++] = (char)cp;
    p->query[p->len]   = '\0';
    p->sel = 0;
}
```
Replace with:
```c
void palette_input_char(Palette *p, unsigned int cp) {
    if (!p->open) return;
    if (p->eat_char) { p->eat_char = SOL_FALSE; return; }
    if (cp < 0x20 || cp > 0x7e) return;             /* v1: printable ASCII only */
    if (p->form) {                                  /* type into the current field */
        char *f  = p->form_vals[p->form_field];
        int   fl = (int)strlen(f);
        if (fl + 1 < PALETTE_FIELD_CAP) { f[fl] = (char)cp; f[fl + 1] = '\0'; }
        return;
    }
    if (p->len + 1 >= PALETTE_QUERY_CAP) return;
    p->query[p->len++] = (char)cp;
    p->query[p->len]   = '\0';
    p->sel = 0;
}
```

### Step 2.4 — `palette.c`: handle form keys in `palette_input_key`

Find (the CANCEL block + the start of the pick block):
```c
    if (k == PALETTE_KEY_CANCEL) {
        p->open = SOL_FALSE; p->prompt = SOL_FALSE; p->pick = SOL_FALSE;
        p->prompt_cb = NULL; p->prompt_label = NULL; p->pick_cb = NULL;
        return SOL_TRUE;
    }

    if (p->pick) {                                   /* picker mode */
```
Replace with:
```c
    if (k == PALETTE_KEY_CANCEL) {
        p->open = SOL_FALSE; p->prompt = SOL_FALSE; p->pick = SOL_FALSE;
        p->form = SOL_FALSE;
        p->prompt_cb = NULL; p->prompt_label = NULL; p->pick_cb = NULL;
        p->form_cb = NULL;
        return SOL_TRUE;
    }

    if (p->form) {                                   /* multi-field form mode */
        if (k == PALETTE_KEY_TAB || k == PALETTE_KEY_DOWN) {
            if (p->form_n > 0) p->form_field = (p->form_field + 1) % p->form_n;
        } else if (k == PALETTE_KEY_UP) {
            if (p->form_n > 0)
                p->form_field = (p->form_field + p->form_n - 1) % p->form_n;
        } else if (k == PALETTE_KEY_BACKSPACE) {
            char *f  = p->form_vals[p->form_field];
            int   fl = (int)strlen(f);
            if (fl > 0) f[fl - 1] = '\0';
        } else if (k == PALETTE_KEY_ENTER) {
            void (*cb)(struct AppState *, const char *const *, int) = p->form_cb;
            const char *vals[PALETTE_FORM_FIELDS];
            int   nf = p->form_n, fi;
            for (fi = 0; fi < nf; fi++) vals[fi] = p->form_vals[fi];
            p->open = SOL_FALSE; p->form = SOL_FALSE; p->form_cb = NULL;
            if (cb) cb(st, vals, nf);
        }
        return SOL_TRUE;
    }

    if (p->pick) {                                   /* picker mode */
```

### Step 2.5 — `palette.c`: render the form in `palette_draw`

**(a)** the mode dispatch. Find:
```c
    if (p->prompt) {
        n = 0; shown = 0;
    } else if (p->pick) {
        n     = palette_pick_rank(p, order, PALETTE_MAX_COMMANDS);
        shown = (n < PALETTE_MAX_ROWS) ? n : PALETTE_MAX_ROWS;
    } else {
```
Replace with:
```c
    if (p->prompt) {
        n = 0; shown = 0;
    } else if (p->form) {
        n = p->form_n; shown = p->form_n;
    } else if (p->pick) {
        n     = palette_pick_rank(p, order, PALETTE_MAX_COMMANDS);
        shown = (n < PALETTE_MAX_ROWS) ? n : PALETTE_MAX_ROWS;
    } else {
```

**(b)** suppress the `:`/prompt query row in form mode (the fields carry their own labels). Find:
```c
        if (p->prompt || p->pick) {
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
```
Replace with:
```c
        if (p->form) {
            const char *ttl = "new";                /* the fields carry their labels */
            while (*ttl && ll < (int)sizeof line - 1) line[ll++] = *ttl++;
            line[ll] = '\0';
        } else {
            if (p->prompt || p->pick) {
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
        }
        ui_text(font, line, box_x + pad, qy, ts, 0.95f, 0.92f, 0.80f, 1.0f);
```

**(c)** the row loop: highlight + render a field row in form mode. Find:
```c
        if (ri == p->sel)
            ui_quad(box_x + pad * 0.5f, ry, box_w - pad, row_h,
                    0.20f, 0.24f, 0.30f, 0.9f);
        if (p->pick) {
            ui_text(font, p->pick_names[order[ri]], box_x + pad, ty, ts,
                    0.92f, 0.92f, 0.92f, 1.0f);
        } else {
```
Replace with:
```c
        if (ri == (p->form ? p->form_field : p->sel))
            ui_quad(box_x + pad * 0.5f, ry, box_w - pad, row_h,
                    0.20f, 0.24f, 0.30f, 0.9f);
        if (p->form) {
            char frow[32 + PALETTE_FIELD_CAP + 4];
            int  fl = 0;
            const char *lb = p->form_labels[ri];
            const char *vv = p->form_vals[ri];
            while (*lb && fl < (int)sizeof frow - 4) frow[fl++] = *lb++;
            frow[fl++] = ':'; frow[fl++] = ' ';
            while (*vv && fl < (int)sizeof frow - 2) frow[fl++] = *vv++;
            if (ri == p->form_field) frow[fl++] = '_';
            frow[fl] = '\0';
            ui_text(font, frow, box_x + pad, ty, ts, 0.92f, 0.92f, 0.92f, 1.0f);
        } else if (p->pick) {
            ui_text(font, p->pick_names[order[ri]], box_x + pad, ty, ts,
                    0.92f, 0.92f, 0.92f, 1.0f);
        } else {
```

### Step 2.6 — `main.c`: route Tab to the palette

Find:
```c
            else if (key == GLFW_KEY_BACKSPACE) pk = PALETTE_KEY_BACKSPACE;
            if (pk != PALETTE_KEY_NONE)
```
Replace with:
```c
            else if (key == GLFW_KEY_BACKSPACE) pk = PALETTE_KEY_BACKSPACE;
            else if (key == GLFW_KEY_TAB)       pk = PALETTE_KEY_TAB;
            if (pk != PALETTE_KEY_NONE)
```

### Step 2.7 — `main.c`: `add_pin_to_map` helper + `new_place_form_cb` + rewrite `add_place_pin_cb`

Find the whole current `add_place_pin_cb`:
```c
static void add_place_pin_cb(AppState *st, const char *ref) {
    sol_u32     h, ph;
    const char *slat, *slon, *pname;
    double      lat, lon, u0, v0, u1, v1;
    float       mw, mh;
    char        b[32], nm[48];
    Mesh        empty;
    if (st->map_view == 0 || !ref || !ref[0]) return;
    h = (sol_u32)atoi(ref);
    if (!scene_get(&st->scene, h)) return;
    slat  = scene_meta_get(&st->scene, h, "lat");
    slon  = scene_meta_get(&st->scene, h, "lon");
    pname = scene_meta_get(&st->scene, h, "name");
    lat = slat ? atof(slat) : 0.0;
    lon = slon ? atof(slon) : 0.0;
    strncpy(nm, pname ? pname : "", sizeof nm - 1);
    nm[sizeof nm - 1] = '\0';
    memset(&empty, 0, sizeof empty);
    ph = scene_add(&st->scene, st->map_view, empty, vec3_make(0.0f, 0.0f, 0.0f),
                   quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, ph, "pin");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    sprintf(b, "%.6f", lat); scene_meta_set(&st->scene, ph, "lat", b);
    sprintf(b, "%.6f", lon); scene_meta_set(&st->scene, ph, "lon", b);
#pragma clang diagnostic pop
    scene_meta_set(&st->scene, ph, "name", nm);
    if (map_window_of(&st->scene, st->map_view, &u0, &v0, &u1, &v1, &mw, &mh))
        resolve_pin(&st->scene, ph, u0, v0, u1, v1, mw, mh);
    st->selected_handle = ph;
    scene_save(&st->scene, "scene.stml");
}
```
Replace it with the shared helper, the form callback, and the slimmer dispatcher (in this order):
```c
/* Create a pin on `map` at (lat,lon) with `name`, resolve + select + save.
   Shared by the Places picker and the New-place form. Snapshot any caller meta
   pointers BEFORE calling — scene_add reallocs. */
static void add_pin_to_map(AppState *st, sol_u32 map, double lat, double lon,
                           const char *name) {
    double  u0, v0, u1, v1;
    float   mw, mh;
    sol_u32 ph;
    char    b[32];
    Mesh    empty;
    if (map == 0) return;
    memset(&empty, 0, sizeof empty);
    ph = scene_add(&st->scene, map, empty, vec3_make(0.0f, 0.0f, 0.0f),
                   quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, ph, "pin");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    sprintf(b, "%.6f", lat); scene_meta_set(&st->scene, ph, "lat", b);
    sprintf(b, "%.6f", lon); scene_meta_set(&st->scene, ph, "lon", b);
#pragma clang diagnostic pop
    scene_meta_set(&st->scene, ph, "name", name ? name : "");
    if (map_window_of(&st->scene, map, &u0, &v0, &u1, &v1, &mw, &mh))
        resolve_pin(&st->scene, ph, u0, v0, u1, v1, mw, mh);
    st->selected_handle = ph;
    scene_save(&st->scene, "scene.stml");
}

/* "+ New place" form submit: vals = {lat, lon, label}. Save a new reusable Place
   in the catalog (child of the places anchor) AND drop a pin on the current map.
   `vals` point into the palette; copy nothing that outlives this call. */
static void new_place_form_cb(AppState *st, const char *const *vals, int n) {
    double      lat, lon;
    sol_u32     anchor, pl;
    char        b[32];
    Mesh        empty;
    const char *label;
    if (st->map_view == 0 || n < 3) return;
    lat   = atof(vals[0]);
    lon   = atof(vals[1]);
    label = vals[2] ? vals[2] : "";
    anchor = places_anchor(st);
    memset(&empty, 0, sizeof empty);
    pl = scene_add(&st->scene, anchor, empty, vec3_make(0.0f, 0.0f, 0.0f),
                   quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(&st->scene, pl, "name", label);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    sprintf(b, "%.6f", lat); scene_meta_set(&st->scene, pl, "lat", b);
    sprintf(b, "%.6f", lon); scene_meta_set(&st->scene, pl, "lon", b);
#pragma clang diagnostic pop
    scene_meta_set(&st->scene, pl, "zoom", "5");
    scene_meta_set(&st->scene, pl, "basemap", "relief");
    add_pin_to_map(st, st->map_view, lat, lon, label);
}

/* Add-place picker callback: the "+ New place" sentinel opens the form; any other
   ref is an existing Place handle -> a pin at its stored view. */
static void add_place_pin_cb(AppState *st, const char *ref) {
    sol_u32     h;
    const char *slat, *slon, *pname;
    double      lat, lon;
    char        nm[48];
    if (st->map_view == 0 || !ref || !ref[0]) return;
    if (strcmp(ref, "+new") == 0) {
        static const char *labels[3] = { "lat", "lon", "label" };
        palette_form(&st->palette, labels, 3, new_place_form_cb);
        return;
    }
    h = (sol_u32)atoi(ref);
    if (!scene_get(&st->scene, h)) return;
    slat  = scene_meta_get(&st->scene, h, "lat");
    slon  = scene_meta_get(&st->scene, h, "lon");
    pname = scene_meta_get(&st->scene, h, "name");
    lat = slat ? atof(slat) : 0.0;
    lon = slon ? atof(slon) : 0.0;
    strncpy(nm, pname ? pname : "", sizeof nm - 1);
    nm[sizeof nm - 1] = '\0';
    add_pin_to_map(st, st->map_view, lat, lon, nm);
}
```

### Step 2.8 — `main.c`: prepend the "+ New place" row in `cmd_add_place`

Find:
```c
    sol_u32            anchor = places_anchor(st);
    int                n = 0;
    sol_u32            i;
    for (i = 0; i < st->scene.count && n < PALETTE_PICK_CAP; i++) {
```
Replace with:
```c
    sol_u32            anchor = places_anchor(st);
    int                n = 0;
    sol_u32            i;
    strncpy(names[0], "+ New place", sizeof names[0] - 1);
    names[0][sizeof names[0] - 1] = '\0';
    strncpy(refs[0], "+new", sizeof refs[0] - 1);
    refs[0][sizeof refs[0] - 1] = '\0';
    namep[0] = names[0]; refp[0] = refs[0]; n = 1;   /* sentinel row atop the list */
    for (i = 0; i < st->scene.count && n < PALETTE_PICK_CAP; i++) {
```

### Step 2.9 — gauntlet build

- [ ] `./build.sh`
- [ ] `./build.sh c89check`
- [ ] `./build.sh asan`
- [ ] `./build.sh metal`

### Step 2.10 — commit

- [ ] `git add palette.h palette.c main.c`
- [ ] Commit:
```
Map pins: "+ New place" via a reusable palette form mode

palette.c gains a form mode (N labeled fields, Tab/arrows to move, Enter
submits all values; new PALETTE_KEY_TAB). "Add place" prepends a
"+ New place" row that opens a lat/lon/label form; submitting saves a new
Places-catalog entry AND drops a pin (shared add_pin_to_map helper).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## After both tasks: human live-verify

- [ ] Click a pin → **only that pin** highlights (never the whole map); the map can't be selected in map view.
- [ ] Markers are **tip-anchored teardrops** (circle head + downward triangle), tip on the exact point; labels clear the head.
- [ ] `:` → **Add place** shows **"+ New place"** at the top of the list.
- [ ] Pick "+ New place" → a **form** with lat / lon / label fields; **Tab / ↑↓** move between them, typing edits the focused field, **Enter** submits, **Esc** cancels.
- [ ] Submitting creates a **catalog place** (it appears in Add-place next time and in the `;` entity browser) **and** a pin on the current map at that point.
- [ ] Existing "Add place → pick a city" still works.

## Self-review notes (spec coverage)

- Item 1 (pin selects individually) → Task 1 step 1.1 (`selection_root` "pin" exception).
- Item 2 (teardrop, tip-anchored, label clears head) → Task 1 steps 1.2–1.3.
- Item 3 form mode (labeled fields, Tab/↑↓, Enter submit, `PALETTE_KEY_TAB`) → Task 2 steps 2.1–2.6.
- Item 3 wiring ("+ New place" sentinel, catalog place + pin, `add_pin_to_map` helper) → Task 2 steps 2.7–2.8.
- No shader / no MSL; gauntlet incl. metal → each task's build step.
- Out-of-scope (edit existing place, hard validation, other form consumers) intentionally not implemented.

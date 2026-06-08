# Solarium — Phase 3 Execution Brief (Items 1–10): The Spatial Knowledge Environment

> **Numbering note.** This is **Phase 3**. Phase 2 (TODO2.md, items 0–9: scene/
> persistence → shadow mapping) is complete, as is the IBL/skybox follow-on. The
> items below are numbered 1–10 *within this phase*; when a reference could be
> ambiguous with Phase 2's items, cite them as "P3 · Item N".

## How to use this document

You have the current engine source. What you do **not** have is the design reasoning
behind this phase. The engine until now is a competent single-light forward PBR renderer
(now with IBL/skybox) sitting on an unusually strong **scene + persistence** spine. This
phase turns that spine into a **spatial knowledge environment**: rooms that mirror folders,
workspaces of aliased files, cards and notes you place by hand and re-find by *position*,
with terrain as an eventual exterior canvas.

The organizing thesis — the thing every item serves — is **object permanence / the method
of loci**: the value of doing this in 3D (rather than a 2D list) is that things persist
*somewhere* and you re-encounter them by being near them (recognition), instead of having
to summon them by name (recall). That is the one solid justification for the third
dimension here, and several invariants below exist only to protect it.

**Part 1 (the Constitution) is binding on every item.** Most of it is already true in the
code; the parts marked NEW are this phase's additions and are load-bearing. Where the
normal, minimal instinct conflicts with this brief, follow the brief.

Each item states its **intent** (the *why*, which you lack), its **spec**, an **interface
sketch** where it helps, and concrete **acceptance criteria**. Decisions reserved to the
project owner are flagged inline and collected in Part 4.

---

## Part 1 — The Constitution

### 1.1 What is already established (maintain it)

These are not new rules; they are facts about the codebase you are extending. Keep them:

- **Strict C89**, `-std=c89 -pedantic-errors -Wall -Wextra`, clean. The forbidden-construct
  list still applies (no compound literals, designated initializers, `for(int)`,
  declarations-after-statements, `//`, `inline`, `<stdint.h>`/`<stdbool.h>`). Use the
  existing `sol_base.h` types (`sol_u32`, `sol_bool`, `SOL_TRUE/FALSE`).
- **The RHI seam is inviolable.** `rhi_gl.c` is the only translation unit that touches GL;
  nothing above `rhi.h` references a `gl*`/`GL_*` symbol. New GPU capability = extend
  `rhi.h` and implement in the backend. (This phase adds real RHI surface — blend state,
  a 2D pipeline, a glyph texture — all of which stays behind the seam.)
- **The scene spine is the substrate.** `SceneObject` (stable `handle` + persistent `nid`,
  both decoupled from array index; `parent` hierarchy; TRS; `mesh`/`mesh_ref`; `material`;
  the `meta`/`relations`/`content` slots) and its STML round-trip are what most of this
  phase composes from. Prefer extending it to inventing parallel structures.
- **Debug GL checking** uses the `glGetError`-at-boundaries fallback (the backend is on
  macOS GL, where `KHR_debug` is unavailable). Keep it silent.

### 1.2 NEW — The permanence invariant (the rule of this phase)

**A placed object's position is sacred. The system never moves it.** Position changes only
by an explicit user action (a drag, item 4), is persisted immediately (it already
round-trips via TRS + `nid`), and survives reload exactly. No auto-layout, no reflow, no
"tidy up" that repositions things the user placed. This is the single most important
invariant in the phase, because the entire object-permanence payoff is invalidated the
moment the parser-note isn't where the user left it. Any feature that wants to arrange
objects automatically must do so only into *unplaced* slots, never over a placed one.

### 1.3 NEW — Mirror vs. Workspace (the truth distinction)

There are two kinds of room, with **opposite relationships to truth**, and the user must
always be able to tell which they are in:

- A **mirror** room reflects a real directory. Its *membership* is governed by disk: if a
  file exists on disk it is in the room; if deleted, it leaves. Its job is fidelity.
- A **workspace** room reflects nothing but the user's intent. It holds *aliases* (pointers
  to real files), notes, and cards, in any arrangement; the same file may be aliased into
  many workspaces; the filesystem has no say in what belongs. Its job is curation.

Do not blur them. Membership-follows-disk applies *only* to mirrors. Arrangement-follows-
the-user (1.2) applies to both. A workspace has no reconciliation problem because the user
is its only authority; a mirror's reconciliation is the one genuinely hard part of item 6.

### 1.4 NEW — Procedural geometry: the box-shaped case, never booleans

All procedural geometry this phase (rooms, walls, terrain) is **parametric construction** —
`MeshBuilder` clients in the shape of the existing `make_box`/`make_grid`. **Do not
implement mesh booleans / CSG.** When geometry needs a hole (a doorway, a window), build the
surrounding surface *as pieces around the gap* (item 5's `make_wall_with_opening`), never by
cutting. This is deliberate: robust mesh booleans are a multi-week numerical-robustness
subsystem and a category of difficulty this project chooses not to enter. The constraint
(axis-aligned, gap-not-cut) is what keeps procedural geometry an afternoon instead of a
career.

### 1.5 The canonical vertex layout (current reality)

The layout is now **12 floats**: position(3) + normal(3) + uv(2) + tangent(4), interleaved.
New emitters call `mb_push_vertex` with pos/normal/uv exactly like `make_box`; the tangent
is filled by `mb_compute_tangents`, which `mesh_from_builder` already runs before upload.
Index type is `sol_u32`. New geometry must emit correct normals and UVs (curved surfaces —
a cylinder, a terrain slope — need *smooth* normals from neighbors, unlike the box's
per-face flat normals).

### 1.6 Dependencies (the policy, and the live decision)

Zero third-party dependencies by default; GLFW is the sanctioned platform layer. One
exception already exists: **`stb_image` is vendored** behind `image.h`, quarantined in
`image.c` and excluded from the C89 check, exactly as GLFW's headers are. That precedent
matters this phase, because **text rendering (item 3) needs font rasterization**, and the
natural companion — `stb_truetype`, quarantined the same way — is the obvious choice *if*
the owner accepts a second stb dependency. Hand-rolling a TrueType rasterizer is a large
undertaking. This is the owner's call (Part 4), but absent direction, follow the `stb_image`
precedent and use `stb_truetype`.

**The complex-script stack is a known future dependency, deliberately deferred behind a seam.**
The owner does multilingual translation work (Hebrew, Arabic, Indic, Greek, …), so correct
shaping is eventually *required*, not optional. But `stb_truetype` only **rasterizes** glyphs —
it does **no shaping and no BiDi** — so complex scripts (RTL reordering, Arabic contextual
forms, Indic conjuncts/reordering, Hebrew niqqud placement) eventually need **HarfBuzz**
(shaping) + **SheenBidi** (the bidirectional algorithm). Both are vendored *later*, quarantined
like `stb_image`: HarfBuzz exposes a pure **C API** (`hb.h` is `extern "C"`), so no C++ is
written here — the only build delta is linking `libharfbuzz`/`libSheenBidi` and the C++ runtime
(`clang++` / `-lc++`), and the TU is excluded from `c89check`. They compose cleanly: HarfBuzz
emits glyph **indices** and `stb_truetype` rasterizes **by index**, so no FreeType is needed.
**This is NOT built now.** Item 3 ships simple-script layout (Latin/Greek — both immediately
useful) behind a *replaceable `text_shape` seam* so the stack drops in later without touching
the atlas, batch, renderer, or call sites. See item 3 and Part 4.

### 1.7 Environment lighting now exists — integrate, do not reinvent

Full IBL + skybox is built and working. The specifics you must respect:

- **What exists, built once at init** (in `main.c`): an equirectangular `.hdr` is loaded to a
  linear `RHI_TEX_RGBA16F` texture, baked into an environment cubemap (`build_env_cubemap`),
  convolved into a diffuse irradiance cubemap (`build_irradiance_map`), GGX-prefiltered into a
  roughness mip chain (`build_prefilter_map`), and integrated into a BRDF LUT
  (`build_brdf_lut`). All four are init-time, not per-frame.
- **The ambient term is now real IBL**, not a constant. The object fragment shader's ambient
  branch is `(kD·irradiance·albedo + prefiltered·(F0·brdf.x + brdf.y))·ao` when `uUseIBL` is
  set, falling back to `0.03·albedo·ao` **only** if the `.hdr` failed to load. New lit
  surfaces (rooms, terrain) inherit this for free by going through the existing shader — do
  **not** add a second ambient path or reintroduce the flat constant.
- **The texture-unit budget on the object shader is now full**: units 0–3 are the material
  maps (albedo/MR/AO/normal), 4 is the shadow map, 5 the irradiance cubemap, 6 the prefilter
  cubemap, 7 the BRDF LUT. A new sampler on the *object* shader has no free unit — but the UI
  layer (item 2) is a separate pipeline/program, so its glyph-atlas/card samplers live in
  their own unit namespace and don't collide.
- **The skybox draws first** in the HDR pass with depth off, filling the background; the
  object loop then draws over it normally. This matters for rooms (item 5) and terrain
  (item 10) — see those items.

The new RHI surface the IBL work added — `rhi_create_texture_hdr`, `rhi_create_cubemap`,
`rhi_begin_cubemap_face`, `rhi_cubemap_generate_mips`, and the per-texture GL `target` so
cubemaps bind through the ordinary `rhi_bind_texture` path — is the established pattern; reuse
it, don't parallel it.

### 1.8 Definition of done (every item)

Builds clean under `-std=c89 -pedantic-errors -Wall -Wextra`; runs clean under ASan+UBSan;
no GL symbol above the seam; the `glGetError` boundary checks stay silent; the stated
checkpoint demonstrably passes; persisted state round-trips (1.2); no new dependency beyond
those approved in Part 4.

### 1.9 NEW — OS access lives behind a thin platform layer

This phase is the first to need real OS services beyond windowing. Item 6 must **enumerate
directories** and read file mtimes to mirror/reconcile real folders; item 9 reads file
contents. C89 has **no** directory API — that is POSIX `<dirent.h>` / `<sys/stat.h>`, a surface
the engine has never touched (today it only does GLFW windowing + `fopen` asset reads).
Quarantine it exactly as GL is quarantined behind `rhi.h`: a small **`platform_fs.c`/`.h`** TU
is the *only* place that includes `<dirent.h>`/`<sys/stat.h>`, exposing a clean C interface
(list a directory → entries with name/kind/mtime; read a file → bytes; stat a path). Everything
above it sees only that interface, never a raw OS call. This is the first real brick of the
eventual own-platform-layer; building it deliberately here keeps OS access from sprawling
across item 6's reconciliation code (and, like `image.c`, the TU may be excluded from the
strict-C89 check if a POSIX header trips it).

### 1.10 NEW — The Room is one data model, pinned once

The Room concept is touched by three items — geometry (item 5), type + source-path + membership
(item 6), graph node (item 7) — and it will rot if each item re-invents it. **Define the Room
representation once, up front in item 5**, and have items 6 and 7 *extend that one definition*
(a field, a relation), never a parallel structure. The representation to settle there: whether a
Room is a dedicated struct or (preferred, per §1.1) a `SceneObject` **anchor** carrying
`room_type` (MIRROR/WORKSPACE) + `source_path` in its `meta` slots, with the convention that a
room's contents `parent` to that anchor (so the existing hierarchy + STML round-trip carry rooms
for free). Pin this before item 6 builds reconciliation on top of it.

---

## Part 2 — Item 1: Harden the persistence + resource spine (gate; do first)

**Intent.** These protect the two properties the entire direction rests on: *trustworthy
persistence* (the palace must reload exactly, or object permanence is a lie) and *unbounded
accretion* (the palace grows without limit, by design). All three bugs pass the current
tests while being broken for real use — the dangerous kind.

**Spec.**
- **STML escaping.** `scene_save` writes attribute values and capture text with raw
  `fprintf("%s")`; the STML reader does no unescaping. The moment a user puts a `"` in a
  meta value, a `<` in a note, or a `"` in a content path, the round-trip corrupts. Add
  escape on write / unescape on read for `< > & "` (and `'` inside single-quoted attrs) in
  both `scene_io.c` and `stml.c`. Also: capture text is dedented/trimmed on load, which
  loses significant leading/trailing whitespace — decide whether note/meta values need it
  preserved (they likely do); if so, give meta values a non-dedenting storage form.
- **Resource-table overflow.** `slot_alloc` in `rhi_gl.c` returns `(*count)++` with no bound
  check against `MAX_BUFFERS` (256), `MAX_TEXTURES` (64), etc.; overflow writes past a
  static array (corruption, not a clean failure). Make `slot_alloc` fail loudly (debug assert
  + a returned sentinel the callers handle), and raise/grow the tables. **`MAX_TEXTURES` (64)
  is now the tighter ceiling, and it is on the palace's critical path:** the IBL maps + HDR
  target + shadow target + the glyph atlas already consume a dozen-plus entries, and in the
  spatial environment *every image card and every open book/page view is another texture* —
  a moodboard of a few dozen images blows 64 fast. Treat raising this (or growing the table)
  as required, not optional.
- **Delete `obj.c` / `obj.h`.** Dead since the glTF loader replaced it, and stale (it still
  emits the old 8-float layout against the now-12-float canonical layout) — a landmine for
  the next caller.
- **Geometry persistence — the mesh-ref resolver (make `scene_load` reconstruct geometry).**
  Today `scene_load` stores the `mesh_ref` *string* (via `scene_mesh_ref_set`) but nothing maps
  it back to a `Mesh`, and `main` never calls `scene_load` at all — so geometry persistence is
  *write-only-tested*: a loaded scene would have every object at the empty (zero) mesh. Build
  the **ref→emitter resolver**: a function that maps a ref name (+ its parameters) to the right
  `MeshBuilder` emitter call and assigns the resulting `Mesh`, run over every object after
  `scene_load`. This is **foundational shared infrastructure** — items 5 (rooms reload at their
  size), 6 (a loaded palace must reconstruct *all* its geometry), and 7 all sit on it — which is
  why it belongs in the gate, not buried in item 5. Items 5/6/10 then only *extend the ref
  vocabulary* (room/wall/terrain params), they don't build the resolver. Pairs with the STML
  escaping fix above: together they make "save → reload an identical scene" actually true, which
  the whole permanence thesis (1.2) depends on.

**Acceptance.** A scene whose meta values contain `"`, `<`, `&`, and leading/trailing
spaces round-trips byte-identically through save→load→save; a scene with referenced geometry
(`<mesh ref="box"/>`) **reloads with its meshes reconstructed**, not empty; allocating past a
resource table fails cleanly instead of corrupting; `obj.*` is gone and the build is still clean.

---

## Part 3 — Items 2–10

### Item 2 — The 2D / in-world UI layer (the keystone)

**Intent.** This is the gate three later items wait behind — labels (everywhere), the
whiteboard's ink (item 8), and the book view (item 9) all need to draw 2D marks and text.
It has been the recurring "real next step" across every direction considered. Build the
lighter half (quads, lines, no text) first so it is usable before the text stack lands.

**Spec.**
- An **immediate-mode** 2D drawing API (the Casey-Muratori lineage): filled quads, outlined
  quads, lines, and textured quads, accumulated into a batch and flushed in a dedicated pass.
  The 2D pipeline uses a vertex format of pos2 + uv + color (all expressible with the existing
  FLOAT2/FLOAT4 formats), depth-test off, blending on.
- **Where it draws is now determined by the actual `render()`, which I have read.** The frame
  is three passes: (0) shadow depth → `shadow_rt`; (1) the HDR pass into `hdr_rt` — skybox
  first with depth off, then the lit object loop; (2) a fullscreen pass to the **window**
  (default framebuffer) that ACES-tonemaps and encodes linear→sRGB via `pow(1/2.2)`. **The UI
  draws after pass 2, over its output, in display/sRGB space** — this resolves the color-space
  decision: pass 2 has already produced gamma-encoded sRGB in a plain (non-sRGB) default
  framebuffer, so the UI shader writes sRGB-ish colors **directly** (no linear→sRGB step,
  unlike the object shader) and blends in that space. Drawing UI *before* the tonemap is wrong
  — ACES would roll off white text to ~0.8 and tint it.
- **Two concrete RHI gaps this forces (both real, both small):**
  1. **`rhi_begin_pass` unconditionally clears** (`glClear(COLOR|DEPTH)`). So the UI cannot
     simply open a fourth pass on the window — it would erase the tonemap result. Either add a
     no-clear / load variant of `rhi_begin_pass` (e.g. a clear-flags parameter), **or** append
     the UI draw calls to the *same* pass as the post triangle (pass 2 already leaves the
     default framebuffer bound and does not call `rhi_end_pass`). Pick one; the clear-flags
     option is the cleaner seam and is reused by item 8/9.
  2. **Blend state** must be added to `RhiPipelineDesc` and implemented in the backend
     (`glEnable(GL_BLEND)` + `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` in
     `rhi_set_pipeline`). **Audit every pipeline-desc site when you do this:** there are ~8 in
     `init_scene` (object, post, shadow, shadow-debug, skybox, skybox-cube, irradiance,
     prefilter, BRDF-LUT), each a stack `RhiPipelineDesc` filled field-by-field — a newly
     added `blend` field left unset on any of them is garbage, silently enabling blending on
     an opaque pass. Set it explicitly (`SOL_FALSE`) at all of them.
- Screen-space first (HUD/debug/panels); **world-space billboarded** marks (a label that
  floats by a card and faces the camera) second.

**Interface sketch.**
```c
void ui_begin(int screen_w, int screen_h);
void ui_quad(float x, float y, float w, float h, float r, float g, float b, float a);
void ui_line(float x0, float y0, float x1, float y1, float thickness, /*rgba*/ ...);
void ui_textured_quad(RhiTexture tex, float x, float y, float w, float h);
/* ui_text(...) arrives with item 3 */
void ui_end(void);   /* flushes the batch — same pass as post, or a no-clear pass */
```

**Acceptance.** A filled rectangle, an outlined rectangle, and a line draw over the
tonemapped scene with correct alpha blending, at correct screen positions, without clearing
the post output or disturbing the 3D depth buffer. A reserved debug-readout area exists (text
fills in at item 3).

**Decisions flagged:** UI color space — **resolved: after-tonemap, sRGB, written directly**
(confirmed by pass 2). The remaining API decision is the `rhi_begin_pass` clear-flags vs
same-pass-append choice; blend-state shape added to `RhiPipelineDesc` (+ the field audit).

---

### Item 3 — Text rendering (SDF glyph atlas + layout)

**Intent.** The honest long pole of the phase: there is no text capability at all today.
Everything readable — labels, notes, the book view — needs it, and once built it unlocks
all of them. Budget weeks, not an afternoon.

**Spec.**
- **SDF glyph atlas.** Rasterize a font into a signed-distance-field atlas texture; the
  shader samples the SDF and thresholds the distance for crisp edges *at any scale* (the
  reason for SDF over a plain bitmap atlas — it scales without blurring, which matters
  because the user zooms in to read).
- **Font rasterization** is the dependency decision (Part 4 / §1.6): `stb_truetype`,
  quarantined like `stb_image`, is the default; hand-rolling is large.
- **Layout.** A function that turns a string into positioned glyph quads: left-to-right line
  breaking / wrapping to a width, monospace handling for code, per-glyph advance/kerning.
  The glyphs are textured quads fed through item 2's batch.
- **Scope now:** one font to start (plus a monospace for code), **single-direction LTR, no
  shaping, no BiDi**, no rich text. Enough to read code and notes legibly. Latin + Greek are
  simple scripts that work with this — both immediately useful for the owner's translation work.
- **Build behind a replaceable `text_shape` seam (§1.6) — this is load-bearing.** Keep
  *"UTF-8 string → array of {glyph index, pen x, pen y}"* as a **single function** the layout
  calls, with a trivial advance-width implementation now. The owner needs complex scripts
  (Hebrew/Arabic/Indic) eventually, and shaping + BiDi **cannot be bolted onto** an advance-width
  layout — the entire string→positioned-glyphs stage gets *replaced*. Isolating it now means the
  later HarfBuzz + SheenBidi implementation swaps *only that one function*, leaving the SDF atlas,
  the item-2 batch, the renderer, and every call site untouched. The seam's output shape —
  `{glyph index, x, y}` — is deliberately the interface HarfBuzz already speaks (it emits glyph
  *indices*, which the atlas rasterizes by index), so the simple and complex implementations are
  drop-in interchangeable.

**Interface sketch.**
```c
Font  font_load(const char *path, int pixel_size);   /* builds the SDF atlas */
Vec2  text_measure(const Font *f, const char *s, float scale);
void  ui_text(const Font *f, const char *s, float x, float y, float scale, /*rgba*/ ...);
/* the seam ui_text + text_measure call — trivial now, HarfBuzz+SheenBidi later: */
int   text_shape(const Font *f, const char *utf8, ShapedGlyph *out, int max);  /* -> {glyph,x,y} */
```

**Acceptance.** A paragraph of prose and a block of monospace code render crisply; zooming
in keeps edges sharp (proves SDF, not a fixed bitmap); text wraps correctly at a width
boundary; the layout reaches glyphs *only* through `text_shape` (so the shaper is swappable).

**Decisions flagged:** font rasterizer (`stb_truetype` vs hand-rolled); which font(s) ship;
**text-script scope** — ship simple LTR (Latin/Greek) now vs. pulling the complex-script stack
(HarfBuzz + SheenBidi, behind the `text_shape` seam) forward (Part 4).

---

### Item 4 — Drag-to-place

**Intent.** This is what makes "organize index cards on a table — but in 3D" actually real.
It is cheap because every piece under it already exists (`scene_pick` returns the stable
handle, objects carry editable TRS, position already round-trips).

**Spec.**
- On pick, enter a drag: each frame, intersect the cursor ray (`camera_ray`) with a **drag
  plane** and set the object's `pos` to the hit, until release. Default plane: the ground
  (`y` = the object's current height). For cards on a wall, the wall's plane. Start with the
  ground-plane case.
- Honors §1.2: the user moves it, it stays, the new `pos` is persisted (it already
  round-trips). Optional grid snapping is a later nicety — flag, don't require.

**Acceptance.** Click and drag an object across the floor; it follows the cursor on the
ground plane and stays where released; reload preserves the new position.

---

### Item 5 — Parametric rooms + walls-with-openings

**Intent.** The box-shaped easy case of procedural generation, playing straight to the
engine's strengths and dodging booleans entirely (§1.4); the substrate the room-graph
(item 7) needs. New geometry is lit by the existing IBL (§1.7), not a new ambient path.

**Spec.**
- **Pin the Room data model here first (§1.10).** Before the geometry, settle the Room
  representation — a `SceneObject` anchor carrying `room_type` + `source_path` in its `meta`
  slots, with contents parented to it — so items 6 and 7 *extend* one definition instead of
  re-inventing it.
- `make_room(w, d, h)`: floor, four walls, ceiling, with **interior-facing normals** (the
  viewer is inside). Decide: flip winding so faces point inward, *or* render room shells
  with back-face culling off. Correct per-surface UVs. Uses `mb_push_vertex` → tangents free.
  **Lighting is automatic and needs nothing special:** the interior normals feed the existing
  IBL ambient (irradiance sampled by the inward normal) plus the spot light, so the room is
  lit the moment it is drawn. And because the skybox draws first with depth off and the walls
  draw over it with depth on (§1.7), the walls *occlude* the sky — you see the environment
  only through an opening or over an open top, never bleeding through a wall. A sealed room
  picking up outdoor irradiance is physically loose but visually fine; a per-room darker
  ambient for sealed interiors is a deferrable refinement, not this item.
- `make_wall_with_opening(w, h, opening_x, opening_w, opening_h)`: a wall built as the pieces
  *around* a rectangular gap (below + left + right + above the opening) — **no boolean cut**.
  This is doorway/window geometry.
- **Parametric mesh-ref — extend the resolver (built in item 1).** The mesh-ref resolver that
  makes `scene_load` reconstruct geometry is now **item 1's job** (the gate). Item 5 only
  *extends its vocabulary*: teach it the `room`/`wall` emitters and let the ref carry parameters
  (e.g. `<mesh ref="room" w="6" d="4" h="3"/>`). That parametric-ref **syntax** is the real
  decision here (Part 4) — keep it consistent with the existing STML style. With the resolver
  already in place, a room round-trips to STML and reloads at its (w,d,h) for free.

**Acceptance.** A room of a given (w,d,h) renders correctly lit from the inside (interior
normals right, no inside-out faces, picks up the environment), and reloads at the same size
from STML; a wall with a door-sized opening renders with no boolean artifacts.

**Decisions flagged:** interior normals via winding-flip vs cull-off; the parametric
mesh-ref syntax (a real format decision — keep it consistent with the existing STML style).

---

### Item 6 — The file / alias / workspace layer (the "3D file browser")

This is the centerpiece. Build the **workspace** first: a cross-cutting, hand-curated,
multi-media room of *aliases* is the capability with no 2D equivalent (the filesystem
forbids one file living in many places), and it is the part actually worth reaching for. The
**mirror** is the easy on-ramp that populates workspaces.

**Intent.** Give the filesystem a spatial body that (a) lets you arrange a folder's contents
like index cards and *feel* its size, and (b) lets you gather aliases to scattered files
plus notes into a room that reflects your intent rather than disk's hierarchy.

**Spec.**
- **Object meaning — a new `kind`** on `SceneObject`: at least `{FILE, ALIAS, NOTE}` (a
  `FOLDER` kind or a folder-flag on `FILE` — Part 4). `content` holds the path for `FILE`
  and `ALIAS`. This is load-bearing for reconciliation. Add it to the struct and the STML.
- **Room meaning — a room type**: `{MIRROR, WORKSPACE}`. A mirror knows its source directory
  path. (Rooms become partly first-class here and partly in item 7 — coordinate. For now a
  room may be a top-level grouping/anchor carrying a type + source path in its metadata.)
- **Directory scanner.** Read a real folder; create a `FILE` object per entry. The room's
  visible population *is* the scale cue — a crowded vs. sparse room surfaces the item count,
  information normally hidden behind "some files." Make density track **count** (primary;
  it matches the index-card intuition — "how many things"); size may be a secondary cue
  (card thickness). Subfolders become enterable (links to item 7).
- **Reconciliation (mirror only — the one genuinely hard part; trust lives here, so the
  rules must be predictable):**
  - *Membership follows disk:* a new file on disk → appears **unplaced** (a tray / at the
    door), never auto-placed over existing arrangement; a removed file → leaves but drops a
    **tombstone** (so a vanished file does not silently erase the note you attached to it).
  - *Arrangement follows the user (§1.2):* a placed file is never auto-moved; its position
    persists keyed by path/`nid`.
  - The user can always tell a mirror from a workspace.
- **Workspace / aliasing.** An alias is an `ALIAS` object whose `content` is the real path —
  a *reference*, not a copy (editing the file edits the one real file). The same path may be
  aliased into many workspaces. Workspaces have **no** reconciliation (nothing auto-arrives
  or leaves); a stale alias whose target was deleted is *flagged*, not silently removed.

**Interface sketch.**
```c
void    room_mirror_scan(Scene *s, sol_u32 room, const char *dirpath); /* reconcile to disk */
sol_u32 workspace_add_alias(Scene *s, sol_u32 room, const char *filepath);
sol_u32 room_add_note(Scene *s, sol_u32 room, const char *text);
/* SceneObject gains: kind; Room gains: type + source_path */
```

**Acceptance.** Point a mirror room at a real folder → every file appears as a placeable
card and the room's density visibly reflects the folder's size; arrange the cards, reload →
arrangement preserved; add a file on disk and rescan → it appears unplaced, the others
unmoved; delete a file on disk and rescan → a tombstone remains and your attached note
survives; alias one file into two workspaces → both hold a live reference to the single
file.

**Decisions flagged:** the object-`kind` set; folder as a kind vs a flag; tombstone UX
(auto-dismiss vs manual); the primary scale signal (count recommended).

---

### Item 7 — The room-graph / portal system

**Intent.** Turns a scene into a navigable *building* (a place, not a diorama) — the thing
that makes the palace feel inhabited. New architecture specific to this direction. Start
dead simple.

**Spec.**
- Rooms are **nodes**, doors are **edges**. A door is an object placed on a wall opening
  (item 5) carrying a reference to a target room.
- Crossing the threshold (proximity, or click-the-door) **transitions**: activate the target
  room's contents, deactivate the current. "Activate" may start trivial — all rooms resident
  in memory, switch which renders — and grow to real **streaming** (load a room from its own
  file on demand) as the palace exceeds memory.
- A mirror room's door into a subfolder links to / generates that subfolder's mirror room
  (folders-as-rooms: entering a folder walks you into it).
- The on-disk form grows to represent multiple rooms + the door edges. **Decide:** one
  monolithic STML vs. **per-room files + a small graph file** (recommended — it scales and
  maps naturally onto "each folder is a room").

**Acceptance.** Two rooms linked by a door; walk through → you are in the other room with its
contents active and the first deactivated; a mirror room's subfolder door → entering shows
that subfolder's contents.

**Decisions flagged:** per-room files vs monolithic (per-room recommended); proximity vs
click transition.

---

### Item 8 — The whiteboard

**Intent.** Mostly composition of the scene spine plus the item-2/3 layer; the surface where
cross-cutting thinking gets pinned and connected. Depends on items 2 and 3.

**Spec.**
- A board surface (a quad/plane) onto which cards (`FILE`/`ALIAS`/`NOTE` objects) attach by
  parenting (the hierarchy already supports this; moving the board moves the cluster).
- **2D ink / arrows between cards**, drawn via item 2's line/vector capability in the board's
  plane — an arrow from card A to card B makes a relation visible (tie it to the existing
  `relations` slot so the connection persists as data, not just pixels).
- Notes are `NOTE` objects rendered with item 3's text.

**Acceptance.** A board holding several file/note cards; draw an arrow connecting two of
them; reload preserves the cards, their positions, and their connections.

---

### Item 9 — The book / file view

**Intent.** The "actually read the file" capability — it turns cards-you-recognize-by-
position into cards-you-can-open, and it graduates the Phase-1 parchment page (which showed a
pre-rendered *image*) into real text. Gated on the full text stack (item 3).

**Spec.**
- Opening a `FILE`/`ALIAS` object yields a **paginated, scrollable** view of the file's text
  contents, rendered via item 3. Reuse `camera_focus` to frame the reader head-on (it
  already does this for the page surface).
- Pagination or continuous scroll; monospace for code; optional syntax highlighting.
- This replaces the Phase-1 page-image placeholder with live text — note the continuity; the
  parchment surface was always the stepping stone to this.

**Acceptance.** Click a code-file card → its contents render as readable, scrollable/
paginated monospace text; a prose file wraps readably.

**Decisions flagged:** syntax highlighting in scope or deferred; pagination vs continuous
scroll.

---

### Item 10 — Terrain (parallel track, low-risk)

**Intent.** The exterior canvas that grounds the whole palace — the place rooms and buildings
sit *on*. Cleanly independent of the critical path (buildable any time), low-risk, and almost
entirely aligned with what the engine already does well. Lit by the existing IBL; the skybox
is its backdrop (§1.7).

**Spec.**
- `make_terrain(heightmap, w, d, subdiv)`: `make_grid` with each vertex's `y` taken from the
  heightmap and **per-vertex normals computed from neighboring heights** (finite difference —
  the grid's flat up-normal is wrong for a slope). Uses `mb_push_vertex` → tangents free.
  **These normals matter doubly now that ambient is real IBL (§1.7):** the irradiance is
  sampled *by the surface normal*, so a correct slope normal makes a hillside pick up sky
  light from the direction it actually faces, while the grid's stock up-normal would make
  every point sample only the overhead sky and read as flat, dead ground. Getting the
  finite-difference normals right is the difference between terrain that looks lit and terrain
  that looks like a painted plane.
- **Heights from a grayscale image** first (`image_load` already exists; brightness =
  elevation). Procedural noise (self-contained fBm) is an **optional** second supplier later
  — additive, not a prerequisite.
- **Slope/height texture blending** in the shader: grass on flat/low ground, rock on steep
  slopes, etc., chosen from the surface normal's steepness + the vertex height (both already
  available in the fragment stage). This is the automatic "looks good with no hand-painting"
  win and a modest extension of the existing material shader — it composes on top of IBL
  ambient, it does not replace it.
- **A ground-height query** `terrain_height_at(x, z)` so a placed room/object sits on the
  surface — the one seam between the exterior canvas and interior rooms.
- **Scope to a site, not a world** → no LOD needed (a single ~256×256 grid covers a generous
  building plot at trivial cost; LOD is the only hard part and the constraint eliminates it).
  A height field is single-valued, so no caves/overhangs — fine for a building canvas.

**Acceptance.** A heightmap image loads as correctly-lit terrain; slope/height blending
textures it plausibly with no hand-painting; a room placed at (x,z) sits on the surface via
the height query.

**Decisions flagged:** image-heightmap first (procedural noise deferred); how many texture
layers in the blend.

---

## Part 4 — Decisions reserved to the project owner

Set these before delegating; an agent guessing them is the real risk.

1. **Text rasterizer + script scope (item 3):** two linked calls. **(a) Rasterizer:**
   `stb_truetype` (a *second* stb dependency, quarantined like `stb_image`) vs. hand-rolled —
   default `stb_truetype` per the `stb_image` precedent, but it is a real new dependency, so
   decide on purpose. **(b) Script scope / timing:** ship **simple LTR (Latin/Greek) now** and
   defer the complex-script stack — **HarfBuzz** (shaping) + **SheenBidi** (BiDi), both
   quarantined, the build linking the C++ runtime — *behind the `text_shape` seam*
   (recommended: the seam makes it a clean drop-in later), **vs.** pulling that stack forward now
   if Hebrew/Arabic/Indic are needed sooner. The owner does multilingual translation work, so
   this stack is a *when*, not an *if* — the seam (item 3) exists precisely to make the *when*
   cheap.
2. **UI compositing into the window (item 2):** color space is **resolved** — after-tonemap,
   sRGB, written directly (pass 2 already produces gamma-encoded sRGB). The remaining call is
   *how the UI avoids the unconditional clear in `rhi_begin_pass`*: add a clear-flags / no-clear
   variant to `rhi_begin_pass` (cleaner, reused by items 8/9) vs. append UI draws to the
   existing post pass. Pick one.
3. **Parametric mesh-ref syntax (item 5):** the concrete STML form for a parameterized shape
   reference (e.g. `<mesh ref="room" w=".." d=".." h=".."/>`).
4. **Object-`kind` set (item 6):** the enumeration (`FILE`/`ALIAS`/`NOTE`/…), and whether a
   folder is its own kind or a flag on `FILE`.
5. **Room-graph storage (item 7):** per-room files + a graph file (recommended) vs. one
   monolithic STML.
6. **Tombstone UX (item 6):** auto-dismiss vs. manual when a mirrored file disappears.
7. **Primary scale signal (item 6):** count (recommended) vs. size for room density.
8. **Syntax highlighting (item 9):** in scope now or deferred.
9. **Terrain texture layers (item 10):** how many blended materials in the slope/height shader.

---

## Part 5 — Deliberately deferred (not this phase)

- **Rendering polish beyond what IBL already buys:** additional punctual lights, transparency
  as a general *material* category (item 2's blending covers UI, not glass/foliage),
  MSAA/bloom. Real, but they make a *renderer* beautiful, not the *tool* exist — the palace
  is useful under the current renderer.
- **Frustum culling** (and a spatial structure for it). The palace accretes objects, so this
  has a real claim — but it is a perf optimization that does not block usefulness at room
  scale. Add it when a populated workspace actually strains `render()`'s unconditional draw.
- **Procedural terrain generation** beyond loaded heightmaps (the fBm noise supplier) —
  additive, after the image path works.
- **The CAD / modeling direction in full:** constraint solvers, mesh booleans, free-form
  sculpting. Explicitly out of scope (§1.4) — these are the unbounded-difficulty traps this
  project routes around. Parametric rooms/walls/terrain are the *only* geometry authoring
  this phase.
- **Splat-painting terrain, procedural texture/material node graphs** — both need a painting
  UI, a separate large effort.
- **Object scripting / behavior (Speculum), a second rendering backend, real text shaping /
  bidi / rich text** — later phases.

The cross-cutting concerns of *this* phase — the permanence invariant (1.2), the mirror/
workspace distinction (1.3), the no-booleans rule (1.4), the parametric mesh-ref, and the
object-`kind` / room-type fields — are pinned here precisely because they are painful to
retrofit, which is the same design discipline the project applies to itself.

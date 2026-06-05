# Solarium Engine — Phase Execution Brief (Items 0–9)

## How to use this document

This is the execution brief for the next phase of the engine. You already have the
current source (`main.c`, `rhi.h`, `sol_math.h`) and its house style. What you do **not**
have is the long-term intent behind the work, and several load-bearing decisions that the
nine-item milestone list silently assumes.

**Part 1 (the Constitution) is binding on every item.** Read it before starting any item.
It encodes invariants that are un-inferable from the milestones and that, if violated, will
produce code that passes its checkpoint while quietly breaking the architecture. Where the
normal, minimal, "ship the smallest thing" instinct conflicts with this brief, follow this
brief — the deviations are deliberate.

Each item below states its **intent** (the *why*, which you lack), its **spec** (what to
build, with decisions already made), an **interface sketch** where it helps, and concrete
**acceptance criteria**.

---

## Part 1 — The Constitution (project-wide invariants)

### 1.1 Language: strict C89 / C90

The current files use C99 constructs (compound literals, designated initializers,
`for (int …)`, declarations after statements, `static inline`, `<stdint.h>`/`<stdbool.h>`).
**That was provisional.** Item 0 brings the codebase to strict C89, and everything after it
is written in strict C89. The build must pass:

```
-std=c89 -pedantic-errors -Wall -Wextra
```

Forbidden constructs and their fixes:

| C99 construct | Fix |
|---|---|
| compound literals `(T){...}` | assign fields into a named local, return the local |
| designated initializers `.field = x` | zero-initialize (`= {0}`), then assign fields by statement |
| mixed declarations and statements | all declarations at the top of the block |
| `for (int i = 0; ...)` | declare the index above the loop |
| `// comment` | `/* comment */` |
| `inline` / `static inline` | plain `static`; use the `SOL_INLINE` macro (empty in C89) if you want intent markers |
| `<stdint.h>`, `uint32_t`, etc. | the project typedefs below |
| `<stdbool.h>`, `bool`, `true`, `false` | `sol_bool` + `SOL_TRUE`/`SOL_FALSE` below |
| `long long`, `snprintf`, variadic macros, designated array init | avoid; use C89-safe equivalents |

Add a single compatibility header, included everywhere, instead of `<stdint.h>`/`<stdbool.h>`:

```c
/* sol_base.h — fundamental types for strict C89. Assumes 32-bit int,
   which holds on all target desktop platforms (Win/Linux/macOS). */
#ifndef SOL_BASE_H
#define SOL_BASE_H

typedef unsigned char  sol_u8;
typedef unsigned short sol_u16;
typedef unsigned int   sol_u32;
typedef int            sol_i32;
typedef float          sol_f32;
typedef double         sol_f64;

typedef int sol_bool;
#define SOL_TRUE  1
#define SOL_FALSE 0

#define SOL_INLINE /* empty: C89 has no inline */

#endif /* SOL_BASE_H */
```

The existing opaque handles (`typedef struct { uint32_t id; } RhiBuffer;`) become `sol_u32`.

### 1.2 The RHI seam is inviolable

This is the single most important architectural rule. **No GL symbol — no `gl*` call, no
`GL_*` constant, no GL/GLEW/glad header — appears in any file except the backend
implementation (`rhi_gl.c`) and its private headers.** Application code, the scene, the
tools, and the math include only `rhi.h`.

The shared files already honor this (`main.c` sets `GLFW_INCLUDE_NONE` and talks only to
`rhi.h`). Maintain it. **When an item needs a new GPU capability, you extend the `rhi.h`
interface and implement it in the backend — you never reach for GL from above the seam.**

Two specific traps in this phase:
- **Item 4 (picking):** do *not* call `glReadPixels` from app code. Use the CPU ray-cast
  method specified there.
- **Item 7 (render targets):** do *not* call `glGenFramebuffers`/`glBindFramebuffer` from
  scene or app code. Add a render-target abstraction to the RHI; all framebuffer GL lives
  in the backend.

The reason this matters beyond tidiness: a second rendering backend (Metal/WebGPU) is a
planned future phase, and the seam is what makes it a new backend rather than a rewrite.

### 1.3 Zero dependencies

No package manager, no third-party libraries by default in the **engine/runtime**. **GLFW is
the sanctioned platform dependency** (window, input, timing), confined to platform/`main`-level
code, never leaking into the RHI or above.

Two decisions this phase forces, ruled here (see §4 — these are the project owner's calls):
- **Image decoding (item 5): AMENDED — vendor `stb_image.h`.** Originally this was "hand-roll a
  minimal TGA reader, no `stb_image`." Flipped by the owner (2026-06): we realistically need
  **PNG** (page scans, diagrams), and hand-rolling PNG means implementing DEFLATE/zlib + CRC —
  a large detour off the palace goal, unlike TGA. `stb_image` (public-domain single-header,
  vendored at `vendor/stb_image.h`) decodes PNG/JPEG/TGA/BMP behind one call. The hand-rolled
  TGA reader is **dropped**. Scope of the exception: image **decode only**, a content-import
  concern — *not* a general loosening. It's quarantined like GLFW (its implementation TU built
  with relaxed warnings, excluded from `c89check`), stays **above the seam** (CPU → RGBA buffer
  handed to `rhi_create_texture`), and the **sRGB-vs-linear** handling remains ours (stb returns
  raw bytes; we mark the GL texture format).
- **glTF loading (item 6):** hand-roll a minimal **`.glb`** parser for the static subset.
  Do *not* use `cgltf` or `assimp`. (Unchanged — a `.glb` for our static subset is tractable to
  hand-roll, and avoids a much heavier dependency.)

The principle, restated: **zero deps in the engine/runtime; a vendored public-domain header is
permitted for a solved content-import problem (image decode).** The renderer, seam, scene/
identity model, math, and camera remain hand-rolled and owned.

### 1.4 What this software is: a persistent knowledge environment, not a game

This is the context the milestones don't carry, and it changes early decisions. The
application is a navigable **memory-palace / spatial PKM** — a walkable space in which the
user places knowledge (texts, notes, references) at locations and re-encounters it by being
near it. **Object permanence is the core value:** the room must save and reload with every
object where the user left it, because the whole cognitive payoff is *recognition* (you
walk past a thing and rediscover it) rather than *recall* (you must remember to search for
it). A space that silently rearranges itself defeats the entire purpose.

Consequence: **identity and persistence are not features to add later — they are properties
the scene has from birth (item 2).** And contrary to normal minimal-build instinct, you are
asked to **deliberately overbuild the seams**: leave identity, metadata, relationship, and
attached-content slots present in the data model even though they are mostly empty this
phase. These cannot be retrofitted cleanly onto a render-only scene; building them in now is
intentional, not premature.

### 1.5 Future integrations that constrain present decisions

Do not build these now. Just do not foreclose them:
- A future **ontology/entity store** will ingest the scene → the scene's on-disk form is
  shaped like an entity graph (objects with IDs, types, attributes, relationships), **not**
  a binary dump and **not** array-index-as-identity. (This is so it can later marry the
  owner's "Smaragda" entity store without coupling to it now.)
- A future **object scripting/behavior layer** means objects will gain behavior → keep the
  object record extensible (the metadata/relationship slots serve this).
- A future **second rendering backend** → §1.2 seam discipline is non-negotiable.
- A future **CAD/constraint + procedural-generation layer** → the mesh builder (item 1) is
  the foundation of all generated geometry; build it general, not single-purpose.

### 1.6 The canonical vertex layout (decided once, used everywhere)

To prevent re-tooling at items 5/6/8, fix the interleaved vertex layout now:

```
position (3 floats), normal (3 floats), uv (2 floats)  =  8 floats / 32 bytes
```

- Tangent (4 floats) is **reserved** for normal-mapped PBR but **not added yet**; design the
  mesh builder and RHI attribute setup so adding it later is a clean extension, not a rewrite.
- **Index type is `sol_u32`** everywhere.
- The current shader's vertex inputs (`aPos`, `aNormal`) extend to include `aUV` at location 2.

### 1.7 Definition of done (applies to every item)

An item is not done until:
- It builds clean under `-std=c89 -pedantic-errors -Wall -Wextra` (no warnings).
- It runs clean under AddressSanitizer + UndefinedBehaviorSanitizer (debug build).
- A debug GL context is requested, and (where the platform's GL exposes `KHR_debug`)
  `glDebugMessageCallback` is installed in the backend and **silent** — no GL errors or
  warnings — during normal operation. Where `KHR_debug` is unavailable, check `glGetError`
  at call boundaries in debug builds. (Install this in item 1 and keep it silent thereafter;
  a new GL warning is a regression.)
- Its stated checkpoint demonstrably passes.
- No GL symbol appears above the seam; no new external dependency was added.

### 1.8 Build target

GL 3.3 core profile, GLFW for the platform layer, the project's existing dev platform and
build script. Match the existing build invocation; the flag set in §1.1/§1.7 is what must
pass.

**Platform decision (Option A — chosen).** The GL backend is **macOS-only this phase**: it
uses the system OpenGL framework and `<OpenGL/gl3.h>`, so no GL function loader is needed
(macOS provides modern GL symbols directly). Cross-platform GL (Linux/Windows) is
**deferred** — it would require a runtime function loader (a hand-rolled `glfwGetProcAddress`
shim, per §1.3's zero-dep stance, rather than vendoring GLEW), and that loader pattern should
be stood up at the start of whatever phase first needs it, not retrofitted. A Metal backend
for macOS is likewise a later phase. The RHI seam (§1.2) is precisely what keeps both the
cross-platform-GL and the Metal options open without touching app code.

---

## Part 2 — Item 0: the C89 conformance pass (gating; do first)

**Intent.** Settle the standard before adding features, while it is ~200 mechanical lines.
Every later item inherits the answer; doing this last would mean re-touching every file.

**Spec.** Bring `main.c`, `rhi.h`, `sol_math.h` (and `rhi_gl.c` if it exists) to strict C89
per §1.1. Specifically, in the current files: the compound literals throughout `sol_math.h`;
the designated initializers in `main.c`'s `RhiPipelineDesc`/attribute setup; the `for (int …)`
loops in `load_obj`, the recenter loops, and `mat4_mul`; the declarations-after-statements in
`render`, `init_scene`, and `main`; `static inline` across `sol_math.h`; and the
`<stdint.h>`/`<stdbool.h>` usage in `rhi.h` (handles become `sol_u32`; `bool`→`sol_bool`).
Add `sol_base.h` and include it where the fundamental types are used.

**Acceptance.** The three (or four) files compile under `-std=c89 -pedantic-errors -Wall
-Wextra` with zero warnings, and the existing Suzanne demo still runs identically.

---

## Part 3 — The nine items

### Item 1 — Close RHI gaps + the mesh builder

**Intent.** Indexed drawing is required for glTF and any real mesh; teardown is required
because a long-running, scene-editing app creates and destroys objects; the mesh builder is
the **seed of all blockout and procedural geometry** (a future CAD/procedural layer builds on
it), so it must be general, not a one-off.

**Spec.**
- RHI additions: `rhi_draw_indexed(first_index, index_count)`; index-buffer creation and
  binding (the `RHI_BUFFER_INDEX` enum already exists); `rhi_destroy_buffer/shader/pipeline/texture`.
- A `MeshBuilder` that appends vertices **in the canonical layout (§1.6)** and indices,
  generalizing the existing `FloatArray`/`fa_push` idiom (which is its seed). Index type `sol_u32`.
- Primitive emitters: `make_box`, `make_plane`, `make_grid` (expect `make_cylinder`/`make_arch`
  soon). **Emitters must generate correct normals and UVs** — per-face normals and seams for
  the box (so a cube isn't smooth-shaded), planar UVs for plane/grid.

**Interface sketch.**
```c
typedef struct {
    sol_f32 *vertices;  sol_u32 vertex_count;  sol_u32 vertex_cap;   /* 8 floats each */
    sol_u32 *indices;   sol_u32 index_count;   sol_u32 index_cap;
} MeshBuilder;

void    mb_init(MeshBuilder *b);
void    mb_free(MeshBuilder *b);
sol_u32 mb_push_vertex(MeshBuilder *b, sol_f32 px, sol_f32 py, sol_f32 pz,
                       sol_f32 nx, sol_f32 ny, sol_f32 nz, sol_f32 u, sol_f32 v);
void    mb_push_triangle(MeshBuilder *b, sol_u32 a, sol_u32 i, sol_u32 c);

void    make_box(MeshBuilder *b, sol_f32 w, sol_f32 h, sol_f32 d);
void    make_plane(MeshBuilder *b, sol_f32 w, sol_f32 d, sol_u32 subdiv);
/* ... a Mesh (vertex buffer + index buffer + index_count) is built from the result. */
```

**Acceptance.** An indexed box produced by the builder renders with correct flat-shaded
faces; creating and destroying buffers in a loop is ASan-clean.

---

### Item 2 — The scene as a persistent, identified object graph

This is the architectural keystone of the phase. Treat it as a real subsystem, not a struct.

**Intent.** Per §1.4: object permanence is the core cognitive value, and it only works if
the scene saves and reloads with stable identity and positions. Build the identity and
persistence in from birth, and overbuild the slots — they cannot be retrofitted.

**Spec — the object record.** Each `SceneObject` has:
- A **stable ID** (`sol_u32`), assigned monotonically at creation, that **survives save/reload
  unchanged and is never reused** — explicitly **not** an array index.
- A **transform stored as TRS** (position vec3, rotation quaternion, scale vec3), composed to
  a `mat4` at render time. (TRS, not a bare matrix: the authoring/CAD layer will want the
  components, and storing real scale is what lets item 8 compute a correct normal matrix.)
- A **mesh handle** and a **material handle**.
- A **parent ID** (`0` = root) for **hierarchy** — needed for the click-a-shelf-to-zoom-into-it
  nesting; transforms compose down the hierarchy.
- **Reserved slots, mostly empty this phase but present in the data model and serialized:**
  a small string→string **metadata** map; a list of **typed relationships** (typed edges to
  other object IDs); and an **attached-content reference** (a content ID/path, for the future
  "this object *is* a book/note").

**Spec — the scene + serialization.**
- A `Scene` holds a growable collection of objects plus an ID→object lookup. Render iterates it.
- **Serialization is human-readable and shaped like an entity graph** (each object: id, type,
  transform, parent, attributes, relationships, content-ref) — **not** a binary dump, **not**
  index-based. Designed so a future ontology store could ingest it; not coupled to one now.
- Persist the next-ID counter so IDs remain stable and unique across sessions.
- Save then load must round-trip to a **semantically identical** scene with stable ordering
  (identical IDs, transforms, hierarchy).

**Interface sketch.**
```c
typedef struct {
    sol_u32     id;        sol_u32 parent;
    Transform   trs;       /* pos (vec3), rot (quat), scale (vec3) */
    RhiBuffer   mesh_vb;   RhiBuffer mesh_ib;  sol_u32 index_count;
    MaterialId  material;
    Metadata    meta;      /* string->string; empty for now           */
    Relation   *relations; sol_u32 relation_count;  /* empty for now  */
    ContentRef  content;   /* attached doc/note; null for now         */
} SceneObject;

sol_u32      scene_add(Scene *s, /* mesh, material, transform */ ...);
SceneObject *scene_get(Scene *s, sol_u32 id);
sol_bool     scene_save(const Scene *s, const char *path);
sol_bool     scene_load(Scene *s, const char *path);
```

**Acceptance.** Build a scene in code with several **nested** objects and a few metadata
entries; save it; reload it; IDs, transforms, hierarchy, and metadata are identical and it
renders identically. IDs are not array indices (verify by deleting an object and confirming
others keep their IDs).

---

### Item 3 — Movable camera + navigation

**Intent.** Being *in* the room (first-person) is the spatial presence the thesis rests on;
orbit is for inspecting an object or diorama.

**Spec.** A `Camera` (position, orientation as yaw/pitch, fov). Two modes: **first-person**
(WASD + mouse-look, with the cursor captured) and **orbit** (drag to rotate about a target,
scroll to dolly). Wire input through the existing GLFW callback pattern. Build the view matrix
with the existing `sol_math` (`mat4_look_at` for orbit; a yaw/pitch forward vector for FP).

**Acceptance.** Walk and look through the persisted scene in first person; toggle to orbit and
rotate around a selected object.

---

### Item 4 — Object picking

**Intent.** The interaction primitive for the entire "the space responds" thesis — pick up a
book, click a shelf to zoom. Nothing in the authoring or knowledge layer happens without it.

**Spec.** A **CPU ray-cast** from the camera through the cursor (or screen center in FP),
tested against object **AABBs** for the broad phase, then optionally a triangle test for
precision. Use the existing vec/mat math. **Method is fixed: CPU ray-cast now — explicitly
NOT an ID-buffer / `glReadPixels` pass**, because that would pull item 7's render targets
forward and would put GL above the seam. The picker **returns the stable object ID from item
2** (not a pointer, not an index).

**Acceptance.** Clicking any object reports its ID and highlights it; with overlapping objects,
the nearest hit along the ray is selected.

---

### Item 5 — Textures + a readable surface  ★ first dogfoodable palace

**Intent.** This is where the engine first **earns its keep**: a crude but real palace you can
read in, reached *before* PBR and shadows. From here, lived use (your actual reading work)
starts steering the build. Hit this milestone, then stop and use it before continuing.

**Spec.**
- Flesh out the stubbed `RhiTexture`: create from pixel data, bind to a sampler slot, sample in
  the fragment shader. Image decode: **`stb_image`** (vendored `vendor/stb_image.h`) → an RGBA
  buffer handed to `rhi_create_texture` (§1.3, amended — supports PNG/JPEG/TGA/BMP).
- **CRITICAL — color space is part of the texture's identity, decided now:** the texture/format
  enum must distinguish **sRGB** textures (albedo, page images → decoded/sampled sRGB→linear)
  from **linear** textures (normal/roughness/data → sampled raw). Encode this at creation now so
  item 8 does not tear up the texture path. (This is the most common rendering bug in this
  whole sequence.)
- The reading surface: a **parchment quad** placed in the room; walking up to it or clicking it
  (**reuses item 4 picking**) zooms the camera to a framed reading view; it displays a page as a
  **pre-rendered image texture**.
- **Scope hard: this is NOT text layout.** Real text rendering — a glyph atlas or SDF text,
  pagination, and the HyperCard-style document model — is a deferred subsystem (see Part 5). The
  page here is an *image*. Do not build a layout engine; and do not mistake this surface for the
  final one — it is the placeholder the future text system will replace.

**Acceptance.** A room contains a book object; you walk to it (or click it), the camera frames
it, and a readable page image is displayed. The sRGB and linear texture paths both exist and are
correct (an sRGB albedo and a linear data texture sample correctly).

---

### Item 6 — glTF (.glb) import

**Intent.** Retire OBJ. This is the bridge to the entire acquired / AI-generated / scanned asset
world — the bookshelf, the desk, the candlestick become real assets with real materials. The
OBJ loader's gaps (negative indices, missing normals) disappear, since glTF is well-specified.

**Spec.** A **hand-rolled minimal `.glb` (binary glTF) parser** per §1.3 — no `cgltf`/`assimp`.
- Supported subset: static meshes, indexed geometry, base-color factor + texture, metallic-
  roughness factors + texture, normal map.
- **Explicitly not supported:** animation, skins, morph targets, glTF cameras, glTF lights.
- Honor glTF conventions: **right-handed, +Y up, counter-clockwise front faces.** Convert glTF
  accessors into the **canonical vertex layout (§1.6)**. Populate the item-2 material handle from
  the glTF material.

**Acceptance.** A downloaded or AI-generated `.glb` renders in the room with correct orientation
and winding and its base-color + textures applied.

---

### Item 7 — Offscreen render targets + HDR pipeline

**Intent.** Your first multi-pass setup and the prerequisite for PBR, shadows, IBL, and the
candlelight glow. The gamma encode moves out of the object shader (the current shader's inline
`pow(color, 1/2.2)`) into a dedicated final pass.

**Spec.**
- Add a **reusable render-target/framebuffer abstraction to the RHI**: create an RT with
  color + depth attachments and selectable formats (including `RGBA16F`); begin/end a pass that
  targets an RT; sample an RT as a texture. **All framebuffer GL stays in the backend (§1.2).**
- Render the scene into an **HDR (`RGBA16F`) buffer**, then a fullscreen pass that **tonemaps**
  (named operator — use **ACES filmic**; Reinhard acceptable as a fallback) and encodes linear→
  sRGB into the default framebuffer. Remove the inline gamma from the object shader.

**Acceptance.** The scene renders through the HDR buffer; bright highlights (e.g. a candle) roll
off smoothly rather than clipping; no banding in the gradients.

---

### Item 8 — PBR materials

**Intent.** Now textures (item 5), glTF materials (item 6), and the linear/HDR pipeline (item 7)
all feed a real BRDF, and the room's materials become physically plausible — wood, brass,
leather, parchment read correctly under light.

**Spec.**
- Replace Blinn-Phong with **Cook-Torrance metallic-roughness**: GGX normal distribution, Smith
  geometry term, Schlick Fresnel, Lambertian diffuse weighted by `(1 - metallic)`.
- Read albedo / metallic / roughness / normal / AO from the glTF material maps.
- **Direct light only** this phase (the existing directional light); IBL is deferred (Part 5).
- **CRITICAL — sampling color spaces (the most common PBR bug):** albedo is sampled sRGB→linear;
  metallic/roughness/normal/AO are sampled **linear**. Honor the item-5 color-space distinction.
- Replace the `mat3(uModel)` normal transform with a proper **normal matrix** (inverse-transpose
  of the upper-left 3×3), computed CPU-side and passed as a uniform — the shortcut breaks under
  non-uniform scale, and item 2 now stores real scale.

**Acceptance.** A glTF asset renders under correct PBR from the direct light; metals and
dielectrics are visibly distinct; no sRGB/linear errors (no washed-out or oversaturated albedo,
correct normal-map lighting).

---

### Item 9 — Shadow mapping (one light)

**Intent.** The single biggest leap toward the design's candlelit room actually feeling like one
— contact and form come from cast shadows.

**Spec.** A **shadow map** (depth-only render to a depth target, **reusing item 7's RT
abstraction**) for **one** shadow-casting light — a **spot** light suits candlelight or a window
shaft. Sample in the lighting pass with **depth bias + PCF** (e.g. 3×3) to soften edges and kill
acne. **Explicitly scope to one light: no cascades/CSM, no point-light cube shadows yet.**

**Acceptance.** Objects cast and receive shadows from the one light; no shadow acne (bias tuned);
edges are soft via PCF.

---

## Part 4 — Decisions reserved to the project owner (encoded here; flip if you disagree)

These are load-bearing and yours, not the agent's, to set. I have encoded the established
default in the spec above; change them in this document before handing it off if you want
otherwise:

1. **Image decoding:** `stb_image` (vendored), decode-only exception. [§1.3 AMENDED, item 5]
2. **glTF loading:** hand-rolled `.glb` (not `cgltf`/`assimp`). [§1.3, item 6]
3. **Object identity:** **DECIDED** — persistent ID is a **ULID-style `nid`** (timestamp+random
   base32: globally unique, mergeable, time-sortable, weak-RNG-tolerant); runtime ID is a
   `sol_u32` handle **mapped from the nid on load** (the Unity-style persistent/runtime split).
   Supersedes the earlier plain-`sol_u32` default. [item 2; see SCENE_FORMAT.md]
4. **Transform storage:** TRS components (not a bare `mat4`). [item 2 — done in 2.1]
5. **Serialization format:** **DECIDED** — **STML** (the project's own tag markup), restricted to
   a small C89 "data profile" subset (elements, attrs incl. boolean, nesting, `</>`, restricted
   `<tag (>` text-capture, `!` raw, comments; no capture-operators/transclusion/selectors).
   Human-readable, entity-graph-shaped, text — as required. Reusable `stml.c` DOM; the future
   document/content model is its second client. [item 2; full spec in SCENE_FORMAT.md]
6. **Tonemap operator:** ACES filmic. [item 7]

**Progress:** Item 0 (C89 conformance) and Item 1 (RHI gaps + MeshBuilder + box/plane/grid)
complete. **Item 2 complete** — the persistent, identified object graph: 2.1 quaternion/TRS,
2.2 Scene + stable IDs, 2.3 hierarchy, 2.4 overbuilt slots, and 2.5 STML serialization
(2.5a stml.c DOM parser → 2.5b ULID nid generator → 2.5c scene_save → 2.5d scene_load +
byte-identical round-trip → 2.5e keystone proof: identity survives deletion and reload).
**Item 3 complete** — movable camera + navigation: 3a promoted sol_math.h → sol_math.c
(compiled module), 3b Camera + first-person walk/fly (keyboard, platform-free via a
CameraInput struct), 3c mouse-look (cursor capture, relative deltas), 3d orbit mode +
FP/orbit toggle (scroll via window user-pointer callback). camera.c is headless-tested.
**Item 4 complete** — object picking (CPU ray-cast, not glReadPixels): 4a Ray/Aabb +
ray_vs_aabb (slab) in sol_math, Mesh local AABB at build time, object_world_matrix moved
into scene.c as scene_world_matrix; 4b scene_pick (nearest AABB hit → stable handle); 4c
camera_ray (analytic) + left-click wiring (FP center / orbit tap-vs-drag); 4d selection
highlight (uHighlight uniform, new rhi_set_uniform_float) + orbit pivots on the clicked
object (snapshot target). Pick math headless-tested (picktest).
**Item 6 complete** — glTF (.glb) import (hand-rolled, no cgltf/assimp): 6a json.c/.h
recursive-descent JSON parser (+jsontest); 6b GLB container + accessor decode → MeshBuilder
(book renders, auto-fit-to-bounds); 6c materials → embedded base-color textures (stb memory
decode, cached by image index); 6d node-tree traversal with world transforms BAKED into
geometry (book + candle stand in the room), OBJ retired. Static subset only; MR/normal maps
parsed-path not used until Item 8.
**Item 5 complete (★ first dogfoodable palace)** — textures + a readable surface: 5a fleshed
out RhiTexture (create/bind/sample, handle table) with a color-space format enum
(RHI_TEX_SRGB8 vs RHI_TEX_RGBA8) + rhi_set_uniform_int, shader samples an albedo; 5b stb_image
decode (image.c quarantined TU, vendor/stb_image.h, §1.3-amended) → sRGB texture; 5c per-object
material (SceneObject.texture + scene_texture_set) + the parchment reading quad (purpose-built
upright vertical quad); 5d click the page → camera_focus frames a head-on reading view. See
SCENE_FORMAT.md and git history. **Next: Item 7 — offscreen render targets + HDR pipeline
(first multi-pass; moves the gamma encode out of the object shader into a final pass).**

Known boundaries deferred out of Item 2 (not bugs — scoped follow-ups):
- `scene_load` restores `mesh_ref` (the name) but not GL geometry; wiring ref→generator
  needs a small asset registry before a loaded scene can render. Until then load is
  exercised headlessly, not called from `main.c`.
- STML raw `!`-capture is recognized (sets `node->raw`) but capture text always runs to the
  next `<`; the line-counted raw variant (text containing `<`) is future, for the item-5
  content model.
- `scene_remove` leaves references to the removed object dangling (handled gracefully); a
  cascade/reparent delete policy is a later editor concern.

---

## Part 5 — Deliberately deferred (do not build in this phase)

So the agent does not wander into these:
- **Real text + reading subsystem** — glyph atlas or SDF text, pagination, and the
  HyperCard-style paginated-document model that will replace the item-5 image surface.
- **IBL** (image-based lighting: irradiance map, prefiltered specular, BRDF LUT) — the natural
  step right after PBR/shadows settle.
- **Baked lighting / lightmaps** — the big static-interior payoff; its own baker subsystem.
- **Object behavior / scripting layer** — the live-object model; the language objects are
  authored in.
- **CAD constraint tier + procedural "dressing" layer** — the authoring tools.
- **Own platform layer** (replacing GLFW).
- **Second rendering backend (Metal/WebGPU)** — the seam-proving capstone, once the RHI surface
  from items 1–9 has stopped moving.

The seam (§1.2), the persistence/identity model (item 2), the vertex layout (§1.6), the
color-space distinction (items 5/8), and the dependency policy (§1.3) are the cross-cutting
concerns of this phase. They are pinned here precisely because they are painful to retrofit —
which is the same design discipline (surface cross-cutting concerns before implementation) the
larger project applies to itself.

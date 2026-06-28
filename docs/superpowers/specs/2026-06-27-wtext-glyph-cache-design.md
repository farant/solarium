# wtext Glyph-Geometry Cache — Design Spec

**Date:** 2026-06-27
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Cut the per-frame CPU cost of world-space text (`wtext`). Measurement shows the
world-text section reaching **8–9 ms/frame on Metal** (≈ half a 60 fps budget),
and that cost tracks **glyphs shaped per frame** (`gly`) almost linearly
(~5000 → ~15000 glyphs when a window widens the view ⇒ ~8–9 ms). The work is
**shaping-bound**: `text_shape` (and `text_wrap`'s per-word re-shaping) run every
frame for every visible block. A content-addressed cache of the built glyph
geometry makes static text a near-free bind+draw, dropping `gly`→~0 in steady
state.

## Background — measured (this session)

Instrumentation (branch `wtext-measure`, commit `b19979b`) added a stats-card
line `text <blk> <gly> <up> <ms>` and per-frame counters:
- `gly` = glyphs positioned by `text_shape` during the world-text section.
- `blk` = `wtext` blocks that drew. `up` = vertex-buffer re-uploads. `ms` =
  wall-time of the `if (state->ui_font)` section (main.c ~15522–15987).

Findings:
- **Shaping-bound.** `ms` ∝ `gly`; `blk`/`up` are secondary. A wrapped block
  shapes its text **~twice**: `text_wrap` shapes each word via
  `text_measure`→`text_shape` to find breaks, then `wt_emit` shapes the whole
  wrapped string again. Plus `text_measure` for every label's fit-shrink.
- **The label loops are not frustum-culled.** The card/note loop already culls
  (`if (vis && !vis[o->handle]) continue;`, main.c:15678, comment: "SHAPING text
  per frame is the real cost"), but the **doorway / bookshelf / folder /
  board-title** label loops shape every *active* matching object every frame,
  on-screen or not. (Doorway labels come from `route_all`'s routes, not a
  vis-indexed object.)
- **Metal allocation churn (secondary).** `rhi_update_buffer` on Metal does
  `newBufferWithBytes` — a fresh `MTLBuffer` **per block per frame** (rhi_metal.m
  ~389). GL orphans via `glBufferData(...STREAM_DRAW)`. The cache erases this for
  free (a hit re-uploads nothing).

Module mechanics (verified):
- `wtext_block` (flat) wraps then calls `wt_emit` (wtext.c:137); `wt_emit`
  shapes into a stack `ShapedGlyph[WT_MAX_GLYPHS]`, builds 6 verts × 5 floats
  per glyph into the **single shared** `g_wt_verts` scratch, uploads into the
  **single shared** `g_wt.vbuffer`, then sets pipeline/buffer/atlas + `uMVP`
  (`viewproj*model`) + `uColor` + draws. The shared buffer is *why* every block
  must rebuild+reupload before its draw.
- `wtext_block_bent` (the turning reader leaf, item 9) changes every frame.

## Key insight — what the geometry depends on

A block's vertex array depends only on **(font, text, px_to_m, wrap_w_m, x,
top_y)**. It does **not** depend on the camera (`viewproj`) or the model
transform or the color — those become the `uMVP`/`uColor` uniforms, set per draw.
The geometry is therefore identical frame-to-frame unless the text or its size
changes, and is **highly cacheable**. (`x`/`top_y` are deterministic per call
site — e.g. `-cw/2 + 2*margin`, or a centered `-nw*lpx*0.5` that is a pure
function of the text — so including them in the key keeps frame-to-frame hits.)

## Decisions (from brainstorming)

- **A content-addressed glyph-geometry cache inside `wtext`.** Key = (font,
  text, px_to_m, wrap_w_m, x, top_y). Value = a **persistent** RHI vertex buffer
  + vertex count, built once. Hit ⇒ bind + `uMVP`/`uColor` + draw (no wrap, no
  shape, no build, no upload). Miss ⇒ full path once, store.
- **Invalidation is automatic / content-addressed.** Text or size change ⇒ new
  key ⇒ miss ⇒ rebuild; the stale entry ages out by LRU. No dirty-tracking, **no
  call-site changes** (the public `wtext_block` signature is unchanged).
- **Bounded + LRU.** A fixed open-addressed table (CAP 512), linear-probe window,
  LRU eviction within the window. Each entry owns one small vertex buffer
  (≤ 512, comfortably under the Metal 4096-buffer cap; scene meshes use ~1.5k).
- **Build geometry as-is (bake x/top_y into the verts), not at origin.** Lowest
  risk — `wt_emit`'s proven vertex math is untouched; x/top_y go in the key.
  (A future refinement could build at origin and fold x/top_y into the model
  translate for a smaller key + more cross-object sharing; not needed now.)
- **Flat path only.** `wtext_block_bent` stays immediate (it animates).
- **No label-loop frustum-culling in v1.** Once static labels are cache hits
  they're nearly free; culling would save only draw calls, not shaping. Revisit
  iff draw-call count becomes the new bottleneck (the HUD line will show it).
- **Pure cache logic in its own headless-tested module** (`wtcache.c`), per the
  project's split-the-risky-logic law (caret.c/furniture.c/scene_test/inventory
  precedent). It performs NO `rhi_*` calls — it stores the `RhiBuffer` handle as
  an opaque payload and reports eviction victims; `wtext.c` owns buffer
  create/destroy. So hashing, collision probing, and LRU eviction are unit-tested
  without a GL context.
- **Keep the instrumentation** as the acceptance instrument (`gly`→~0 steady
  state, `ms` cliff). Extend it with a per-frame **miss** count to watch for
  thrash.

## Non-Goals

- No change to `wtext_block_bent` / the reader leaf turn.
- No batching/instancing of draw calls (a possible later pass if draws dominate
  after shaping is gone).
- No label-loop culling (deferred; the cache subsumes most of its benefit).
- No `text_shape`/`text_wrap`/font/atlas changes; the §1.6 text seam is untouched.
- No shader / MSL twin (the pipeline is unchanged; only buffer ownership moves).
- No scene.stml or persistence impact (pure render-side cache).

## Architecture

### 1. `wtcache.h` / `wtcache.c` — the pure cache table (NO rhi_* calls)

```
#define WTCACHE_CAP   512        /* power of two; open-addressed */
#define WTCACHE_PROBE 16         /* linear-probe / eviction window */
#define WTCACHE_TEXT  WT_WRAP_CAP /* stored pre-wrap key text (2048) */

typedef struct {
    sol_bool   used;
    sol_u32    hash;
    const void *font;            /* pointer identity (opaque) */
    float      px2m, wrap, x, top_y;
    char       text[WTCACHE_TEXT];
    RhiBuffer  buffer;           /* opaque payload — wtcache never touches it */
    int        vc;               /* vertex count */
    int        last_frame;       /* LRU recency */
} WtCacheEntry;

typedef struct { WtCacheEntry e[WTCACHE_CAP]; } WtCache;

void wtcache_init(WtCache *c);                       /* zero it */
/* Look up; on hit update last_frame and return the slot (>=0), else -1. */
int  wtcache_find(WtCache *c, const void *font, const char *text,
                  float px2m, float wrap, float x, float top_y, int frame);
/* Reserve a slot for a NEW entry (miss path): a free slot in the probe window,
   else the LRU victim within it. Writes the key + last_frame, marks used,
   sets vc=0. If a live entry was displaced, returns its buffer in *evicted
   (id 0 if none) so the caller can destroy it. Returns the slot. */
int  wtcache_claim(WtCache *c, const void *font, const char *text,
                   float px2m, float wrap, float x, float top_y, int frame,
                   RhiBuffer *evicted);
/* Store the built payload into a claimed slot. */
void wtcache_set(WtCache *c, int slot, RhiBuffer buffer, int vc);
```

- **Hash:** FNV-1a 32-bit over (font pointer bytes, the four float bytes, the
  text bytes). Exact compare on all fields on a hash match (collision-safe).
- **Probing:** start at `hash & (CAP-1)`, linear, up to `WTCACHE_PROBE` slots.
  `find` stops at the first `!used` slot (miss). `claim` replaces (never tombstones
  — replacement keeps probe chains intact): use a free slot in the window, else
  evict the entry with the smallest `last_frame` in the window.
- **Why testable:** all of the above is plain data manipulation. `wtcache_test.c`
  (a `wtcachetest` build target, like `carettest`) exercises: hit after insert;
  key-field discrimination (text/px2m/x change ⇒ miss); collision probing;
  LRU picks the oldest victim and returns its (fake) buffer; recency updates on
  hit. `RhiBuffer` is just `{sol_u32 id}` so fakes need no GL.

### 2. `wtext.c` — integrate the cache into the flat path

- Add `WtCache g_wt_cache;` + `int g_wt_frame;` to the module; `wtext_init`
  calls `wtcache_init`; `wtext_shutdown` destroys every used entry's buffer then
  the rest.
- Add `void wtext_frame_begin(void);` → `g_wt_frame++;` (called once per frame
  from `render`, so `last_frame` advances). The existing `wtext_stats_reset`
  can fold this in, or it's a sibling call.
- Factor `wt_emit` into two halves:
  - `wt_build(font, src, x, top_y, px2m, &vc)` → fills `g_wt_verts`, returns vc
    (the current per-glyph math, bend==NULL path only).
  - `wt_draw(buffer, vc, viewproj, model, font, r, g, b)` → set pipeline, bind
    buffer, bind atlas, `uMVP = viewproj*model`, `uColor`, `rhi_draw`.
- `wtext_block` (flat) becomes:
  ```
  wrap if needed -> src
  slot = wtcache_find(cache, f, utf8 /*pre-wrap key*/, px2m, wrap_w_m, x, top_y, frame)
  if slot >= 0:                                  /* HIT */
      wt_draw(entry.buffer, entry.vc, viewproj, model, f, r,g,b)
  else:                                          /* MISS */
      vc = wt_build(f, src, x, top_y, px2m)
      if vc == 0: return                         /* whitespace-only: nothing to cache */
      slot = wtcache_claim(cache, f, utf8, px2m, wrap_w_m, x, top_y, frame, &evicted)
      if evicted.id: rhi_destroy_buffer(evicted)
      buffer = rhi_create_buffer(VERTEX, g_wt_verts, vc*stride)
      wtcache_set(cache, slot, buffer, vc)
      wt_draw(buffer, vc, viewproj, model, f, r,g,b)
  ```
  Key on the **pre-wrap** `utf8` (wrapping is deterministic from utf8+params, so
  storing/comparing the pre-wrap text is sufficient and avoids re-wrapping on a
  hit). The flat path stops using the shared `g_wt.vbuffer`; it is **kept only as
  the bent path's scratch buffer**. The shared `g_wt_verts` scratch stays (the
  build target before each upload, flat-miss and bent).
- `wtext_block_bent` keeps calling the immediate `wt_emit`/`wt_build`+`wt_draw`
  on the shared scratch with a throwaway buffer — unchanged behavior. (It can
  reuse a single module-scratch buffer for the bent path so we don't reintroduce
  per-frame churn there; bent text is one block, the open book.)
- Counters: `blk`++ per drawn block (hit or miss), `up`++ only on miss (a real
  upload), add `miss`++ on miss. Hits do zero shaping ⇒ `gly` falls to the
  newly-appeared/changed blocks only.

### 3. `wtext.h` — expose the frame tick + extend stats

```
void wtext_frame_begin(void);                 /* advance LRU clock; once/frame */
void wtext_stats_get(int *blocks, int *uploads, int *misses);  /* +misses */
```

### 4. `main.c` — wire it

- Call `wtext_frame_begin()` once per frame (beside the existing
  `wtext_stats_reset()` at the top of the world-text section).
- HUD line gains the miss count: `text %dblk %ldgly %dup %dmiss %4.2fms`
  (the panel already grew to 212px for the line).

### 5. `build.sh` — add the `wtcachetest` target

Mirror the `carettest` recipe: compile `wtcache.c` + `wtcache_test.c` with
ASan/UBSan, no GL link. Add to the standard gauntlet docs.

## Data Flow

```
frame: wtext_frame_begin() (clock++), wtext_stats_reset(), text_shape_stats_reset()
each visible block -> wtext_block():
    find(key) HIT  -> wt_draw(cached buffer)                 [no shape/upload]
    find(key) MISS -> wt_build (shape once) -> claim (LRU)   [evict old buffer]
                      -> create buffer -> set -> wt_draw
steady state: nearly all HITs -> gly ~ 0, ms collapses
text edit / resize: that block's key changes -> 1 MISS -> rebuild, old ages out
```

## File Touch List

- **NEW `wtcache.h` / `wtcache.c`** — pure table (hash, find/claim/set, LRU); no rhi_* calls.
- **NEW `wtcache_test.c`** — headless unit test; **`build.sh` `wtcachetest` target**.
- **`wtext.c`** — cache integration (split `wt_emit` → `wt_build`+`wt_draw`;
  flat path find/hit/miss; per-entry buffers; bent path keeps a scratch buffer;
  shutdown frees entries; `wtext_frame_begin`; miss counter).
- **`wtext.h`** — `wtext_frame_begin`; `wtext_stats_get` gains `misses`.
- **`main.c`** — call `wtext_frame_begin`; HUD line adds `miss`.
- **`build.sh`** — `wtcachetest` target.
- No shader, no scene_io, no font/text changes.

## Testing

- **Headless unit:** `wtcachetest` — hit/miss discrimination, collision probe,
  LRU victim selection (returns the right displaced buffer), recency update.
- **Build gauntlet:** `c89check` / GL / Metal / `carettest` / **`wtcachetest`**.
- **Live-verify (Fran, both backends), via the HUD `text` line:**
  - Steady state in a text-dense view: `gly` collapses to ~0, `ms` drops far
    below the 8–9 ms peak; `miss` is ~0 when standing still.
  - Turn toward a window into a dense area: a **one-frame** `gly`/`miss` spike as
    new entries build, then settles (the known reveal hitch).
  - Type in a note: that note misses while the text changes (others stay hits);
    text renders identically to before (no glyph/position regression).
  - Resize a note / fit-shrunk label: rebuilds once, looks identical.
  - Walk a long sweep through many rooms: `miss` stays low (no LRU thrash); if it
    stays pinned high, CAP is too small — bump `WTCACHE_CAP`.
  - Visual parity: card front+back labels, doorway both sides, folder/shelf/board
    titles, note bodies all identical to pre-cache.

## Risks

- **Visual parity.** The cache must reproduce identical geometry. Mitigation:
  `wt_build` is the *unchanged* vertex math; the cache only persists its output.
- **LRU thrash if CAP too small** for a very dense sweep ⇒ misses pinned high,
  no win. Mitigation: the `miss` HUD counter makes it visible; CAP is a one-line
  bump; entries are cheap (≤512 small buffers).
- **Eviction correctness** (evict an in-use entry → rebuild thrash, or a probe
  chain break). Mitigation: replacement-not-tombstone keeps chains intact;
  `wtcache_test` pins the LRU + probe behavior headless.
- **Buffer-handle leak on evict.** `wtcache_claim` returns the displaced buffer
  and the caller MUST `rhi_destroy_buffer` it; shutdown frees all used entries.
- **Reveal hitch** — first frame of a newly-revealed dense area still shapes all
  its new blocks (one-frame spike). Accepted; amortizable later (budget N builds
  per frame) if it bothers in practice.
- **The font pointer as identity** assumes a stable `Font *` per atlas (true: the
  app holds `ui_font`/`reader_font` for the process lifetime). A font reload
  would need a cache clear — out of scope (no font hot-reload exists).

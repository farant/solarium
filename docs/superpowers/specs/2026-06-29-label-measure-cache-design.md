# Label Text: Measure Cache + Frustum-Culled Loops — Design Spec

**Date:** 2026-06-29
**Status:** Approved (brainstorm), pending implementation plan
**Author:** Fran + Claude

## Goal

Kill the residual per-frame world-text CPU left after the glyph cache (merged
`4f353e9`). That cache made the *draw* of static text free, but the world-text
section still shapes ~5000 glyphs/frame (down from 15000; `ms` ~3.5–4 on Metal,
down from 8–9). Two untouched costs remain, both on labels:

1. **The `text_measure` pre-pass.** File/folder tablet names, door labels, and
   bookshelf/folder/board titles each call `text_measure(font, text)` EVERY
   frame — to center (`x0 = -width·scale/2`) and fit-shrink (`px2m =
   usable/width`). `text_measure` shapes the whole string via `text_shape`. The
   glyph cache cannot help: this measure runs *before* the draw and its result
   feeds the glyph-cache key. This is almost all of the residual `gly`.
2. **Un-culled label loops.** The card/note loop is frustum-culled
   (`if (vis && !vis[o->handle]) continue;`), but the **doorway / bookshelf /
   folder / board-title** loops shape+draw every *active* label in the whole
   world, on-screen or not.

Fix: cache `text_measure` (so static labels stop re-shaping for width → `gly`→~0)
AND frustum-cull the four un-culled loops (so off-screen labels are skipped
before they measure or draw).

## Background — verified (current code, post-glyph-cache)

- `text_measure(const Font *f, const char *utf8, float scale, float *out_w,
  float *out_h)` (text.c:97) shapes `utf8`, finds the max line width + total
  height at base size, multiplies by `scale`. Width/height at `scale==1.0` are a
  **pure function of (font, text)**.
- Per-frame callers inside the world-text section (`if (state->ui_font)`,
  main.c ~15522–15987), all passing `scale==1.0`: card spine name (15719), card
  front/back name (15744) — **these are in the already-`vis`-culled card loop**;
  doorway label (15869), bookshelf label (15899), folder label (15924), board
  title (15951) — **these four loops are NOT culled**; plus the reader-image
  filename (15603, only while reading an image). (Out-of-section callers
  14851/14868 = editor/route prep, 16626 = HUD — NOT the residual, left alone.)
- The four un-culled loops scan `state->scene.count` (or the routes, for
  doorways), filter by `mesh_ref`+`scene_object_active` (or `route.valid`), and
  draw a label per match regardless of view.
- Frustum machinery EXISTS and is used elsewhere this frame: `frustum_from_vp(mat4)
  → Frustum` (six Gribb-Hartmann planes, sol_math.h:74), `frustum_intersects_aabb(
  const Frustum*, Aabb) → sol_bool` (positive-vertex test, conservative,
  sol_math.h:75). `Aabb = { vec3 min, max; }` (sol_types.h:22). `mat4_mul_point(
  mat4, vec3)` (sol_math.h:67). The section already computes `mat4 vp =
  mat4_mul(proj, view);`.
- The just-merged glyph cache established the pattern this mirrors: a pure
  content-addressed table in its own module (`wtcache.c`/`.h`, headless-tested),
  used by thin glue, with a once-per-frame LRU clock (`wtext_frame_begin`). The
  HUD `text … gly … ms` line is the acceptance instrument (`gly`→0 = the cache
  is working). text.c is engine-only (it includes `ui.h`); no headless test
  links it, so adding a tmcache dependency there only affects the main build.

## Decisions (from brainstorming)

- **Lever A — a `text_measure` cache** keyed by **(font, text)** → **(w, h)** at
  scale 1.0. Same content-addressed idea as `wtcache`, but **simpler — the
  payload is two floats, so there is no GPU-buffer lifecycle** (eviction is a
  plain overwrite, no resource to free). A pure module `tmcache.c`/`.h` (the
  table) + a headless `tmcachetest`, mirroring `wtcache`. Thin glue
  `text_measure_cached` in text.c wraps `text_measure`: hit returns the stored
  width (zero shaping), miss shapes once and stores. Entries never go stale
  (font immutable, text in the key), so no per-frame reset — only LRU when full,
  driven by a once-per-frame clock `text_measure_frame_begin()`.
- **Lever B — frustum-cull the four un-culled loops** (doorway / bookshelf /
  folder / board). Cull on the **label's own world anchor** (NOT the object's
  `vis`), so a label that floats *above* an off-screen object (board titles sit
  ~2 lines up) isn't wrongly hidden, and the route-derived door labels (no scene
  object) are handled uniformly. Build a small margin `Aabb` around the anchor
  and `frustum_intersects_aabb`; skip if outside. **Cull BEFORE measuring** so
  off-screen labels cost nothing (the anchor's vertical offset uses a constant
  `lpx`, not the measured width, so it's computable pre-measure). The card loop
  keeps its existing `vis` cull (its labels are ON the card — `vis` is accurate
  there).
- **Parity:** all label callers pass `scale==1.0`, so a cached hit returns
  `w0·1.0 == w0` — byte-identical to the plain measure; no layout shift.
- **Acceptance = the existing HUD `gly`/`ms`** (no new counter needed; a measure
  miss shapes → shows up in `gly`, so `gly`→~0 steady-state proves both levers).

## Non-Goals

- No change to `text_measure` itself (the seam stays); `text_measure_cached` is a
  sibling. The §1.6 `text_shape` seam is untouched.
- No caching of `text_wrap`'s internal per-word measures (those run only on a
  glyph-cache miss now — not the residual; and word-level entries would pollute
  the cache).
- No change to the glyph cache, shaders, scene_io, or persistence. No MSL twin
  (no shader touched).
- The out-of-section `text_measure` callers (editor/route prep, HUD) are left on
  the plain path — they aren't the measured residual.
- No new HUD counter (the existing `gly` is the instrument).

## Architecture

### 1. `tmcache.h` / `tmcache.c` — the pure measure cache (NO rhi_*, NO GL)

```
#define TMCACHE_CAP   1024   /* entries; power of two (open-addressed) */
#define TMCACHE_PROBE 16     /* linear-probe + eviction window */
#define TMCACHE_TEXT  256    /* stored key text; labels/paths are short */

typedef struct {
    sol_bool    used;
    sol_u32     hash;
    const void *font;
    int         len;
    char        text[TMCACHE_TEXT];
    float       w, h;            /* measured at scale 1.0 */
    int         last_frame;      /* LRU recency */
} TmCacheEntry;

typedef struct { TmCacheEntry e[TMCACHE_CAP]; } TmCache;

void    tmcache_init(TmCache *c);                       /* zero it */
sol_u32 tmcache_hash(const void *font, const char *text, int len);
/* Hit: set last_frame=frame, return slot (>=0). Miss: -1. */
int     tmcache_find(TmCache *c, const void *font, const char *text, int len, int frame);
/* Miss path: a free slot in the probe window, else the LRU victim; writes the
   key + frame, marks used, w=h=0. Returns the slot. (No evicted payload — the
   value is plain floats, nothing to free.) */
int     tmcache_claim(TmCache *c, const void *font, const char *text, int len, int frame);
void    tmcache_set(TmCache *c, int slot, float w, float h);
```

- Identical hashing/probe/LRU mechanics to `wtcache` (FNV-1a over font ptr +
  text bytes + len; open-addressed linear probe, window `TMCACHE_PROBE`,
  stop-at-first-unused find; claim takes the first free slot else evicts the
  smallest `last_frame`). The no-delete invariant (slots only go unused→used or
  used→used) keeps find correct. **Simpler than wtcache: `tmcache_claim` returns
  no evicted buffer** (the displaced entry's floats just vanish). `tmcache_claim`
  asserts `len >= 0 && len < TMCACHE_TEXT` (caller's contract, leaves room for
  the NUL).
- Pure (no `rhi_*`/GL) → `tmcache_test.c` (`tmcachetest` target) exercises
  headless: empty→miss; claim+set→find returns stored (w,h); field
  discrimination (font / text / len each → miss); engineered-collision LRU
  eviction (oldest evicted, newer survive); find() recency-bump protects an
  entry from eviction. (Same test shape as `wtcache_test.c`.)

### 2. `text_measure_cached` + the clock (text.c / text.h)

text.h adds:
```
void text_measure_cached(const Font *f, const char *utf8, float scale,
                         float *out_w, float *out_h);
void text_measure_frame_begin(void);   /* advance the measure-cache LRU clock; once/frame */
```
text.c adds a file-static `TmCache g_tm_cache;` (zero-initialised in BSS = a
valid empty cache; `tmcache_init` exists for the test's local caches) + `int
g_tm_frame;`, and:
```
text_measure_frame_begin(): g_tm_frame++;

text_measure_cached(f, utf8, scale, out_w, out_h):
    if !f or !utf8: zero outputs, return
    len = strlen(utf8)
    if len >= TMCACHE_TEXT: text_measure(f, utf8, scale, out_w, out_h); return   /* uncacheable */
    slot = tmcache_find(&g_tm_cache, f, utf8, len, g_tm_frame)
    if slot >= 0: w0,h0 = entry.w, entry.h                  /* HIT — no shaping */
    else: text_measure(f, utf8, 1.0, &w0, &h0);             /* MISS — shape once */
          slot = tmcache_claim(&g_tm_cache, f, utf8, len, g_tm_frame)
          tmcache_set(&g_tm_cache, slot, w0, h0)
    if out_w: *out_w = w0 * scale
    if out_h: *out_h = h0 * scale
```
(text.c `#include "tmcache.h"`. The cache is content-addressed and permanently
valid, so there is no reset — the clock only orders LRU recency.)

### 3. Frustum-cull the four un-culled loops (main.c)

At the top of the world-text section (after `mat4 vp = mat4_mul(proj, view);`):
```
Frustum lf = frustum_from_vp(vp);   /* label-cull frustum (this section only) */
```
A small helper near the section (or inline per loop):
```
/* a label whose world anchor is outside the view (plus a margin) is skipped
   before it measures or draws. margin ~3m: generous so edge labels don't pop. */
static int label_in_view(const Frustum *f, vec3 anchor) {
    Aabb b;
    float m = 3.0f;
    b.min = vec3_make(anchor.x - m, anchor.y - m, anchor.z - m);
    b.max = vec3_make(anchor.x + m, anchor.y + m, anchor.z + m);
    return frustum_intersects_aabb(f, b);
}
```
In each of the four loops, compute the label's world anchor and `continue` if
not in view — placed BEFORE the `text_measure` call:
- **Doorway** (15843…): anchor ≈ `vec3_make(door.x + nx*0.30, door.y +
  ROUTE_DOOR_H + 0.2, door.z + nz*0.30)`.
- **Bookshelf** (15880…): `mat4_mul_point(scene_world_matrix(s, o),
  vec3_make(0, h + 0.02 + 2*lpx*lh, 0.16))` (lpx = 0.18/lh, constant).
- **Folder** (15905…): `mat4_mul_point(scene_world_matrix(s, o),
  vec3_make(0, fh + 2*lpx*lh, 0.06))` (lpx = 0.135/lh, constant).
- **Board title** (15930…): `mat4_mul_point(scene_world_matrix(s, o),
  vec3_make(0, bh + 0.04 + 2*lpx*lh, 0.04))` (lpx = 0.12/lh, constant).
The `lpx`/`h`/`fh`/`bh` needed for the anchor are already computed in each loop
just above (or are constants) — reorder so the anchor + cull come before
`text_measure`.

### 4. Use the cached measure + wire the clock (main.c)

- Replace `text_measure(...)` with `text_measure_cached(...)` at the in-section
  per-frame label callers: 15603 (reader image filename), 15719 + 15744 (card
  names), 15869 (doorway), 15899 (bookshelf), 15924 (folder), 15951 (board).
- Call `text_measure_frame_begin();` once per frame, beside the existing
  `wtext_frame_begin();` (just before `wtext_stats_reset();`).

### 5. `build.sh`

- Add `tmcache.c` right after `wtcache.c` in the four engine source lists
  (the substring `wtext.c wtcache.c scene.c` → `wtext.c wtcache.c tmcache.c
  scene.c`).
- Add a `tmcachetest` target mirroring `wtcachetest` (clang c11 + ASan/UBSan,
  `tmcache.c tmcache_test.c -o tmcache_test`, no GL).

## Data Flow

```
frame: text_measure_frame_begin() (LRU clock++)  [beside wtext_frame_begin]
each label loop:
    compute world anchor -> label_in_view(lf, anchor)? no -> continue (skip: no measure, no draw)
    text_measure_cached(font, text) -> HIT (no shape) | MISS (shape once, store)
    -> center/fit -> wtext_block (glyph-cache HIT: bind+draw)
steady state: labels' measures HIT + draws HIT -> gly ~ 0, ms collapses further
off-screen labels: culled before measuring -> not counted at all
```

## File Touch List

- **NEW `tmcache.h` / `tmcache.c`** — pure measure-cache table (hash/probe/LRU,
  no rhi_*).
- **NEW `tmcache_test.c`** — headless test; **`build.sh` `tmcachetest` target**.
- **`text.c` / `text.h`** — `text_measure_cached` + `text_measure_frame_begin` +
  the `g_tm_cache` static (include `tmcache.h`).
- **`main.c`** — the `label_in_view` cull on the 4 un-culled loops; swap the
  in-section `text_measure` callers to `text_measure_cached`; call
  `text_measure_frame_begin()` once per frame.
- **`build.sh`** — `tmcache.c` in the 4 source lists + `tmcachetest`.
- No shader, no scene_io, no glyph-cache change.

## Testing

- **Headless:** `tmcachetest` — hit/miss, field discrimination, LRU eviction +
  recency-protection (same shape as `wtcache_test`).
- **Gauntlet:** `c89check` / GL / Metal / `carettest` / `wtcachetest` /
  **`tmcachetest`**.
- **Live-verify (Fran, both backends), via the HUD `text` line:**
  - In the dense-window view that was ~5000 `gly` / ~3.5–4 ms: `gly` should now
    collapse toward ~0 and `ms` drop further (labels stop re-measuring).
  - Turn AWAY from a label-dense cluster: `gly`/`ms` stay low (off-screen labels
    culled) — and the cluster's labels are gone from view, with no flicker at
    the screen edge as you pan (margin tuning: if labels pop in/out at the edge,
    raise the 3 m margin; report it).
  - Visual parity: door labels (both sides), folder/shelf/board titles, and
    file/folder tablet names render in exactly the same positions/sizes as
    before (centering + fit-shrink unchanged).
  - A long sweep through many rooms keeps `gly` low (no measure-cache thrash; if
    it stays high, bump `TMCACHE_CAP`).

## Risks

- **Cull margin too small → edge popping.** A label whose anchor is just
  off-screen but whose text extends on-screen could vanish. Mitigation: a
  generous 3 m margin (labels are centered on the anchor; 3 m ≈ a 6 m-wide label
  budget) + tune in live-verify. Over-inclusion is cheap (the surviving measure
  is a cache hit).
- **Anchor approximation for door labels.** The doorway anchor uses the door
  point + up offset, not the exact rotated label origin — fine for a margin
  cull (it only decides skip/keep, not placement).
- **Parity.** Cached measure returns `w0·scale`; all label callers use
  `scale==1.0` so it's byte-identical to the plain measure. No layout shift.
- **Cache thrash if `TMCACHE_CAP` too small** for a huge label set → `gly` stays
  up. Visible on the HUD; one-line CAP bump; entries are cheap (no GPU resource).
- **text.c gains a tmcache dependency** — text.c is engine-only (includes ui.h;
  no headless test links it), so `tmcache.c` only needs adding to the main build
  lists (done in §5). The `tmcachetest` links `tmcache.c` alone (pure).
- **`label_in_view` is conservative** (positive-vertex AABB test) — it can keep
  a few just-outside labels, never wrongly drops an inside one. Culling stays an
  optimization, never a correctness change.

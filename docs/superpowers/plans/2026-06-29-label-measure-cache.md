# Label Measure Cache + Frustum-Culled Label Loops — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the residual per-frame world-text CPU (the `text_measure` pre-pass for label centering/fit-shrink) by caching `text_measure`, and skip off-screen labels by frustum-culling the four un-culled label loops.

**Architecture:** A pure `tmcache.c`/`.h` table keyed `(font, text)` → `(w, h)` — like `wtcache` but simpler (two-float payload, no GPU lifecycle), headless-tested. Glue `text_measure_cached` in `text.c` wraps `text_measure` (hit = zero shaping). `main.c` culls the door/shelf/folder/board loops on each label's world anchor (before measuring) and swaps the in-section `text_measure` callers to the cached variant. No shader, no scene_io change.

**Tech Stack:** C89 engine sources (`-std=c89 -pedantic-errors -Werror -Wextra`); `*_test.c` build c11+ASan/UBSan. Spec: `docs/superpowers/specs/2026-06-29-label-measure-cache-design.md`. Off `main` (tip `4f353e9`).

**House rules:** engine `.c` strict C89 (declarations at block top, no `//`); `*_test.c` may be c11. NEVER `git add NOTES.stml`/`paper-picture.png`. Commit bodies end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Work on a feature branch off `main`.

**Verified facts:** `text_measure(const Font*, const char*, float scale, float *out_w, float *out_h)` (text.c:97) — width/height at scale 1.0 are a pure function of (font,text). `Aabb = { vec3 min, max; }` (sol_types.h:22). `frustum_from_vp(mat4)→Frustum` (sol_math.h:74), `frustum_intersects_aabb(const Frustum*, Aabb)→sol_bool` (sol_math.h:75), `mat4_mul_point(mat4, vec3)→vec3` (sol_math.h:67) — all already used in main.c (frustum cull at 14529). The world-text section opens `if (state->ui_font) {` (main.c:15530) with locals `uf`/`lh`/`vp` (`mat4 vp = mat4_mul(proj, view);`); the section reset is `text_shape_stats_reset(); wtext_frame_begin(); wtext_stats_reset();` at 15526-15528. The four un-culled loops: doorway 15843, bookshelf 15886, folder 15911, board 15936 (each computes `lpx`/`h`/`fh`/`bh` BEFORE its `text_measure`). Card loop (vis-culled) measures at 15719 + 15744 (both `&name_w`); reader-image measure at 15603 (`&nw`, preceded by `cpx = (bp[1] * 0.020f) / lh;`). text.h: `text_measure` decl at 46-47. text.c includes at 5-8; `text_measure` body ends before `text_wrap` (141). build.sh: `wtext.c wtcache.c scene.c` appears 4× (engine lists); `wtcachetest` block at build.sh:94. `ROUTE_DOOR_H` is in scope in the doorway loop (used at 15876).

---

## Task 1: The pure `tmcache` table + headless test + build wiring

**Files:** Create `tmcache.h`, `tmcache.c`, `tmcache_test.c`; modify `build.sh`.

- [ ] **Step 1: Write `tmcache.h`**
```c
/* tmcache.h — a content-addressed cache of text_measure results (P9 perf #2b).
   PURE: no rhi_*, no GL. Keyed by (font, text) -> (w, h) at scale 1.0. Simpler
   than wtcache — the payload is two floats, so eviction is a plain overwrite
   (no GPU resource to free). Hashing / probing / LRU are unit-tested headless. */
#ifndef TMCACHE_H
#define TMCACHE_H

#include "sol_base.h"   /* sol_u32 / sol_bool */

#define TMCACHE_CAP   1024   /* entries; MUST be a power of two (open-addressed) */
#define TMCACHE_PROBE 16     /* linear-probe + eviction window */
#define TMCACHE_TEXT  256    /* stored key text; the caller's len MUST be < this */

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

void    tmcache_init(TmCache *c);
sol_u32 tmcache_hash(const void *font, const char *text, int len);
/* Hit: set last_frame=frame, return slot (>=0). Miss: -1. */
int     tmcache_find(TmCache *c, const void *font, const char *text, int len, int frame);
/* Miss path: a free slot in the probe window, else the LRU victim. Writes the
   key + frame, marks used, w=h=0. Returns the slot. (No evicted payload.) */
int     tmcache_claim(TmCache *c, const void *font, const char *text, int len, int frame);
void    tmcache_set(TmCache *c, int slot, float w, float h);

#endif /* TMCACHE_H */
```
(Include `sol_base.h` directly — tmcache needs no `RhiBuffer`, so it does NOT include rhi.h. Confirm `sol_base.h` defines `sol_u32`/`sol_bool`; if they live in `sol_types.h` instead, include that one. rhi.h includes `sol_base.h` for these, so `sol_base.h` is correct.)

- [ ] **Step 2: Write the failing test `tmcache_test.c`** (c11; mirrors `wtcache_test.c`):
```c
/* tmcache_test.c — headless unit test for the pure text-measure cache.
   Build: ./build.sh tmcachetest && ./tmcache_test  (ASan + UBSan). */
#include "tmcache.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main(void) {
    static TmCache c;
    const void *F = (const void *)0x1000;
    int slot, slot2;

    tmcache_init(&c);

    /* 1) empty -> miss */
    CHECK(tmcache_find(&c, F, "hello", 5, 1) < 0);

    /* 2) claim + set -> find hits with the stored (w,h) */
    slot = tmcache_claim(&c, F, "hello", 5, 1);
    CHECK(slot >= 0);
    tmcache_set(&c, slot, 3.5f, 1.25f);
    slot2 = tmcache_find(&c, F, "hello", 5, 2);
    CHECK(slot2 == slot);
    CHECK(c.e[slot2].w == 3.5f);
    CHECK(c.e[slot2].h == 1.25f);

    /* 3) every key field discriminates (each a distinct miss) */
    CHECK(tmcache_find(&c, F, "hellp", 5, 3) < 0);                       /* text */
    CHECK(tmcache_find(&c, F, "hell",  4, 3) < 0);                       /* len  */
    CHECK(tmcache_find(&c, (const void *)0x2000, "hello", 5, 3) < 0);    /* font */

    /* 4) LRU eviction within a full probe window evicts the oldest. Engineer
       TMCACHE_PROBE+1 texts that collide to one start bucket. */
    {
        char keys[64][16];
        int  found = 0, n, want = TMCACHE_PROBE + 1;
        sol_u32 bucket0 = 0; sol_bool have_bucket = 0;
        static TmCache cc;
        tmcache_init(&cc);
        for (n = 0; found < want && n < 100000; n++) {
            char t[16]; sol_u32 h, bk;
            snprintf(t, sizeof t, "k%d", n);
            h  = tmcache_hash(F, t, (int)strlen(t));
            bk = h & (TMCACHE_CAP - 1);
            if (!have_bucket) { bucket0 = bk; have_bucket = 1; }
            if (bk == bucket0) { strcpy(keys[found], t); found++; }
        }
        CHECK(found == want);
        for (n = 0; n < TMCACHE_PROBE; n++) {            /* fill window, frames 10..25 */
            int s = tmcache_claim(&cc, F, keys[n], (int)strlen(keys[n]), 10 + n);
            tmcache_set(&cc, s, (float)n, 0.0f);
        }
        {   /* the (PROBE+1)th claim evicts the LRU (frame 10 = keys[0]) */
            int s = tmcache_claim(&cc, F, keys[TMCACHE_PROBE],
                                  (int)strlen(keys[TMCACHE_PROBE]), 99);
            CHECK(s >= 0);
            CHECK(tmcache_find(&cc, F, keys[0], (int)strlen(keys[0]), 200) < 0);  /* evicted */
            CHECK(tmcache_find(&cc, F, keys[1], (int)strlen(keys[1]), 200) >= 0); /* survives */
        }
    }

    /* 5) find() bumps recency: a touched entry survives while a colder neighbour
       is evicted instead. */
    {
        char keys[64][16];
        int  found = 0, n, want = TMCACHE_PROBE + 1;
        sol_u32 bucket0 = 0; sol_bool have_bucket = 0;
        static TmCache cc2;
        tmcache_init(&cc2);
        for (n = 0; found < want && n < 100000; n++) {
            char t[16]; sol_u32 h, bk;
            snprintf(t, sizeof t, "m%d", n);
            h  = tmcache_hash(F, t, (int)strlen(t));
            bk = h & (TMCACHE_CAP - 1);
            if (!have_bucket) { bucket0 = bk; have_bucket = 1; }
            if (bk == bucket0) { strcpy(keys[found], t); found++; }
        }
        CHECK(found == want);
        for (n = 0; n < TMCACHE_PROBE; n++) {            /* frames 10..25 */
            int s = tmcache_claim(&cc2, F, keys[n], (int)strlen(keys[n]), 10 + n);
            tmcache_set(&cc2, s, (float)n, 0.0f);
        }
        CHECK(tmcache_find(&cc2, F, keys[0], (int)strlen(keys[0]), 50) >= 0);  /* touch keys[0] */
        {   /* next claim now evicts keys[1] (frame 11), keys[0] protected */
            (void)tmcache_claim(&cc2, F, keys[TMCACHE_PROBE],
                                (int)strlen(keys[TMCACHE_PROBE]), 60);
            CHECK(tmcache_find(&cc2, F, keys[0], (int)strlen(keys[0]), 70) >= 0);  /* protected */
            CHECK(tmcache_find(&cc2, F, keys[1], (int)strlen(keys[1]), 70) < 0);   /* evicted */
        }
    }

    if (g_fail == 0) printf("tmcache_test: all passed\n");
    else             printf("tmcache_test: %d FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 3: Add the `tmcachetest` build target.** In `build.sh`, immediately after the `wtcachetest` block (its `fi`, ~build.sh:101), insert:
```sh
# Build + run the pure text-measure cache test under the sanitizers.
if [ "$MODE" = "tmcachetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        tmcache.c tmcache_test.c \
        -o tmcache_test
    echo "built ./tmcache_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 4: Run the test to verify it FAILS first.** `./build.sh tmcachetest` → link error (undefined `tmcache_*`). That is the red.

- [ ] **Step 5: Write `tmcache.c`** (strict C89):
```c
/* tmcache.c — see tmcache.h. Pure: hashing + open-addressed probing + LRU
   eviction over fixed storage. No rhi_*. Single-threaded engine. */
#include "tmcache.h"
#include <string.h>
#include <assert.h>

void tmcache_init(TmCache *c) {
    memset(c, 0, sizeof *c);
}

/* FNV-1a 32-bit. Hashing `len` bytes of text makes different-length strings
   hash differently, so no separate length mix is needed. */
static void fnv(sol_u32 *h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= (sol_u32)b[i];
        *h *= 16777619u;
    }
}

sol_u32 tmcache_hash(const void *font, const char *text, int len) {
    sol_u32 h = 2166136261u;
    fnv(&h, &font, sizeof font);
    fnv(&h, text,  (size_t)len);
    return h;
}

static sol_bool key_eq(const TmCacheEntry *e, sol_u32 hash, const void *font,
                       const char *text, int len) {
    if (!e->used || e->hash != hash) return SOL_FALSE;
    if (e->font != font || e->len != len) return SOL_FALSE;
    return (sol_bool)(memcmp(e->text, text, (size_t)len) == 0);
}

int tmcache_find(TmCache *c, const void *font, const char *text, int len, int frame) {
    sol_u32 hash  = tmcache_hash(font, text, len);
    int     start = (int)(hash & (TMCACHE_CAP - 1));
    int     i;
    for (i = 0; i < TMCACHE_PROBE; i++) {
        int           s = (start + i) & (TMCACHE_CAP - 1);
        TmCacheEntry *e = &c->e[s];
        if (!e->used) return -1;                    /* contiguous prefix ended: absent */
        if (key_eq(e, hash, font, text, len)) {
            e->last_frame = frame;
            return s;
        }
    }
    return -1;
}

int tmcache_claim(TmCache *c, const void *font, const char *text, int len, int frame) {
    sol_u32       hash  = tmcache_hash(font, text, len);
    int           start = (int)(hash & (TMCACHE_CAP - 1));
    int           i, slot = -1, best;
    TmCacheEntry *e;
    assert(len >= 0 && len < TMCACHE_TEXT);         /* caller's contract: room for the NUL */

    for (i = 0; i < TMCACHE_PROBE; i++) {           /* a free slot wins — no eviction */
        int s = (start + i) & (TMCACHE_CAP - 1);
        if (!c->e[s].used) { slot = s; break; }
    }
    if (slot < 0) {                                 /* else evict the LRU in the window */
        best = start & (TMCACHE_CAP - 1);
        for (i = 1; i < TMCACHE_PROBE; i++) {
            int s = (start + i) & (TMCACHE_CAP - 1);
            if (c->e[s].last_frame < c->e[best].last_frame) best = s;
        }
        slot = best;
    }

    e = &c->e[slot];
    e->used  = SOL_TRUE;
    e->hash  = hash;
    e->font  = font;
    e->len   = len;
    memcpy(e->text, text, (size_t)len);
    e->text[len] = '\0';
    e->w = 0.0f; e->h = 0.0f;
    e->last_frame = frame;
    return slot;
}

void tmcache_set(TmCache *c, int slot, float w, float h) {
    c->e[slot].w = w;
    c->e[slot].h = h;
}
```

- [ ] **Step 6: Build + run the test GREEN.** `./build.sh tmcachetest && ./tmcache_test` → `tmcache_test: all passed`, no sanitizer output. Fix until green.

- [ ] **Step 7: Add `tmcache.c` to the engine source lists.** In `build.sh`, replace EVERY occurrence (4) of `wtext.c wtcache.c scene.c` with `wtext.c wtcache.c tmcache.c scene.c`. Verify: `grep -c "wtcache.c tmcache.c scene.c" build.sh` → 4.

- [ ] **Step 8: Verify the engine builds with the new (unused) source.** `./build.sh c89check && ./build.sh` → `c89check: PASS` + `built ./solarium`.

- [ ] **Step 9: Commit.**
```bash
git add tmcache.h tmcache.c tmcache_test.c build.sh
git commit -m "$(cat <<'EOF'
Label measure cache: the pure tmcache table + headless test

Content-addressed (font,text)->(w,h) table (hash + open-addressed linear
probe + LRU eviction), simpler than wtcache (two-float payload, no GPU
lifecycle). No rhi_*, so unit-tested headless (tmcachetest). Wired into
the build source lists; not yet used.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Cached measure + frustum cull (`text.c`/`.h` + `main.c`)

**Files:** Modify `text.c`, `text.h`, `main.c`. One task/commit.

- [ ] **Step 1: Declare the cached measure + clock (`text.h`).** After the `text_measure` declaration (text.h:46-47, the two-line prototype ending `float *out_w, float *out_h);`), add:
```c

/* Cached text_measure (P9 perf #2b): same result, but (font,text)->(w,h) is
   memoized so a per-frame label stops re-shaping for its width. Identical to
   text_measure for any given (font,text,scale). Advance the LRU clock once per
   frame with text_measure_frame_begin(). */
void text_measure_cached(const Font *f, const char *utf8, float scale,
                         float *out_w, float *out_h);
void text_measure_frame_begin(void);
```

- [ ] **Step 2: Implement the cached measure (`text.c`).** Add the include with the others (after `#include <string.h>` at text.c:8):
```c
#include "tmcache.h"
```
Then, immediately AFTER the `text_measure` function body (it ends just before `text_wrap`/the wrapping section ~text.c:118-141 — place this after `text_measure`'s closing `}`), add:
```c
/* ---------------------------------------------- the measure cache (#2b) */
static TmCache g_tm_cache;   /* zero-init in BSS == a valid empty cache */
static int     g_tm_frame = 0;

void text_measure_frame_begin(void) { g_tm_frame++; }

void text_measure_cached(const Font *f, const char *utf8, float scale,
                         float *out_w, float *out_h) {
    int   len, slot;
    float w0, h0;
    if (!f || !utf8) { if (out_w) *out_w = 0.0f; if (out_h) *out_h = 0.0f; return; }
    len = (int)strlen(utf8);
    if (len >= TMCACHE_TEXT) {                      /* uncacheable: measure directly */
        text_measure(f, utf8, scale, out_w, out_h);
        return;
    }
    slot = tmcache_find(&g_tm_cache, (const void *)f, utf8, len, g_tm_frame);
    if (slot >= 0) {                                /* HIT — no shaping */
        w0 = g_tm_cache.e[slot].w;
        h0 = g_tm_cache.e[slot].h;
    } else {                                        /* MISS — shape once, store */
        text_measure(f, utf8, 1.0f, &w0, &h0);
        slot = tmcache_claim(&g_tm_cache, (const void *)f, utf8, len, g_tm_frame);
        tmcache_set(&g_tm_cache, slot, w0, h0);
    }
    if (out_w) *out_w = w0 * scale;
    if (out_h) *out_h = h0 * scale;
}
```
(No `tmcache_init` call needed for `g_tm_cache` — a file-static `TmCache` is zero in BSS, i.e. `used==0` everywhere, a valid empty cache.)

- [ ] **Step 3: Add the `label_in_view` cull helper (`main.c`).** Immediately BEFORE `static void render(AppState *state) {` (main.c:14930), add:
```c
/* A label whose world anchor is outside the view (plus a generous margin) is
   skipped before it measures or draws. The margin keeps edge labels from
   popping as you pan; over-inclusion is cheap (a surviving measure is a cache
   hit). Conservative (positive-vertex AABB) — culling stays an optimization. */
static int label_in_view(const Frustum *f, vec3 anchor) {
    Aabb  b;
    float m = 3.0f;
    b.min = vec3_make(anchor.x - m, anchor.y - m, anchor.z - m);
    b.max = vec3_make(anchor.x + m, anchor.y + m, anchor.z + m);
    return (int)frustum_intersects_aabb(f, b);
}
```

- [ ] **Step 4: Tick the measure-cache clock once per frame (`main.c`).** At the section reset (main.c:15526-15528), add the tick beside `wtext_frame_begin();`:
```c
    text_shape_stats_reset();        /* P9 perf #2 measure: scope to this section */
    wtext_frame_begin();             /* advance the glyph-cache LRU clock */
    text_measure_frame_begin();      /* advance the measure-cache LRU clock */
    wtext_stats_reset();
```

- [ ] **Step 5: Build the cull frustum once in the section (`main.c`).** The section's `vp` declaration (main.c:15533) reads exactly `        mat4        vp  = mat4_mul(proj, view);` (column-aligned). Replace it with:
```c
        mat4        vp  = mat4_mul(proj, view);
        Frustum     lf  = frustum_from_vp(vp);   /* label-cull frustum (this section) */
```
(`lf` is a declaration with an initializer, so it stays in the block's declaration zone — the `if (state->ui_font)` body opens with `uf`/`lh`/`vp` declarations; add `lf` among them.)

- [ ] **Step 6: Doorway loop — cull + cached measure (`main.c`).** Replace (main.c:15868-15870):
```c
                    lpx = 0.35f / lh;                       /* ~35 cm tall letters */
                    text_measure(uf, nm, 1.0f, &nw, (float *)0);
                    x0  = -nw * lpx * 0.5f;                 /* centered over the opening */
```
with:
```c
                    lpx = 0.35f / lh;                       /* ~35 cm tall letters */
                    if (!label_in_view(&lf, vec3_make(door.x + nx * 0.30f,
                            door.y + ROUTE_DOOR_H + 0.2f, door.z + nz * 0.30f)))
                        continue;                           /* off-screen: skip measure + draw */
                    text_measure_cached(uf, nm, 1.0f, &nw, (float *)0);
                    x0  = -nw * lpx * 0.5f;                 /* centered over the opening */
```

- [ ] **Step 7: Bookshelf loop — cull + cached measure (`main.c`).** Replace (main.c:15897-15900):
```c
                h   = mesh_ref_param("bookshelf", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.18f / lh;                          /* ~18 cm letters */
                text_measure(uf, lbl, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;                    /* centered on the shelf */
```
with:
```c
                h   = mesh_ref_param("bookshelf", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.18f / lh;                          /* ~18 cm letters */
                if (!label_in_view(&lf, mat4_mul_point(scene_world_matrix(&state->scene, o),
                        vec3_make(0.0f, h + 0.02f + 2.0f * lpx * lh, 0.16f))))
                    continue;                              /* off-screen: skip measure + draw */
                text_measure_cached(uf, lbl, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;                    /* centered on the shelf */
```

- [ ] **Step 8: Folder loop — cull + cached measure (`main.c`).** Replace (main.c:15922-15925):
```c
                fh  = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.135f / lh;                       /* ~13.5 cm letters (3x) */
                text_measure(uf, lnk, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
```
with:
```c
                fh  = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.135f / lh;                       /* ~13.5 cm letters (3x) */
                if (!label_in_view(&lf, mat4_mul_point(scene_world_matrix(&state->scene, o),
                        vec3_make(0.0f, fh + 2.0f * lpx * lh, 0.06f))))
                    continue;                            /* off-screen: skip measure + draw */
                text_measure_cached(uf, lnk, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
```

- [ ] **Step 9: Board-title loop — cull + cached measure (`main.c`).** Replace (main.c:15949-15952):
```c
                bh  = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.12f / lh;                        /* ~12 cm letters */
                text_measure(uf, ap, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
```
with:
```c
                bh  = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.12f / lh;                        /* ~12 cm letters */
                if (!label_in_view(&lf, mat4_mul_point(scene_world_matrix(&state->scene, o),
                        vec3_make(0.0f, bh + 0.04f + 2.0f * lpx * lh, 0.04f))))
                    continue;                            /* off-screen: skip measure + draw */
                text_measure_cached(uf, ap, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
```

- [ ] **Step 10: Card-label measures → cached (`main.c`).** Both card sites are identical (`&name_w`). Replace ALL occurrences of:
```c
                    text_measure(uf, nm, 1.0f, &name_w, (float *)0);
```
with:
```c
                    text_measure_cached(uf, nm, 1.0f, &name_w, (float *)0);
```
(Use replace-all. NOTE the two occurrences have DIFFERENT leading indentation — the spine one at 15719 is more deeply nested. If your editor requires exact whitespace, do them as two edits using each line's own indentation; both become `text_measure_cached`. There is no other `&name_w` measure, so this is safe.)

- [ ] **Step 11: Reader-image filename measure → cached (`main.c`).** Replace (main.c:15602-15603):
```c
                    cpx = (bp[1] * 0.020f) / lh;
                    text_measure(uf, nm, 1.0f, &nw, (float *)0);
```
with:
```c
                    cpx = (bp[1] * 0.020f) / lh;
                    text_measure_cached(uf, nm, 1.0f, &nw, (float *)0);
```

- [ ] **Step 12: Full build gauntlet.** Run, all must pass:
```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test && ./build.sh wtcachetest && ./wtcache_test && ./build.sh tmcachetest && ./tmcache_test
```
Expect: `c89check: PASS`, `built ./solarium`, `built ./solarium-metal`, `caret_test: all passed`, `wtcache_test: all passed`, `tmcache_test: all passed`. Confirm `grep -n "text_measure(uf" main.c` shows ONLY the swapped-away contexts are gone (the four loop + two card + reader callers now read `text_measure_cached`); the out-of-section callers (14851/14868/16626) intentionally still read `text_measure`. Fix any C89/-Werror issue and re-run until green. Do NOT run `./solarium`/`./solarium-metal` (no display).

- [ ] **Step 13: Commit (one green commit).**
```bash
git add tmcache.h text.c text.h main.c
git commit -m "$(cat <<'EOF'
Label measure cache + frustum-culled label loops

text_measure_cached memoizes (font,text)->(w,h) via tmcache, so the
per-frame label centering/fit-shrink pre-pass stops re-shaping static
text (the residual gly after the glyph cache). The four un-culled label
loops (door/shelf/folder/board) now frustum-cull on each label's world
anchor BEFORE measuring, skipping off-screen labels entirely. The
measure-cache LRU clock ticks once per frame. Parity: all callers pass
scale==1.0 so the cached result is byte-identical. No shader.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```
(`tmcache.h` is added here only if Step 2's `#include "tmcache.h"` is the first time text.c references it and `tmcache.h` wasn't already committed — it WAS committed in Task 1, so `git add tmcache.h` is harmless/no-op; the real changes are text.c/.h + main.c.)

---

## Self-Review notes (for the implementer)

- **Spec coverage:** Task 1 = `tmcache.c/.h` + `tmcachetest` + build wiring (spec §1, §5). Task 2 = `text_measure_cached`/clock (spec §2), the four-loop frustum cull (spec §3), the cached-measure swaps + clock tick (spec §4). Non-goals respected: no `text_measure`/seam change, no `text_wrap` internal caching, no glyph-cache/shader change, out-of-section callers untouched.
- **Parity:** every swapped caller passes `scale==1.0`, so `text_measure_cached` returns `w0*1.0 == w0`, identical to `text_measure` — no centering/fit-shrink shift. `wt_build`/glyph geometry unchanged.
- **Cull correctness:** `frustum_intersects_aabb` is conservative (never drops an inside label); the anchor uses each loop's already-computed `lpx`/`h`/`fh`/`bh` (constants w.r.t. the measure) so the cull runs BEFORE `text_measure_cached` — off-screen labels cost nothing. The card loop keeps its existing `vis` cull (labels are ON the card).
- **Open-addressing invariant** (same as wtcache): slots go unused→used or used→used (eviction-replacement), never back, so `tmcache_find`'s stop-at-first-unused is correct. The headless test pins hit/miss, field discrimination, LRU eviction, and recency protection.
- **C89:** `tmcache.c` / `text.c` keep declarations at block top, no `//`. `label_in_view`'s `Aabb b; float m;` are at the function top. `Frustum lf = ...` is a declaration in the section's declaration zone (after `vp`).
- **No reset needed:** measure entries are permanently valid (font immutable, text in the key); the clock only orders LRU. `g_tm_cache` is BSS-zero = valid empty.
- **Human live-verify (after the gauntlet, both backends), via the HUD `text` line:** in the dense view that was ~5000 gly / ~3.5-4 ms, `gly` should collapse toward ~0 and `ms` drop further; turning away from a label cluster keeps gly/ms low (culled) with no edge flicker as you pan (raise the 3 m margin in `label_in_view` if labels pop); door labels (both sides), folder/shelf/board titles, and file/folder tablet names render in identical positions/sizes; a long multi-room sweep keeps gly low (no thrash → CAP ok).

# wtext Glyph-Geometry Cache — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make static world-space text a near-free bind+draw by caching its built glyph geometry, eliminating the per-frame `text_shape`/`text_wrap`/upload that drives the world-text section to 8–9 ms on Metal.

**Architecture:** A pure, headless-tested `wtcache.c` table (hash + linear-probe + LRU, no `rhi_*` calls) keyed by `(font, text, px_to_m, wrap_w_m, x, top_y)` → a persistent vertex buffer + vert count. `wtext.c`'s flat path does find→hit (bind+draw) or miss (build once, store); the bent path stays immediate. Invalidation is automatic (key change ⇒ rebuild ⇒ stale entry ages out); no call-site, scene, or shader changes.

**Tech Stack:** C89 (engine sources `-std=c89 -pedantic-errors -Werror -Wextra`; the `*_test.c` build c11+ASan/UBSan). Spec: `docs/superpowers/specs/2026-06-27-wtext-glyph-cache-design.md`. Built on branch `wtext-measure` (the instrumentation it uses + proves against).

**House rules:** engine `.c` strict C89 (declarations at block top, no `//`, no mid-block decls); `*_test.c` may be c11. NEVER `git add NOTES.stml`/`paper-picture.png`. Commit bodies end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Work stays on `wtext-measure`.

**Verified facts:** `RhiBuffer` = `{ sol_u32 id; }` (rhi.h:12); `rhi_create_buffer(RhiBufferType, const void*, size_t)` (rhi.h:98), `RHI_BUFFER_VERTEX` (rhi.h:19), `rhi_destroy_buffer(RhiBuffer)` (rhi.h:209). rhi.h includes only `<stddef.h>` + `sol_base.h` (GL-free → safe in a headless test; `wtcache.c` calls **no** `rhi_*`, so the test links with no GL). `wt_emit` (wtext.c:137) builds 6 verts × `WT_VERT_FLOATS`(5) per glyph into the shared `g_wt_verts`, uploads to the shared `g_wt.vbuffer`, then binds pipeline/atlas + `uMVP`/`uColor` + draws; `WT_MAX_GLYPHS` 4096, `WT_WRAP_CAP` 2048 (wtext.h). build.sh lists engine sources explicitly on lines 16/408/427/445, each containing `text.c wtext.c scene.c`; `carettest` recipe at build.sh:81–88. The instrumentation (already on this branch) put `text_shape_stats_reset(); wtext_stats_reset(); t_text0 = glfwGetTime();` before `if (state->ui_font)` and a snapshot block after it, and a HUD line `text %dblk %ldgly %dup %4.2fms`; `wtext_stats_get` is currently `(int*, int*)`.

---

## Task 1: The pure `wtcache` table + headless test + build wiring

**Files:** Create `wtcache.h`, `wtcache.c`, `wtcache_test.c`; modify `build.sh`.

- [ ] **Step 1: Write `wtcache.h`**

Create `wtcache.h`:
```c
/* wtcache.h — a content-addressed cache of built world-text glyph geometry
   (P9 perf #2). PURE: no rhi_* calls, no GL — it stores an RhiBuffer handle as
   an opaque payload and reports eviction victims; wtext.c owns the GPU buffers.
   So the hashing / probing / LRU eviction is unit-tested headless. */
#ifndef WTCACHE_H
#define WTCACHE_H

#include "rhi.h"   /* RhiBuffer + sol_base types (sol_u32 / sol_bool) */

#define WTCACHE_CAP   512    /* entries; MUST be a power of two (open-addressed) */
#define WTCACHE_PROBE 16     /* linear-probe + eviction window length */
#define WTCACHE_TEXT  2048   /* stored pre-wrap key text; matches WT_WRAP_CAP */

typedef struct {
    sol_bool    used;
    sol_u32     hash;
    const void *font;                 /* pointer identity (opaque) */
    float       px2m, wrap, x, top_y;
    int         len;                  /* text length in bytes (excl. NUL) */
    char        text[WTCACHE_TEXT];
    RhiBuffer   buffer;               /* opaque payload — wtcache never reads/frees it */
    int         vc;                   /* vertex count */
    int         last_frame;           /* LRU recency */
} WtCacheEntry;

typedef struct { WtCacheEntry e[WTCACHE_CAP]; } WtCache;

void    wtcache_init(WtCache *c);

/* The shared hash over the key fields (find + claim agree on it). */
sol_u32 wtcache_hash(const void *font, const char *text, int len,
                     float px2m, float wrap, float x, float top_y);

/* Look up; on hit set last_frame=frame and return the slot (>=0), else -1. */
int     wtcache_find(WtCache *c, const void *font, const char *text, int len,
                     float px2m, float wrap, float x, float top_y, int frame);

/* Reserve a slot for a NEW key (miss path): a free slot in the probe window,
   else the LRU victim within it. Writes the key + frame, marks used, vc=0,
   buffer.id=0. If a live entry was displaced, returns its buffer via *evicted
   (id 0 when none) so the caller can rhi_destroy_buffer it. Returns the slot. */
int     wtcache_claim(WtCache *c, const void *font, const char *text, int len,
                      float px2m, float wrap, float x, float top_y, int frame,
                      RhiBuffer *evicted);

/* Store the built payload into a claimed slot. */
void    wtcache_set(WtCache *c, int slot, RhiBuffer buffer, int vc);

#endif /* WTCACHE_H */
```

- [ ] **Step 2: Write the failing test `wtcache_test.c`**

Create `wtcache_test.c` (c11; mirrors `caret_test.c` style — `printf` + `assert`, returns non-zero on failure via an `ok` counter):
```c
/* wtcache_test.c — headless unit test for the pure wtext glyph cache.
   Build: ./build.sh wtcachetest && ./wtcache_test  (ASan + UBSan). */
#include "wtcache.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static RhiBuffer buf(sol_u32 id) { RhiBuffer b; b.id = id; return b; }

int main(void) {
    static WtCache c;          /* static: 512 * ~2KB is large for the stack */
    const void *F = (const void *)0x1000;   /* a fake font identity */
    int slot, slot2;
    RhiBuffer evicted;

    wtcache_init(&c);

    /* 1) empty -> miss */
    CHECK(wtcache_find(&c, F, "hello", 5, 1.0f, 0.0f, 0.0f, 0.0f, 1) < 0);

    /* 2) claim + set -> find hits with the stored payload */
    slot = wtcache_claim(&c, F, "hello", 5, 1.0f, 0.0f, 0.0f, 0.0f, 1, &evicted);
    CHECK(slot >= 0);
    CHECK(evicted.id == 0);
    wtcache_set(&c, slot, buf(7), 12);
    slot2 = wtcache_find(&c, F, "hello", 5, 1.0f, 0.0f, 0.0f, 0.0f, 2);
    CHECK(slot2 == slot);
    CHECK(c.e[slot2].buffer.id == 7);
    CHECK(c.e[slot2].vc == 12);

    /* 3) every key field discriminates (each a distinct miss) */
    CHECK(wtcache_find(&c, F, "hellp", 5, 1.0f, 0.0f, 0.0f, 0.0f, 3) < 0); /* text */
    CHECK(wtcache_find(&c, F, "hello", 5, 2.0f, 0.0f, 0.0f, 0.0f, 3) < 0); /* px2m */
    CHECK(wtcache_find(&c, F, "hello", 5, 1.0f, 9.0f, 0.0f, 0.0f, 3) < 0); /* wrap */
    CHECK(wtcache_find(&c, F, "hello", 5, 1.0f, 0.0f, 1.0f, 0.0f, 3) < 0); /* x */
    CHECK(wtcache_find(&c, F, "hello", 5, 1.0f, 0.0f, 0.0f, 1.0f, 3) < 0); /* top_y */
    CHECK(wtcache_find(&c, (const void *)0x2000, "hello", 5,
                       1.0f, 0.0f, 0.0f, 0.0f, 3) < 0);                    /* font */

    /* 4) LRU eviction within a full probe window returns the oldest's buffer.
       Engineer WTCACHE_PROBE+1 texts that collide to one start bucket. */
    {
        char keys[64][16];
        int  found = 0, n, want = WTCACHE_PROBE + 1;
        sol_u32 bucket0 = 0; sol_bool have_bucket = 0;
        WtCache cc;
        wtcache_init(&cc);
        for (n = 0; found < want && n < 100000; n++) {
            char t[16]; sol_u32 h; sol_u32 bk;
            sprintf(t, "k%d", n);
            h  = wtcache_hash(F, t, (int)strlen(t), 1.0f, 0.0f, 0.0f, 0.0f);
            bk = h & (WTCACHE_CAP - 1);
            if (!have_bucket) { bucket0 = bk; have_bucket = 1; }
            if (bk == bucket0) { strcpy(keys[found], t); found++; }
        }
        CHECK(found == want);
        /* claim the first PROBE at frames 10..(10+PROBE-1); buffers 100.. */
        for (n = 0; n < WTCACHE_PROBE; n++) {
            int s = wtcache_claim(&cc, F, keys[n], (int)strlen(keys[n]),
                                  1.0f, 0.0f, 0.0f, 0.0f, 10 + n, &evicted);
            CHECK(s >= 0);
            CHECK(evicted.id == 0);          /* window not full yet */
            wtcache_set(&cc, s, buf(100u + (sol_u32)n), 1);
        }
        /* the (PROBE+1)th claim must evict the LRU (frame 10 -> buffer 100) */
        {
            int s = wtcache_claim(&cc, F, keys[WTCACHE_PROBE],
                                  (int)strlen(keys[WTCACHE_PROBE]),
                                  1.0f, 0.0f, 0.0f, 0.0f, 99, &evicted);
            CHECK(s >= 0);
            CHECK(evicted.id == 100u);                                  /* the oldest */
            wtcache_set(&cc, s, buf(999u), 1);
            /* key0 (evicted) now misses; key1 and the newcomer hit */
            CHECK(wtcache_find(&cc, F, keys[0], (int)strlen(keys[0]),
                               1.0f, 0.0f, 0.0f, 0.0f, 200) < 0);
            CHECK(wtcache_find(&cc, F, keys[1], (int)strlen(keys[1]),
                               1.0f, 0.0f, 0.0f, 0.0f, 200) >= 0);
            CHECK(wtcache_find(&cc, F, keys[WTCACHE_PROBE],
                               (int)strlen(keys[WTCACHE_PROBE]),
                               1.0f, 0.0f, 0.0f, 0.0f, 200) >= 0);
        }
    }

    if (g_fail == 0) printf("wtcache_test: all passed\n");
    else             printf("wtcache_test: %d FAILED\n", g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 3: Add the `wtcachetest` build target**

In `build.sh`, immediately after the `carettest` block (the `fi` at line ~89), insert:
```sh
# Build + run the pure world-text glyph cache test under the sanitizers.
# wtcache.c calls no rhi_* and includes only header-declared types, so it links
# with no GL.
if [ "$MODE" = "wtcachetest" ]; then
    clang -std=c11 -g -O1 -fno-omit-frame-pointer \
        -fsanitize=address,undefined \
        -Wall -Wextra \
        wtcache.c wtcache_test.c \
        -o wtcache_test
    echo "built ./wtcache_test (ASan + UBSan) — run it; sanitizers report on stderr"
    exit 0
fi
```

- [ ] **Step 4: Run the test to verify it fails (no implementation yet)**

Run: `./build.sh wtcachetest`
Expected: **link error** — undefined symbols `wtcache_init`/`wtcache_find`/`wtcache_claim`/`wtcache_set`/`wtcache_hash` (wtcache.c is empty/absent). That's the red.

- [ ] **Step 5: Write `wtcache.c`**

Create `wtcache.c` (strict C89 — declarations at block top, no `//`):
```c
/* wtcache.c — see wtcache.h. Pure: hashing + open-addressed probing + LRU
   eviction over fixed storage. No rhi_* calls; the RhiBuffer is carried as an
   opaque payload. Single-threaded engine; no locking. */
#include "wtcache.h"
#include <string.h>

void wtcache_init(WtCache *c) {
    memset(c, 0, sizeof *c);
}

/* FNV-1a 32-bit over raw key bytes (floats by their storage bytes — the same
   inputs give the same bit pattern, so the hash is deterministic). */
static void fnv(sol_u32 *h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= (sol_u32)b[i];
        *h *= 16777619u;
    }
}

sol_u32 wtcache_hash(const void *font, const char *text, int len,
                     float px2m, float wrap, float x, float top_y) {
    sol_u32 h = 2166136261u;
    fnv(&h, &font,  sizeof font);
    fnv(&h, &px2m,  sizeof px2m);
    fnv(&h, &wrap,  sizeof wrap);
    fnv(&h, &x,     sizeof x);
    fnv(&h, &top_y, sizeof top_y);
    fnv(&h, text,   (size_t)len);
    return h;
}

static sol_bool key_eq(const WtCacheEntry *e, sol_u32 hash, const void *font,
                       const char *text, int len, float px2m, float wrap,
                       float x, float top_y) {
    if (!e->used || e->hash != hash) return SOL_FALSE;
    if (e->font != font || e->len != len) return SOL_FALSE;
    if (e->px2m != px2m || e->wrap != wrap) return SOL_FALSE;
    if (e->x != x || e->top_y != top_y) return SOL_FALSE;
    return (sol_bool)(memcmp(e->text, text, (size_t)len) == 0);
}

int wtcache_find(WtCache *c, const void *font, const char *text, int len,
                 float px2m, float wrap, float x, float top_y, int frame) {
    sol_u32 hash  = wtcache_hash(font, text, len, px2m, wrap, x, top_y);
    int     start = (int)(hash & (WTCACHE_CAP - 1));
    int     i;
    for (i = 0; i < WTCACHE_PROBE; i++) {
        int           s = (start + i) & (WTCACHE_CAP - 1);
        WtCacheEntry *e = &c->e[s];
        if (!e->used) return -1;                    /* contiguous prefix ended: absent */
        if (key_eq(e, hash, font, text, len, px2m, wrap, x, top_y)) {
            e->last_frame = frame;
            return s;
        }
    }
    return -1;                                       /* not within the probe window */
}

int wtcache_claim(WtCache *c, const void *font, const char *text, int len,
                  float px2m, float wrap, float x, float top_y, int frame,
                  RhiBuffer *evicted) {
    sol_u32       hash  = wtcache_hash(font, text, len, px2m, wrap, x, top_y);
    int           start = (int)(hash & (WTCACHE_CAP - 1));
    int           i, slot = -1, best;
    WtCacheEntry *e;
    RhiBuffer     none;
    none.id = 0;
    if (evicted) *evicted = none;

    /* 1) a free slot in the window wins — no eviction */
    for (i = 0; i < WTCACHE_PROBE; i++) {
        int s = (start + i) & (WTCACHE_CAP - 1);
        if (!c->e[s].used) { slot = s; break; }
    }
    /* 2) else evict the LRU (smallest last_frame) within the window */
    if (slot < 0) {
        best = start & (WTCACHE_CAP - 1);
        for (i = 1; i < WTCACHE_PROBE; i++) {
            int s = (start + i) & (WTCACHE_CAP - 1);
            if (c->e[s].last_frame < c->e[best].last_frame) best = s;
        }
        slot = best;
        if (c->e[slot].used && c->e[slot].buffer.id && evicted)
            *evicted = c->e[slot].buffer;
    }

    e = &c->e[slot];
    e->used  = SOL_TRUE;
    e->hash  = hash;
    e->font  = font;
    e->px2m  = px2m; e->wrap = wrap; e->x = x; e->top_y = top_y;
    e->len   = len;
    memcpy(e->text, text, (size_t)len);
    e->text[len] = '\0';
    e->buffer = none;             /* the caller fills it via wtcache_set */
    e->vc     = 0;
    e->last_frame = frame;
    return slot;
}

void wtcache_set(WtCache *c, int slot, RhiBuffer buffer, int vc) {
    c->e[slot].buffer = buffer;
    c->e[slot].vc     = vc;
}
```

- [ ] **Step 6: Build + run the test green**

Run: `./build.sh wtcachetest && ./wtcache_test`
Expected: `wtcache_test: all passed`, no sanitizer output. Fix until green.

- [ ] **Step 7: Add `wtcache.c` to the engine source lists**

In `build.sh`, add `wtcache.c` right after `wtext.c` in EVERY engine source list. The string `text.c wtext.c scene.c` appears on lines 16 (c89check), 408 (metal), 427 (asan), 445 (main GL) — replace each occurrence:
```
text.c wtext.c scene.c
```
with:
```
text.c wtext.c wtcache.c scene.c
```
(Use a global replace — all four lines share the exact substring.)

- [ ] **Step 8: Verify the engine still builds with the new (unused) source**

Run: `./build.sh c89check && ./build.sh`
Expected: `c89check: PASS` and `built ./solarium`. (wtcache.c compiles under C89 and links; nothing calls it yet.)

- [ ] **Step 9: Commit**

```bash
git add wtcache.h wtcache.c wtcache_test.c build.sh
git commit -m "$(cat <<'EOF'
wtext glyph cache: the pure wtcache table + headless test

Content-addressed table (hash + open-addressed linear probe + LRU
eviction) keyed by (font,text,px2m,wrap,x,top_y) -> an opaque RhiBuffer
payload + vert count. No rhi_* calls, so it's unit-tested headless
(wtcachetest). Wired into the build source lists; not yet used.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Integrate the cache (`wtext.c` / `wtext.h` + `main.c` wiring)

**Files:** Modify `wtext.c`, `wtext.h`, `main.c`. (One task/commit: the `wtext_stats_get` arity change and its main.c call site must land together to keep every commit green.)

- [ ] **Step 1: Extend the public interface (`wtext.h`)**

Add the frame tick and the `misses` stat. Replace:
```c
void wtext_stats_reset(void);
void wtext_stats_get(int *blocks, int *uploads);
```
with:
```c
void wtext_stats_reset(void);
void wtext_stats_get(int *blocks, int *uploads, int *misses);
void wtext_frame_begin(void);    /* advance the LRU clock; call once per frame */
```

- [ ] **Step 2: Add the cache state + counter to `wtext.c`**

Add the include (after the existing includes near the top of `wtext.c`):
```c
#include "wtcache.h"
#include <string.h>      /* strlen, for the cache-key length */
```
Add module state next to the existing counters (after `static int g_wt_blocks = 0; static int g_wt_uploads = 0;`):
```c
static int     g_wt_misses = 0;      /* cacheable blocks that had to rebuild this frame */
static WtCache g_wt_cache;           /* the glyph-geometry cache (flat path) */
static int     g_wt_frame  = 0;      /* LRU clock, advanced once per frame */
```

- [ ] **Step 3: Update reset/get + add `wtext_frame_begin`**

Replace the existing stats functions:
```c
void wtext_stats_reset(void) { g_wt_blocks = 0; g_wt_uploads = 0; }

void wtext_stats_get(int *blocks, int *uploads) {
    if (blocks)  *blocks  = g_wt_blocks;
    if (uploads) *uploads = g_wt_uploads;
}
```
with:
```c
void wtext_stats_reset(void) { g_wt_blocks = 0; g_wt_uploads = 0; g_wt_misses = 0; }

void wtext_stats_get(int *blocks, int *uploads, int *misses) {
    if (blocks)  *blocks  = g_wt_blocks;
    if (uploads) *uploads = g_wt_uploads;
    if (misses)  *misses  = g_wt_misses;
}

void wtext_frame_begin(void) { g_wt_frame++; }
```

- [ ] **Step 4: Init the cache; free its buffers at shutdown**

In `wtext_init`, after `g_wt.ready = SOL_TRUE;` (and before `return SOL_TRUE;`), add:
```c
    wtcache_init(&g_wt_cache);
    g_wt_frame = 0;
```
Replace `wtext_shutdown`:
```c
void wtext_shutdown(void) {
    if (!g_wt.ready) return;
    rhi_destroy_buffer(g_wt.vbuffer);
    rhi_destroy_pipeline(g_wt.pipeline);
    rhi_destroy_shader(g_wt.shader);
    g_wt.ready = SOL_FALSE;
}
```
with:
```c
void wtext_shutdown(void) {
    int i;
    if (!g_wt.ready) return;
    for (i = 0; i < WTCACHE_CAP; i++)               /* free every cached entry's buffer */
        if (g_wt_cache.e[i].used && g_wt_cache.e[i].buffer.id)
            rhi_destroy_buffer(g_wt_cache.e[i].buffer);
    rhi_destroy_buffer(g_wt.vbuffer);
    rhi_destroy_pipeline(g_wt.pipeline);
    rhi_destroy_shader(g_wt.shader);
    g_wt.ready = SOL_FALSE;
}
```

- [ ] **Step 5: Split `wt_emit` into `wt_build` (CPU) + `wt_draw` (GPU bind/draw)**

Replace the whole `wt_emit` function (wtext.c ~135–195, from the `/* the shared emitter:` comment through its closing `}`) with these two functions. `wt_build` is the existing shaping + vertex math, returning the vertex count; `wt_draw` is the existing pipeline/bind/uniform/draw tail, taking the buffer to draw:
```c
/* Build a block's glyph quads into the shared scratch; returns the vertex
   count (0 = nothing inked). Flat when bend is NULL (z = 0), else each glyph's
   left/right edges ride the curve — a piecewise-flat chord per glyph. */
static int wt_build(const Font *f, const char *src,
                    float x, float top_y, float px_to_m,
                    WtextBend bend, void *user, float lift) {
    ShapedGlyph shaped[WT_MAX_GLYPHS];
    float       baseline;
    int         n, i, vc = 0;

    n = text_shape(f, src, shaped, WT_MAX_GLYPHS);
    baseline = top_y - font_ascent(f) * px_to_m;   /* top edge -> first baseline */

    for (i = 0; i < n; i++) {
        const FontGlyph *gl = font_glyph(f, shaped[i].glyph);
        float gx, gy, gw, gh;
        if (!gl || gl->w <= 0.0f) continue;        /* ink-less: advance only */
        gx = x + (shaped[i].x + gl->xoff) * px_to_m;
        gy = baseline - (shaped[i].y + gl->yoff) * px_to_m;   /* the quad's TOP */
        gw = gl->w * px_to_m;
        gh = gl->h * px_to_m;
        if (!bend) {
            vc = wt_vert(vc, gx,      gy,      0.0f, gl->u0, gl->v0);
            vc = wt_vert(vc, gx + gw, gy,      0.0f, gl->u1, gl->v0);
            vc = wt_vert(vc, gx + gw, gy - gh, 0.0f, gl->u1, gl->v1);
            vc = wt_vert(vc, gx,      gy,      0.0f, gl->u0, gl->v0);
            vc = wt_vert(vc, gx + gw, gy - gh, 0.0f, gl->u1, gl->v1);
            vc = wt_vert(vc, gx,      gy - gh, 0.0f, gl->u0, gl->v1);
        } else {
            float ax, az, atx, atz, bx, bz, btx, btz;
            float anx, anz, bnx, bnz, x0, z0, x1, z1;
            bend(gx,      user, &ax, &az, &atx, &atz);
            bend(gx + gw, user, &bx, &bz, &btx, &btz);
            anx = -atz; anz = atx;                 /* the curve's 2D normal */
            bnx = -btz; bnz = btx;
            x0 = ax + anx * lift;  z0 = az + anz * lift;
            x1 = bx + bnx * lift;  z1 = bz + bnz * lift;
            vc = wt_vert(vc, x0, gy,      z0, gl->u0, gl->v0);
            vc = wt_vert(vc, x1, gy,      z1, gl->u1, gl->v0);
            vc = wt_vert(vc, x1, gy - gh, z1, gl->u1, gl->v1);
            vc = wt_vert(vc, x0, gy,      z0, gl->u0, gl->v0);
            vc = wt_vert(vc, x1, gy - gh, z1, gl->u1, gl->v1);
            vc = wt_vert(vc, x0, gy - gh, z0, gl->u0, gl->v1);
        }
    }
    return vc;
}

/* Draw an already-built buffer of `vc` vertices for this block. */
static void wt_draw(RhiBuffer buffer, int vc, mat4 viewproj, mat4 model,
                    const Font *f, float r, float g, float b) {
    mat4 mvp = mat4_mul(viewproj, model);
    rhi_set_pipeline(g_wt.pipeline);
    rhi_bind_vertex_buffer(buffer);
    rhi_bind_texture(font_atlas(f), 0);
    rhi_set_uniform_int("uTex", 0);
    rhi_set_uniform_mat4("uMVP", mvp.m);
    rhi_set_uniform_vec3("uColor", r, g, b);
    rhi_draw(0, vc);
}
```

- [ ] **Step 6: Rewrite `wtext_block` (flat) to use the cache**

Replace the existing `wtext_block`:
```c
void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b) {
    char        wrapped[WT_WRAP_CAP];
    const char *src = utf8;

    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f) return;
    if (wrap_w_m > 0.0f) {
        if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
            src = wrapped;
    }
    wt_emit(f, viewproj, model, src, x, top_y, px_to_m,
            (WtextBend)0, (void *)0, 0.0f, r, g, b);
}
```
with:
```c
void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b) {
    char        wrapped[WT_WRAP_CAP];
    const char *src = utf8;
    int         len, slot, vc;
    RhiBuffer   buf, evicted;

    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f) return;
    len = (int)strlen(utf8);

    if (len < WTCACHE_TEXT) {                       /* cacheable: the common case */
        slot = wtcache_find(&g_wt_cache, (const void *)f, utf8, len,
                            px_to_m, wrap_w_m, x, top_y, g_wt_frame);
        if (slot >= 0) {                            /* HIT — no shape, no upload */
            g_wt_blocks++;
            wt_draw(g_wt_cache.e[slot].buffer, g_wt_cache.e[slot].vc,
                    viewproj, model, f, r, g, b);
            return;
        }
        if (wrap_w_m > 0.0f) {                      /* MISS — build once, store */
            if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
                src = wrapped;
        }
        vc = wt_build(f, src, x, top_y, px_to_m, (WtextBend)0, (void *)0, 0.0f);
        if (vc == 0) return;                        /* whitespace-only: nothing to cache */
        slot = wtcache_claim(&g_wt_cache, (const void *)f, utf8, len,
                             px_to_m, wrap_w_m, x, top_y, g_wt_frame, &evicted);
        if (evicted.id) rhi_destroy_buffer(evicted);
        buf = rhi_create_buffer(RHI_BUFFER_VERTEX, g_wt_verts,
                                (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
        wtcache_set(&g_wt_cache, slot, buf, vc);
        g_wt_blocks++; g_wt_uploads++; g_wt_misses++;
        wt_draw(buf, vc, viewproj, model, f, r, g, b);
        return;
    }

    /* uncacheable (huge string, e.g. a big reader page): the immediate path on
       the shared scratch buffer — one block, no scaling concern */
    if (wrap_w_m > 0.0f) {
        if (text_wrap(f, utf8, px_to_m, wrap_w_m, wrapped, WT_WRAP_CAP) > 0)
            src = wrapped;
    }
    vc = wt_build(f, src, x, top_y, px_to_m, (WtextBend)0, (void *)0, 0.0f);
    if (vc == 0) return;
    rhi_update_buffer(g_wt.vbuffer, g_wt_verts,
                      (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
    g_wt_blocks++; g_wt_uploads++;
    wt_draw(g_wt.vbuffer, vc, viewproj, model, f, r, g, b);
}
```

- [ ] **Step 7: Rewrite `wtext_block_bent` to use the split (unchanged behavior)**

Replace the existing `wtext_block_bent`:
```c
void wtext_block_bent(const Font *f, mat4 viewproj, mat4 model,
                      const char *utf8, float x, float top_y, float px_to_m,
                      WtextBend bend, void *user, float lift,
                      float r, float g, float b) {
    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f || !bend) return;
    wt_emit(f, viewproj, model, utf8, x, top_y, px_to_m,
            bend, user, lift, r, g, b);
}
```
with:
```c
void wtext_block_bent(const Font *f, mat4 viewproj, mat4 model,
                      const char *utf8, float x, float top_y, float px_to_m,
                      WtextBend bend, void *user, float lift,
                      float r, float g, float b) {
    int vc;
    if (!g_wt.ready || !f || !utf8 || px_to_m <= 0.0f || !bend) return;
    vc = wt_build(f, utf8, x, top_y, px_to_m, bend, user, lift);   /* the leaf turns: rebuild */
    if (vc == 0) return;
    rhi_update_buffer(g_wt.vbuffer, g_wt_verts,
                      (size_t)vc * WT_VERT_FLOATS * sizeof(sol_f32));
    g_wt_blocks++; g_wt_uploads++;
    wt_draw(g_wt.vbuffer, vc, viewproj, model, f, r, g, b);
}
```

- [ ] **Step 8: Add the `t_text_misses` AppState field (`main.c`)**

The interface arity change (`wtext_stats_get`) must land together with its call site, so `main.c` is updated in this same task/commit to keep every commit green. Find the instrumentation fields added on this branch (search `t_text_blocks, t_text_uploads`) and extend that line:
```c
    int   t_text_blocks, t_text_uploads;     /* wtext blocks drawn / buffer re-uploads */
```
to:
```c
    int   t_text_blocks, t_text_uploads, t_text_misses; /* wtext blocks / re-uploads / cache misses */
```

- [ ] **Step 9: Advance the LRU clock once per frame (`main.c`)**

In `render`, find the section reset (search `wtext_stats_reset();` — it sits just before `if (state->ui_font) {`) and add the frame tick before it:
```c
    text_shape_stats_reset();        /* P9 perf #2 measure: scope to this section */
    wtext_frame_begin();             /* advance the glyph-cache LRU clock */
    wtext_stats_reset();
    t_text0 = glfwGetTime();
    if (state->ui_font) {
```

- [ ] **Step 10: Read the miss count in the snapshot block (`main.c`)**

Find the snapshot block (search `wtext_stats_get(&tb, &tu);`). Replace:
```c
        int  tb, tu;
        long tc, tg;
        text_shape_stats_get(&tc, &tg);
        wtext_stats_get(&tb, &tu);
        state->t_text_shape_calls  = tc;
        state->t_text_shape_glyphs = tg;
        state->t_text_blocks       = tb;
        state->t_text_uploads      = tu;
        state->t_text_ms = (float)((glfwGetTime() - t_text0) * 1000.0);
```
with:
```c
        int  tb, tu, tm;
        long tc, tg;
        text_shape_stats_get(&tc, &tg);
        wtext_stats_get(&tb, &tu, &tm);
        state->t_text_shape_calls  = tc;
        state->t_text_shape_glyphs = tg;
        state->t_text_blocks       = tb;
        state->t_text_uploads      = tu;
        state->t_text_misses       = tm;
        state->t_text_ms = (float)((glfwGetTime() - t_text0) * 1000.0);
```

- [ ] **Step 11: Show the miss count on the HUD (`main.c`)**

Find the HUD text line (search `"text %dblk %ldgly %dup %4.2fms"`). Replace:
```c
            sprintf(line, "text %dblk %ldgly %dup %4.2fms",
                    state->t_text_blocks, state->t_text_shape_glyphs,
                    state->t_text_uploads, (double)state->t_text_ms);
```
with:
```c
            sprintf(line, "text %dblk %ldgly %dup %dmiss %4.2fms",
                    state->t_text_blocks, state->t_text_shape_glyphs,
                    state->t_text_uploads, state->t_text_misses,
                    (double)state->t_text_ms);
```

- [ ] **Step 12: Full build gauntlet**

```bash
./build.sh c89check && ./build.sh && ./build.sh metal && ./build.sh carettest && ./caret_test && ./build.sh wtcachetest && ./wtcache_test
```
All must pass: `c89check: PASS`, `built ./solarium`, `built ./solarium-metal`, `caret_test: all passed`, `wtcache_test: all passed`. Fix any C89/-Werror/arity issue and re-run until green. (If `c89check` flags `wt_emit` as an unused function, confirm it was fully replaced by `wt_build`/`wt_draw` — no `wt_emit` should remain.) Do NOT run `./solarium` (no display).

- [ ] **Step 13: Commit (the whole integration, one green commit)**

```bash
git add wtext.c wtext.h main.c
git commit -m "$(cat <<'EOF'
wtext glyph cache: cache the flat path (hit = bind+draw, no reshape)

Split wt_emit into wt_build (shape+verts) + wt_draw (bind+uniforms+draw).
wtext_block now finds the (font,text,px2m,wrap,x,top_y) key: a hit draws
the persistent cached buffer with zero shaping/upload; a miss builds once
into its own buffer and stores it (LRU-evicting + freeing the victim).
Huge strings fall back to the immediate shared-buffer path. The bent
reader leaf stays immediate (it animates). main.c ticks the LRU clock
(wtext_frame_begin) once per frame and shows a per-frame cache-miss count
on the stats card (text Nblk Mgly Kup Pmiss X.XXms). No shader change.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review notes (for the implementer)

- **Spec coverage:** Task 1 = `wtcache.c/.h` pure table + `wtcachetest` + build wiring (spec §1, §5). Task 2 = the flat-path cache integration (`wt_build`/`wt_draw` split, per-entry buffers, bent path immediate, shutdown frees, frame tick, miss counter) AND its main.c wiring + HUD miss (spec §2, §3, §4) — kept in one task so every commit builds green. Non-goals respected (no bent caching, no label culling, no shader, no scene_io).
- **Build-at-as-is decision:** `wt_build` keeps the exact vertex math `wt_emit` had — geometry is byte-identical to before, so visual parity holds. The cache only persists the bytes.
- **Open-addressing correctness:** slots go unused→used (claim's first-free) or used→used (eviction-replacement) — **never** back to unused. So each probe chain's used region is a contiguous prefix, and `wtcache_find` stopping at the first `!used` slot is correct (a present key is always before the first hole). The headless test pins hit/miss, field discrimination, and LRU-victim return.
- **Eviction frees the GPU buffer:** `wtcache_claim` returns the displaced buffer; `wtext_block` calls `rhi_destroy_buffer(evicted)` before creating the new one. `wtext_shutdown` frees every still-used entry. No buffer-handle leak.
- **`gly`→0 is the proof:** a hit never calls `wt_build` (no `text_shape`) nor `text_wrap` (no per-word `text_shape`), so it adds zero glyphs. Only misses shape. The HUD `gly`/`ms` collapsing in steady state IS the acceptance.
- **C89:** `wtcache.c`, `wtext.c` keep declarations at block top, no `//`. `wtcache_test.c` is c11 (test-only, like `caret_test.c`) — not in the c89check source list. `static WtCache c;` in the test avoids a ~1 MB stack frame.
- **Metal:** no MSL change (the pipeline/shaders are untouched; only buffer ownership moved). The per-block `MTLBuffer` alloc churn is erased for hits (no `rhi_create/update_buffer` on a hit).
- **Human live-verify (after the gauntlet, both backends), via the HUD `text` line:** steady-state `gly`→~0 and `ms` far below the 8–9 ms peak; `miss`≈0 when still; a one-frame `gly`/`miss` spike turning toward a dense window, then settle; typing a note misses only that note and renders identically; resizing a note rebuilds once; a long multi-room sweep keeps `miss` low (no thrash → CAP ok); all label/card/note/doorway text visually identical to pre-cache.

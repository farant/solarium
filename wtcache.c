/* wtcache.c — see wtcache.h. Pure: hashing + open-addressed probing + LRU
   eviction over fixed storage. No rhi_* calls; the RhiBuffer is carried as an
   opaque payload. Single-threaded engine; no locking. */
#include "wtcache.h"
#include <string.h>
#include <assert.h>

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

/* Assumes canonical-bit finite floats; -0.0/NaN would just cause benign extra
   misses (never a false hit), and the real inputs are finite positives. */
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
    assert(len >= 0 && len < WTCACHE_TEXT);  /* caller's contract: leaves room for the NUL */
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

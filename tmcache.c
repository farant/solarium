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
    assert(len >= 0 && len < TMCACHE_TEXT);
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

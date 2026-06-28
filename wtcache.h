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

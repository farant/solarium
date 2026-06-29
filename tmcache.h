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

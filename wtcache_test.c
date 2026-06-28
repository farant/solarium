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

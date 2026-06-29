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

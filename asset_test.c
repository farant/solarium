/* asset_test.c — headless checks for the registry core (P4 item 4 piece 1):
   ownership as arithmetic, proven with counting fake destructors — the GPU
   never appears. Covers acquire-miss/add/acquire-hit, refcounts to zero
   destroying exactly once, re-acquire after death, the ACQUIRE-BEFORE-
   RELEASE swap order (the survivor never dies), unpaired release, payload
   shape guards, update-in-place, and store_free. `build.sh assettest`. */

#include "asset.h"

#include <stdio.h>
#include <string.h>

/* the counting fake: every destruction is recorded with its payload value */
typedef struct {
    int  destroyed;
    int  last_value;
} DestroyLog;

static void log_destroy(void *payload, void *user) {
    DestroyLog *log = (DestroyLog *)user;
    log->destroyed += 1;
    log->last_value = *(int *)payload;
}

int main(void) {
    AssetStore s;
    DestroyLog log;
    int        v, out;

    log.destroyed = 0;
    log.last_value = 0;
    asset_store_init(&s, log_destroy, &log);

    /* miss -> add -> hit, with the payload riding along */
    v = 41;
    if (asset_acquire(&s, "room|6|4|3", &out, sizeof out)) {
        printf("FAIL: acquire before add must miss\n"); return 1;
    }
    if (!asset_store_add(&s, "room|6|4|3", &v, sizeof v)) {
        printf("FAIL: add must succeed\n"); return 1;
    }
    if (!asset_acquire(&s, "room|6|4|3", &out, sizeof out) || out != 41) {
        printf("FAIL: acquire after add must hit with the payload\n"); return 1;
    }
    printf("miss/add/hit: ok (live=%d refs=%d)\n",
           asset_live_count(&s), asset_ref_total(&s));
    if (asset_live_count(&s) != 1 || asset_ref_total(&s) != 2) {
        printf("FAIL: one entry, two refs expected\n"); return 1;
    }

    /* two releases reach zero; the destructor fires exactly once */
    asset_release(&s, "room|6|4|3");
    if (log.destroyed != 0) { printf("FAIL: early destroy\n"); return 1; }
    asset_release(&s, "room|6|4|3");
    if (log.destroyed != 1 || log.last_value != 41) {
        printf("FAIL: zero must destroy exactly once, with the payload\n");
        return 1;
    }
    if (asset_acquire(&s, "room|6|4|3", &out, sizeof out)) {
        printf("FAIL: a dead key must miss again\n"); return 1;
    }
    printf("refcount-to-zero destroys once: ok\n");

    /* unpaired release is a loud no-op */
    if (asset_release(&s, "never-added")) {
        printf("FAIL: releasing an unknown key must refuse\n"); return 1;
    }

    /* THE SWAP ORDER: a survivor shared by old and new scenes. Naive
       release-old-first kills and recreates it; acquire-new-first keeps its
       count off zero the whole way — it must never die. */
    v = 7;
    asset_store_add(&s, "survivor", &v, sizeof v);          /* old scene: ref 1 */
    log.destroyed = 0;
    if (!asset_acquire(&s, "survivor", &out, sizeof out)) { /* NEW scene acquires FIRST */
        printf("FAIL: survivor must hit\n"); return 1;
    }
    asset_release(&s, "survivor");                          /* old scene releases */
    if (log.destroyed != 0) {
        printf("FAIL: the survivor died during the swap\n"); return 1;
    }
    if (asset_live_count(&s) != 1 || asset_ref_total(&s) != 1) {
        printf("FAIL: survivor should remain at ref 1\n"); return 1;
    }
    /* ... and the bad order, demonstrated: release to zero, then 'acquire'
       misses — the asset would have been destroyed and recreated */
    asset_release(&s, "survivor");
    if (log.destroyed != 1 || asset_acquire(&s, "survivor", &out, sizeof out)) {
        printf("FAIL: release-first order must kill (that's the lesson)\n");
        return 1;
    }
    printf("swap order (acquire-first preserves the survivor): ok\n");

    /* shape guards: oversized payloads refused; size mismatch refuses
       (a double against a stored int — genuinely different bytes) */
    {
        unsigned char big[ASSET_PAYLOAD_MAX + 1];
        double        d = 1.0;
        memset(big, 0, sizeof big);
        if (asset_store_add(&s, "too-big", big, sizeof big)) {
            printf("FAIL: oversized payload must be refused\n"); return 1;
        }
        v = 5;
        asset_store_add(&s, "an-int", &v, sizeof v);
        if (asset_acquire(&s, "an-int", &d, sizeof d)) {
            printf("FAIL: size mismatch must refuse\n"); return 1;
        }
        asset_release(&s, "an-int");          /* back to zero: gone */
    }
    printf("payload shape guards: ok\n");

    /* update-in-place: bytes change, key and refcount don't (hot reload) */
    v = 100;
    asset_store_add(&s, "tex|paper.png|srgb", &v, sizeof v);
    v = 200;
    if (!asset_store_update(&s, "tex|paper.png|srgb", &v, sizeof v)) {
        printf("FAIL: update must hit\n"); return 1;
    }
    if (!asset_acquire(&s, "tex|paper.png|srgb", &out, sizeof out) || out != 200) {
        printf("FAIL: update must replace the payload in place\n"); return 1;
    }
    if (asset_ref_total(&s) != 2) {
        printf("FAIL: update must not touch refcounts\n"); return 1;
    }
    printf("update-in-place: ok\n");

    /* store_free destroys every live payload */
    log.destroyed = 0;
    asset_store_free(&s);
    if (log.destroyed != 1) {
        printf("FAIL: store_free must destroy the live entry\n"); return 1;
    }
    printf("store_free sweeps the living: ok\n");

    printf("asset_test: OK\n");
    return 0;
}

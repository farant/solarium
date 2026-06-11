/* asset.c — see asset.h. Pure bookkeeping; no GPU, no scene. C89.
   Linear scans throughout: acquire/release happen at load and mint, never
   per frame, and the store holds hundreds of entries at most. */

#include "asset.h"

#include <stdlib.h>
#include <string.h>

static char *asset_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char  *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void asset_store_init(AssetStore *s, AssetDestroyFn destroy, void *user) {
    s->entries = NULL;
    s->count   = 0;
    s->cap     = 0;
    s->destroy = destroy;
    s->user    = user;
}

void asset_store_free(AssetStore *s) {
    int i;
    for (i = 0; i < s->count; i++) {
        if (s->destroy) s->destroy(s->entries[i].payload, s->user);
        free(s->entries[i].key);
    }
    free(s->entries);
    s->entries = NULL;
    s->count   = 0;
    s->cap     = 0;
}

static AssetEntry *find_entry(const AssetStore *s, const char *key) {
    int i;
    for (i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].key, key) == 0) return &s->entries[i];
    }
    return NULL;
}

sol_bool asset_acquire(AssetStore *s, const char *key,
                       void *payload_out, size_t size) {
    AssetEntry *e = find_entry(s, key);
    if (e == NULL) return SOL_FALSE;
    if (size != e->size) return SOL_FALSE;     /* same key, wrong shape: refuse */
    e->refcount += 1;
    memcpy(payload_out, e->payload, size);
    return SOL_TRUE;
}

sol_bool asset_store_add(AssetStore *s, const char *key,
                         const void *payload, size_t size) {
    AssetEntry *e;
    char       *k;
    if (size > ASSET_PAYLOAD_MAX) return SOL_FALSE;
    if (find_entry(s, key) != NULL) return SOL_FALSE;  /* add is for misses only */
    k = asset_strdup(key);
    if (k == NULL) return SOL_FALSE;
    if (s->count == s->cap) {
        int         ncap = s->cap ? s->cap * 2 : 32;
        AssetEntry *ne   = (AssetEntry *)realloc(s->entries,
                               (size_t)ncap * sizeof *ne);
        if (ne == NULL) { free(k); return SOL_FALSE; }
        s->entries = ne;
        s->cap     = ncap;
    }
    e = &s->entries[s->count++];
    e->key      = k;
    e->refcount = 1;
    e->size     = size;
    memcpy(e->payload, payload, size);
    return SOL_TRUE;
}

sol_bool asset_release(AssetStore *s, const char *key) {
    AssetEntry *e = find_entry(s, key);
    if (e == NULL) return SOL_FALSE;           /* unpaired release: loud no-op */
    e->refcount -= 1;
    if (e->refcount <= 0) {
        if (s->destroy) s->destroy(e->payload, s->user);
        free(e->key);
        *e = s->entries[--s->count];           /* order-free swap-remove */
    }
    return SOL_TRUE;
}

void asset_store_visit(AssetStore *s, AssetVisitFn fn, void *user) {
    int i;
    for (i = 0; i < s->count; i++)
        fn(s->entries[i].key, s->entries[i].payload, user);
}

sol_bool asset_store_update(AssetStore *s, const char *key,
                            const void *payload, size_t size) {
    AssetEntry *e = find_entry(s, key);
    if (e == NULL || size != e->size) return SOL_FALSE;
    memcpy(e->payload, payload, size);         /* key + refcount untouched */
    return SOL_TRUE;
}

int asset_live_count(const AssetStore *s) {
    return s->count;
}

int asset_ref_total(const AssetStore *s) {
    int i, n = 0;
    for (i = 0; i < s->count; i++) n += s->entries[i].refcount;
    return n;
}

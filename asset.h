/* asset.h — the asset registry core (P4 item 4): refcounted ownership for
   shared assets. Ownership as arithmetic: acquire = +1, release = -1, zero
   = the destructor runs and the entry dies. This module is PURE BOOKKEEPING
   — keys, counts, payload bytes, and a destructor callback. It never
   touches the GPU: the mesh and texture flavors (pieces 2/3) wire their own
   creation and destruction around it, which is exactly what makes ownership
   headless-testable (assettest injects counting fakes).

   The contract that retires the L-reload leak: the registry is the ONLY
   code that destroys a registered asset; everyone else borrows. Swaps go
   acquire-new-first, release-old-second, so an asset present on both sides
   never sees zero mid-swap — survivors keep their buffers. */

#ifndef ASSET_H
#define ASSET_H

#include <stddef.h>
#include "sol_base.h"

/* payloads are stored BY COPY (a Mesh, an RhiTexture + sidecar) — no
   pointers into the store survive a realloc, because none are handed out */
#define ASSET_PAYLOAD_MAX 64

typedef void (*AssetDestroyFn)(void *payload, void *user);
typedef void (*AssetVisitFn)(const char *key, void *payload, void *user);

typedef struct {
    char         *key;
    int           refcount;
    size_t        size;
    unsigned char payload[ASSET_PAYLOAD_MAX];
} AssetEntry;

typedef struct {
    AssetEntry    *entries;
    int            count, cap;
    AssetDestroyFn destroy;     /* runs at refcount zero and at store_free */
    void          *user;        /* threaded through to destroy/visit */
} AssetStore;

void asset_store_init(AssetStore *s, AssetDestroyFn destroy, void *user);
void asset_store_free(AssetStore *s);   /* destroys every live payload */

/* The acquire-or-create handshake, split in two on purpose (creation stays
   with the caller, bookkeeping here): asset_acquire looks the key up — on a
   hit it bumps the refcount and copies the payload out (SOL_TRUE); on a
   miss (SOL_FALSE) the caller creates the asset and registers it with
   asset_store_add, which stores a copy at refcount 1. */
sol_bool asset_acquire(AssetStore *s, const char *key,
                       void *payload_out, size_t size);
sol_bool asset_store_add(AssetStore *s, const char *key,
                         const void *payload, size_t size);

/* -1; at zero the destructor runs on the stored payload and the entry is
   removed. Releasing an unknown key is a loud no-op (returns SOL_FALSE) —
   an unpaired release is a bug worth hearing about. */
sol_bool asset_release(AssetStore *s, const char *key);

/* hot reload's scan (piece 3) + in-place payload update: the entry keeps
   its key and refcount; only the bytes change (a handle is a NAME — usually
   the payload doesn't even change, the GPU contents behind it do) */
void     asset_store_visit(AssetStore *s, AssetVisitFn fn, void *user);
sol_bool asset_store_update(AssetStore *s, const char *key,
                            const void *payload, size_t size);

/* the HUD's instruments */
int asset_live_count(const AssetStore *s);   /* entries alive */
int asset_ref_total(const AssetStore *s);    /* refs held across them */

#endif /* ASSET_H */

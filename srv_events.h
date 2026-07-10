/* srv_events.h — the append-only event store + projections (spec §5). Append
   writes the event row AND updates the projection tables in ONE transaction
   under the write lock, so they can never drift; projections stay rebuildable
   from the log. events.id is the global sync cursor. Unknown entity kinds
   append fine and simply aren't projected yet (forward compatibility).
   Client-op idempotency keys arrive with sub-project 2 as migration v2. */
#ifndef SRV_EVENTS_H
#define SRV_EVENTS_H

#include "srv_db.h"
#include "nid.h"

typedef struct SrvEventIn {
    long        ts;              /* 0 = stamp with time(NULL) */
    long        actor_id;        /* users.id; 0 = system (seed) */
    const char *origin_device;   /* "" ok; echo-suppression key for sync */
    const char *entity_kind;     /* "board" | future kinds */
    const char *entity_nid;
    const char *op;              /* "create" | "update" | "delete" */
    const char *payload;         /* JSON object text (changed fields) */
} SrvEventIn;

typedef struct SrvEventOut {
    long  id, ts, actor_id;
    char  origin_device[64];
    char  entity_kind[16];
    char  entity_nid[NID_LEN + 1];
    char  op[8];
    char *payload;               /* malloc'd; freed by srv_events_free */
} SrvEventOut;

int  srv_events_append(SrvDb *db, const SrvEventIn *in, long *out_id);
int  srv_events_after (SrvDb *db, long cursor, int max,
                       SrvEventOut **out, int *count);
void srv_events_free  (SrvEventOut *evs, int count);

#endif /* SRV_EVENTS_H */

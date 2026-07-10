/* srv_db.h — solsrv SQLite layer: open/migrate, WAL, ONE mutex-guarded write
   connection + short-lived read-only connections (spec §4). Server-only TU —
   never linked into the engine. Concurrency contract: EVERY touch of db->w
   happens between srv_db_wlock/srv_db_wunlock (multi-statement transactions
   included); reads go through srv_db_ropen so they never block the writer. */
#ifndef SRV_DB_H
#define SRV_DB_H

#include <pthread.h>
#include <sqlite3.h>

typedef struct SrvDb {
    sqlite3        *w;
    pthread_mutex_t wlock;
    char            path[512];
} SrvDb;

int      srv_db_open  (SrvDb *db, const char *path);   /* 0 ok, -1 fail (logged) */
void     srv_db_close (SrvDb *db);

void     srv_db_wlock  (SrvDb *db);
void     srv_db_wunlock(SrvDb *db);

/* Run sql on the write connection. Caller MUST hold wlock. 0 ok, -1 logged. */
int      srv_db_exec  (SrvDb *db, const char *sql);

sqlite3 *srv_db_ropen (SrvDb *db);                      /* NULL on fail (logged) */
void     srv_db_rclose(sqlite3 *c);

#endif /* SRV_DB_H */

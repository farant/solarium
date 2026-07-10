/* srv_web.h — the server's pure HTML layer: a growable string builder, the
   HTML escape, and whole-page renderers (spec §7 web surface). No sqlite, no
   civetweb, no GL — Task 8 feeds it rows and ships the bytes. Every dynamic
   value MUST go through sb_put_html. */
#ifndef SRV_WEB_H
#define SRV_WEB_H

#include "nid.h"
#include <stddef.h>

typedef struct Sb {
    char  *buf;   /* always NUL-terminated */
    size_t len, cap;
} Sb;

void sb_init    (Sb *s);   /* aborts on OOM (server posture) */
void sb_free    (Sb *s);
void sb_puts    (Sb *s, const char *str);
void sb_put_html(Sb *s, const char *str);   /* & < > " ' escaped */
void sb_put_long(Sb *s, long v);

typedef struct WebBoardRow {
    char nid[NID_LEN + 1];
    char title[256];
    long updated_at;
} WebBoardRow;

void web_page_login (Sb *out, const char *err_msg);   /* err_msg NULL = none */
void web_page_boards(Sb *out, const WebBoardRow *rows, int n);
void web_page_error (Sb *out, int status, const char *msg);

#endif /* SRV_WEB_H */

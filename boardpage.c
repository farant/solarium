#include "boardpage.h"
#include <string.h>

int boardpage_slugify(const char *in, char *out, int cap) {
    int         oi   = 0;
    int         pend = 0;          /* a separator run is pending */
    const char *p;
    if (cap < 2) { if (cap > 0) out[0] = '\0'; return 0; }
    out[oi++] = '/';
    if (in) {
        for (p = in; *p; p++) {
            unsigned char c = (unsigned char)*p;
            int isnum = (c >= '0' && c <= '9');
            int isup  = (c >= 'A' && c <= 'Z');
            int islo  = (c >= 'a' && c <= 'z');
            if (isnum || isup || islo) {
                if (pend && oi > 1 && oi < cap - 1) out[oi++] = '-';
                pend = 0;
                if (oi < cap - 1)
                    out[oi++] = (char)(isup ? (c - 'A' + 'a') : c);
            } else {
                pend = 1;          /* space/punct: defer one dash, collapse runs,
                                      and never flush a trailing one */
            }
        }
    }
    /* trim any trailing dash left by buffer-full truncation */
    while (oi > 1 && out[oi - 1] == '-') oi--;
    out[oi] = '\0';
    return oi;
}

/* page comparator: "/" sorts before everything, else natural (numeric) order. */
static int page_cmp(const char *a, const char *b) {
    int ra = (strcmp(a, "/") == 0), rb = (strcmp(b, "/") == 0);
    if (ra != rb) return ra ? -1 : 1;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            const char *ea = a, *eb = b;   /* compare two digit runs numerically */
            int la, lb;
            while (*ea >= '0' && *ea <= '9') ea++;
            while (*eb >= '0' && *eb <= '9') eb++;
            while (a < ea - 1 && *a == '0') a++;   /* strip leading zeros */
            while (b < eb - 1 && *b == '0') b++;
            la = (int)(ea - a); lb = (int)(eb - b);
            if (la != lb) return la < lb ? -1 : 1;        /* fewer digits = smaller */
            while (a < ea) { if (*a != *b) return *a < *b ? -1 : 1; a++; b++; }
        } else {
            if (ca != cb) return ca < cb ? -1 : 1;
            a++; b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

static void page_add_unique(char out[][PAGE_SLUG_CAP], int *count, int cap,
                            const char *s) {
    int i;
    if (!s || !s[0]) return;
    if (*count >= cap) return;
    for (i = 0; i < *count; i++)
        if (strcmp(out[i], s) == 0) return;
    strncpy(out[*count], s, PAGE_SLUG_CAP - 1);
    out[*count][PAGE_SLUG_CAP - 1] = '\0';
    (*count)++;
}

int boardpage_collect(const char *const *pages, int n, const char *active,
                      char out[][PAGE_SLUG_CAP], int cap) {
    const char *act = (active && active[0]) ? active : "/";
    int count = 0, i, j;
    page_add_unique(out, &count, cap, "/");
    page_add_unique(out, &count, cap, act);
    for (i = 0; i < n; i++)
        page_add_unique(out, &count, cap, pages ? pages[i] : (const char *)0);
    /* insertion sort by page_cmp */
    for (i = 1; i < count; i++) {
        char key[PAGE_SLUG_CAP];
        strncpy(key, out[i], PAGE_SLUG_CAP - 1);
        key[PAGE_SLUG_CAP - 1] = '\0';
        j = i - 1;
        while (j >= 0 && page_cmp(out[j], key) > 0) {
            strncpy(out[j + 1], out[j], PAGE_SLUG_CAP - 1);
            out[j + 1][PAGE_SLUG_CAP - 1] = '\0';
            j--;
        }
        strncpy(out[j + 1], key, PAGE_SLUG_CAP - 1);
        out[j + 1][PAGE_SLUG_CAP - 1] = '\0';
    }
    return count;
}

/* advance past the next space-delimited token; sets start and len (len=0 at end). */
static const char *page_next_tok(const char *p, const char **start, int *len) {
    while (*p == ' ') p++;
    *start = p;
    while (*p && *p != ' ') p++;
    *len = (int)(p - *start);
    return p;
}

int boardpage_list(const char *stored, const char *active,
                   char out[][PAGE_SLUG_CAP], int cap) {
    int         count = 0;
    const char *p;
    page_add_unique(out, &count, cap, "/");
    if (stored) {
        p = stored;
        while (*p) {
            const char *start;
            int len;
            p = page_next_tok(p, &start, &len);
            if (len > 0 && len < PAGE_SLUG_CAP) {
                char tok[PAGE_SLUG_CAP];
                memcpy(tok, start, (size_t)len); tok[len] = '\0';
                page_add_unique(out, &count, cap, tok);
            }
        }
    }
    page_add_unique(out, &count, cap, (active && active[0]) ? active : "/");
    return count;
}

int boardpage_contains(const char *list, const char *slug) {
    const char *p;
    int slen;
    if (!list || !slug || !slug[0]) return 0;
    slen = (int)strlen(slug);
    p = list;
    while (*p) {
        const char *start;
        int len;
        p = page_next_tok(p, &start, &len);
        if (len == slen && strncmp(start, slug, (size_t)slen) == 0) return 1;
    }
    return 0;
}

void boardpage_serialize(const char list[][PAGE_SLUG_CAP], int n,
                         char *out, int cap) {
    int i, oi = 0;
    if (cap <= 0) return;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        int len, need;
        if (strcmp(list[i], "/") == 0) continue;
        len  = (int)strlen(list[i]);
        need = (oi > 0 ? 1 : 0) + len;            /* sep + token */
        if (oi + need >= cap) break;              /* won't fit -> stop cleanly */
        if (oi > 0) out[oi++] = ' ';
        memcpy(out + oi, list[i], (size_t)len); oi += len;
    }
    out[oi] = '\0';
}

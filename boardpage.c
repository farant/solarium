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

/* page comparator: "/" sorts before everything, else plain strcmp. */
static int page_cmp(const char *a, const char *b) {
    int ra = (strcmp(a, "/") == 0);
    int rb = (strcmp(b, "/") == 0);
    if (ra != rb) return ra ? -1 : 1;
    return strcmp(a, b);
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

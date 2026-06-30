#include "browser.h"
#include "fuzzy.h"

void browser_reset(Browser *b) {
    int i;
    b->focus = 0; b->filtering = SOL_FALSE;
    for (i = 0; i < BROWSER_COLS; i++) { b->sel[i] = 0; b->filter[i][0] = '\0'; b->flen[i] = 0; }
}

void browser_clamp(Browser *b, const int counts[BROWSER_COLS]) {
    int i;
    for (i = 0; i < BROWSER_COLS; i++) {
        if (b->sel[i] < 0) b->sel[i] = 0;
        if (counts[i] <= 0) b->sel[i] = 0;
        else if (b->sel[i] > counts[i] - 1) b->sel[i] = counts[i] - 1;
    }
}

BrowserAction browser_key(Browser *b, BrowserKey k, const int counts[BROWSER_COLS]) {
    int f = b->focus;
    if (b->filtering) {
        switch (k) {
        case BROWSER_KEY_CANCEL:
            b->filtering = SOL_FALSE;
            b->filter[f][0] = '\0'; b->flen[f] = 0; b->sel[f] = 0; break;
        case BROWSER_KEY_ENTER:
            b->filtering = SOL_FALSE; break;
        case BROWSER_KEY_BACKSPACE:
            if (b->flen[f] > 0) { b->flen[f]--; b->filter[f][b->flen[f]] = '\0'; b->sel[f] = 0; }
            break;
        case BROWSER_KEY_UP:   if (b->sel[f] > 0) b->sel[f]--; break;
        case BROWSER_KEY_DOWN: if (counts[f] > 0 && b->sel[f] < counts[f] - 1) b->sel[f]++; break;
        default: break;
        }
        browser_clamp(b, counts);
        return BROWSER_NONE;
    }
    switch (k) {
    case BROWSER_KEY_CANCEL: return BROWSER_CLOSE;
    case BROWSER_KEY_LEFT:   if (b->focus > 0) b->focus--; break;
    case BROWSER_KEY_RIGHT:  if (b->focus < BROWSER_COLS - 1) b->focus++; break;
    case BROWSER_KEY_UP:     if (b->sel[f] > 0) b->sel[f]--; break;
    case BROWSER_KEY_DOWN:   if (counts[f] > 0 && b->sel[f] < counts[f] - 1) b->sel[f]++; break;
    case BROWSER_KEY_ENTER:
        if (b->focus < BROWSER_COLS - 1) { b->focus++; break; }
        return BROWSER_ACTIVATE;
    case BROWSER_KEY_FILTER: b->filtering = SOL_TRUE; break;
    default: break;
    }
    browser_clamp(b, counts);
    return BROWSER_NONE;
}

void browser_char(Browser *b, char c) {
    int f = b->focus;
    if (!b->filtering) return;
    if (c < 0x20 || c > 0x7e) return;
    if (b->flen[f] >= BROWSER_FILTER_CAP - 1) return;
    b->filter[f][b->flen[f]++] = c;
    b->filter[f][b->flen[f]] = '\0';
    b->sel[f] = 0;
}

int browser_rank(const char *filter, const char *const *names, int n, int *out, int cap) {
    int score[BROWSER_MAX_ITEMS];
    int i, j, cnt = 0;
    for (i = 0; i < n && cnt < cap && cnt < BROWSER_MAX_ITEMS; i++) {
        int sc;
        if (fuzzy_match(filter, names[i], &sc, (int *)0, 0)) { out[cnt] = i; score[cnt] = sc; cnt++; }
    }
    for (i = 1; i < cnt; i++) {
        int ti = out[i], ts = score[i];
        j = i - 1;
        while (j >= 0 && score[j] < ts) { out[j+1] = out[j]; score[j+1] = score[j]; j--; }
        out[j+1] = ti; score[j+1] = ts;
    }
    return cnt;
}

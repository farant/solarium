/* caret.c — see caret.h. Pure note-caret layout: no font, no GL, no scene. */

#include "caret.h"

int caret_cplen(unsigned char lead) {
    if (lead < 0x80u) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;                                  /* stray continuation: make progress */
}

int caret_reconcile(const char *src, const char *wrapped, int *out_src, int cap) {
    int si = 0, wi = 0;
    while (wrapped[wi] != '\0' && wi < cap) {
        if (wrapped[wi] == src[si]) {
            out_src[wi] = si;
            si++; wi++;
        } else {
            out_src[wi] = si;                  /* an inserted soft-break '\n' */
            if (wrapped[wi] == '\n')
                while (src[si] == ' ') si++;   /* consume the collapsed break-spaces */
            else if (src[si] != '\0')
                si++;                           /* defensive: stay in lockstep */
            wi++;
        }
    }
    return wi;
}

int caret_field_build(const char *src, const char *wrapped, const int *map,
                      const float *adv, int wlen, float line_h, float space_adv,
                      CaretField *out) {
    int   wi, line, srclen = 0, cur_src;
    float pen = 0.0f;
    while (src[srclen] != '\0') srclen++;
    out->slot_count = 0;
    out->line_count = 0;
    out->line_h     = line_h;
    /* open line 0 with a leading slot at x=0 */
    out->lines[0].slot0  = 0;
    out->lines[0].nslots = 0;
    out->lines[0].line   = 0;
    out->line_count      = 1;
    line = 0;
    cur_src = (wlen > 0) ? map[0] : srclen;
    out->slots[0].src = cur_src;
    out->slots[0].x   = 0.0f;
    out->slot_count   = 1;
    out->lines[0].nslots = 1;
    wi = 0;
    while (wi < wlen) {
        if (wrapped[wi] == '\n') {             /* close line; open the next */
            int next = (wi + 1 < wlen) ? map[wi + 1] : srclen;
            wi++;
            if (out->line_count >= CARET_MAX_LINES) break;
            line = out->line_count;
            out->lines[line].slot0  = out->slot_count;
            out->lines[line].nslots = 0;
            out->lines[line].line   = line;
            out->line_count++;
            pen = 0.0f;
            cur_src = next;
            if (out->slot_count < CARET_MAX_SLOTS) {
                out->slots[out->slot_count].src = next;
                out->slots[out->slot_count].x   = 0.0f;
                out->slot_count++;
                out->lines[line].nslots++;
            }
            continue;
        }
        {   /* one codepoint: advance the pen, emit a trailing slot */
            int n = caret_cplen((unsigned char)wrapped[wi]);
            int after = map[wi] + n;   /* the source byte just past this codepoint */
            pen += adv[wi];
            cur_src = after;
            if (out->slot_count < CARET_MAX_SLOTS) {
                out->slots[out->slot_count].src = after;
                out->slots[out->slot_count].x   = pen;
                out->slot_count++;
                out->lines[line].nslots++;
            }
            wi += n;
        }
    }
    /* trailing source bytes text_wrap dropped (spaces at a line/text end): give
       each a slot on the current line, advancing one space width, so the caret
       reflects typed trailing spaces even though they don't render. */
    while (cur_src < srclen) {
        pen += space_adv;
        if (out->slot_count < CARET_MAX_SLOTS) {
            out->slots[out->slot_count].src = cur_src + 1;
            out->slots[out->slot_count].x   = pen;
            out->slot_count++;
            out->lines[line].nslots++;
        }
        cur_src++;
    }
    return out->line_count;
}

int caret_slot_for_offset(const CaretField *cf, int cursor) {
    int i, best = -1, bd = 0, d;
    for (i = 0; i < cf->slot_count; i++)
        if (cf->slots[i].src == cursor) return i;
    for (i = 0; i < cf->slot_count; i++) {     /* nearest as a fallback */
        d = cf->slots[i].src - cursor; if (d < 0) d = -d;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

int caret_line_of_slot(const CaretField *cf, int slot) {
    int i;
    for (i = 0; i < cf->line_count; i++)
        if (slot >= cf->lines[i].slot0 &&
            slot <  cf->lines[i].slot0 + cf->lines[i].nslots) return i;
    return cf->line_count > 0 ? cf->line_count - 1 : 0;
}

int caret_slot_nearest_x(const CaretField *cf, int line, float goal_x) {
    int   i, s0, s1, best = -1;
    float bd = 0.0f, d;
    if (line < 0 || line >= cf->line_count) return -1;
    s0 = cf->lines[line].slot0;
    s1 = s0 + cf->lines[line].nslots;
    for (i = s0; i < s1; i++) {
        d = cf->slots[i].x - goal_x; if (d < 0.0f) d = -d;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    return best;
}

static int caret_class(unsigned char c) {
    if (c == ' ' || c == '\t' || c == '\n') return 0;          /* space */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c >= 0x80u) return 1;  /* word */
    return 2;                                                  /* other (punct) */
}

void caret_word_at(const char *src, int off, int *start, int *end) {
    int len = 0, cls, s, e;
    while (src[len] != '\0') len++;
    if (off < 0) off = 0;
    if (off > len) off = len;
    if (off >= len) {                       /* end of text: take the run ending here */
        if (len == 0) { *start = 0; *end = 0; return; }
        cls = caret_class((unsigned char)src[len - 1]);
        s = len;
        while (s > 0 && caret_class((unsigned char)src[s - 1]) == cls) s--;
        *start = s; *end = len; return;
    }
    cls = caret_class((unsigned char)src[off]);
    s = off; while (s > 0   && caret_class((unsigned char)src[s - 1]) == cls) s--;
    e = off; while (e < len && caret_class((unsigned char)src[e])     == cls) e++;
    *start = s; *end = e;
}

int caret_sel_spans(const CaretField *cf, int lo, int hi, CaretSpan *out, int cap) {
    int n = 0, slo, shi, llo, lhi, li;
    if (lo >= hi) return 0;
    slo = caret_slot_for_offset(cf, lo);
    shi = caret_slot_for_offset(cf, hi);
    if (slo < 0 || shi < 0) return 0;
    llo = caret_line_of_slot(cf, slo);
    lhi = caret_line_of_slot(cf, shi);
    for (li = llo; li <= lhi && n < cap; li++) {
        int   last = cf->lines[li].slot0 + cf->lines[li].nslots - 1;
        float x0   = (li == llo) ? cf->slots[slo].x : 0.0f;
        float x1   = (li == lhi) ? cf->slots[shi].x : cf->slots[last].x;
        out[n].line = li; out[n].x0 = x0; out[n].x1 = x1;
        n++;
    }
    return n;
}

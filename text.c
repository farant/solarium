/* text.c — the trivial text_shape implementation + ui_text (P3 item 3b).
   See text.h: this whole file below text_shape's signature is the part a
   future HarfBuzz + SheenBidi swap replaces; everything around it stays. */

#include "text.h"
#include "ui.h"

#include <string.h>

/* ----------------------------------------------------------------- UTF-8 */
/* Decode one UTF-8 sequence at p into *cp; return the byte after it. The
   encoding is self-synchronizing: lead bytes declare their length by their
   high bits (0xxxxxxx=1, 110=2, 1110=3, 11110=4) and continuation bytes are
   always 10xxxxxx. Anything malformed decodes as U+FFFD and consumes ONE
   byte, so bad input can never stall or run past the terminator. */
static const unsigned char *utf8_decode(const unsigned char *p, unsigned long *cp) {
    unsigned char b = p[0];
    unsigned long c;
    int           len, i;

    if (b < 0x80) { *cp = b; return p + 1; }            /* ASCII fast path */
    if      ((b & 0xE0) == 0xC0) { c = b & 0x1F; len = 1; }
    else if ((b & 0xF0) == 0xE0) { c = b & 0x0F; len = 2; }
    else if ((b & 0xF8) == 0xF0) { c = b & 0x07; len = 3; }
    else { *cp = 0xFFFD; return p + 1; }                /* stray continuation/invalid */

    for (i = 1; i <= len; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return p + 1; }
        c = (c << 6) | (unsigned long)(p[i] & 0x3F);
    }
    /* reject overlong encodings, the surrogate range, and codepoints past
       U+10FFFF (encodable but not Unicode) — strictness over silently
       divergent strings */
    if ((len == 1 && c < 0x80) || (len == 2 && c < 0x800) ||
        (len == 3 && c < 0x10000) ||
        (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFFul) {
        *cp = 0xFFFD;
        return p + 1;
    }
    *cp = c;
    return p + len + 1;
}

/* ------------------------------------------------------------- the seam */
int text_shape(const Font *f, const char *utf8, ShapedGlyph *out, int max) {
    const unsigned char *p = (const unsigned char *)utf8;
    float pen_x = 0.0f, pen_y = 0.0f;
    int   count = 0;
    int   prev  = 0;        /* previous glyph, for kerning; 0 = none */

    while (*p != '\0' && count < max) {
        unsigned long    cp;
        int              gi;
        const FontGlyph *g;

        p = utf8_decode(p, &cp);
        if (cp == '\n') {                       /* next baseline */
            pen_x = 0.0f;
            pen_y += font_line_height(f);
            prev = 0;                           /* no kerning across lines */
            continue;
        }
        gi = font_glyph_index(f, cp);
        if (gi == 0) { prev = 0; continue; }    /* font has no shape for it */
        g = font_glyph(f, gi);
        if (!g)      { prev = 0; continue; }    /* not baked (outside ranges) */

        if (prev != 0) pen_x += font_kern(f, prev, gi);
        out[count].glyph = gi;
        out[count].x     = pen_x;
        out[count].y     = pen_y;
        count++;
        pen_x += g->advance;
        prev = gi;
    }
    return count;
}

/* ------------------------------------------------------------- drawing */
#define TEXT_MAX_GLYPHS 1024    /* per ui_text call; longer strings truncate */

void text_measure(const Font *f, const char *utf8, float scale,
                  float *out_w, float *out_h) {
    ShapedGlyph shaped[TEXT_MAX_GLYPHS];
    int         n, i;
    float       w = 0.0f, max_y = 0.0f;

    n = text_shape(f, utf8, shaped, TEXT_MAX_GLYPHS);
    for (i = 0; i < n; i++) {
        const FontGlyph *g = font_glyph(f, shaped[i].glyph);
        float e = shaped[i].x + (g ? g->advance : 0.0f);
        if (e > w) w = e;
        if (shaped[i].y > max_y) max_y = shaped[i].y;
    }
    if (out_w) *out_w = w * scale;
    if (out_h) *out_h = (max_y + font_line_height(f)) * scale;
}

void ui_text(const Font *f, const char *utf8, float x, float y, float scale,
             float r, float g, float b, float a) {
    ShapedGlyph shaped[TEXT_MAX_GLYPHS];
    int         n, i;

    if (!f) return;
    n = text_shape(f, utf8, shaped, TEXT_MAX_GLYPHS);
    for (i = 0; i < n; i++) {
        const FontGlyph *gl = font_glyph(f, shaped[i].glyph);
        if (!gl || gl->w <= 0.0f) continue;     /* ink-less (space): advance only */
        ui_glyph_quad(font_atlas(f),
                      x + (shaped[i].x + gl->xoff) * scale,
                      y + (shaped[i].y + gl->yoff) * scale,
                      gl->w * scale, gl->h * scale,
                      gl->u0, gl->v0, gl->u1, gl->v1,
                      r, g, b, a);
    }
}

/* ------------------------------------------------------------- wrapping */
/* Layout sits ABOVE the seam: wrapping is a string -> string transform
   (inserting '\n' at greedy break points), measured per word through
   text_measure, then drawn by the ordinary ui_text. No glyph access of its
   own — the HarfBuzz swap doesn't touch this. */
#define WRAP_BUF 2048
#define WORD_BUF 256

int text_wrap(const Font *f, const char *utf8, float scale, float max_width,
              char *out, int cap) {
    char        word[WORD_BUF];
    const char *p = utf8;
    size_t      oi = 0;
    float       line_w, space_w, limit;
    int         lines = 1;

    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    if (!f || !utf8 || max_width <= 0.0f || scale <= 0.0f) return 0;
    text_measure(f, " ", 1.0f, &space_w, (float *)0);
    limit  = max_width / scale;        /* compare in base-size units */
    line_w = 0.0f;

    while (*p != '\0' && oi + WORD_BUF + 2 < (size_t)cap) {
        size_t wl = 0;
        float  ww;
        int    nsp, k;

        if (*p == '\n') {                       /* explicit break passes through */
            out[oi++] = '\n';
            lines++;
            line_w = 0.0f;
            p++;
            continue;
        }
        nsp = 0;
        while (*p == ' ') { p++; nsp++; }        /* count this run of spaces */
        if (*p == '\0') break;                   /* trailing spaces at end of text: dropped */
        if (*p == '\n') {                         /* a run before a hard '\n': keep it */
            for (k = 0; k < nsp && oi + 2 < (size_t)cap; k++) out[oi++] = ' ';
            continue;
        }

        while (*p != '\0' && *p != ' ' && *p != '\n' && wl < WORD_BUF - 1)
            word[wl++] = *p++;
        word[wl] = '\0';
        text_measure(f, word, 1.0f, &ww, (float *)0);

        if (line_w > 0.0f && line_w + (float)nsp * space_w + ww > limit) {
            out[oi++] = '\n';                   /* the word wraps; the space run is dropped */
            lines++;
            line_w = 0.0f;
        } else {                                 /* preserve the run of spaces (leading or interior) */
            for (k = 0; k < nsp && oi + 2 < (size_t)cap; k++) {
                out[oi++] = ' ';
                line_w += space_w;
            }
        }
        /* a single word wider than the limit gets its own line, unbroken */
        memcpy(out + oi, word, wl);
        oi += wl;
        line_w += ww;
    }
    out[oi] = '\0';
    return lines;
}

int ui_text_wrapped(const Font *f, const char *utf8, float x, float y,
                    float scale, float max_width,
                    float r, float g, float b, float a) {
    char out[WRAP_BUF];
    int  lines = text_wrap(f, utf8, scale, max_width, out, WRAP_BUF);
    if (lines > 0) ui_text(f, out, x, y, scale, r, g, b, a);
    return lines;
}

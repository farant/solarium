/* text.h — text shaping + drawing (P3 item 3b). Strict C89, above every seam.

   THE LOAD-BEARING SEAM (§1.6): text_shape is the single function that turns
   a UTF-8 string into positioned glyphs — {glyph index, pen x, pen y}. The
   implementation here is the trivial one (decode codepoint -> glyph index ->
   advance + kerning; LTR only, no shaping, no BiDi). Complex scripts
   (Hebrew/Arabic/Indic — a WHEN, not an if) CANNOT be bolted onto an
   advance-width loop: the whole string->glyphs stage gets REPLACED, by
   HarfBuzz + SheenBidi, swapping ONLY this function. Its output shape is
   deliberately what HarfBuzz natively emits (glyph indices + positions), so
   the two implementations are drop-in interchangeable. Layout and rendering
   must reach glyphs ONLY through text_shape. */
#ifndef TEXT_H
#define TEXT_H

#include "font.h"

typedef struct {
    int   glyph;     /* glyph index into the font (atlas key) */
    float x, y;      /* pen position, px at the font's base size; y is the
                        baseline (0 = first line), +y down */
} ShapedGlyph;

/* Shape utf8 into out[0..max). Returns the glyph count. '\n' advances to the
   next baseline. Codepoints the font lacks or hasn't baked are skipped (no
   advance) — honest until a fallback/tofu policy exists. Invalid UTF-8
   decodes as U+FFFD and never stalls. */
int text_shape(const Font *f, const char *utf8, ShapedGlyph *out, int max);

/* Draw a line (or '\n'-separated lines) of text. x,y position the FIRST
   BASELINE (not the top-left: descenders hang below, ascenders rise above —
   add font_ascent()*scale to a top edge to get the baseline). scale is a
   factor on the font's base pixel size; SDF keeps any scale crisp. */
void ui_text(const Font *f, const char *utf8, float x, float y, float scale,
             float r, float g, float b, float a);

#endif /* TEXT_H */

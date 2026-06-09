/* font.h — SDF glyph atlas (P3 item 3a). Wraps the quarantined stb_truetype
   TU (font.c) behind a clean interface, like image.h wraps stb_image.

   A Font owns: the TTF bytes, one R8 atlas texture holding every baked
   glyph's signed distance field (0.5 = the contour; the text shader
   thresholds it, which is why edges stay crisp at any zoom), and a CPU
   table of per-glyph placement data.

   THE TABLE IS KEYED BY GLYPH INDEX, NEVER BY CODEPOINT — load-bearing for
   the future HarfBuzz swap (shapers emit indices; ligatures and contextual
   forms have no codepoint). codepoint->index goes through font_glyph_index
   (the trivial cmap lookup) until a real shaper replaces that step.

   All pixel metrics are at the font's BASE pixel size; SDF decode makes
   scaling them linear and lossless. */
#ifndef FONT_H
#define FONT_H

#include "rhi.h"   /* RhiTexture (rhi.h carries sol_base) */

/* one baked glyph: its atlas rect + how to place its quad (y-down, screen
   convention: yoff is from the BASELINE to the quad top, negative = above) */
typedef struct {
    int   glyph;            /* glyph index */
    float u0, v0, u1, v1;   /* atlas rect */
    float xoff, yoff;       /* pen (on the baseline) -> quad top-left, px */
    float w, h;             /* quad size, px (0 for ink-less glyphs: space) */
    float advance;          /* pen advance, px */
} FontGlyph;

typedef struct Font Font;   /* opaque; owned by font_load/font_destroy */

/* Load a TTF and bake the SDF atlas (ASCII + Greek ranges; the packer can
   append more later). pixel_size is the base rasterization height. NULL on
   failure (missing file, bad font, atlas upload failed). Needs rhi_init. */
Font *font_load(const char *path, float pixel_size);
void  font_destroy(Font *f);

RhiTexture       font_atlas(const Font *f);
int              font_atlas_size(const Font *f);                 /* px, square */
const FontGlyph *font_glyph(const Font *f, int glyph_index);     /* NULL if not baked */
int              font_glyph_index(const Font *f, unsigned long codepoint);  /* 0 = missing */

float font_ascent(const Font *f);        /* baseline below the line top, px */
float font_line_height(const Font *f);   /* ascent - descent + gap, px */
float font_kern(const Font *f, int g1, int g2);   /* pair adjustment, px */

#endif /* FONT_H */

/* font.c — the second quarantined translation unit: it pulls in stb_truetype
   (the sanctioned font-rasterization dependency, extending §1.3's stb
   dispensation from image decode to fonts) and wraps it behind the clean
   font.h. EXCLUDED from build.sh c89check, exactly like image.c.

   Baking: each glyph is rasterized ONCE as a signed distance field
   (stbtt_GetGlyphSDF) at the base pixel size and shelf-packed into one R8
   atlas. Distance, not coverage, is what survives bilinear filtering — the
   text shader thresholds at 0.5 to reconstruct the contour per screen pixel,
   so the same atlas is crisp at every zoom. */

#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_truetype config + warning quarantine (it is not our code to lint) */
#define STB_TRUETYPE_IMPLEMENTATION
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcomment"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif

#include "vendor/stb_truetype.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#define ATLAS_SIZE   1024   /* px, square; R8 -> 1MB */
#define SDF_PADDING  5      /* px of distance ramp around each glyph */
#define SDF_ONEDGE   128    /* the byte value meaning "exactly on the contour" */
#define ATLAS_GUTTER 1      /* px between packed glyphs (bleed protection) */

struct Font {
    unsigned char  *ttf;        /* the file bytes; stb reads them lazily */
    stbtt_fontinfo  info;
    float           scale;      /* font units -> px at the base size */
    float           ascent, descent, linegap;   /* px */
    RhiTexture      atlas;
    FontGlyph      *glyphs;     /* sorted by glyph index for bsearch */
    int             glyph_count;
};

static unsigned char *slurp(const char *path) {
    FILE          *f;
    long           size;
    unsigned char *buf;
    size_t         got;

    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { free(buf); return NULL; }
    return buf;
}

/* the baked ranges: ASCII + Greek and Coptic (per the item-3 scope; the
   shelf packer state would let a dynamic atlas append more later) */
typedef struct { unsigned long first, last; } CpRange;
static const CpRange BAKE_RANGES[] = {
    { 0x0020, 0x007E },     /* printable ASCII */
    { 0x0370, 0x03FF }      /* Greek and Coptic */
};
#define BAKE_RANGE_COUNT (sizeof(BAKE_RANGES) / sizeof(BAKE_RANGES[0]))

static int glyph_cmp(const void *a, const void *b) {
    const FontGlyph *ga = (const FontGlyph *)a;
    const FontGlyph *gb = (const FontGlyph *)b;
    return (ga->glyph > gb->glyph) - (ga->glyph < gb->glyph);
}

Font *font_load(const char *path, float pixel_size) {
    Font          *f;
    unsigned char *atlas_px;
    int            pen_x, pen_y, row_h;
    int            cap;
    sol_u32        ri;

    f = (Font *)calloc(1, sizeof(Font));
    if (!f) return NULL;

    f->ttf = slurp(path);
    if (!f->ttf) { free(f); return NULL; }
    if (!stbtt_InitFont(&f->info, f->ttf, stbtt_GetFontOffsetForIndex(f->ttf, 0))) {
        free(f->ttf); free(f);
        return NULL;
    }

    f->scale = stbtt_ScaleForPixelHeight(&f->info, pixel_size);
    {
        int a, d, g;
        stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
        f->ascent  = (float)a * f->scale;
        f->descent = (float)d * f->scale;       /* negative (below baseline) */
        f->linegap = (float)g * f->scale;
    }

    atlas_px = (unsigned char *)calloc(1, (size_t)ATLAS_SIZE * ATLAS_SIZE);
    if (!atlas_px) { free(f->ttf); free(f); return NULL; }

    cap   = 256;
    f->glyphs = (FontGlyph *)malloc((size_t)cap * sizeof(FontGlyph));
    if (!f->glyphs) { free(atlas_px); free(f->ttf); free(f); return NULL; }

    pen_x = pen_y = 0;
    row_h = 0;

    for (ri = 0; ri < BAKE_RANGE_COUNT; ri++) {
        unsigned long cp;
        for (cp = BAKE_RANGES[ri].first; cp <= BAKE_RANGES[ri].last; cp++) {
            int            gi, adv, lsb, gw, gh, gx, gy, j;
            unsigned char *sdf;
            FontGlyph     *out;

            gi = stbtt_FindGlyphIndex(&f->info, (int)cp);
            if (gi == 0) continue;                       /* font has no shape for it */
            for (j = 0; j < f->glyph_count; j++) {       /* dedup: many cp, one glyph */
                if (f->glyphs[j].glyph == gi) break;
            }
            if (j < f->glyph_count) continue;

            if (f->glyph_count == cap) {
                FontGlyph *grown;
                cap *= 2;
                grown = (FontGlyph *)realloc(f->glyphs, (size_t)cap * sizeof(FontGlyph));
                if (!grown) continue;                    /* keep what we have */
                f->glyphs = grown;
            }
            out = &f->glyphs[f->glyph_count];
            memset(out, 0, sizeof *out);
            out->glyph = gi;

            stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
            out->advance = (float)adv * f->scale;

            /* the SDF bitmap: padded by SDF_PADDING px of distance ramp,
               value SDF_ONEDGE on the contour, falling to 0 at the padding
               edge (pixel_dist_scale = onedge/padding). NULL = no ink
               (space): advance-only glyph. */
            sdf = stbtt_GetGlyphSDF(&f->info, f->scale, gi, SDF_PADDING,
                                    (unsigned char)SDF_ONEDGE,
                                    (float)SDF_ONEDGE / (float)SDF_PADDING,
                                    &gw, &gh, &gx, &gy);
            if (sdf) {
                int   row;
                if (pen_x + gw + ATLAS_GUTTER > ATLAS_SIZE) {   /* shelf packing */
                    pen_x  = 0;
                    pen_y += row_h + ATLAS_GUTTER;
                    row_h  = 0;
                }
                if (pen_y + gh + ATLAS_GUTTER > ATLAS_SIZE) {
                    fprintf(stderr, "font: atlas full, dropping glyphs from U+%04lX\n", cp);
                    stbtt_FreeSDF(sdf, NULL);
                    break;
                }
                for (row = 0; row < gh; row++) {
                    memcpy(atlas_px + (size_t)(pen_y + row) * ATLAS_SIZE + pen_x,
                           sdf + (size_t)row * gw, (size_t)gw);
                }
                stbtt_FreeSDF(sdf, NULL);

                out->u0 = (float)pen_x / (float)ATLAS_SIZE;
                out->v0 = (float)pen_y / (float)ATLAS_SIZE;
                out->u1 = (float)(pen_x + gw) / (float)ATLAS_SIZE;
                out->v1 = (float)(pen_y + gh) / (float)ATLAS_SIZE;
                out->xoff = (float)gx;       /* stb already speaks y-down screen space */
                out->yoff = (float)gy;       /* baseline -> bitmap top (negative above) */
                out->w    = (float)gw;
                out->h    = (float)gh;

                pen_x += gw + ATLAS_GUTTER;
                if (gh > row_h) row_h = gh;
            }
            f->glyph_count++;
        }
    }

    qsort(f->glyphs, (size_t)f->glyph_count, sizeof(FontGlyph), glyph_cmp);

    f->atlas = rhi_create_texture(atlas_px, ATLAS_SIZE, ATLAS_SIZE, RHI_TEX_R8);
    free(atlas_px);
    if (!f->atlas.id) { font_destroy(f); return NULL; }
    return f;
}

void font_destroy(Font *f) {
    if (!f) return;
    if (f->atlas.id) rhi_destroy_texture(f->atlas);
    free(f->glyphs);
    free(f->ttf);
    free(f);
}

RhiTexture font_atlas(const Font *f)     { return f->atlas; }
int        font_atlas_size(const Font *f) { (void)f; return ATLAS_SIZE; }

const FontGlyph *font_glyph(const Font *f, int glyph_index) {
    FontGlyph key;
    key.glyph = glyph_index;
    return (const FontGlyph *)bsearch(&key, f->glyphs, (size_t)f->glyph_count,
                                      sizeof(FontGlyph), glyph_cmp);
}

int font_glyph_index(const Font *f, unsigned long codepoint) {
    return stbtt_FindGlyphIndex(&f->info, (int)codepoint);
}

float font_ascent(const Font *f)      { return f->ascent; }
float font_line_height(const Font *f) { return f->ascent - f->descent + f->linegap; }

float font_kern(const Font *f, int g1, int g2) {
    return (float)stbtt_GetGlyphKernAdvance(&f->info, g1, g2) * f->scale;
}

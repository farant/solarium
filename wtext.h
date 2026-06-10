/* wtext.h — world-space SDF text (P3 item 8). The same atlas and the same
   text_shape seam as the screen overlay, ridden through model-view-projection
   instead of pixels->NDC. This is the SDF payoff: the atlas stores DISTANCE,
   and the fragment shader's fwidth threshold keeps the anti-aliasing band
   ~one display pixel wide at ANY projected scale — so one 48px bake serves
   a 14px HUD label and a note card you walk right up to.

   Calls must run inside an open 3D pass (the HDR scene pass): the text is
   INK — unlit flat color, alpha-blended, depth-tested against the geometry
   already drawn, riding through ACES like everything else. */
#ifndef WTEXT_H
#define WTEXT_H

#include "font.h"
#include "sol_types.h"

sol_bool wtext_init(void);      /* needs rhi_init; builds pipeline + buffer */
void     wtext_shutdown(void);

/* One text block on the z=0 plane of `model` (a world transform), in that
   plane's LOCAL meters, y-up: (x, top_y) is the block's top-left corner.
   px_to_m converts the font's pixel units to meters (line height in m =
   font_line_height * px_to_m). wrap_w_m > 0 word-wraps to that width.
   Shapes, uploads, and draws immediately — a few blocks per frame is the
   intended scale (notes + card labels), not a text engine. */
void wtext_block(const Font *f, mat4 viewproj, mat4 model, const char *utf8,
                 float x, float top_y, float px_to_m, float wrap_w_m,
                 float r, float g, float b);

/* The BENT block (item 9: text riding the turning leaf). The bend maps a
   text-plane x to a point on a curve in the model's XZ section — (bx, bz)
   with unit tangent (tx, tz) — bending the plane around axes parallel to
   text-Y (columns stay straight). Each glyph quad's left and right edges
   are evaluated on the curve, so glyphs are piecewise-flat chords: small
   relative to any sane curl radius, and the SDF threshold keeps them
   crisp through the whole motion. `lift` displaces along the curve's 2D
   normal (-tz, tx): the side of the surface the text sits on. No wrap —
   bent text is pre-paginated. */
typedef void (*WtextBend)(float x, void *user, float *bx, float *bz,
                          float *tx, float *tz);
void wtext_block_bent(const Font *f, mat4 viewproj, mat4 model,
                      const char *utf8, float x, float top_y, float px_to_m,
                      WtextBend bend, void *user, float lift,
                      float r, float g, float b);

#endif /* WTEXT_H */

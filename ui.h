/* ui.h — immediate-mode 2D overlay (P3 item 2). No widget objects: every
   frame the caller re-declares the overlay as draw calls between ui_begin and
   ui_end; vertices accumulate in a transient batch that ui_end uploads and
   draws once, in a no-clear ("load") pass over the tonemapped frame.

   Colors are DISPLAY-REFERRED sRGB, written as given — no lighting, no
   tonemap, no gamma encode — and blended in sRGB space (the desktop-UI
   convention, chosen deliberately over linear-light correctness).

   Coordinates are FRAMEBUFFER PIXELS (retina pixels, not window points),
   origin top-left, y-down. ui_vw/ui_vh convert viewport-relative percentages
   (CSS vw/vh) to pixels; container-relative % and pt/em wait for a layout
   system and text (items 3+). Call order is paint order (painter's
   algorithm): later calls draw on top.

   Above the seam: talks only to rhi.h, never GL. */
#ifndef UI_H
#define UI_H

#include "sol_base.h"
#include "rhi.h"

/* Once, after rhi_init: creates the UI pipeline, white texture, and the
   per-frame stream buffer. SOL_FALSE if a GPU resource failed. */
sol_bool ui_init(void);
void     ui_shutdown(void);

/* Per frame: open the batch for a screen of the given framebuffer-pixel size. */
void ui_begin(int screen_w, int screen_h);

/* Viewport-relative units: percent of the current frame's width/height in
   pixels. Composable at the call site (mix freely with pixel values), the
   axis explicit in the name — a 10vw x 10vw quad IS square, unlike CSS %. */
float ui_vw(float pct);
float ui_vh(float pct);

/* A filled axis-aligned rectangle. */
void ui_quad(float x, float y, float w, float h,
             float r, float g, float b, float a);

/* The rectangle's border only: four thin quads of thickness t (inset). */
void ui_quad_outline(float x, float y, float w, float h, float t,
                     float r, float g, float b, float a);

/* A line segment of thickness t — built as a rotated quad, NOT GL lines
   (macOS core profile caps glLineWidth at 1.0). */
void ui_line(float x0, float y0, float x1, float y1, float t,
             float r, float g, float b, float a);

/* A textured rectangle, tinted white. Expects display-ready texel values
   (linear-format RGBA8 holding sRGB-encoded data, like a UI atlas): the UI
   pass does no color-space conversion, so a RHI_TEX_SRGB8 scene texture
   (sampled to linear) will come out dark here. */
void ui_textured_quad(RhiTexture tex, float x, float y, float w, float h);

/* One SDF glyph quad with explicit UVs, drawn through the TEXT pipeline
   (the smoothstep threshold decode — see ui.c). text.c's ui_text is the
   intended caller; the batch breaks between geometry and text spans. */
void ui_glyph_quad(RhiTexture atlas, float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1,
                   float r, float g, float b, float a);

/* Upload the batch and draw it (one buffer update; one draw per texture
   change — untextured quads share the internal 1x1 white texture, so a
   typical overlay is a single draw). Safe to call with an empty batch. */
void ui_end(void);

#endif /* UI_H */

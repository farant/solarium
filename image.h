/* image.h — minimal image loading (clean interface; stb_image is hidden in
   image.c so callers stay strict-C89). Produces an 8-bit RGBA buffer above the
   seam, handed to rhi_create_texture. See SCENE_FORMAT / TODO2 §1.3 (amended). */
#ifndef IMAGE_H
#define IMAGE_H

#include "sol_base.h"

typedef struct {
    unsigned char *pixels;   /* RGBA8, top row last (flipped for GL on load) */
    int            w, h;
} Image;

/* A float (HDR) image: linear RGBA radiance, values may exceed 1.0. Used for
   equirectangular .hdr environment maps (skybox / IBL). NOT sRGB — already
   linear, so it goes to an RHI_TEX_RGBA16F texture, never SRGB8. */
typedef struct {
    float *pixels;
    int    w, h;
} HdrImage;

sol_bool image_load(const char *path, Image *out);                          /* from a file */
sol_bool image_load_from_memory(const unsigned char *data, int len, Image *out);  /* embedded bytes */
void     image_free(Image *img);

/* Pixel dimensions of an image file via a cheap header-only read (no full
   decode). Returns false if the file can't be inspected. */
sol_bool image_dims(const char *path, int *out_w, int *out_h);

sol_bool image_load_hdr(const char *path, HdrImage *out);   /* Radiance .hdr -> linear float */
void     image_hdr_free(HdrImage *img);

/* Largest (w,h) in METERS that fits inside (field_w x field_h) at the source
   image's aspect ratio (letterbox: one axis meets the field, the other is
   smaller). Any non-positive input yields *out_w = *out_h = 0. Pure math —
   no stb, no GL. */
void image_fit_box(int src_w, int src_h, float field_w, float field_h,
                   float *out_w, float *out_h);

#endif /* IMAGE_H */

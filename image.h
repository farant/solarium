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

sol_bool image_load(const char *path, Image *out);   /* SOL_FALSE on failure */
void     image_free(Image *img);

#endif /* IMAGE_H */

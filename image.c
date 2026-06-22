/* image.c — the one quarantined translation unit: it pulls in stb_image (the
   sanctioned image-decode dependency, §1.3 amended) and wraps it behind the
   clean image.h. EXCLUDED from build.sh c89check — stb is not held to our
   strict-C89 standard, just as GLFW's headers aren't. */

#include "image.h"

/* stb_image config + warning quarantine (it is not our code to lint) */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
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

#include "vendor/stb_image.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

sol_bool image_load(const char *path, Image *out) {
    int            w, h, ch;
    unsigned char *data;

    stbi_set_flip_vertically_on_load(1);            /* GL wants row 0 at the bottom */
    data = stbi_load(path, &w, &h, &ch, 4);         /* force 4 channels -> RGBA8 */
    if (!data) return SOL_FALSE;

    out->pixels = data;
    out->w = w;
    out->h = h;
    return SOL_TRUE;
}

sol_bool image_load_from_memory(const unsigned char *data, int len, Image *out) {
    int            w, h, ch;
    unsigned char *px;

    stbi_set_flip_vertically_on_load(1);
    px = stbi_load_from_memory(data, len, &w, &h, &ch, 4);   /* force RGBA8 */
    if (!px) return SOL_FALSE;

    out->pixels = px;
    out->w = w;
    out->h = h;
    return SOL_TRUE;
}

void image_free(Image *img) {
    if (img->pixels) {
        stbi_image_free(img->pixels);
        img->pixels = (unsigned char *)0;
    }
}

sol_bool image_load_hdr(const char *path, HdrImage *out) {
    int    w, h, ch;
    float *data;

    /* No flip: equirect orientation is resolved in the skybox shader by how the
       view direction maps to UV, not by the row order on upload. stbi_loadf
       decodes Radiance RGBE into LINEAR float radiance (values may exceed 1.0). */
    stbi_set_flip_vertically_on_load(0);
    data = stbi_loadf(path, &w, &h, &ch, 4);        /* force 4 channels -> RGBA float */
    if (!data) return SOL_FALSE;

    out->pixels = data;
    out->w = w;
    out->h = h;
    return SOL_TRUE;
}

void image_hdr_free(HdrImage *img) {
    if (img->pixels) {
        stbi_image_free(img->pixels);
        img->pixels = (float *)0;
    }
}

void image_fit_box(int src_w, int src_h, float field_w, float field_h,
                   float *out_w, float *out_h) {
    float aspect;
    *out_w = 0.0f;
    *out_h = 0.0f;
    if (src_w <= 0 || src_h <= 0 || field_w <= 0.0f || field_h <= 0.0f) return;
    aspect = (float)src_w / (float)src_h;          /* width per height */
    if (field_w / field_h > aspect) {              /* field wider than image: height-bound */
        *out_h = field_h;
        *out_w = field_h * aspect;
    } else {                                       /* width-bound */
        *out_w = field_w;
        *out_h = field_w / aspect;
    }
}

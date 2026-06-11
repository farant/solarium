/* wav.c — see wav.h. C89. */

#include "wav.h"

#include <stdio.h>

static void put_u16(unsigned char *b, unsigned int v) {
    b[0] = (unsigned char)(v & 0xFFu);
    b[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_u32(unsigned char *b, unsigned long v) {
    b[0] = (unsigned char)(v & 0xFFu);
    b[1] = (unsigned char)((v >> 8) & 0xFFu);
    b[2] = (unsigned char)((v >> 16) & 0xFFu);
    b[3] = (unsigned char)((v >> 24) & 0xFFu);
}

sol_bool wav_write_pcm16(const char *path, const float *samples,
                         int frames, int rate) {
    /* the canonical 44-byte header: RIFF [size] WAVE, fmt chunk (PCM,
       mono, 16-bit), data chunk */
    unsigned char h[44];
    unsigned long data_bytes = (unsigned long)frames * 2ul;
    FILE *f;
    int   i, ok;

    h[0]='R'; h[1]='I'; h[2]='F'; h[3]='F';
    put_u32(h + 4, 36ul + data_bytes);
    h[8]='W'; h[9]='A'; h[10]='V'; h[11]='E';
    h[12]='f'; h[13]='m'; h[14]='t'; h[15]=' ';
    put_u32(h + 16, 16ul);                       /* fmt chunk size */
    put_u16(h + 20, 1u);                         /* PCM */
    put_u16(h + 22, 1u);                         /* mono */
    put_u32(h + 24, (unsigned long)rate);
    put_u32(h + 28, (unsigned long)rate * 2ul);  /* byte rate */
    put_u16(h + 32, 2u);                         /* block align */
    put_u16(h + 34, 16u);                        /* bits per sample */
    h[36]='d'; h[37]='a'; h[38]='t'; h[39]='a';
    put_u32(h + 40, data_bytes);

    f = fopen(path, "wb");
    if (f == NULL) return SOL_FALSE;
    ok = fwrite(h, 1, 44, f) == 44;
    for (i = 0; ok && i < frames; i++) {
        float v = samples[i];
        long  q;
        unsigned char s[2];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        q = (long)(v * 32767.0f);
        put_u16(s, (unsigned int)(q & 0xFFFFl));
        ok = fwrite(s, 1, 2, f) == 2;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) remove(path);                       /* never leave a half-file */
    return ok ? SOL_TRUE : SOL_FALSE;
}

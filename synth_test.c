/* synth_test.c — headless checks for the synthesizer (P4 item 8 piece 1):
   sound is the most testable signal there is — determinism is memcmp,
   pitch is zero-crossing counting, envelopes are windowed RMS, the 8-bit
   knobs leave countable fingerprints. Ends by rendering every preset to
   a .wav (the audition: `afplay blip.wav`) and round-tripping the writer's
   bytes. `build.sh synthtest`. */

#include "synth.h"
#include "wav.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

static float buf [SYNTH_RATE * 3];
static float buf2[SYNTH_RATE * 3];

/* a plain 440 Hz sine, one second, every other knob off */
static void base_params(float *p) {
    int i;
    for (i = 0; i < SYNTH_PARAMS; i++) p[i] = 0.0f;
    p[0]  = 3.0f;      /* sine */
    p[2]  = 1.0f;      /* sustain 1s */
    p[5]  = 440.0f;
    p[10] = 1.0f;      /* arpm neutral */
    p[12] = 0.5f;
    p[18] = 1.0f;      /* gain */
}

static int zero_crossings(const float *s, int from, int to) {
    int i, n = 0;
    for (i = from + 1; i < to; i++) {
        if ((s[i - 1] < 0.0f && s[i] >= 0.0f) ||
            (s[i - 1] >= 0.0f && s[i] < 0.0f)) n++;
    }
    return n;
}

static float rms(const float *s, int from, int to) {
    double acc = 0.0;
    int    i;
    for (i = from; i < to; i++) acc += (double)s[i] * (double)s[i];
    return (float)sqrt(acc / (double)(to - from));
}

int main(void) {
    float p[SYNTH_PARAMS];
    int   n, n2, i;

    /* the schema is introspectable — TODO5's synth book will build its
       pages from exactly these calls */
    if (synth_param_count() != SYNTH_PARAMS ||
        strcmp(synth_param_names()[0], "wave") != 0 ||
        strcmp(synth_param_names()[19], "jitter") != 0) {
        printf("FAIL: schema introspection\n"); return 1;
    }
    if (synth_preset("blip") == (const float *)0 ||
        synth_preset("no-such-sound") != (const float *)0 ||
        synth_preset_count() < 5) {
        printf("FAIL: preset lookup\n"); return 1;
    }
    printf("schema + presets (%d knobs, %d sounds): ok\n",
           synth_param_count(), synth_preset_count());

    /* determinism: same params + same seed = bit-identical */
    n  = synth_render(synth_preset("step"), 7u, buf,  SYNTH_RATE * 3);
    n2 = synth_render(synth_preset("step"), 7u, buf2, SYNTH_RATE * 3);
    if (n != n2 || memcmp(buf, buf2, (size_t)n * sizeof(float)) != 0) {
        printf("FAIL: same seed must render the same bytes\n"); return 1;
    }
    /* jitter: a different seed is a different footstep */
    n2 = synth_render(synth_preset("step"), 8u, buf2, SYNTH_RATE * 3);
    if (n == n2 && memcmp(buf, buf2, (size_t)n * sizeof(float)) == 0) {
        printf("FAIL: jitter must vary across seeds\n"); return 1;
    }
    /* jitter 0: the seed must NOT matter (pure waves; noise still may) */
    base_params(p);
    n  = synth_render(p, 1u, buf,  SYNTH_RATE * 3);
    n2 = synth_render(p, 99u, buf2, SYNTH_RATE * 3);
    if (n != n2 || memcmp(buf, buf2, (size_t)n * sizeof(float)) != 0) {
        printf("FAIL: jitter 0 must ignore the seed\n"); return 1;
    }
    printf("determinism + seed jitter: ok\n");

    /* duration is the envelope's arithmetic */
    base_params(p);
    p[1] = 0.1f; p[2] = 0.2f; p[4] = 0.3f;
    n = synth_render(p, 1u, buf, SYNTH_RATE * 3);
    if (n != (int)(0.6f * SYNTH_RATE)) {
        printf("FAIL: duration must be attack+sustain+decay (%d)\n", n);
        return 1;
    }

    /* pitch: a 440 Hz sine crosses zero ~880 times a second */
    base_params(p);
    n = synth_render(p, 1u, buf, SYNTH_RATE * 3);
    i = zero_crossings(buf, 0, n);
    if (i < 850 || i > 910) {
        printf("FAIL: 440 Hz sine has %d crossings (want ~880)\n", i);
        return 1;
    }
    printf("duration + pitch via crossings: ok\n");

    /* envelope: both ends are quiet relative to the loud middle (the
       attack->decay junction at t=0.3s is the peak) */
    base_params(p);
    p[1] = 0.3f; p[2] = 0.0f; p[4] = 0.2f;
    n = synth_render(p, 1u, buf, SYNTH_RATE * 3);
    {
        int   pk   = (int)(0.3f * SYNTH_RATE);
        float peak = rms(buf, pk - n / 25, pk + n / 25);
        if (rms(buf, 0, n / 20) >= peak * 0.3f) {
            printf("FAIL: attack must ramp up from quiet\n"); return 1;
        }
        if (rms(buf, n - n / 20, n) >= peak * 0.3f) {
            printf("FAIL: decay must fade out\n"); return 1;
        }
    }
    printf("envelope shape via windowed RMS: ok\n");

    /* the arpeggio's jump: crossing RATE steps up by ~arpm */
    {
        const float *blip = synth_preset("blip");
        float before, after;
        n = synth_render(blip, 1u, buf, SYNTH_RATE * 3);
        before = (float)zero_crossings(buf, 0, (int)(0.04f * SYNTH_RATE))
                 / 0.04f;
        after  = (float)zero_crossings(buf, (int)(0.06f * SYNTH_RATE),
                                       (int)(0.10f * SYNTH_RATE)) / 0.04f;
        if (after / before < 1.3f || after / before > 1.7f) {
            printf("FAIL: arpeggio ratio %.2f (want ~1.5)\n",
                   (double)(after / before));
            return 1;
        }
    }

    /* the low-pass is "muffled", measurably: a 6 kHz sine through a
       300 Hz one-pole loses most of its energy */
    {
        float full, cut;
        base_params(p);
        p[5] = 6000.0f;
        n = synth_render(p, 1u, buf, SYNTH_RATE * 3);
        full = rms(buf, 0, n);
        p[14] = 300.0f;
        n = synth_render(p, 1u, buf, SYNTH_RATE * 3);
        cut = rms(buf, 0, n);
        if (cut > full * 0.3f) {
            printf("FAIL: low-pass barely attenuated (%.3f vs %.3f)\n",
                   (double)cut, (double)full);
            return 1;
        }
    }
    printf("arpeggio + low-pass: ok\n");

    /* the 8-bit knobs leave fingerprints: crush 3 = at most 17 distinct
       amplitudes; decim 50 = runs of >= 50 identical samples */
    {
        float seen[64];
        int   count = 0, runs_ok = 1, run = 1;
        base_params(p);
        p[16] = 3.0f;
        n = synth_render(p, 1u, buf, SYNTH_RATE * 3);
        for (i = 0; i < n; i++) {
            int k, found = 0;
            for (k = 0; k < count; k++) {
                if (buf[i] == seen[k]) { found = 1; break; }
            }
            if (!found && count < 64) seen[count++] = buf[i];
        }
        if (count > 17) {
            printf("FAIL: crush 3 produced %d distinct levels\n", count);
            return 1;
        }
        base_params(p);
        p[17] = 50.0f;
        n = synth_render(p, 1u, buf, SYNTH_RATE * 3);
        for (i = 1; i < n; i++) {
            if (buf[i] == buf[i - 1]) run++;
            else { if (run < 50 && i > 50 && i < n - 50) runs_ok = 0; run = 1; }
        }
        if (!runs_ok) { printf("FAIL: decim 50 must hold values\n"); return 1; }
        printf("crush levels + decim holds: ok\n");
    }

    /* a loop is seamless by construction: the first sample continues the
       last (a raw render of the same sine would jump at the wrap) */
    {
        float step;
        base_params(p);
        p[5] = 220.0f;
        n = synth_render_loop(p, 1u, buf, SYNTH_RATE * 3, SYNTH_RATE / 4);
        if (n != SYNTH_RATE - SYNTH_RATE / 4) {
            printf("FAIL: loop length must be frames - blend\n"); return 1;
        }
        step = fabsf(buf[1] - buf[0]) + 1e-4f;     /* one sample's worth */
        if (fabsf(buf[0] - buf[n - 1]) > step * 2.0f) {
            printf("FAIL: loop seam jumps (%.5f vs step %.5f)\n",
                   (double)fabsf(buf[0] - buf[n - 1]), (double)step);
            return 1;
        }
        printf("loop blend seam continuity: ok\n");
    }

    /* every preset renders sane: nonzero, audible, clamped, no NaN */
    for (i = 0; i < synth_preset_count(); i++) {
        const char  *nm = synth_preset_name(i);
        const float *pp = synth_preset(nm);
        int j;
        n = synth_render(pp, 7u, buf, SYNTH_RATE * 3);
        if (n <= 0 || rms(buf, 0, n) < 0.005f) {
            printf("FAIL: preset %s is silent\n", nm); return 1;
        }
        for (j = 0; j < n; j++) {
            if (!(buf[j] >= -1.0f && buf[j] <= 1.0f)) {   /* also catches NaN */
                printf("FAIL: preset %s out of range at %d\n", nm, j);
                return 1;
            }
        }
    }
    printf("all presets render in range: ok\n");

    /* the WAV writer round-trips its own bytes */
    {
        unsigned char h[44];
        FILE *f;
        for (i = 0; i < 100; i++) buf[i] = (float)i / 100.0f;
        if (!wav_write_pcm16("synth_test_out.wav", buf, 100, SYNTH_RATE)) {
            printf("FAIL: wav write\n"); return 1;
        }
        f = fopen("synth_test_out.wav", "rb");
        if (!f || fread(h, 1, 44, f) != 44) {
            printf("FAIL: wav reopen\n"); if (f) fclose(f); return 1;
        }
        if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0 ||
            h[24] + (h[25] << 8) + ((long)h[26] << 16) != SYNTH_RATE ||
            h[40] != 200 || h[41] != 0) {                 /* 100 frames * 2 */
            printf("FAIL: wav header fields\n"); fclose(f); return 1;
        }
        {
            unsigned char s[2];
            long          q;
            fseek(f, 44 + 50 * 2, SEEK_SET);              /* sample 50 = 0.5 */
            if (fread(s, 1, 2, f) != 2) { printf("FAIL: wav data\n"); fclose(f); return 1; }
            q = s[0] + ((long)s[1] << 8);
            if (q < 16000 || q > 16800) {
                printf("FAIL: wav sample value %ld (want ~16384)\n", q);
                fclose(f); return 1;
            }
        }
        fclose(f);
        remove("synth_test_out.wav");
        printf("wav writer round-trip: ok\n");
    }

    /* THE AUDITION: every preset to disk — this is the verify step that
       needs ears, not asserts */
    for (i = 0; i < synth_preset_count(); i++) {
        const char *nm = synth_preset_name(i);
        char path[64];
        n = synth_render(synth_preset(nm), 7u, buf, SYNTH_RATE * 3);
        snprintf(path, sizeof path, "%s.wav", nm);
        if (!wav_write_pcm16(path, buf, n, SYNTH_RATE)) {
            printf("FAIL: could not export %s\n", path); return 1;
        }
        printf("  exported %-12s (%.2fs) — afplay %s\n",
               path, (double)n / (double)SYNTH_RATE, path);
    }

    printf("synth_test: ALL OK\n");
    return 0;
}

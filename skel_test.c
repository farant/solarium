/* skel_test.c — headless checks for skeletal animation (P4 item 9 piece 1):
   hierarchy composition against hand-computed matrices, THE BIND-POSE
   IDENTITY INVARIANT (posed at the bind pose, every palette entry is the
   identity — any convention error anywhere breaks it loudly), channel
   sampling (lerp/slerp/step/clamp/wrap), and the real files: RiggedSimple
   (2 joints — skinning's hello world) and Fox (24 joints, 3 named clips).
   File sections SKIP loudly if the gitignored glbs are absent.
   `build.sh skeltest`. */

#include "skel.h"
#include "glb.h"
#include "sol_math.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

/* link-level quarantine: glb.c touches the GPU at two seams (textures,
   mesh upload). Skeleton loading never EXECUTES them, but the linker
   wants the symbols — so the test provides inert ones (the asset_test
   injected-fake spirit, applied at link time). */
Mesh mesh_from_builder(MeshBuilder *b) {
    Mesh m;
    (void)b;
    memset(&m, 0, sizeof m);
    return m;
}
Mesh mesh_from_skinned(const sol_f32 *verts, sol_u32 vert_count,
                       const sol_u32 *indices, sol_u32 index_count) {
    Mesh m;
    (void)verts; (void)vert_count; (void)indices; (void)index_count;
    memset(&m, 0, sizeof m);
    return m;
}
RhiTexture rhi_create_texture(const void *p, int w, int h, RhiTextureFormat f) {
    RhiTexture t;
    (void)p; (void)w; (void)h; (void)f;
    t.id = 0;
    return t;
}

static int feq(float a, float b, float eps) {
    return fabsf(a - b) < eps;
}

static float mat_identity_err(const mat4 *m) {
    mat4  id = mat4_identity();
    float worst = 0.0f;
    int   i;
    for (i = 0; i < 16; i++) {
        float d = fabsf(m->m[i] - id.m[i]);
        if (d > worst) worst = d;
    }
    return worst;
}

static void simple_skel(Skeleton *sk) {
    memset(sk, 0, sizeof *sk);
    sk->joint_count = 2;
    sk->parent[0] = -1; sk->parent[1] = 0;
    sk->order[0]  = 0;  sk->order[1]  = 1;
    sk->rest_t[0] = vec3_make(1.0f, 0.0f, 0.0f);
    sk->rest_t[1] = vec3_make(0.0f, 2.0f, 0.0f);
    sk->rest_r[0] = quat_identity();
    sk->rest_r[1] = quat_identity();
    sk->rest_s[0] = vec3_make(1.0f, 1.0f, 1.0f);
    sk->rest_s[1] = vec3_make(1.0f, 1.0f, 1.0f);
    sk->root_pre[0] = mat4_identity();
    sk->root_pre[1] = mat4_identity();
    sk->inverse_bind[0] = mat4_identity();
    sk->inverse_bind[1] = mat4_identity();
}

int main(void) {
    Skeleton sk;
    vec3     lt[SKEL_MAX_JOINTS], ls[SKEL_MAX_JOINTS];
    quat     lr[SKEL_MAX_JOINTS];
    mat4     pal[SKEL_MAX_JOINTS];

    /* quat_from_mat4 inverts quat_to_mat4 (up to sign), including a
       near-180-degree rotation — the branch the naive formula fumbles */
    {
        quat qs[2], rt;
        int  i;
        qs[0] = quat_from_axis_angle(vec3_normalize(vec3_make(1, 2, 3)), 1.1f);
        qs[1] = quat_from_axis_angle(vec3_make(0, 1, 0), 3.10f);
        for (i = 0; i < 2; i++) {
            rt = quat_from_mat4(quat_to_mat4(qs[i]));
            if (rt.w * qs[i].w < 0.0f) {       /* same rotation, other sign */
                rt.x = -rt.x; rt.y = -rt.y; rt.z = -rt.z; rt.w = -rt.w;
            }
            if (!feq(rt.x, qs[i].x, 1e-5f) || !feq(rt.y, qs[i].y, 1e-5f) ||
                !feq(rt.z, qs[i].z, 1e-5f) || !feq(rt.w, qs[i].w, 1e-5f)) {
                printf("FAIL: quat_from_mat4 round-trip (case %d)\n", i);
                return 1;
            }
        }
        printf("quat_from_mat4 round-trip: ok\n");
    }

    /* hierarchy composition: child world = parent's translate + own */
    simple_skel(&sk);
    skel_pose(&sk, (const SkelClip *)0, 0.0f, SOL_FALSE, lt, lr, ls);
    skel_palette(&sk, lt, lr, ls, pal);
    if (!feq(pal[1].m[12], 1.0f, 1e-5f) || !feq(pal[1].m[13], 2.0f, 1e-5f)) {
        printf("FAIL: chain translation (got %.3f %.3f)\n",
               (double)pal[1].m[12], (double)pal[1].m[13]);
        return 1;
    }

    /* a rotated root carries its child: 90deg about Z swings the child's
       (0,2,0) offset to (-2,0,0), so the child sits at (-1,0,0) */
    sk.rest_r[0] = quat_from_axis_angle(vec3_make(0, 0, 1), 1.57079633f);
    skel_pose(&sk, (const SkelClip *)0, 0.0f, SOL_FALSE, lt, lr, ls);
    skel_palette(&sk, lt, lr, ls, pal);
    if (!feq(pal[1].m[12], -1.0f, 1e-4f) || !feq(pal[1].m[13], 0.0f, 1e-4f)) {
        printf("FAIL: rotated chain (got %.3f %.3f)\n",
               (double)pal[1].m[12], (double)pal[1].m[13]);
        return 1;
    }
    printf("hierarchy composition: ok\n");

    /* THE INVARIANT: with inverse binds = the bind worlds' inverses,
       posing AT the bind pose yields identity palettes — vertices unmoved */
    simple_skel(&sk);
    sk.inverse_bind[0] = mat4_translate(vec3_make(-1.0f, 0.0f, 0.0f));
    sk.inverse_bind[1] = mat4_translate(vec3_make(-1.0f, -2.0f, 0.0f));
    skel_pose(&sk, (const SkelClip *)0, 0.0f, SOL_FALSE, lt, lr, ls);
    skel_palette(&sk, lt, lr, ls, pal);
    if (mat_identity_err(&pal[0]) > 1e-5f || mat_identity_err(&pal[1]) > 1e-5f) {
        printf("FAIL: bind pose must yield identity palettes\n");
        return 1;
    }
    printf("bind-pose identity invariant: ok\n");

    /* channel sampling: lerp midpoint, end clamps, STEP hold, loop wrap */
    {
        static float times[3]  = { 0.0f, 1.0f, 2.0f };
        static float vals[9]   = { 0,0,0,  10,0,0,  10,20,0 };
        SkelChannel  ch;
        SkelClip     clip;
        ch.joint = 1; ch.path = SKEL_PATH_T; ch.interp = SKEL_INTERP_LINEAR;
        ch.key_count = 3; ch.times = times; ch.values = vals;
        clip.name = (char *)"t"; clip.duration = 2.0f;
        clip.channels = &ch; clip.channel_count = 1;

        skel_pose(&sk, &clip, 0.5f, SOL_FALSE, lt, lr, ls);
        if (!feq(lt[1].x, 5.0f, 1e-5f)) {
            printf("FAIL: lerp midpoint (got %.3f)\n", (double)lt[1].x);
            return 1;
        }
        skel_pose(&sk, &clip, -3.0f, SOL_FALSE, lt, lr, ls);
        if (!feq(lt[1].x, 0.0f, 1e-5f)) { printf("FAIL: clamp before\n"); return 1; }
        skel_pose(&sk, &clip, 9.0f, SOL_FALSE, lt, lr, ls);
        if (!feq(lt[1].y, 20.0f, 1e-5f)) { printf("FAIL: clamp after\n"); return 1; }
        skel_pose(&sk, &clip, 2.5f, SOL_TRUE, lt, lr, ls);   /* wraps to 0.5 */
        if (!feq(lt[1].x, 5.0f, 1e-5f)) { printf("FAIL: loop wrap\n"); return 1; }
        ch.interp = SKEL_INTERP_STEP;
        skel_pose(&sk, &clip, 0.9f, SOL_FALSE, lt, lr, ls);
        if (!feq(lt[1].x, 0.0f, 1e-5f)) { printf("FAIL: STEP must hold\n"); return 1; }
        /* an un-animated property keeps the REST value */
        if (!feq(lt[0].x, 1.0f, 1e-5f)) {
            printf("FAIL: untargeted joints keep the rest pose\n"); return 1;
        }
    }

    /* rotation channels SLERP: identity -> 90Z sampled at half is 45deg */
    {
        static float times[2] = { 0.0f, 1.0f };
        static float vals[8]  = { 0,0,0,1,  0,0,0.70710678f,0.70710678f };
        SkelChannel  ch;
        SkelClip     clip;
        ch.joint = 0; ch.path = SKEL_PATH_R; ch.interp = SKEL_INTERP_LINEAR;
        ch.key_count = 2; ch.times = times; ch.values = vals;
        clip.name = (char *)"r"; clip.duration = 1.0f;
        clip.channels = &ch; clip.channel_count = 1;
        skel_pose(&sk, &clip, 0.5f, SOL_FALSE, lt, lr, ls);
        if (!feq(lr[0].z, 0.38268343f, 1e-4f) ||
            !feq(lr[0].w, 0.92387953f, 1e-4f)) {
            printf("FAIL: slerp midpoint (got z=%.5f w=%.5f)\n",
                   (double)lr[0].z, (double)lr[0].w);
            return 1;
        }
    }
    printf("channel sampling (lerp/clamp/wrap/step/slerp/rest): ok\n");

    /* ---- the real files (gitignored; skip loudly if absent) ---- */
    {
        SkelData sd;
        FILE    *probe = fopen("RiggedSimple.glb", "rb");
        if (probe) {
            fclose(probe);
            if (!glb_load_skeleton("RiggedSimple.glb", &sd)) {
                printf("FAIL: RiggedSimple should load\n"); return 1;
            }
            if (sd.skel.joint_count != 2 || sd.clip_count != 1 ||
                sd.skel.parent[0] != -1 || sd.skel.parent[1] != 0 ||
                sd.clips[0].duration <= 0.0f) {
                printf("FAIL: RiggedSimple shape (joints %d clips %d)\n",
                       sd.skel.joint_count, sd.clip_count);
                return 1;
            }
            /* RiggedSimple's REST pose is not its BIND pose (legal glTF —
               the bind lives only in the IBMs; this Blender export's node
               rests differ by a 90-degree turn). So no identity assert
               here; what IS true: its palettes are pure rotations+offsets,
               so every column must stay unit-length and finite — at rest
               AND mid-clip — and the two poses must differ. */
            {
                mat4  rest_pal[SKEL_MAX_JOINTS];
                int   j, c, pass;
                float diff = 0.0f;
                for (pass = 0; pass < 2; pass++) {
                    skel_pose(&sd.skel, pass ? &sd.clips[0] : (const SkelClip *)0,
                              pass ? sd.clips[0].duration * 0.5f : 0.0f,
                              SOL_TRUE, lt, lr, ls);
                    skel_palette(&sd.skel, lt, lr, ls, pal);
                    for (j = 0; j < sd.skel.joint_count; j++) {
                        if (pass == 0) rest_pal[j] = pal[j];
                        for (c = 0; c < 3; c++) {
                            float *col = pal[j].m + c * 4;
                            float  len = sqrtf(col[0]*col[0] + col[1]*col[1]
                                               + col[2]*col[2]);
                            if (!(len > 0.85f && len < 1.15f)) {
                                printf("FAIL: palette column %d not ~unit "
                                       "(%.4f, joint %d pass %d)\n", c,
                                       (double)len, j, pass);
                                return 1;
                            }
                        }
                    }
                }
                for (j = 0; j < sd.skel.joint_count; j++) {   /* the clip
                                                       animates SOME joint */
                    for (c = 0; c < 16; c++) {
                        float d2 = fabsf(pal[j].m[c] - rest_pal[j].m[c]);
                        if (d2 > diff) diff = d2;
                    }
                }
                if (diff < 1e-4f) {
                    printf("FAIL: mid-clip pose must differ from rest\n");
                    return 1;
                }
            }
            skel_data_free(&sd);
            printf("RiggedSimple (2 joints, 1 clip, sane palettes): ok\n");
        } else {
            printf("SKIPPED: RiggedSimple.glb not found\n");
        }

        probe = fopen("Fox.glb", "rb");
        if (probe) {
            fclose(probe);
            if (!glb_load_skeleton("Fox.glb", &sd)) {
                printf("FAIL: Fox should load\n"); return 1;
            }
            if (sd.skel.joint_count != 24 || sd.clip_count != 3 ||
                strcmp(sd.clips[0].name, "Survey") != 0 ||
                strcmp(sd.clips[1].name, "Walk") != 0 ||
                strcmp(sd.clips[2].name, "Run") != 0) {
                printf("FAIL: Fox shape (joints %d clips %d '%s')\n",
                       sd.skel.joint_count, sd.clip_count,
                       sd.clip_count ? sd.clips[0].name : "?");
                return 1;
            }
            {
                int a;
                for (a = 0; a < 3; a++) {
                    if (sd.clips[a].channel_count <= 0 ||
                        sd.clips[a].duration <= 0.0f) {
                        printf("FAIL: Fox clip %d empty\n", a); return 1;
                    }
                }
            }
            /* Fox's rest pose IS its bind pose (the common case) — so the
               identity invariant holds on real data here; this is the
               asset-specific regression check for the whole import path */
            skel_pose(&sd.skel, (const SkelClip *)0, 0.0f, SOL_FALSE, lt, lr, ls);
            skel_palette(&sd.skel, lt, lr, ls, pal);
            {
                int   j;
                float worst = 0.0f;
                for (j = 0; j < sd.skel.joint_count; j++) {
                    float e = mat_identity_err(&pal[j]);
                    if (e > worst) worst = e;
                }
                printf("Fox bind-pose identity err: %.6f (24 joints)\n",
                       (double)worst);
                if (worst > 1e-2f) {
                    printf("FAIL: Fox bind pose must be ~identity\n");
                    return 1;
                }
            }
            skel_data_free(&sd);
            printf("Fox (24 joints, Survey/Walk/Run): ok\n");
        } else {
            printf("SKIPPED: Fox.glb not found\n");
        }
    }

    printf("skel_test: ALL OK\n");
    return 0;
}

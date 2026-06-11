/* skel.c — see skel.h. Pure CPU; no GL, no I/O. C89. */

#include "skel.h"
#include "sol_math.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* sample one channel at t into out[3] or out[4]: clamp outside the key
   range, bracket inside it, hold (STEP) or interpolate (LINEAR) */
static void chan_sample(const SkelChannel *ch, float t, float *out) {
    int nc = (ch->path == SKEL_PATH_R) ? 4 : 3;
    int i, k, c;
    if (ch->key_count <= 0) return;
    if (t <= ch->times[0]) {
        for (c = 0; c < nc; c++) out[c] = ch->values[c];
        return;
    }
    if (t >= ch->times[ch->key_count - 1]) {
        const float *v = ch->values + (size_t)(ch->key_count - 1) * nc;
        for (c = 0; c < nc; c++) out[c] = v[c];
        return;
    }
    k = 0;
    for (i = 1; i < ch->key_count; i++) {
        if (t < ch->times[i]) { k = i - 1; break; }
    }
    {
        const float *va = ch->values + (size_t)k * nc;
        const float *vb = va + nc;
        float span = ch->times[k + 1] - ch->times[k];
        float u    = span > 0.0f ? (t - ch->times[k]) / span : 0.0f;
        if (ch->interp == SKEL_INTERP_STEP) u = 0.0f;
        if (ch->path == SKEL_PATH_R) {
            quat a, b, q;
            a.x = va[0]; a.y = va[1]; a.z = va[2]; a.w = va[3];
            b.x = vb[0]; b.y = vb[1]; b.z = vb[2]; b.w = vb[3];
            q = quat_slerp(a, b, u);
            out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
        } else {
            for (c = 0; c < nc; c++) out[c] = va[c] + (vb[c] - va[c]) * u;
        }
    }
}

void skel_pose(const Skeleton *sk, const SkelClip *clip, float t,
               sol_bool loop, vec3 *out_t, quat *out_r, vec3 *out_s) {
    int i;
    memcpy(out_t, sk->rest_t, (size_t)sk->joint_count * sizeof(vec3));
    memcpy(out_r, sk->rest_r, (size_t)sk->joint_count * sizeof(quat));
    memcpy(out_s, sk->rest_s, (size_t)sk->joint_count * sizeof(vec3));
    if (clip == NULL) return;
    if (loop && clip->duration > 0.0f) {
        t = fmodf(t, clip->duration);
        if (t < 0.0f) t += clip->duration;
    }
    for (i = 0; i < clip->channel_count; i++) {
        const SkelChannel *ch = &clip->channels[i];
        float v[4];
        if (ch->joint < 0 || ch->joint >= sk->joint_count) continue;
        chan_sample(ch, t, v);
        switch (ch->path) {
        case SKEL_PATH_T:
            out_t[ch->joint] = vec3_make(v[0], v[1], v[2]);
            break;
        case SKEL_PATH_R:
            out_r[ch->joint].x = v[0];
            out_r[ch->joint].y = v[1];
            out_r[ch->joint].z = v[2];
            out_r[ch->joint].w = v[3];
            break;
        default:
            out_s[ch->joint] = vec3_make(v[0], v[1], v[2]);
            break;
        }
    }
}

void skel_palette(const Skeleton *sk, const vec3 *lt, const quat *lr,
                  const vec3 *ls, mat4 *out) {
    mat4 world[SKEL_MAX_JOINTS];
    int  k, j;
    for (k = 0; k < sk->joint_count; k++) {
        mat4 local;
        j     = sk->order[k];
        local = mat4_from_trs(lt[j], lr[j], ls[j]);
        if (sk->parent[j] < 0) {
            world[j] = mat4_mul(sk->root_pre[j], local);
        } else {
            world[j] = mat4_mul(world[sk->parent[j]], local);
        }
    }
    for (j = 0; j < sk->joint_count; j++) {
        out[j] = mat4_mul(world[j], sk->inverse_bind[j]);
    }
}

void skel_data_free(SkelData *sd) {
    int i, c;
    for (i = 0; i < sd->clip_count; i++) {
        SkelClip *cl = &sd->clips[i];
        for (c = 0; c < cl->channel_count; c++) {
            free(cl->channels[c].times);
            free(cl->channels[c].values);
        }
        free(cl->channels);
        free(cl->name);
    }
    free(sd->clips);
    sd->clips = (SkelClip *)0;
    sd->clip_count = 0;
}

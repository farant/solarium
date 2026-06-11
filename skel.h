/* skel.h — skeletal animation, the CPU half (P4 item 9): the skeleton as
   COMPACT ASSET DATA, not scene objects (the approved reserved decision —
   a rig is geometry-adjacent; the scene graph stays clean and the file
   stores arrangement, never armatures). A joint is a local TRS + a parent
   index: scene 2.3's parent-chain walk over arrays. The one new idea is
   the INVERSE BIND matrix — "subtract where the joint was at bind time,
   add where it is now":

       palette[j] = jointWorld[j] * inverseBind[j]

   which yields THE testable invariant: posed at the bind pose, every
   palette entry is the identity and the mesh comes out exactly unmoved.

   The pose is VIEW STATE (§1.6): sampled fresh every frame from absolute
   t, never persisted — the file stores which clip and what speed, never a
   frame of the dance. Pure C89; no GL, no I/O (glb.c parses files INTO
   these structs). Headless-tested as `build.sh skeltest`. */

#ifndef SKEL_H
#define SKEL_H

#include "sol_base.h"
#include "sol_types.h"

/* GL 3.3 guarantees exactly 1024 vertex-uniform components = 64 mat4s;
   the cap keeps us inside the spec's letter (a game rig is 30-60). */
#define SKEL_MAX_JOINTS 64

typedef struct {
    int  joint_count;
    int  parent[SKEL_MAX_JOINTS];        /* -1 = root */
    int  order[SKEL_MAX_JOINTS];         /* parents-before-children walk
                                            order, computed at import (the
                                            bvh-refit lesson: sort once,
                                            sweep forever) */
    vec3 rest_t[SKEL_MAX_JOINTS];        /* the rest pose: what channels
                                            DON'T animate keeps these */
    quat rest_r[SKEL_MAX_JOINTS];
    vec3 rest_s[SKEL_MAX_JOINTS];
    mat4 root_pre[SKEL_MAX_JOINTS];      /* static non-joint ANCESTOR
                                            transforms folded per root
                                            (identity elsewhere): glTF skins
                                            compose in scene space */
    mat4 inverse_bind[SKEL_MAX_JOINTS];
} Skeleton;

enum { SKEL_PATH_T = 0, SKEL_PATH_R = 1, SKEL_PATH_S = 2 };
enum { SKEL_INTERP_LINEAR = 0, SKEL_INTERP_STEP = 1 };

/* one animated property of one joint: parallel time/value key arrays
   (owned, heap) — glTF's sampler, resolved and joint-remapped */
typedef struct {
    int    joint;
    int    path;          /* SKEL_PATH_* */
    int    interp;        /* SKEL_INTERP_* */
    int    key_count;
    float *times;
    float *values;        /* key_count * (4 for R, 3 for T/S) */
} SkelChannel;

typedef struct {
    char        *name;     /* owned; "clip0"-style if the file had none */
    float        duration; /* max keyframe time across channels */
    SkelChannel *channels;
    int          channel_count;
} SkelClip;

/* one skin + its clips: what glb_load_skeleton fills, the asset payload */
typedef struct {
    Skeleton  skel;
    SkelClip *clips;
    int       clip_count;
} SkelData;

/* sample a clip at time t into local-pose arrays (each SKEL_MAX_JOINTS
   long), starting from the rest pose — channels override their targets.
   clip == NULL = the rest pose. loop wraps t by the clip's duration;
   otherwise the ends clamp. LINEAR lerps vectors and SLERPS rotations
   (the book-flight slerp, cashing its item-9 promise); STEP holds. */
void skel_pose(const Skeleton *sk, const SkelClip *clip, float t,
               sol_bool loop, vec3 *out_t, quat *out_r, vec3 *out_s);

/* compose local TRS through the parent chain (order[]), then multiply
   the inverse binds: out[j] = world[j] * inverse_bind[j] — the palette
   the vertex shader consumes. */
void skel_palette(const Skeleton *sk, const vec3 *lt, const quat *lr,
                  const vec3 *ls, mat4 *out);

void skel_data_free(SkelData *sd);

#endif /* SKEL_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>              /* strcmp — room-shell refs (item 7) */
#include <math.h>                /* cosf — spot-light cone cosines (item 9a) */
#include <time.h>                /* time — seeds the codex mint (P3 item 9) */

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>          /* platform: window, input, time — not GL */

#include "rhi.h"                 /* the graphics seam — no GL above here */
#include "mesh.h"
#include "gothic.h"              /* the kit: church_plan + queries (P6) */
#include "flora.h"               /* trees: the canopy + species (P7) */
#include "rock.h"                /* boulders + the pebble unit (P7 item 6) */
#include "texgen.h"              /* synthesized material maps (texture side-quest) */
#include "scene.h"
#include "sol_math.h"
#include "camera.h"
#include "image.h"
#include "glb.h"
#include "command.h"             /* the shared command registry (palette) */
#include "palette.h"             /* the command palette overlay (Task 3) */
#include "ui.h"                  /* the 2D overlay (P3 item 2) */
#include "mirror.h"              /* mirror rooms reflect real folders (P3 item 6) */
#include "font.h"                /* the SDF glyph atlas (P3 item 3) */
#include "text.h"                /* text_shape seam + ui_text (P3 item 3b) */
#include "wtext.h"               /* world-space SDF text — note cards (P3 item 8) */
#include "platform_fs.h"         /* fs_read_file — the reader's pages (P3 item 9) */
#include "platform_clipboard.h"  /* clipboard_read_image — Cmd+V paste (clipboard-paste-images) */
#include "nid.h"                 /* nid_generate — library/<nid>.png filenames */
#include "collide.h"             /* the world's lateral push-back (P4 item 1) */
#include "route.h"
#include "editor.h"              /* the top-down spatial-tree editor (Task 2-4) */
#include "descend.h"             /* fs-tree descent: plant a folder as a door */
#include "workspace.h"           /* active-workspace view filter (Portals) */
#include "bvh.h"                 /* the spatial index (P4 item 2) */
#include "asset.h"               /* refcounted ownership for shared assets (P4 item 4) */
#include "component.h"           /* behavior as data — the overlay doctrine (P4 item 6) */
#include "particles.h"           /* the pool: runtime-only ephemera (P4 item 7) */
#include "synth.h"               /* sounds minted from params (P4 item 8) */
#include "mixer.h"               /* the pure mixing core the callback shares */
#include "platform_audio.h"      /* the fifth quarantine: CoreAudio + the ring */
#include "stml.h"                /* sounds.stml: the ear's hot-reloadable knobs */
#include "furniture.h"           /* scene-free furniture catalog + meshes (Furniture & Filing) */
#include "inventory.h"           /* scene-free inventory grid layout math (the bag) */
#include "widget.h"              /* immediate-mode widget core (TODO5 app books) */
#include "app_synth.h"           /* the synth book's page layout (TODO5) */
#include "boardpage.h"           /* page slugs + board page-list (Board Pages) */
#include "multiselect.h"         /* board multi-select set ops (Board Multi-Select) */
#include "caret.h"               /* caret layout (pure; font glue = caret_build below) */

/* glb models come through the registry (P4 item 4 piece 3) — defined with
   the stores below; forward-declared because the import layer sits above
   them in this file */
static sol_bool glb_acquire_model(const char *path, GlbModel *out);
static void     glb_part_key(const char *path, int index, char *buf);

/* one island's grass (P4 item 3): a static instance buffer + the island it
   grows on — drawn only when the island itself survives the frustum. P7
   item 7 adds a sparse FLOWER buffer (a second bloom mesh, bright tints,
   patchy) — the meadow grown up. */
typedef struct {
    RhiBuffer data;
    int       count;
    RhiBuffer flowers;
    int       flower_count;
    sol_u32   island;
} MeadowPatch;

/* one instanced-ornament population (P6 item 10, generalized to KINDS
   in P7 item 4): a static instance buffer of local slots riding a
   source object's transform + visibility bit (the meadow arrangement).
   kind selects the unit mesh AND the material: balusters wear the
   source's stone; leaf clusters wear the canopy green carried here. */
enum { ORN_BALUSTER = 0, ORN_LEAF_BROAD, ORN_LEAF_CONIFER, ORN_KIND_COUNT };
typedef struct {
    RhiBuffer data;
    int       count;
    sol_u32   source;
    int       kind;
    Material  material;   /* used when kind != ORN_BALUSTER */
} OrnamentPatch;

/* the FIELD forest (P7 item 5): the meadow law promoted to whole trees.
   Derived per island from its seed; drawn through the ornament PBR
   pipeline — each variant's WOOD instanced across its placements, every
   tree's CANOPY clusters merged per leaf-kind. ~6 draws for a wood. A
   forest is not scene objects; it is shared variant meshes + per-island
   instance buffers, the meadow at organism scale. */
#define FOREST_VARIANT_COUNT 6   /* 0-4 trees, 5 = the shrub (item 7) */
typedef struct {
    sol_u32   island;
    RhiBuffer wood[FOREST_VARIANT_COUNT];
    int       wood_count[FOREST_VARIANT_COUNT];
    RhiBuffer canopy[2];        /* [0] = broadleaf, [1] = conifer */
    int       canopy_count[2];
    RhiBuffer scree;            /* FIELD scree (item 6): pebbles, ghost */
    int       scree_count;
    Material  wood_mat[FOREST_VARIANT_COUNT];   /* bark per variant */
} ForestPatch;

#define LOOK_SPEED        1.5f     /* radians/sec for keyboard look           */
#define MOUSE_SENSITIVITY 0.0025f  /* radians per pixel; NOT dt-scaled        */

#define SHADOW_MAP_SIZE   2048     /* one cascade's depth-map resolution (item 9b/P8.6) */
/* The sun (P8 item 6): a DIRECTIONAL light with cascaded shadow maps. No
   position/cone/falloff — parallel rays, an orthographic box per cascade,
   each fitted to a slice of the camera frustum (near = tight + crisp, far =
   broad). light_pos/light_target now encode only a DIRECTION. */
#define SHADOW_CASCADES   2        /* reserved decision #4: 2 (measure up to 3) */
#define SHADOW_DIST       60.0f    /* sun shadows fitted out to here (camera far = 100) */
#define SHADOW_CASTER_PAD 40.0f    /* world units pulled toward the sun so tall/behind
                                      geometry still casts into the slice's box */
/* a shadow-casting SPOT sconce (P8 item 7): candle-light that throws pier
   shadows. ONE designated caster (opt-in via meta cast=1), a single perspective
   depth map — the spot path Item 6 retired from the sun, re-homed on a lamp. */
#define SPOT_SHADOW_SIZE  1024     /* one room, not the island — smaller than the sun's 2048 */
#define SPOT_SHADOW_NEAR  0.3f
#define SPOT_SHADOW_FAR   18.0f
#define SPOT_SWAY_AMP     0.035f   /* metres the flame dances → the shadows sway */

/* P8 items 2 & 3: ONE fog medium — the atmospheric haze (item 2) and the
   god-ray in-scatter density (item 3) read the same field, so shafts and
   haze can never disagree. Eyeball-tuned constants (no hot reload yet). */
#define FOG_DENSITY       0.012f
#define FOG_HEIGHT        0.0f
#define FOG_FALLOFF       0.035f
#define GODRAY_INTENSITY  3.0f     /* shaft brightness; 0 = no god-rays. Halved for
                                      P8.6: the directional sun scatters island-wide,
                                      not in a 20m cone, so the same number reads brighter */

#define ENV_CUBE_SIZE   1024   /* per-face resolution of the environment cubemap */
#define IRRADIANCE_SIZE 32     /* irradiance is very low-frequency: tiny is plenty */
#define PREFILTER_SIZE  128    /* specular prefilter base (mip 0 = sharpest reflection) */
#define PREFILTER_MIPS  5      /* roughness levels 0..1 across mips 0..4 */
#define BRDF_LUT_SIZE   512    /* BRDF integration LUT (NoV x roughness) */

/* --- shaders: GLSL source handed to the backend (still app-authored) --- */
/* Shader sources are DATA the app hands down through the unchanged
   rhi_create_shader — the language is selected at compile time with the
   backend (item 10's approved decision: twins live HERE, beside their GLSL,
   so the pair stays in sync under one comment). -DSOL_RHI_METAL picks MSL.

   MSL conventions (rhi_metal.m's contract): entry points vmain/fmain;
   vertex stream = buffer(0), instance stream = buffer(1), the vertex
   uniform struct = buffer(2), the fragment uniform struct = buffer(0) of
   the FRAGMENT namespace; textures/samplers use the same slot numbers
   rhi_bind_texture uses. Twins that sample RENDER TARGETS flip v (Metal's
   texture row 0 is the TOP); CPU-uploaded textures need no flip. */
#ifdef SOL_RHI_METAL

static const char *VERTEX_SRC =        /* the full twin of the GLSL below */
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float3 pos     [[attribute(0)]];\n"
    "    float3 normal  [[attribute(1)]];\n"
    "    float2 uv      [[attribute(2)]];\n"
    "    float4 tangent [[attribute(3)]];\n"
    "};\n"
    "struct VU {\n"
    "    float4x4 uModel;\n"
    "    float4x4 uView;\n"
    "    float4x4 uProj;\n"
    "    float3x3 uNormalMatrix;\n"
    "};\n"
    "struct VOut {\n"
    "    float4 pos [[position]];\n"
    "    float3 normal;\n"
    "    float3 worldPos;\n"
    "    float2 uv;\n"
    "    float4 tangent;\n"
    "};\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float4 worldPos = u.uModel * float4(v.pos, 1.0);\n"
    "    o.pos = u.uProj * (u.uView * worldPos);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z [-w,w] -> Metal [0,w] */
    "    o.normal = u.uNormalMatrix * v.normal;\n"
    "    o.worldPos = worldPos.xyz;\n"
    "    o.uv = v.uv;\n"
    "    float3x3 lin = float3x3(u.uModel[0].xyz, u.uModel[1].xyz, u.uModel[2].xyz);\n"
    "    o.tangent = float4(lin * v.tangent.xyz, v.tangent.w);\n"
    "    return o;\n"
    "}\n";

/* The FULL PBR twin (stage c/d replaced stage b's fixed-sun stand-in) — a
   line-for-line translation of the GLSL below: same GGX/Smith/Fresnel
   trio, same brdfDirect for every light, same windowed point loop, same
   split-sum IBL, same 3x3 PCF shadow. The shadow map and depth compare
   are GLSL-identical because the VS twins store GL's depth mapping and
   the backend's negative viewport keeps GL's row layout. */
static const char *FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant float PI = 3.14159265359;\n"
    "struct VOut {\n"
    "    float4 pos [[position]];\n"
    "    float3 normal;\n"
    "    float3 worldPos;\n"
    "    float2 uv;\n"
    "    float4 tangent;\n"
    "};\n"
    "struct FU {\n"
    "    float3   uViewPos;\n"
    "    float    uHighlight;\n"
    "    float    uUseAlbedoTex;\n"
    "    float    uUseMRTex;\n"
    "    float    uUseAOTex;\n"
    "    float    uAOStrength;\n"
    "    float    uUseNormalTex;\n"
    "    float    uNormalScale;\n"
    "    float3   uBaseColor;\n"
    "    float    uTerrainBlend;\n"
    "    float    uTerrainY0;\n"
    "    float    uTerrainAmp;\n"
    "    float    uMetallic;\n"
    "    float    uRoughness;\n"
    "    float3   uLightPos;\n"
    "    float3   uLightDir;\n"
    "    float3   uLightColor;\n"
    "    float    uLightIntensity;\n"
    "    float    uCosInner;\n"
    "    float    uCosOuter;\n"
    "    float3   uEmissive;\n"
    "    float    uOpacity;\n"
    "    int      uPointCount;\n"
    "    float3   uPointPos[8];\n"
    "    float3   uPointColor[8];\n"
    "    float    uPointRadius[8];\n"
    "    float4x4 uLightVP;\n"                                /* near cascade (P8 item 6) */
    "    float4x4 uLightVP1;\n"                               /* far cascade */
    "    float4x4 uSpotVP;\n"                                 /* the sconce's shadow matrix (P8 item 7) */
    "    float3   uSpotPos;\n"
    "    float3   uSpotDir;\n"
    "    float3   uSpotColor;\n"                              /* premultiplied by intensity*flicker */
    "    float    uSpotRadius;\n"
    "    float    uSpotCosInner;\n"
    "    float    uSpotCosOuter;\n"
    "    float    uSpotEnabled;\n"                            /* 0 = no caster */
    "    float    uUseIBL;\n"
    "    float    uAmbientScale;\n"
    "};\n"
    "static float distributionGGX(float NoH, float rough) {\n"
    "    float a = rough*rough; float a2 = a*a;\n"
    "    float d = NoH*NoH*(a2 - 1.0) + 1.0;\n"
    "    return a2 / (PI * d * d);\n"
    "}\n"
    "static float geometrySchlickGGX(float NdotX, float k) {\n"
    "    return NdotX / (NdotX*(1.0 - k) + k);\n"
    "}\n"
    "static float geometrySmith(float NoV, float NoL, float rough) {\n"
    "    float k = (rough + 1.0)*(rough + 1.0) / 8.0;\n"
    "    return geometrySchlickGGX(NoV, k) * geometrySchlickGGX(NoL, k);\n"
    "}\n"
    "static float3 fresnelSchlick(float HoV, float3 F0) {\n"
    "    return F0 + (1.0 - F0) * pow(1.0 - HoV, 5.0);\n"
    "}\n"
    "static float3 brdfDirect(float3 N, float3 V, float3 L, float3 albedo,\n"
    "                         float metallic, float roughness, float3 F0) {\n"
    "    float3 H  = normalize(L + V);\n"
    "    float NoV = max(dot(N, V), 0.0001);\n"
    "    float NoL = max(dot(N, L), 0.0);\n"
    "    float NoH = max(dot(N, H), 0.0);\n"
    "    float HoV = max(dot(H, V), 0.0);\n"
    "    float D = distributionGGX(NoH, roughness);\n"
    "    float G = geometrySmith(NoV, NoL, roughness);\n"
    "    float3 F = fresnelSchlick(HoV, F0);\n"
    "    float3 specular = (D * G) * F / (4.0 * NoV * NoL + 0.0001);\n"
    "    float3 kD = (float3(1.0) - F) * (1.0 - metallic);\n"
    "    return (kD * albedo / PI + specular) * NoL;\n"
    "}\n"
    "static float3 fresnelSchlickRoughness(float cosTheta, float3 F0, float rough) {\n"
    "    return F0 + (max(float3(1.0 - rough), F0) - F0) * pow(1.0 - cosTheta, 5.0);\n"
    "}\n"
    "static float pcfShadow(depth2d<float> smap, sampler smp, float3 proj, float bias) {\n"
    "    float2 texel = 1.0 / float2(smap.get_width(), smap.get_height());\n"
    "    float sum = 0.0;\n"
    "    for (int x = -1; x <= 1; ++x)\n"
    "        for (int y = -1; y <= 1; ++y) {\n"
    "            float closest = smap.sample(smp,\n"
    "                                proj.xy + float2((float)x, (float)y) * texel);\n"
    "            sum += (proj.z - bias > closest) ? 1.0 : 0.0;\n"
    "        }\n"
    "    return sum / 9.0;\n"
    "}\n"
    "static bool inCascade(float3 p) {\n"
    "    return p.x > 0.0 && p.x < 1.0 && p.y > 0.0 && p.y < 1.0 && p.z <= 1.0;\n"
    "}\n"
    "static float shadowFactor(float3 worldPos, float NoL, constant FU &u,\n"
    "                          depth2d<float> m0, sampler sm0,\n"
    "                          depth2d<float> m1, sampler sm1) {\n"   /* P8 item 6: nearest covering cascade */
    "    float bias = max(0.0025 * (1.0 - NoL), 0.0008);\n"
    "    float4 l0 = u.uLightVP * float4(worldPos, 1.0);\n"
    "    float3 p0 = l0.xyz / l0.w * 0.5 + 0.5;\n"
    "    if (inCascade(p0)) return pcfShadow(m0, sm0, p0, bias);\n"
    "    float4 l1 = u.uLightVP1 * float4(worldPos, 1.0);\n"
    "    float3 p1 = l1.xyz / l1.w * 0.5 + 0.5;\n"
    "    if (inCascade(p1)) return pcfShadow(m1, sm1, p1, bias);\n"
    "    return 0.0;\n"
    "}\n"
    "static float spotShadow(float3 worldPos, constant FU &u,\n"
    "                        depth2d<float> smap, sampler smp) {\n"   /* P8 item 7: one perspective map */
    "    float4 lp = u.uSpotVP * float4(worldPos, 1.0);\n"
    "    float3 pr = lp.xyz / lp.w * 0.5 + 0.5;\n"
    "    if (lp.w <= 0.0 || !inCascade(pr)) return 0.0;\n"
    "    return pcfShadow(smap, smp, pr, 0.0015);\n"
    "}\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float>   uAlbedoTex     [[texture(0)]],\n"
    "                      texture2d<float>   uMRTex         [[texture(1)]],\n"
    "                      texture2d<float>   uAOTex         [[texture(2)]],\n"
    "                      texture2d<float>   uNormalTex     [[texture(3)]],\n"
    "                      depth2d<float>     uShadowMap     [[texture(4)]],\n"
    "                      texturecube<float> uIrradianceMap [[texture(5)]],\n"
    "                      texturecube<float> uPrefilterMap  [[texture(6)]],\n"
    "                      texture2d<float>   uBrdfLUT       [[texture(7)]],\n"
    "                      depth2d<float>     uShadowMap1    [[texture(8)]],\n"   /* far cascade (P8 item 6) */
    "                      depth2d<float>     uSpotShadowMap [[texture(9)]],\n"   /* the sconce (P8 item 7) */
    "                      sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]],\n"
    "                      sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]],\n"
    "                      sampler s4 [[sampler(4)]], sampler s5 [[sampler(5)]],\n"
    "                      sampler s6 [[sampler(6)]], sampler s7 [[sampler(7)]],\n"
    "                      sampler s8 [[sampler(8)]], sampler s9 [[sampler(9)]]) {\n"
    "    float3 albedo = u.uBaseColor;\n"
    "    if (u.uUseAlbedoTex > 0.5) albedo *= uAlbedoTex.sample(s0, v.uv).rgb;\n"
    "    float metallic  = u.uMetallic;\n"
    "    float roughness = u.uRoughness;\n"
    "    if (u.uUseMRTex > 0.5) {\n"
    "        float3 mr = uMRTex.sample(s1, v.uv).rgb;\n"
    "        roughness *= mr.g;\n"
    "        metallic  *= mr.b;\n"
    "    }\n"
    "    roughness = max(roughness, 0.04);\n"
    "    float3 N = normalize(v.normal);\n"
    "    if (u.uTerrainBlend > 0.5) {\n"
    "        float slope = clamp(1.0 - N.y, 0.0, 1.0);\n"
    "        float relh  = clamp((v.worldPos.y - u.uTerrainY0) / max(u.uTerrainAmp, 0.001), 0.0, 1.0);\n"
    "        float3 moss = float3(0.30, 0.42, 0.22);\n"
    "        float3 rock = float3(0.46, 0.44, 0.40);\n"
    "        float3 crag = float3(0.64, 0.62, 0.56);\n"
    "        albedo = mix(moss, rock, smoothstep(0.15, 0.45, slope));\n"
    "        albedo = mix(albedo, crag, smoothstep(0.55, 0.95, relh));\n"
    "    }\n"
    "    if (u.uUseNormalTex > 0.5) {\n"
    "        float3 T = normalize(v.tangent.xyz - dot(v.tangent.xyz, N) * N);\n"
    "        float3 B = cross(N, T) * v.tangent.w;\n"
    "        float3 n = uNormalTex.sample(s3, v.uv).rgb * 2.0 - 1.0;\n"
    "        n.xy *= u.uNormalScale;\n"
    "        N = normalize(float3x3(T, B, N) * n);\n"
    "    }\n"
    "    float3 V = normalize(u.uViewPos - v.worldPos);\n"
    "    float NoV = max(dot(N, V), 0.0001);\n"
    "    float3 F0 = mix(float3(0.04), albedo, metallic);\n"
    "    float3 L = -u.uLightDir;\n"                          /* the sun: parallel rays (P8 item 6) */
    "    float3 radiance = u.uLightColor * u.uLightIntensity;\n"  /* directional: no falloff, no cone */
    "    float shadow = shadowFactor(v.worldPos, max(dot(N, L), 0.0), u,\n"
    "                                uShadowMap, s4, uShadowMap1, s8);\n"
    "    float3 Lo = brdfDirect(N, V, L, albedo, metallic, roughness, F0)\n"
    "              * radiance * (1.0 - shadow);\n"
    "    if (u.uSpotEnabled > 0.5) {\n"                        /* the shadow-casting sconce (P8 item 7) */
    "        float3 sl    = u.uSpotPos - v.worldPos;\n"
    "        float  sd    = length(sl);\n"
    "        float3 Ls    = sl / max(sd, 0.0001);\n"
    "        float  swf   = clamp(1.0 - pow(sd / u.uSpotRadius, 4.0), 0.0, 1.0);\n"
    "        float  scone = smoothstep(u.uSpotCosOuter, u.uSpotCosInner, dot(-Ls, u.uSpotDir));\n"
    "        float3 srad  = u.uSpotColor * (swf * swf / (sd * sd + 1.0)) * scone;\n"
    "        float3 Lspot = brdfDirect(N, V, Ls, albedo, metallic, roughness, F0) * srad;\n"
    "        Lo += Lspot * (1.0 - spotShadow(v.worldPos, u, uSpotShadowMap, s9));\n"
    "    }\n"
    "    for (int pi = 0; pi < u.uPointCount; pi++) {\n"
    "        float3 pl = u.uPointPos[pi] - v.worldPos;\n"
    "        float d2 = dot(pl, pl);\n"
    "        float d  = sqrt(d2);\n"
    "        float3 Lp = pl / max(d, 0.0001);\n"
    "        float wf = clamp(1.0 - pow(d / u.uPointRadius[pi], 4.0), 0.0, 1.0);\n"
    "        float3 rp = u.uPointColor[pi] * (wf * wf / (d2 + 1.0));\n"
    "        Lo += brdfDirect(N, V, Lp, albedo, metallic, roughness, F0) * rp;\n"
    "    }\n"
    "    float ao = 1.0;\n"
    "    if (u.uUseAOTex > 0.5) ao = 1.0 + u.uAOStrength * (uAOTex.sample(s2, v.uv).r - 1.0);\n"
    "    float3 ambient;\n"
    "    if (u.uUseIBL > 0.5) {\n"
    "        float3 kS_amb = fresnelSchlickRoughness(NoV, F0, roughness);\n"
    "        float3 kD_amb = (1.0 - kS_amb) * (1.0 - metallic);\n"
    "        float3 irr    = uIrradianceMap.sample(s5, N).rgb;\n"
    "        float3 diffuseIBL = irr * albedo;\n"
    "        float3 R = reflect(-V, N);\n"
    "        float3 prefiltered = uPrefilterMap.sample(s6, R, level(roughness * 4.0)).rgb;\n"
    "        float2 brdf = uBrdfLUT.sample(s7, float2(NoV, roughness)).rg;\n"
    "        float3 specularIBL = prefiltered * (F0 * brdf.x + brdf.y);\n"
    "        ambient = (kD_amb * diffuseIBL + specularIBL) * ao;\n"
    "    } else {\n"
    "        ambient = 0.03 * albedo * ao;\n"
    "    }\n"
    "    float3 color = ambient * u.uAmbientScale + Lo + u.uEmissive;\n"
    "    color = mix(color, float3(1.0, 0.85, 0.30), u.uHighlight * 0.5);\n"
    "    return float4(color, u.uOpacity);\n"
    "}\n";

#else /* GLSL */

static const char *VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "layout (location = 3) in vec4 aTangent;\n"           /* xyz + handedness w (item 8d) */
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "uniform mat3 uNormalMatrix;\n"
    "out vec3 vNormal;\n"
    "out vec3 vWorldPos;\n"
    "out vec2 vUV;\n"
    "out vec4 vTangent;\n"
    "void main() {\n"
    "    vec4 worldPos = uModel * vec4(aPos, 1.0);\n"
    "    gl_Position = uProj * uView * worldPos;\n"
    "    vNormal = uNormalMatrix * aNormal;\n"   /* covector transform: correct under non-uniform scale */
    "    vWorldPos = worldPos.xyz;\n"
    "    vUV = aUV;\n"
    "    vTangent = vec4(mat3(uModel) * aTangent.xyz, aTangent.w);\n"  /* tangent vector: model linear part */
    "}\n";

static const char *FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3 vNormal;\n"
    "in vec3 vWorldPos;\n"
    "in vec2 vUV;\n"
    "in vec4 vTangent;\n"
    "uniform vec3  uViewPos;\n"
    "uniform float uHighlight;\n"                            /* 0 = normal, 1 = selected */
    "uniform sampler2D uAlbedoTex;\n"
    "uniform float uUseAlbedoTex;\n"                         /* 0 = base_color only */
    "uniform sampler2D uMRTex;\n"
    "uniform float uUseMRTex;\n"                             /* 0 = scalar factors only */
    "uniform sampler2D uAOTex;\n"
    "uniform float uUseAOTex;\n"
    "uniform float uAOStrength;\n"
    "uniform sampler2D uNormalTex;\n"
    "uniform float uUseNormalTex;\n"
    "uniform float uNormalScale;\n"
    "uniform vec3  uBaseColor;\n"                            /* baseColorFactor (linear) */
    "uniform float uTerrainBlend;\n"   /* item 10: 1 = slope/height palette */
    "uniform float uTerrainY0;\n"      /* the plot's world base height */
    "uniform float uTerrainAmp;\n"     /* its relief, for height normalization */
    "uniform float uMetallic;\n"
    "uniform float uRoughness;\n"
    "uniform vec3  uLightPos;\n"                             /* spot light: position (item 9a) */
    "uniform vec3  uLightDir;\n"                             /* normalized aim, light -> scene */
    "uniform vec3  uLightColor;\n"
    "uniform float uLightIntensity;\n"
    "uniform float uCosInner;\n"                             /* cos(inner half-angle) */
    "uniform float uCosOuter;\n"                             /* cos(outer half-angle) */
    "uniform vec3  uEmissive;\n"                             /* emitted light (P4 item 5) */
    "uniform float uOpacity;\n"                              /* P9 item 2: 1=opaque, <1=glass */
    "uniform int   uPointCount;\n"                           /* point lights (P4 item 5) */
    "uniform vec3  uPointPos[8];\n"
    "uniform vec3  uPointColor[8];\n"                        /* color * intensity, premultiplied */
    "uniform float uPointRadius[8];\n"                       /* the falloff WINDOW's edge */
    "uniform mat4  uLightVP;\n"                              /* near cascade proj*view (P8 item 6) */
    "uniform sampler2D uShadowMap;\n"                        /* near cascade depth */
    "uniform mat4  uLightVP1;\n"                             /* far cascade proj*view */
    "uniform sampler2D uShadowMap1;\n"                       /* far cascade depth */
    "uniform float uSpotEnabled;\n"                          /* shadow-casting sconce (P8 item 7); 0 = none */
    "uniform vec3  uSpotPos;\n"
    "uniform vec3  uSpotDir;\n"
    "uniform vec3  uSpotColor;\n"                            /* premultiplied by intensity*flicker */
    "uniform float uSpotRadius;\n"
    "uniform float uSpotCosInner;\n"
    "uniform float uSpotCosOuter;\n"
    "uniform mat4  uSpotVP;\n"                               /* the sconce's perspective shadow matrix */
    "uniform sampler2D uSpotShadowMap;\n"                    /* unit 9 */
    "uniform samplerCube uIrradianceMap;\n"                  /* diffuse IBL (B3) */
    "uniform float uUseIBL;\n"                               /* 0 = flat ambient fallback */
    "uniform float uAmbientScale;\n"                         /* 1 outdoors; <1 inside a sealed room (item 7) */
    "uniform samplerCube uPrefilterMap;\n"                   /* specular IBL: prefiltered env (C3) */
    "uniform sampler2D   uBrdfLUT;\n"                        /* specular IBL: BRDF integration */
    "out vec4 FragColor;\n"
    "const float PI = 3.14159265359;\n"
    "\n"
    "float distributionGGX(float NoH, float rough) {\n"      /* D: microfacet alignment */
    "    float a = rough*rough; float a2 = a*a;\n"
    "    float d = NoH*NoH*(a2 - 1.0) + 1.0;\n"
    "    return a2 / (PI * d * d);\n"
    "}\n"
    "float geometrySchlickGGX(float NdotX, float k) {\n"     /* one side of G */
    "    return NdotX / (NdotX*(1.0 - k) + k);\n"
    "}\n"
    "float geometrySmith(float NoV, float NoL, float rough) {\n"   /* G: shadow/mask */
    "    float k = (rough + 1.0)*(rough + 1.0) / 8.0;\n"     /* direct-lighting remap */
    "    return geometrySchlickGGX(NoV, k) * geometrySchlickGGX(NoL, k);\n"
    "}\n"
    "vec3 fresnelSchlick(float HoV, vec3 F0) {\n"            /* F: grazing reflectance */
    "    return F0 + (1.0 - F0) * pow(1.0 - HoV, 5.0);\n"
    "}\n"
    "vec3 brdfDirect(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic,\n"
    "                float roughness, vec3 F0) {\n"          /* ONE BRDF, every light (P4 item 5):
                                                                the spot and the point loop both
                                                                come through here, so a material
                                                                responds identically to each */
    "    vec3  H   = normalize(L + V);\n"
    "    float NoV = max(dot(N, V), 0.0001);\n"
    "    float NoL = max(dot(N, L), 0.0);\n"
    "    float NoH = max(dot(N, H), 0.0);\n"
    "    float HoV = max(dot(H, V), 0.0);\n"
    "    float D = distributionGGX(NoH, roughness);\n"
    "    float G = geometrySmith(NoV, NoL, roughness);\n"
    "    vec3  F = fresnelSchlick(HoV, F0);\n"
    "    vec3  specular = (D * G) * F / (4.0 * NoV * NoL + 0.0001);\n"
    "    vec3  kD = (vec3(1.0) - F) * (1.0 - metallic);\n"
    "    return (kD * albedo / PI + specular) * NoL;\n"
    "}\n"
    "vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float rough) {\n"  /* ambient F (no half-vector): view angle + roughness (B3) */
    "    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - cosTheta, 5.0);\n"
    "}\n"
    "float pcfShadow(sampler2D smap, vec3 proj, float bias) {\n"   /* 3x3 PCF: soften the edge */
    "    vec2 texel = 1.0 / vec2(textureSize(smap, 0));\n"
    "    float sum = 0.0;\n"
    "    int x, y;\n"
    "    for (x = -1; x <= 1; ++x)\n"
    "        for (y = -1; y <= 1; ++y) {\n"
    "            float closest = texture(smap, proj.xy + vec2(x, y) * texel).r;\n"
    "            sum += (proj.z - bias > closest) ? 1.0 : 0.0;\n"   /* 1 = something closer to the sun */
    "        }\n"
    "    return sum / 9.0;\n"
    "}\n"
    "bool inCascade(vec3 p) {\n"                              /* fully inside this map's [0,1] box */
    "    return p.x > 0.0 && p.x < 1.0 && p.y > 0.0 && p.y < 1.0 && p.z <= 1.0;\n"
    "}\n"
    "float shadowFactor(vec3 worldPos, float NoL) {\n"       /* 0 = lit, 1 = shadowed. P8 item 6:
                                                                pick the NEAREST covering cascade
                                                                (near map first = crisper). */
    "    float bias = max(0.0025 * (1.0 - NoL), 0.0008);\n"   /* slope-scaled: more at grazing angles */
    "    vec4 l0 = uLightVP * vec4(worldPos, 1.0);\n"
    "    vec3 p0 = l0.xyz / l0.w * 0.5 + 0.5;\n"
    "    if (inCascade(p0)) return pcfShadow(uShadowMap, p0, bias);\n"
    "    vec4 l1 = uLightVP1 * vec4(worldPos, 1.0);\n"
    "    vec3 p1 = l1.xyz / l1.w * 0.5 + 0.5;\n"
    "    if (inCascade(p1)) return pcfShadow(uShadowMap1, p1, bias);\n"
    "    return 0.0;\n"                                       /* beyond every cascade = lit */
    "}\n"
    "float spotShadow(vec3 worldPos) {\n"                    /* P8 item 7: one perspective map (the spot path item 6 retired) */
    "    vec4 lp = uSpotVP * vec4(worldPos, 1.0);\n"
    "    vec3 pr = lp.xyz / lp.w * 0.5 + 0.5;\n"
    "    if (lp.w <= 0.0 || !inCascade(pr)) return 0.0;\n"    /* outside the cone's frustum = lit */
    "    return pcfShadow(uSpotShadowMap, pr, 0.0015);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec3 albedo = uBaseColor;\n"
    "    if (uUseAlbedoTex > 0.5) albedo *= texture(uAlbedoTex, vUV).rgb;\n"  /* sRGB tex -> linear on sample */
    "    float metallic  = uMetallic;\n"
    "    float roughness = uRoughness;\n"
    "    if (uUseMRTex > 0.5) {\n"
    "        vec3 mr = texture(uMRTex, vUV).rgb;\n"           /* linear: G=roughness, B=metallic */
    "        roughness *= mr.g;\n"                            /* factor x texture */
    "        metallic  *= mr.b;\n"
    "    }\n"
    "    roughness = max(roughness, 0.04);\n"                 /* clamp AFTER compositing */
    "\n"
    "    vec3 N = normalize(vNormal);\n"                       /* renormalize after interp */
    /* item 10: terrain wears a slope/height palette instead of a flat
       base color — moss where flat and low, stone as it steepens, pale
       crag up high. Both inputs are already here (the world normal and
       the fragment height); this is the "looks right with no hand-
       painting" win in procedural-palette form. */
    "    if (uTerrainBlend > 0.5) {\n"
    "        float slope = clamp(1.0 - N.y, 0.0, 1.0);\n"
    "        float relh  = clamp((vWorldPos.y - uTerrainY0) / max(uTerrainAmp, 0.001), 0.0, 1.0);\n"
    "        vec3 moss = vec3(0.30, 0.42, 0.22);\n"
    "        vec3 rock = vec3(0.46, 0.44, 0.40);\n"
    "        vec3 crag = vec3(0.64, 0.62, 0.56);\n"
    "        albedo = mix(moss, rock, smoothstep(0.15, 0.45, slope));\n"
    "        albedo = mix(albedo, crag, smoothstep(0.55, 0.95, relh));\n"
    "    }\n"
    "    if (uUseNormalTex > 0.5) {\n"
    "        vec3 T = normalize(vTangent.xyz - dot(vTangent.xyz, N) * N);\n"  /* re-orthogonalize vs N */
    "        vec3 B = cross(N, T) * vTangent.w;\n"             /* bitangent (handedness) */
    "        vec3 n = texture(uNormalTex, vUV).rgb * 2.0 - 1.0;\n"  /* decode [0,1] -> [-1,1] */
    "        n.xy *= uNormalScale;\n"                          /* bump strength */
    "        N = normalize(mat3(T, B, N) * n);\n"              /* tangent space -> world */
    "    }\n"
    "    vec3 V = normalize(uViewPos - vWorldPos);\n"          /* direction TO the camera */
    "    float NoV = max(dot(N, V), 0.0001);\n"                /* the ambient terms reuse this */
    "    vec3 F0 = mix(vec3(0.04), albedo, metallic);\n"      /* dielectric 4% vs metal-tinted */
    "\n"
    "    vec3 L = -uLightDir;\n"                               /* the sun: parallel rays (P8 item 6) */
    "    vec3 radiance = uLightColor * uLightIntensity;\n"     /* directional: no falloff, no cone */
    "    float shadow = shadowFactor(vWorldPos, max(dot(N, L), 0.0));\n"
    "    vec3 Lo = brdfDirect(N, V, L, albedo, metallic, roughness, F0)\n"
    "            * radiance * (1.0 - shadow);\n"
    "\n"
    "    if (uSpotEnabled > 0.5) {\n"                          /* the shadow-casting sconce (P8 item 7) */
    "        vec3  sl    = uSpotPos - vWorldPos;\n"
    "        float sd    = length(sl);\n"
    "        vec3  Ls    = sl / max(sd, 0.0001);\n"
    "        float swf   = clamp(1.0 - pow(sd / uSpotRadius, 4.0), 0.0, 1.0);\n"   /* windowed inverse-square */
    "        float scone = smoothstep(uSpotCosOuter, uSpotCosInner, dot(-Ls, uSpotDir));\n"
    "        vec3  srad  = uSpotColor * (swf * swf / (sd * sd + 1.0)) * scone;\n"
    "        vec3  Lspot = brdfDirect(N, V, Ls, albedo, metallic, roughness, F0) * srad;\n"
    "        Lo += Lspot * (1.0 - spotShadow(vWorldPos));\n"
    "    }\n"
    "\n"
    "    for (int pi = 0; pi < uPointCount; pi++) {\n"         /* point lights (P4 item 5) */
    "        vec3  pl = uPointPos[pi] - vWorldPos;\n"
    "        float d2 = dot(pl, pl);\n"
    "        float d  = sqrt(d2);\n"
    "        vec3  Lp = pl / max(d, 0.0001);\n"
    "        float wf = clamp(1.0 - pow(d / uPointRadius[pi], 4.0), 0.0, 1.0);\n"
    "        vec3  rp = uPointColor[pi] * (wf * wf / (d2 + 1.0));\n"  /* WINDOWED inverse-
                                                                square: physics inside,
                                                                exactly zero at the radius
                                                                — no pop at the boundary */
    "        Lo += brdfDirect(N, V, Lp, albedo, metallic, roughness, F0) * rp;\n"
    "    }\n"
    "\n"
    "    float ao = 1.0;\n"
    "    if (uUseAOTex > 0.5) ao = 1.0 + uAOStrength * (texture(uAOTex, vUV).r - 1.0);\n"
    "    vec3 ambient;\n"                                     /* indirect light (AO modulates this only; direct Lo untouched) */
    "    if (uUseIBL > 0.5) {\n"
    "        vec3 kS_amb = fresnelSchlickRoughness(NoV, F0, roughness);\n"
    "        vec3 kD_amb = (1.0 - kS_amb) * (1.0 - metallic);\n"
    "        vec3 irr    = texture(uIrradianceMap, N).rgb;\n"  /* environment diffuse (B2 baked the 1/PI) */
    "        vec3 diffuseIBL = irr * albedo;\n"
    "        vec3 R = reflect(-V, N);\n"                        /* specular IBL (C3): mirror direction */
    "        vec3 prefiltered = textureLod(uPrefilterMap, R, roughness * 4.0).rgb;\n"  /* roughness -> mip */
    "        vec2 brdf = texture(uBrdfLUT, vec2(NoV, roughness)).rg;\n"
    "        vec3 specularIBL = prefiltered * (F0 * brdf.x + brdf.y);\n"  /* split-sum recombine */
    "        ambient = (kD_amb * diffuseIBL + specularIBL) * ao;\n"
    "    } else {\n"
    "        ambient = 0.03 * albedo * ao;\n"                  /* fallback if the .hdr didn't load */
    "    }\n"
    "    vec3 color = ambient * uAmbientScale + Lo + uEmissive;\n"  /* emissive ADDS
                                                                after lighting: seen by
                                                                the camera, felt by nobody
                                                                (P4 item 5) */
    "    color = mix(color, vec3(1.0, 0.85, 0.30), uHighlight * 0.5);\n"  /* selection tint (linear) */
    "    FragColor = vec4(color, uOpacity);\n"               /* LINEAR -> HDR; alpha = P9 item 2 opacity */
    "}\n";

#endif /* SOL_RHI_METAL — the core pair */

/* --- the fullscreen tonemap/encode pass (item 7b): samples the HDR buffer and
   writes the display image to the window. The vertex shader synthesizes one
   screen-covering triangle from gl_VertexID, so it needs no vertex buffer. --- */
/* --- the meadow shader (P4 item 3): stream 0 = one tuft (crossed tapered
   quads, y in [0,1]), stream 1 = where / how-tall / what-green each copy
   is. No per-instance uniforms anywhere — the population is DATA. Yaw comes
   FREE from gl_InstanceID (golden-angle steps decorrelate neighbors:
   variation costing zero per-instance bytes); the tips sway on uTime, roots
   pinned; the root-to-tip gradient fakes the occlusion real lighting would
   give — honest v1, the meadow meets real lights in item 5. */
#ifdef SOL_RHI_METAL
static const char *MEADOW_VERTEX_SRC =        /* [[instance_id]] = gl_InstanceID;
                                       the instance stream rides buffer(1),
                                       stepped by the pipeline's layout */
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float3 pos  [[attribute(0)]];\n"
    "    float4 inst [[attribute(1)]];\n"
    "    float4 tint [[attribute(2)]];\n"
    "};\n"
    "struct VU { float4x4 uView; float4x4 uProj; float uTime;\n"
    "            float3 uWind; };\n"
    "struct VOut { float4 pos [[position]]; float3 tint; float root; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], uint iid [[instance_id]],\n"
    "                  constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float a = (float)iid * 2.39996;\n"
    "    float c = cos(a), s = sin(a);\n"
    "    float3 p = float3(c*v.pos.x + s*v.pos.z, v.pos.y, -s*v.pos.x + c*v.pos.z) * v.inst.w;\n"
    "    float ph = u.uTime * 1.5 + dot(v.inst.xz, u.uWind.xy) * 0.4 + a;\n"  /* gust front */
    "    float bend = sin(ph) * (0.05 + 0.18 * u.uWind.z) * v.pos.y * v.inst.w;\n"
    "    p.x += u.uWind.x * bend + sin(u.uTime*2.1 + a*5.0) * 0.02 * v.pos.y * v.inst.w;\n"
    "    p.z += u.uWind.y * bend + cos(u.uTime*1.7 + a*3.0) * 0.02 * v.pos.y * v.inst.w;\n"
    "    o.tint = v.tint.rgb;\n"
    "    o.root = v.pos.y;\n"
    "    o.pos = u.uProj * (u.uView * float4(v.inst.xyz + p, 1.0));\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    return o;\n"
    "}\n";
static const char *MEADOW_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float3 tint; float root; };\n"
    "fragment float4 fmain(VOut v [[stage_in]]) {\n"
    "    return float4(v.tint * (0.35 + 0.65 * v.root), 1.0);\n"
    "}\n";
#else /* GLSL */
static const char *MEADOW_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec4 aInst;\n"       /* xyz = world pos, w = scale */
    "layout (location = 2) in vec4 aTint;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "uniform float uTime;\n"
    "uniform vec3 uWind;\n"
    "out vec3  vTint;\n"
    "out float vRoot;\n"
    "void main() {\n"
    "    float a = float(gl_InstanceID) * 2.39996;\n"   /* the golden angle */
    "    float c = cos(a), s = sin(a);\n"
    "    vec3 p = vec3(c*aPos.x + s*aPos.z, aPos.y, -s*aPos.x + c*aPos.z) * aInst.w;\n"
    "    float ph = uTime * 1.5 + dot(aInst.xz, uWind.xy) * 0.4 + a;\n"   /* gust front */
    "    float bend = sin(ph) * (0.05 + 0.18 * uWind.z) * aPos.y * aInst.w;\n"
    "    p.x += uWind.x * bend + sin(uTime*2.1 + a*5.0) * 0.02 * aPos.y * aInst.w;\n"
    "    p.z += uWind.y * bend + cos(uTime*1.7 + a*3.0) * 0.02 * aPos.y * aInst.w;\n"
    "    vTint = aTint.rgb;\n"
    "    vRoot = aPos.y;\n"
    "    gl_Position = uProj * uView * vec4(aInst.xyz + p, 1.0);\n"
    "}\n";
static const char *MEADOW_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3  vTint;\n"
    "in float vRoot;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vec4(vTint * (0.35 + 0.65 * vRoot), 1.0);\n"
    "}\n";
#endif /* SOL_RHI_METAL — the meadow */

/* --- the instanced-ornament shader (P6 item 10: the item-7 debt paid) ---
   The meadow proved the per-instance machinery; ornament wants the FULL
   PBR fragment over it. Stream 0 = the canonical 12-float vertex (one
   baluster), stream 1 = where/turned/scaled each copy stands IN ITS
   CARCASS'S LOCAL FRAME — uModel is the balustrade's own transform, so
   dragging the carcass carries its copies for free (the meadow's island
   trick). Shares FRAGMENT_SRC (a copy is lit like any surface) and the
   shadow fragment (masonry casts). Normals ride the inverse scale —
   the covector rule survives the per-instance stretch. */
#ifdef SOL_RHI_METAL
static const char *ORNAMENT_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float3 pos     [[attribute(0)]];\n"
    "    float3 normal  [[attribute(1)]];\n"
    "    float2 uv      [[attribute(2)]];\n"
    "    float4 tangent [[attribute(3)]];\n"
    "    float4 instA   [[attribute(4)]];\n"   /* xyz local pos, w yaw */
    "    float4 instB   [[attribute(5)]];\n"   /* x xz-scale, y y-scale */
    "};\n"
    "struct VU {\n"
    "    float4x4 uModel;\n"
    "    float4x4 uView;\n"
    "    float4x4 uProj;\n"
    "    float3x3 uNormalMatrix;\n"
    "    float    uTime;\n"
    "    float3   uWind;\n"
    "};\n"
    "struct VOut {\n"
    "    float4 pos [[position]];\n"
    "    float3 normal;\n"
    "    float3 worldPos;\n"
    "    float2 uv;\n"
    "    float4 tangent;\n"
    "};\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float c = cos(v.instA.w), s = sin(v.instA.w);\n"
    "    float3 sp = float3(v.pos.x * v.instB.x, v.pos.y * v.instB.y, v.pos.z * v.instB.x);\n"
    "    float3 lp = float3(c*sp.x + s*sp.z, sp.y, -s*sp.x + c*sp.z) + v.instA.xyz;\n"
    "    float sway = v.instB.w * sp.y;\n"   /* amp 0 = no move (balusters) */
    "    float wph = u.uTime * 1.4 + dot(v.instA.xz, u.uWind.xy) * 0.4 + v.instB.z;\n"
    "    float bend = sway * (0.5 + 0.9 * u.uWind.z) * sin(wph);\n"
    "    lp.x += u.uWind.x * bend + sway * sin(u.uTime*2.0 + v.instB.z) * 0.3;\n"
    "    lp.z += u.uWind.y * bend + sway * cos(u.uTime*1.6 + v.instB.z) * 0.3;\n"
    "    float4 worldPos = u.uModel * float4(lp, 1.0);\n"
    "    o.pos = u.uProj * (u.uView * worldPos);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    float3 ln = float3(v.normal.x / v.instB.x, v.normal.y / v.instB.y, v.normal.z / v.instB.x);\n"
    "    ln = float3(c*ln.x + s*ln.z, ln.y, -s*ln.x + c*ln.z);\n"
    "    o.normal = u.uNormalMatrix * ln;\n"
    "    o.worldPos = worldPos.xyz;\n"
    "    o.uv = float2(v.uv.x * v.instB.x, v.uv.y * v.instB.y);\n"  /* world-scale UVs survive the stretch */
    "    float3 lt = float3(c*v.tangent.x + s*v.tangent.z, v.tangent.y, -s*v.tangent.x + c*v.tangent.z);\n"
    "    float3x3 lin = float3x3(u.uModel[0].xyz, u.uModel[1].xyz, u.uModel[2].xyz);\n"
    "    o.tangent = float4(lin * lt, v.tangent.w);\n"
    "    return o;\n"
    "}\n";
static const char *ORNAMENT_SHADOW_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float3 pos   [[attribute(0)]];\n"
    "    float4 instA [[attribute(4)]];\n"
    "    float4 instB [[attribute(5)]];\n"
    "};\n"
    "struct VU { float4x4 uModel; float4x4 uLightVP; };\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float c = cos(v.instA.w), s = sin(v.instA.w);\n"
    "    float3 sp = float3(v.pos.x * v.instB.x, v.pos.y * v.instB.y, v.pos.z * v.instB.x);\n"
    "    float3 lp = float3(c*sp.x + s*sp.z, sp.y, -s*sp.x + c*sp.z) + v.instA.xyz;\n"
    "    o.pos = u.uLightVP * (u.uModel * float4(lp, 1.0));\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    return o;\n"
    "}\n";
#else /* GLSL */
static const char *ORNAMENT_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "layout (location = 3) in vec4 aTangent;\n"
    "layout (location = 4) in vec4 aInstA;\n"      /* xyz local pos, w yaw */
    "layout (location = 5) in vec4 aInstB;\n"      /* x xz-scale, y y-scale */
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "uniform mat3 uNormalMatrix;\n"
    "uniform float uTime;\n"
    "uniform vec3 uWind;\n"
    "out vec3 vNormal;\n"
    "out vec3 vWorldPos;\n"
    "out vec2 vUV;\n"
    "out vec4 vTangent;\n"
    "void main() {\n"
    "    float c = cos(aInstA.w), s = sin(aInstA.w);\n"
    "    vec3 sp = vec3(aPos.x * aInstB.x, aPos.y * aInstB.y, aPos.z * aInstB.x);\n"
    "    vec3 lp = vec3(c*sp.x + s*sp.z, sp.y, -s*sp.x + c*sp.z) + aInstA.xyz;\n"
    "    float sway = aInstB.w * sp.y;\n"      /* amp 0 = no move (balusters) */
    "    float wph = uTime * 1.4 + dot(aInstA.xz, uWind.xy) * 0.4 + aInstB.z;\n"
    "    float bend = sway * (0.5 + 0.9 * uWind.z) * sin(wph);\n"
    "    lp.x += uWind.x * bend + sway * sin(uTime*2.0 + aInstB.z) * 0.3;\n"
    "    lp.z += uWind.y * bend + sway * cos(uTime*1.6 + aInstB.z) * 0.3;\n"
    "    vec4 worldPos = uModel * vec4(lp, 1.0);\n"
    "    gl_Position = uProj * uView * worldPos;\n"
    "    vec3 ln = vec3(aNormal.x / aInstB.x, aNormal.y / aInstB.y, aNormal.z / aInstB.x);\n"
    "    ln = vec3(c*ln.x + s*ln.z, ln.y, -s*ln.x + c*ln.z);\n"   /* covector: inverse scale, then the yaw */
    "    vNormal = uNormalMatrix * ln;\n"
    "    vWorldPos = worldPos.xyz;\n"
    "    vUV = vec2(aUV.x * aInstB.x, aUV.y * aInstB.y);\n"  /* world-scale UVs survive the stretch */
    "    vec3 lt = vec3(c*aTangent.x + s*aTangent.z, aTangent.y, -s*aTangent.x + c*aTangent.z);\n"
    "    vTangent = vec4(mat3(uModel) * lt, aTangent.w);\n"
    "}\n";
static const char *ORNAMENT_SHADOW_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 4) in vec4 aInstA;\n"
    "layout (location = 5) in vec4 aInstB;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uLightVP;\n"
    "void main() {\n"
    "    float c = cos(aInstA.w), s = sin(aInstA.w);\n"
    "    vec3 sp = vec3(aPos.x * aInstB.x, aPos.y * aInstB.y, aPos.z * aInstB.x);\n"
    "    vec3 lp = vec3(c*sp.x + s*sp.z, sp.y, -s*sp.x + c*sp.z) + aInstA.xyz;\n"
    "    gl_Position = uLightVP * uModel * vec4(lp, 1.0);\n"
    "}\n";
#endif /* SOL_RHI_METAL — the instanced ornament */

/* --- the water shader (P7 item 8: the phase's one renderer feature) ---
   A flat disc lit entirely in the fragment: two SCROLLING copies of one
   synthesized ripple normal (texgen animates by phase, not re-render);
   the surface normal rippled; the view reflected into the PREFILTER
   cubemap (the IBL pays a third time); Schlick fresnel blends a deep
   water tint into that sky reflection; a directional glint sparkles; a
   rim alpha-fade dissolves the disc into the shore. Alpha-blended LAST,
   after the opaque scene + particles. No planar reflections (the v1
   line). uModel places it; uv stays LOCAL (rim fade + ripple scale). */
#ifdef SOL_RHI_METAL
static const char *WATER_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float3 pos [[attribute(0)]]; float3 normal [[attribute(1)]];\n"
    "             float2 uv [[attribute(2)]]; float4 tangent [[attribute(3)]]; };\n"
    "struct VU { float4x4 uModel; float4x4 uView; float4x4 uProj; };\n"
    "struct VOut { float4 pos [[position]]; float3 worldPos; float2 uv; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float4 wp = u.uModel * float4(v.pos, 1.0);\n"
    "    o.worldPos = wp.xyz;\n"
    "    o.uv = v.uv;\n"
    "    o.pos = u.uProj * (u.uView * wp);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"
    "    return o;\n"
    "}\n";
static const char *WATER_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct FU { float3 uViewPos; float uTime; float3 uWaterTint;\n"
    "            float uWaterAlpha; float uPondR; float uRippleStr;\n"
    "            float3 uLightDir; float3 uLightColor; };\n"
    "struct VOut { float4 pos [[position]]; float3 worldPos; float2 uv; };\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float>   uRippleTex    [[texture(0)]],\n"
    "                      texturecube<float> uPrefilterMap [[texture(6)]],\n"
    "                      sampler s0 [[sampler(0)]], sampler s6 [[sampler(6)]]) {\n"
    "    float2 uv1 = v.uv * 0.6 + u.uTime * float2(0.018, 0.011);\n"
    "    float2 uv2 = v.uv * 0.37 - u.uTime * float2(0.012, 0.016);\n"
    "    float3 t1 = uRippleTex.sample(s0, uv1).rgb * 2.0 - 1.0;\n"
    "    float3 t2 = uRippleTex.sample(s0, uv2).rgb * 2.0 - 1.0;\n"
    "    float2 pert = (t1.xy + t2.xy) * u.uRippleStr;\n"
    "    float3 N = normalize(float3(pert.x, 1.0, pert.y));\n"
    "    float3 V = normalize(u.uViewPos - v.worldPos);\n"
    "    float3 R = reflect(-V, N);\n"
    "    float3 sky = uPrefilterMap.sample(s6, R, level(1.5)).rgb;\n"
    "    float fres = 0.02 + 0.98 * pow(1.0 - max(dot(N, V), 0.0), 5.0);\n"
    "    float3 col = mix(u.uWaterTint, sky, fres);\n"
    "    float3 H = normalize(V - u.uLightDir);\n"
    "    float glint = pow(max(dot(N, H), 0.0), 220.0);\n"
    "    col += u.uLightColor * glint * 2.5;\n"
    "    float rim = clamp((u.uPondR - length(v.uv)) / max(u.uPondR*0.22, 0.01), 0.0, 1.0);\n"
    "    return float4(col, u.uWaterAlpha * rim);\n"
    "}\n";
#else /* GLSL */
static const char *WATER_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "layout (location = 3) in vec4 aTangent;\n"
    "uniform mat4 uModel; uniform mat4 uView; uniform mat4 uProj;\n"
    "out vec3 vWorldPos; out vec2 vUV;\n"
    "void main() {\n"
    "    vec4 wp = uModel * vec4(aPos, 1.0);\n"
    "    vWorldPos = wp.xyz;\n"
    "    vUV = aUV;\n"
    "    gl_Position = uProj * uView * wp;\n"
    "}\n";
static const char *WATER_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec3 vWorldPos; in vec2 vUV;\n"
    "uniform vec3 uViewPos;\n"
    "uniform float uTime;\n"
    "uniform vec3 uWaterTint;\n"
    "uniform float uWaterAlpha;\n"
    "uniform float uPondR;\n"
    "uniform float uRippleStr;\n"
    "uniform vec3 uLightDir;\n"
    "uniform vec3 uLightColor;\n"
    "uniform sampler2D uRippleTex;\n"
    "uniform samplerCube uPrefilterMap;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    vec2 uv1 = vUV * 0.6 + uTime * vec2(0.018, 0.011);\n"
    "    vec2 uv2 = vUV * 0.37 - uTime * vec2(0.012, 0.016);\n"
    "    vec3 t1 = texture(uRippleTex, uv1).rgb * 2.0 - 1.0;\n"
    "    vec3 t2 = texture(uRippleTex, uv2).rgb * 2.0 - 1.0;\n"
    "    vec2 pert = (t1.xy + t2.xy) * uRippleStr;\n"
    "    vec3 N = normalize(vec3(pert.x, 1.0, pert.y));\n"
    "    vec3 V = normalize(uViewPos - vWorldPos);\n"
    "    vec3 R = reflect(-V, N);\n"
    "    vec3 sky = textureLod(uPrefilterMap, R, 1.5).rgb;\n"
    "    float fres = 0.02 + 0.98 * pow(1.0 - max(dot(N, V), 0.0), 5.0);\n"
    "    vec3 col = mix(uWaterTint, sky, fres);\n"
    "    vec3 H = normalize(V - uLightDir);\n"
    "    float glint = pow(max(dot(N, H), 0.0), 220.0);\n"
    "    col += uLightColor * glint * 2.5;\n"
    "    float rim = clamp((uPondR - length(vUV)) / max(uPondR*0.22, 0.01), 0.0, 1.0);\n"
    "    FragColor = vec4(col, uWaterAlpha * rim);\n"
    "}\n";
#endif /* SOL_RHI_METAL — the water */

/* --- the particle shader (P4 item 7): BILLBOARDS — one shared unit quad,
   expanded in the vertex shader along the camera's own right/up so every
   quad faces the eye exactly. Those axes are FREE: the view matrix is the
   camera's inverse, the inverse of a rotation is its transpose, so the
   camera's world-space basis sits in uView's ROWS. The fragment is a soft
   disc computed from the quad's UVs ((1-d^2)^2, clamped — beyond d=1 the
   parabola climbs again) — no texture, no sampler. Additive output
   premultiplies rgb by alpha*falloff; the written alpha is never read
   (GL_ONE/GL_ONE). Drawn in the HDR pass: rgb > 1 sails into bloom. */
#ifdef SOL_RHI_METAL
static const char *PARTICLE_VERTEX_SRC =      /* the camera's basis still sits
                                       in uView's ROWS — float4x4 indexes
                                       columns in MSL exactly as GLSL does */
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float2 corner [[attribute(0)]];\n"
    "    float4 inst   [[attribute(1)]];\n"
    "    float4 color  [[attribute(2)]];\n"
    "};\n"
    "struct VU { float4x4 uView; float4x4 uProj; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; float4 color; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float3 right = float3(u.uView[0][0], u.uView[1][0], u.uView[2][0]);\n"
    "    float3 up    = float3(u.uView[0][1], u.uView[1][1], u.uView[2][1]);\n"
    "    float3 world = v.inst.xyz + right * (v.corner.x * v.inst.w)\n"
    "                             + up    * (v.corner.y * v.inst.w);\n"
    "    o.uv    = v.corner * 2.0;\n"
    "    o.color = v.color;\n"
    "    o.pos = u.uProj * (u.uView * float4(world, 1.0));\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    return o;\n"
    "}\n";
static const char *PARTICLE_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; float4 color; };\n"
    "struct PU { float uNear; float uFar; };\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      constant PU &u [[buffer(0)]],\n"
    "                      texture2d<float> uSceneDepth [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float f = max(1.0 - dot(v.uv, v.uv), 0.0);\n"
    "    f = f * f;\n"
    "    float2 uv = v.pos.xy / float2(uSceneDepth.get_width(), uSceneDepth.get_height());\n"
    "    float sd = uSceneDepth.sample(s0, uv).r;\n"          /* P8 item 4: soft fade near geometry */
    "    float ls = (2.0*u.uNear*u.uFar)/(u.uFar+u.uNear-(2.0*sd-1.0)*(u.uFar-u.uNear));\n"
    "    float lp = (2.0*u.uNear*u.uFar)/(u.uFar+u.uNear-(2.0*v.pos.z-1.0)*(u.uFar-u.uNear));\n"
    "    float soft = clamp((ls - lp) / 0.5, 0.0, 1.0);\n"
    "    return float4(v.color.rgb * (v.color.a * f * soft), 1.0);\n"
    "}\n";
#else /* GLSL */
static const char *PARTICLE_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec2 aCorner;\n"     /* the unit quad, +-0.5 */
    "layout (location = 1) in vec4 aInst;\n"       /* xyz = world pos, w = size */
    "layout (location = 2) in vec4 aColor;\n"      /* rgba; envelope already in a */
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "out vec2 vUV;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);\n"  /* row 0 */
    "    vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);\n"  /* row 1 */
    "    vec3 world = aInst.xyz + right * (aCorner.x * aInst.w)\n"
    "                           + up    * (aCorner.y * aInst.w);\n"
    "    vUV    = aCorner * 2.0;\n"                /* -1..1 across the quad */
    "    vColor = aColor;\n"
    "    gl_Position = uProj * uView * vec4(world, 1.0);\n"
    "}\n";
static const char *PARTICLE_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uSceneDepth;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    float f = max(1.0 - dot(vUV, vUV), 0.0);\n"
    "    f = f * f;\n"
    "    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uSceneDepth, 0));\n"  /* P8 item 4: soft fade */
    "    float sd = texture(uSceneDepth, uv).r;\n"
    "    float ls = (2.0*uNear*uFar)/(uFar+uNear-(2.0*sd-1.0)*(uFar-uNear));\n"
    "    float lp = (2.0*uNear*uFar)/(uFar+uNear-(2.0*gl_FragCoord.z-1.0)*(uFar-uNear));\n"
    "    float soft = clamp((ls - lp) / 0.5, 0.0, 1.0);\n"
    "    FragColor = vec4(vColor.rgb * (vColor.a * f * soft), 1.0);\n"
    "}\n";
#endif /* SOL_RHI_METAL — the particles */

#ifdef SOL_RHI_METAL

/* Full-fidelity twins, GLSL-identical math: the backend's negative-height
   viewport gives every offscreen target GL's row layout, so sampling needs
   no flips (stage c/d retired stage b's per-twin v-flip). */
static const char *POST_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut vmain(uint vid [[vertex_id]]) {\n"
    "    VOut o;\n"
    "    float2 p = float2((float)((vid << 1) & 2), (float)(vid & 2));\n"
    "    o.uv = p;\n"
    "    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    return o;\n"
    "}\n";

static const char *POST_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct FU { float4x4 uInvViewProj; float3 uFogColor; float3 uAerialColor;\n"
    "            float3 uCamPos; float uBloomStrength; float uExposure;\n"
    "            float uFogStrength; float uFogDensity; float uFogHeight; float uFogFalloff;\n"
    "            float3 uGradeTint; float uGradeContrast; float uGradeSaturation;\n"
    "            float uVignetteStrength; float uVignetteRadius; float uLutMix; };\n"
    "static float3 aces(float3 x) {\n"            /* Narkowicz ACES filmic fit */
    "    return clamp((x * (2.51*x + 0.03)) / (x * (2.43*x + 0.59) + 0.14), 0.0, 1.0);\n"
    "}\n"
    "static float height_fog(constant FU &u, float3 P) {\n"      /* P8 item 2: analytic height fog */
    "    float L  = length(P - u.uCamPos);\n"
    "    float dY = P.y - u.uCamPos.y;\n"
    "    float c  = u.uFogDensity * exp(-u.uFogFalloff * (u.uCamPos.y - u.uFogHeight));\n"
    "    float bd = u.uFogFalloff * dY;\n"
    "    float ig = (abs(bd) > 1e-4) ? (1.0 - exp(-bd)) / bd : 1.0;\n"
    "    return 1.0 - exp(-L * c * ig);\n"
    "}\n"
    "static float3 lut3(texture2d<float> lut, sampler s, float3 c) {\n"  /* P9 item 1: 2D-strip LUT, trilinear by hand */
    "    float N = 16.0, W = N * N;\n"
    "    c = clamp(c, 0.0, 1.0);\n"
    "    float bf = c.b * (N - 1.0);\n"
    "    float b0 = floor(bf), bfrac = bf - b0;\n"
    "    float b1 = min(b0 + 1.0, N - 1.0);\n"
    "    float u0 = (b0 * N + 0.5 + c.r * (N - 1.0)) / W;\n"           /* half-texel insets keep each tap inside its blue slice */
    "    float u1 = (b1 * N + 0.5 + c.r * (N - 1.0)) / W;\n"
    "    float vv = (c.g * (N - 1.0) + 0.5) / N;\n"
    "    float3 a = lut.sample(s, float2(u0, vv), level(0)).rgb;\n"     /* level(0): ignore GL's mip chain */
    "    float3 b = lut.sample(s, float2(u1, vv), level(0)).rgb;\n"
    "    return mix(a, b, bfrac);\n"
    "}\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float> uHdr   [[texture(0)]],\n"
    "                      texture2d<float> uBloom [[texture(1)]],\n"
    "                      depth2d<float>   uDepth [[texture(2)]],\n"
    "                      texture2d<float> uGodray [[texture(3)]],\n"
    "                      texture2d<float> uAO     [[texture(4)]],\n"
    "                      texture2d<float> uLut    [[texture(5)]],\n"
    "                      sampler s0 [[sampler(0)]],\n"
    "                      sampler s1 [[sampler(1)]],\n"
    "                      sampler s2 [[sampler(2)]],\n"
    "                      sampler s3 [[sampler(3)]],\n"
    "                      sampler s4 [[sampler(4)]],\n"
    "                      sampler s5 [[sampler(5)]]) {\n"
    "    float3 hdr    = uHdr.sample(s0, v.uv).rgb * uAO.sample(s4, v.uv).r\n"  /* P8 item 5: AO on the lit scene only */
    "                  + uBloom.sample(s1, v.uv).rgb * u.uBloomStrength\n"
    "                  + uGodray.sample(s3, v.uv).rgb;\n"
    "    hdr          *= u.uExposure;\n"
    "    float d       = uDepth.sample(s2, v.uv);\n"
    "    if (d < 1.0) {\n"                                    /* skip the sky (far plane) */
    "        float4 wp = u.uInvViewProj * float4(v.uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "        hdr       = mix(hdr, u.uAerialColor, height_fog(u, wp.xyz / wp.w));\n"
    "    }\n"
    "    float3 mapped = aces(hdr);\n"
    "    float3 ldr    = pow(mapped, float3(1.0 / 2.2));\n"
    "    ldr           = mix(ldr, u.uFogColor, u.uFogStrength);\n"   /* under-water tint */
    "    ldr *= u.uGradeTint;\n"                                     /* P9 item 1: color grade */
    "    ldr  = (ldr - 0.5) * u.uGradeContrast + 0.5;\n"
    "    float glum = dot(ldr, float3(0.2126, 0.7152, 0.0722));\n"
    "    ldr  = mix(float3(glum), ldr, u.uGradeSaturation);\n"
    "    ldr  = mix(ldr, lut3(uLut, s5, ldr), u.uLutMix);\n"          /* P9 item 1: the baked LUT look */
    "    float2 vd = v.uv - 0.5;\n"
    "    float vig = 1.0 - u.uVignetteStrength * smoothstep(u.uVignetteRadius, 1.0, length(vd) * 1.41421356);\n"
    "    ldr *= vig;\n"
    "    return float4(clamp(ldr, 0.0, 1.0), 1.0);\n"
    "}\n";

#else /* GLSL */

static const char *POST_VERTEX_SRC =
    "#version 330 core\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"  /* (0,0) (2,0) (0,2) */
    "    vUV = p;\n"                                                  /* 0..2, interps 0..1 on screen */
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"             /* (-1,-1) (3,-1) (-1,3) */
    "}\n";

static const char *POST_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uHdr;\n"
    "uniform sampler2D uBloom;\n"                            /* the chain's top level (P4 i5) */
    "uniform float uBloomStrength;\n"
    "uniform float uExposure;\n"
    "uniform vec3 uFogColor;\n"
    "uniform float uFogStrength;\n"   /* under-water tint (item 8) */
    "uniform sampler2D uDepth;\n"                            /* scene depth (P8 item 2) */
    "uniform mat4 uInvViewProj;\n"
    "uniform vec3 uCamPos;\n"
    "uniform vec3 uAerialColor;\n"                           /* horizon haze color */
    "uniform float uFogDensity;\n"                           /* 0 = no atmospheric fog */
    "uniform float uFogHeight;\n"
    "uniform float uFogFalloff;\n"
    "uniform sampler2D uGodray;\n"                           /* P8 item 3: the shafts */
    "uniform sampler2D uAO;\n"                               /* P8 item 5: ambient occlusion */
    "uniform vec3 uGradeTint;\n"                             /* P9 item 1: color grade */
    "uniform float uGradeContrast;\n"
    "uniform float uGradeSaturation;\n"
    "uniform float uVignetteStrength;\n"
    "uniform float uVignetteRadius;\n"
    "uniform sampler2D uLut;\n"                              /* P9 item 1: 2D-strip color LUT */
    "uniform float uLutMix;\n"
    "out vec4 FragColor;\n"
    "vec3 aces(vec3 x) {\n"                                  /* Narkowicz ACES filmic fit */
    "    return clamp((x * (2.51*x + 0.03)) / (x * (2.43*x + 0.59) + 0.14), 0.0, 1.0);\n"
    "}\n"
    "float height_fog(vec3 P) {\n"                           /* P8 item 2: analytic height fog */
    "    float L  = length(P - uCamPos);\n"
    "    float dY = P.y - uCamPos.y;\n"
    "    float c  = uFogDensity * exp(-uFogFalloff * (uCamPos.y - uFogHeight));\n"
    "    float bd = uFogFalloff * dY;\n"
    "    float ig = (abs(bd) > 1e-4) ? (1.0 - exp(-bd)) / bd : 1.0;\n"
    "    return 1.0 - exp(-L * c * ig);\n"
    "}\n"
    "vec3 lut3(vec3 c) {\n"                                  /* P9 item 1: 2D-strip LUT, trilinear by hand */
    "    float N = 16.0, W = N * N;\n"
    "    c = clamp(c, 0.0, 1.0);\n"
    "    float bf = c.b * (N - 1.0);\n"
    "    float b0 = floor(bf), bfrac = bf - b0;\n"
    "    float b1 = min(b0 + 1.0, N - 1.0);\n"
    "    float u0 = (b0 * N + 0.5 + c.r * (N - 1.0)) / W;\n"  /* half-texel insets keep each tap inside its blue slice */
    "    float u1 = (b1 * N + 0.5 + c.r * (N - 1.0)) / W;\n"
    "    float vv = (c.g * (N - 1.0) + 0.5) / N;\n"
    "    vec3 a = textureLod(uLut, vec2(u0, vv), 0.0).rgb;\n"  /* LOD 0: ignore GL's mip chain */
    "    vec3 b = textureLod(uLut, vec2(u1, vv), 0.0).rgb;\n"
    "    return mix(a, b, bfrac);\n"
    "}\n"
    "void main() {\n"
    "    vec3 hdr    = texture(uHdr, vUV).rgb * texture(uAO, vUV).r\n"  /* P8 item 5: AO on the lit scene only */
    "                + texture(uBloom, vUV).rgb * uBloomStrength\n"
    "                + texture(uGodray, vUV).rgb;\n"  /* the glow is
                                                                radiance too: add BEFORE
                                                                exposure + tonemap so ACES
                                                                rolls it off naturally */
    "    hdr        *= uExposure;\n"
    "    float d     = texture(uDepth, vUV).r;\n"            /* [0,1] nonlinear */
    "    if (d < 1.0) {\n"                                /* skip the sky (far plane) */
    "        vec4 wp = uInvViewProj * vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "        hdr     = mix(hdr, uAerialColor, height_fog(wp.xyz / wp.w));\n"
    "    }\n"
    "    vec3 mapped = aces(hdr);\n"                           /* tonemap: roll off HDR -> [0,1] */
    "    vec3 ldr    = pow(mapped, vec3(1.0 / 2.2));\n"        /* linear -> sRGB for display */
    "    ldr         = mix(ldr, uFogColor, uFogStrength);\n"   /* wading under the surface */
    "    ldr        *= uGradeTint;\n"                          /* P9 item 1: color grade */
    "    ldr         = (ldr - 0.5) * uGradeContrast + 0.5;\n"
    "    float glum  = dot(ldr, vec3(0.2126, 0.7152, 0.0722));\n"
    "    ldr         = mix(vec3(glum), ldr, uGradeSaturation);\n"
    "    ldr         = mix(ldr, lut3(ldr), uLutMix);\n"             /* P9 item 1: the baked LUT look */
    "    vec2 vd     = vUV - 0.5;\n"
    "    float vig   = 1.0 - uVignetteStrength * smoothstep(uVignetteRadius, 1.0, length(vd) * 1.41421356);\n"
    "    ldr        *= vig;\n"
    "    FragColor   = vec4(clamp(ldr, 0.0, 1.0), 1.0);\n"
    "}\n";

#endif /* SOL_RHI_METAL — the post pair */

/* --- P9 item 3: weathering decals — an UNLIT textured-alpha quad. The atlas
   carries color + an alpha mask (stains dark, moss green); alpha-over into the
   HDR buffer modulates the lit wall. Reuses the 12-float mesh layout (reads
   only pos + uv). --- */
#ifdef SOL_RHI_METAL
static const char *DECAL_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float3 pos [[attribute(0)]]; float2 uv [[attribute(2)]]; };\n"
    "struct DU { float4x4 uModel; float4x4 uView; float4x4 uProj; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant DU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float4 wp = u.uModel * float4(v.pos, 1.0);\n"
    "    o.pos = u.uProj * (u.uView * wp);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"
    "    o.uv = v.uv;\n"
    "    return o;\n"
    "}\n";
static const char *DECAL_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      texture2d<float> uDecalAtlas [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float4 t = uDecalAtlas.sample(s0, v.uv);\n"
    "    return float4(t.rgb, t.a);\n"
    "}\n";
#else /* GLSL */
static const char *DECAL_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aUV;\n"
    "    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);\n"
    "}\n";
static const char *DECAL_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uDecalAtlas;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    vec4 t = texture(uDecalAtlas, vUV);\n"
    "    FragColor = vec4(t.rgb, t.a);\n"
    "}\n";
#endif /* SOL_RHI_METAL — the decal pair */

/* the portal energy pane (Portal Material): an UNLIT, opaque procedural membrane
   — domain-warped value-noise swirl animated by uTime, dark->bright by the swirl,
   with a bright glow toward the frame edge. Modeled on the water twin. */
#ifdef SOL_RHI_METAL
static const char *PORTAL_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float3 pos [[attribute(0)]]; float3 normal [[attribute(1)]];\n"
    "             float2 uv [[attribute(2)]]; float4 tangent [[attribute(3)]]; };\n"
    "struct VU { float4x4 uModel; float4x4 uView; float4x4 uProj; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float4 wp = u.uModel * float4(v.pos, 1.0);\n"
    "    o.uv = v.uv;\n"
    "    o.pos = u.uProj * (u.uView * wp);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"
    "    return o;\n"
    "}\n";
static const char *PORTAL_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct FU { float3 uPortalColor; float uTime; };\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "static float phash(float2 p){ p = fract(p*float2(123.34,456.21));\n"
    "    p += dot(p, p+45.32); return fract(p.x*p.y); }\n"
    "static float pnoise(float2 p){\n"
    "    float2 i = floor(p), f = fract(p);\n"
    "    float a = phash(i), b = phash(i+float2(1.0,0.0));\n"
    "    float c = phash(i+float2(0.0,1.0)), d = phash(i+float2(1.0,1.0));\n"
    "    float2 u = f*f*(3.0-2.0*f);\n"
    "    return mix(mix(a,b,u.x), mix(c,d,u.x), u.y); }\n"
    "static float pfbm(float2 p){ return 0.6*pnoise(p) + 0.4*pnoise(p*2.03 + 7.1); }\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]]) {\n"
    "    float2 p = v.uv * 3.0;\n"
    "    float t = u.uTime * 0.3;\n"
    "    float2 warp = float2(pnoise(p + t), pnoise(p - t*0.8 + 5.2));\n"
    "    float n = pfbm(p + warp*1.5 + t*0.5);\n"
    "    float2 c = v.uv - 0.5;\n"
    "    float edge = smoothstep(0.20, 0.50, max(abs(c.x), abs(c.y)));\n"
    "    float3 col = u.uPortalColor * mix(0.25, 1.0, n);\n"
    "    col += float3(0.30, 0.50, 0.95) * edge * 0.8;\n"
    "    return float4(col, 1.0);\n"
    "}\n";
#else /* GLSL */
static const char *PORTAL_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "layout (location = 3) in vec4 aTangent;\n"
    "uniform mat4 uModel; uniform mat4 uView; uniform mat4 uProj;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aUV;\n"
    "    gl_Position = uProj * uView * uModel * vec4(aPos, 1.0);\n"
    "}\n";
static const char *PORTAL_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform vec3 uPortalColor;\n"
    "uniform float uTime;\n"
    "out vec4 FragColor;\n"
    "float phash(vec2 p){ p = fract(p*vec2(123.34,456.21));\n"
    "    p += dot(p, p+45.32); return fract(p.x*p.y); }\n"
    "float pnoise(vec2 p){\n"
    "    vec2 i = floor(p), f = fract(p);\n"
    "    float a = phash(i), b = phash(i+vec2(1.0,0.0));\n"
    "    float c = phash(i+vec2(0.0,1.0)), d = phash(i+vec2(1.0,1.0));\n"
    "    vec2 u = f*f*(3.0-2.0*f);\n"
    "    return mix(mix(a,b,u.x), mix(c,d,u.x), u.y); }\n"
    "float pfbm(vec2 p){ return 0.6*pnoise(p) + 0.4*pnoise(p*2.03 + 7.1); }\n"
    "void main() {\n"
    "    vec2 p = vUV * 3.0;\n"
    "    float t = uTime * 0.3;\n"
    "    vec2 warp = vec2(pnoise(p + t), pnoise(p - t*0.8 + 5.2));\n"
    "    float n = pfbm(p + warp*1.5 + t*0.5);\n"
    "    vec2 c = vUV - 0.5;\n"
    "    float edge = smoothstep(0.20, 0.50, max(abs(c.x), abs(c.y)));\n"
    "    vec3 col = uPortalColor * mix(0.25, 1.0, n);\n"
    "    col += vec3(0.30, 0.50, 0.95) * edge * 0.8;\n"
    "    FragColor = vec4(col, 1.0);\n"
    "}\n";
#endif /* SOL_RHI_METAL — the portal energy pane */

/* --- god-rays (P8 item 3): march the view ray through the spot-light's
   shadow volume, accumulating in-scatter wherever the ray is LIT. The medium
   is the SAME height-fog field as item 2 (lit, not ambient). Crude on purpose
   per the no-TAA constitution: 48 steps, a per-pixel dithered start (the slow
   camera forgives the noise), ONE shadow tap per step, no temporal reuse.
   Half-res; reconstructs world pos like item 2; shares POST_VERTEX_SRC. The
   result is added in post like a bloom level. --- */
#ifdef SOL_RHI_METAL
static const char *GODRAY_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct GU { float4x4 uInvViewProj; float4x4 uLightVP; float4x4 uLightVP1;\n"
    "            float3 uCamPos; float3 uLightColor; float3 uLightDir;\n"
    "            float uFogDensity; float uFogHeight; float uFogFalloff;\n"
    "            float uGodrayIntensity; };\n"
    "static float hash12(float2 p) {\n"                       /* Dave Hoskins hash */
    "    float3 p3 = fract(float3(p.xyx) * 0.1031);\n"
    "    p3 += dot(p3, p3.yzx + 33.33);\n"
    "    return fract((p3.x + p3.y) * p3.z);\n"
    "}\n"
    "static float hg(float c, float g) {\n"                   /* Henyey-Greenstein, 1/4pi folded in */
    "    float gg = g * g;\n"
    "    return (0.0795775 * (1.0 - gg)) / pow(1.0 + gg - 2.0 * g * c, 1.5);\n"
    "}\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      constant GU &u [[buffer(0)]],\n"
    "                      depth2d<float> uDepth      [[texture(0)]],\n"
    "                      depth2d<float> uShadowMap  [[texture(1)]],\n"
    "                      depth2d<float> uShadowMap1 [[texture(8)]],\n"   /* far cascade (P8 item 6) */
    "                      sampler s0 [[sampler(0)]],\n"
    "                      sampler s1 [[sampler(1)]],\n"
    "                      sampler s8 [[sampler(8)]]) {\n"
    "    float  d  = uDepth.sample(s0, v.uv);\n"
    "    float4 wp = u.uInvViewProj * float4(v.uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "    float3 P  = wp.xyz / wp.w;\n"                         /* geometry, or far-plane for sky */
    "    float3 dir = P - u.uCamPos;\n"
    "    float  len = length(dir);\n"
    "    dir = (len > 1e-4) ? dir / len : float3(0.0, 0.0, 1.0);\n"
    "    const int N = 48;\n"
    "    float stp = min(len, 45.0) / float(N);\n"   /* P8.6: god-rays are a NEAR-field effect — cap the
                                                        march so a long ray into the open doesn't pile a
                                                        giant lit column onto the sun-direction hotspot */
    "    float dither = hash12(v.uv * 4096.0);\n"
    "    float accum = 0.0;\n"
    "    int i;\n"
    "    for (i = 0; i < N; i++) {\n"
    "        float3 s = u.uCamPos + dir * (float(i) + dither) * stp;\n"
    "        float lit = 1.0;\n"                               /* directional sun: lit unless a cascade shadows it (P8 item 6) */
    "        float4 l0 = u.uLightVP * float4(s, 1.0);\n"
    "        float3 p0 = l0.xyz / l0.w * 0.5 + 0.5;\n"
    "        if (p0.x > 0.0 && p0.x < 1.0 && p0.y > 0.0 && p0.y < 1.0 && p0.z <= 1.0) {\n"
    "            lit = (p0.z - 0.002 > uShadowMap.sample(s1, p0.xy)) ? 0.0 : 1.0;\n"
    "        } else {\n"
    "            float4 l1 = u.uLightVP1 * float4(s, 1.0);\n"
    "            float3 p1 = l1.xyz / l1.w * 0.5 + 0.5;\n"
    "            if (p1.x > 0.0 && p1.x < 1.0 && p1.y > 0.0 && p1.y < 1.0 && p1.z <= 1.0)\n"
    "                lit = (p1.z - 0.002 > uShadowMap1.sample(s8, p1.xy)) ? 0.0 : 1.0;\n"
    "        }\n"
    "        float rho = u.uFogDensity * exp(-u.uFogFalloff * max(s.y - u.uFogHeight, 0.0));\n"   /* P8.6: a LAYER — thins upward, never thickens below (no runaway beams beneath floating islands) */
    "        accum += lit * rho * stp;\n"
    "    }\n"
    "    float phase = hg(dot(dir, u.uLightDir), 0.6);\n"      /* forward scatter toward the source */
    "    float3 col = accum * phase * u.uLightColor * u.uGodrayIntensity;\n"
    "    return float4(col, 1.0);\n"
    "}\n";
#else
static const char *GODRAY_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uDepth;\n"
    "uniform sampler2D uShadowMap;\n"                         /* near cascade (P8 item 6) */
    "uniform sampler2D uShadowMap1;\n"                        /* far cascade */
    "uniform mat4 uInvViewProj;\n"
    "uniform mat4 uLightVP;\n"
    "uniform mat4 uLightVP1;\n"
    "uniform vec3 uCamPos;\n"
    "uniform vec3 uLightColor;\n"
    "uniform vec3 uLightDir;\n"
    "uniform float uFogDensity;\n"
    "uniform float uFogHeight;\n"
    "uniform float uFogFalloff;\n"
    "uniform float uGodrayIntensity;\n"
    "out vec4 FragColor;\n"
    "float hash12(vec2 p) {\n"                                /* Dave Hoskins hash */
    "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
    "    p3 += dot(p3, p3.yzx + 33.33);\n"
    "    return fract((p3.x + p3.y) * p3.z);\n"
    "}\n"
    "float hg(float c, float g) {\n"                          /* Henyey-Greenstein, 1/4pi folded in */
    "    float gg = g * g;\n"
    "    return (0.0795775 * (1.0 - gg)) / pow(1.0 + gg - 2.0 * g * c, 1.5);\n"
    "}\n"
    "void main() {\n"
    "    float d   = texture(uDepth, vUV).r;\n"
    "    vec4 wp = uInvViewProj * vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
    "    vec3 P  = wp.xyz / wp.w;\n"                           /* geometry, or far-plane for sky */
    "    vec3 dir = P - uCamPos;\n"
    "    float len = length(dir);\n"
    "    dir = (len > 1e-4) ? dir / len : vec3(0.0, 0.0, 1.0);\n"
    "    const int N = 48;\n"
    "    float stp = min(len, 45.0) / float(N);\n"   /* P8.6: god-rays are a NEAR-field effect — cap the
                                                        march so a long ray into the open doesn't pile a
                                                        giant lit column onto the sun-direction hotspot */
    "    float dither = hash12(vUV * 4096.0);\n"
    "    float accum = 0.0;\n"
    "    int i;\n"
    "    for (i = 0; i < N; i++) {\n"
    "        vec3 s = uCamPos + dir * (float(i) + dither) * stp;\n"
    "        float lit = 1.0;\n"                               /* directional sun: lit unless a cascade shadows it (P8 item 6) */
    "        vec4 l0 = uLightVP * vec4(s, 1.0);\n"
    "        vec3 p0 = l0.xyz / l0.w * 0.5 + 0.5;\n"
    "        if (p0.x > 0.0 && p0.x < 1.0 && p0.y > 0.0 && p0.y < 1.0 && p0.z <= 1.0) {\n"
    "            lit = (p0.z - 0.002 > texture(uShadowMap, p0.xy).r) ? 0.0 : 1.0;\n"
    "        } else {\n"
    "            vec4 l1 = uLightVP1 * vec4(s, 1.0);\n"
    "            vec3 p1 = l1.xyz / l1.w * 0.5 + 0.5;\n"
    "            if (p1.x > 0.0 && p1.x < 1.0 && p1.y > 0.0 && p1.y < 1.0 && p1.z <= 1.0)\n"
    "                lit = (p1.z - 0.002 > texture(uShadowMap1, p1.xy).r) ? 0.0 : 1.0;\n"
    "        }\n"
    "        float rho = uFogDensity * exp(-uFogFalloff * max(s.y - uFogHeight, 0.0));\n"   /* P8.6: a LAYER — thins upward, never thickens below (no runaway beams beneath floating islands) */
    "        accum += lit * rho * stp;\n"
    "    }\n"
    "    float phase = hg(dot(dir, uLightDir), 0.6);\n"        /* forward scatter toward the source */
    "    vec3 col = accum * phase * uLightColor * uGodrayIntensity;\n"
    "    FragColor = vec4(col, 1.0);\n"
    "}\n";
#endif

/* --- soft particles (P8 item 4): a trivial depth copy. A particle pass can't
   sample the depth attachment it draws into (a feedback loop), so we copy the
   scene depth to a readable color texture first; the particle FS reads THIS
   for its soft fade. Shares POST_VERTEX_SRC. --- */
#ifdef SOL_RHI_METAL
static const char *DEPTHCOPY_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      depth2d<float> uDepth [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    return float4(uDepth.sample(s0, v.uv));\n"
    "}\n";
#else
static const char *DEPTHCOPY_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uDepth;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    FragColor = vec4(texture(uDepth, vUV).r);\n"
    "}\n";
#endif

/* --- SSAO (P8 item 5): hemisphere ambient occlusion. Reconstruct view pos +
   a derivative normal from the depth, sample a baked 16-pt hemisphere kernel
   (per-pixel hash-rotated), count how many land inside closer geometry within
   uRadius (a range check kills haloing across big gaps). Then a bilateral blur
   denoises. Half-res; post-multiplied into the lit scene. Shares
   POST_VERTEX_SRC. --- */
#ifdef SOL_RHI_METAL
static const char *SSAO_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct SU { float4x4 uProj; float4x4 uInvProj; float uRadius; float uBias; float uStrength; };\n"
    "static float3 vpos(float2 uv, depth2d<float> dT, sampler s, constant SU &u) {\n"
    "    float d = dT.sample(s, uv);\n"
    "    float4 c = u.uInvProj * float4(uv*2.0-1.0, d*2.0-1.0, 1.0);\n"
    "    return c.xyz / c.w;\n"
    "}\n"
    "static float h12(float2 p) {\n"
    "    float3 p3 = fract(float3(p.xyx) * 0.1031);\n"
    "    p3 += dot(p3, p3.yzx + 33.33);\n"
    "    return fract((p3.x + p3.y) * p3.z);\n"
    "}\n"
    "static float3 kern(int i) {\n"                              /* a hemisphere spiral, clustered near the origin */
    "    float fi = (float(i) + 0.5) / 16.0;\n"
    "    float z = fi; float r = sqrt(max(0.0, 1.0 - z*z));\n"
    "    float a = float(i) * 2.4;\n"
    "    return float3(cos(a)*r, sin(a)*r, z) * mix(0.1, 1.0, fi*fi);\n"
    "}\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      constant SU &u [[buffer(0)]],\n"
    "                      depth2d<float> uDepth [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float d = uDepth.sample(s0, v.uv);\n"
    "    if (d >= 1.0) return float4(1.0);\n"                    /* sky: no occlusion */
    "    float3 P = vpos(v.uv, uDepth, s0, u);\n"
    "    float2 e = 1.5 / float2(uDepth.get_width(), uDepth.get_height());\n"  /* best-neighbor normal (P8 item 5) */
    "    float3 Pr = vpos(v.uv + float2(e.x,0.0), uDepth, s0, u);\n"
    "    float3 Pl = vpos(v.uv - float2(e.x,0.0), uDepth, s0, u);\n"
    "    float3 Pu = vpos(v.uv + float2(0.0,e.y), uDepth, s0, u);\n"
    "    float3 Pd = vpos(v.uv - float2(0.0,e.y), uDepth, s0, u);\n"
    "    float3 ddx = abs(Pr.z - P.z) < abs(Pl.z - P.z) ? (Pr - P) : (P - Pl);\n"
    "    float3 ddy = abs(Pu.z - P.z) < abs(Pd.z - P.z) ? (Pu - P) : (P - Pd);\n"
    "    float3 Nrm = normalize(cross(ddx, ddy));\n"
    "    float rot = h12(fmod(v.pos.xy, 4.0)) * 6.2831853;\n"    /* 4x4 tile, killed exactly by the 4x4 box blur */
    "    float3 rv = float3(cos(rot), sin(rot), 0.0);\n"
    "    float3 T = normalize(rv - Nrm * dot(rv, Nrm));\n"
    "    float3x3 TBN = float3x3(T, cross(Nrm, T), Nrm);\n"
    "    float occ = 0.0;\n"
    "    for (int i = 0; i < 16; i++) {\n"
    "        float3 sp = P + (TBN * kern(i)) * u.uRadius;\n"
    "        float4 off = u.uProj * float4(sp, 1.0);\n"
    "        float2 suv = (off.xy / off.w) * 0.5 + 0.5;\n"
    "        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;\n"
    "        float sz = vpos(suv, uDepth, s0, u).z;\n"
    "        float rc = smoothstep(0.0, 1.0, u.uRadius / max(0.001, abs(P.z - sz)));\n"
    "        if (sz >= sp.z + u.uBias) occ += rc;\n"
    "    }\n"
    "    float ao = 1.0 - (occ / 16.0) * u.uStrength;\n"
    "    return float4(ao);\n"
    "}\n";
static const char *SSAO_BLUR_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct BU { float uNear; float uFar; };\n"
    "static float lin(float d, constant BU &u) {\n"
    "    return (2.0*u.uNear*u.uFar)/(u.uFar+u.uNear-(2.0*d-1.0)*(u.uFar-u.uNear));\n"
    "}\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      constant BU &u [[buffer(0)]],\n"
    "                      texture2d<float> uAO    [[texture(0)]],\n"
    "                      depth2d<float>   uDepth [[texture(1)]],\n"
    "                      sampler s0 [[sampler(0)]],\n"
    "                      sampler s1 [[sampler(1)]]) {\n"
    "    float2 texel = 1.0 / float2(uAO.get_width(), uAO.get_height());\n"
    "    float cd = lin(uDepth.sample(s1, v.uv), u);\n"          /* LINEAR depth: slope vs silhouette in metres */
    "    float sum = 0.0, wsum = 0.0;\n"
    "    for (int x = -2; x <= 1; x++)\n"
    "        for (int y = -2; y <= 1; y++) {\n"
    "            float2 uv = v.uv + float2(x, y) * texel;\n"
    "            float w = max(0.0, 1.0 - abs(lin(uDepth.sample(s1, uv), u) - cd) / 0.5);\n"
    "            sum += uAO.sample(s0, uv).r * w; wsum += w;\n"
    "        }\n"
    "    return float4(wsum > 0.0 ? sum / wsum : uAO.sample(s0, v.uv).r);\n"
    "}\n";
#else
static const char *SSAO_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uDepth;\n"
    "uniform mat4 uProj;\n"
    "uniform mat4 uInvProj;\n"
    "uniform float uRadius;\n"
    "uniform float uBias;\n"
    "uniform float uStrength;\n"
    "out vec4 FragColor;\n"
    "vec3 vpos(vec2 uv) {\n"
    "    float d = texture(uDepth, uv).r;\n"
    "    vec4 c = uInvProj * vec4(uv*2.0-1.0, d*2.0-1.0, 1.0);\n"
    "    return c.xyz / c.w;\n"
    "}\n"
    "float h12(vec2 p) {\n"
    "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
    "    p3 += dot(p3, p3.yzx + 33.33);\n"
    "    return fract((p3.x + p3.y) * p3.z);\n"
    "}\n"
    "vec3 kern(int i) {\n"                                       /* a hemisphere spiral, clustered near the origin */
    "    float fi = (float(i) + 0.5) / 16.0;\n"
    "    float z = fi; float r = sqrt(max(0.0, 1.0 - z*z));\n"
    "    float a = float(i) * 2.4;\n"
    "    return vec3(cos(a)*r, sin(a)*r, z) * mix(0.1, 1.0, fi*fi);\n"
    "}\n"
    "void main() {\n"
    "    float d = texture(uDepth, vUV).r;\n"
    "    if (d >= 1.0) { FragColor = vec4(1.0); return; }\n"     /* sky: no occlusion */
    "    vec3 P = vpos(vUV);\n"
    "    vec2 e = 1.5 / vec2(textureSize(uDepth, 0));\n"        /* best-neighbor normal (P8 item 5) */
    "    vec3 Pr = vpos(vUV + vec2(e.x,0.0)), Pl = vpos(vUV - vec2(e.x,0.0));\n"
    "    vec3 Pu = vpos(vUV + vec2(0.0,e.y)), Pd = vpos(vUV - vec2(0.0,e.y));\n"
    "    vec3 ddx = abs(Pr.z - P.z) < abs(Pl.z - P.z) ? (Pr - P) : (P - Pl);\n"
    "    vec3 ddy = abs(Pu.z - P.z) < abs(Pd.z - P.z) ? (Pu - P) : (P - Pd);\n"
    "    vec3 Nrm = normalize(cross(ddx, ddy));\n"
    "    float rot = h12(mod(gl_FragCoord.xy, 4.0)) * 6.2831853;\n"  /* 4x4 tile, killed exactly by the 4x4 box blur */
    "    vec3 rv = vec3(cos(rot), sin(rot), 0.0);\n"
    "    vec3 T = normalize(rv - Nrm * dot(rv, Nrm));\n"
    "    mat3 TBN = mat3(T, cross(Nrm, T), Nrm);\n"
    "    float occ = 0.0;\n"
    "    int i;\n"
    "    for (i = 0; i < 16; i++) {\n"
    "        vec3 sp = P + (TBN * kern(i)) * uRadius;\n"
    "        vec4 off = uProj * vec4(sp, 1.0);\n"
    "        vec2 suv = (off.xy / off.w) * 0.5 + 0.5;\n"
    "        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;\n"
    "        float sz = vpos(suv).z;\n"
    "        float rc = smoothstep(0.0, 1.0, uRadius / max(0.001, abs(P.z - sz)));\n"
    "        if (sz >= sp.z + uBias) occ += rc;\n"
    "    }\n"
    "    float ao = 1.0 - (occ / 16.0) * uStrength;\n"
    "    FragColor = vec4(ao);\n"
    "}\n";
static const char *SSAO_BLUR_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uAO;\n"
    "uniform sampler2D uDepth;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "out vec4 FragColor;\n"
    "float lin(float d) { return (2.0*uNear*uFar)/(uFar+uNear-(2.0*d-1.0)*(uFar-uNear)); }\n"
    "void main() {\n"
    "    vec2 texel = 1.0 / vec2(textureSize(uAO, 0));\n"
    "    float cd = lin(texture(uDepth, vUV).r);\n"              /* LINEAR depth: slope vs silhouette in metres */
    "    float sum = 0.0, wsum = 0.0;\n"
    "    int x, y;\n"
    "    for (x = -2; x <= 1; x++)\n"
    "        for (y = -2; y <= 1; y++) {\n"
    "            vec2 uv = vUV + vec2(x, y) * texel;\n"
    "            float w = max(0.0, 1.0 - abs(lin(texture(uDepth, uv).r) - cd) / 0.5);\n"
    "            sum += texture(uAO, uv).r * w; wsum += w;\n"
    "        }\n"
    "    FragColor = vec4(wsum > 0.0 ? sum / wsum : texture(uAO, vUV).r);\n"
    "}\n";
#endif

/* --- the bloom chain (P4 item 5 piece 3): how a screen says "brighter than
   white". Extract what exceeds the threshold (with a SOFT KNEE — a hard
   cut flickers as pixels cross it), walk DOWN a half-res chain (each level
   blurs a little; small blurs at low res compose into one huge cheap blur),
   then walk back UP with a 3x3 tent, ACCUMULATING additively into each
   level. The combine lives in the tonemap above. All fullscreen triangles
   through POST_VERTEX_SRC. --- */
#ifdef SOL_RHI_METAL

static const char *BLOOM_EXTRACT_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "fragment float4 fmain(VOut v [[stage_in]],\n"
    "                      texture2d<float> uSrc [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float3 c    = uSrc.sample(s0, v.uv).rgb;\n"
    "    float  b    = max(c.r, max(c.g, c.b));\n"
    "    float  soft = clamp(b - 1.0 + 0.5, 0.0, 1.0);\n"
    "    soft        = soft * soft / 2.0;\n"
    "    float  w    = max(b - 1.0, soft) / max(b, 1e-4);\n"
    "    return float4(c * w, 1.0);\n"
    "}\n";
static const char *BLOOM_DOWN_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct FU { float3 uTexel; };\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float> uSrc [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float2 t = u.uTexel.xy;\n"
    "    float3 c = uSrc.sample(s0, v.uv + float2(-t.x, -t.y)).rgb\n"
    "             + uSrc.sample(s0, v.uv + float2( t.x, -t.y)).rgb\n"
    "             + uSrc.sample(s0, v.uv + float2(-t.x,  t.y)).rgb\n"
    "             + uSrc.sample(s0, v.uv + float2( t.x,  t.y)).rgb;\n"
    "    return float4(c * 0.25, 1.0);\n"
    "}\n";
static const char *BLOOM_UP_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct FU { float3 uTexel; };\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float> uSrc [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float2 t = u.uTexel.xy;\n"
    "    float3 c = uSrc.sample(s0, v.uv + float2(-t.x, -t.y)).rgb * 1.0\n"
    "             + uSrc.sample(s0, v.uv + float2( 0.0, -t.y)).rgb * 2.0\n"
    "             + uSrc.sample(s0, v.uv + float2( t.x, -t.y)).rgb * 1.0\n"
    "             + uSrc.sample(s0, v.uv + float2(-t.x,  0.0)).rgb * 2.0\n"
    "             + uSrc.sample(s0, v.uv).rgb                     * 4.0\n"
    "             + uSrc.sample(s0, v.uv + float2( t.x,  0.0)).rgb * 2.0\n"
    "             + uSrc.sample(s0, v.uv + float2(-t.x,  t.y)).rgb * 1.0\n"
    "             + uSrc.sample(s0, v.uv + float2( 0.0,  t.y)).rgb * 2.0\n"
    "             + uSrc.sample(s0, v.uv + float2( t.x,  t.y)).rgb * 1.0;\n"
    "    return float4(c / 16.0, 1.0);\n"
    "}\n";

#else /* GLSL */

static const char *BLOOM_EXTRACT_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uSrc;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"                                        /* Karis soft threshold, T=1 K=0.5 */
    "    vec3  c    = texture(uSrc, vUV).rgb;\n"
    "    float b    = max(c.r, max(c.g, c.b));\n"
    "    float soft = clamp(b - 1.0 + 0.5, 0.0, 1.0);\n"     /* inside the knee... */
    "    soft       = soft * soft / 2.0;\n"                  /* ...rises smoothly */
    "    float w    = max(b - 1.0, soft) / max(b, 1e-4);\n"
    "    FragColor  = vec4(c * w, 1.0);\n"
    "}\n";
static const char *BLOOM_DOWN_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uSrc;\n"
    "uniform vec3 uTexel;\n"                                 /* 1/src size in xy */
    "out vec4 FragColor;\n"
    "void main() {\n"                                        /* 4 bilinear taps = 4x4 box */
    "    vec2 t = uTexel.xy;\n"
    "    vec3 c = texture(uSrc, vUV + vec2(-t.x, -t.y)).rgb\n"
    "           + texture(uSrc, vUV + vec2( t.x, -t.y)).rgb\n"
    "           + texture(uSrc, vUV + vec2(-t.x,  t.y)).rgb\n"
    "           + texture(uSrc, vUV + vec2( t.x,  t.y)).rgb;\n"
    "    FragColor = vec4(c * 0.25, 1.0);\n"
    "}\n";
static const char *BLOOM_UP_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uSrc;\n"
    "uniform vec3 uTexel;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"                                        /* 3x3 tent; ADDS via blend */
    "    vec2 t = uTexel.xy;\n"
    "    vec3 c = texture(uSrc, vUV + vec2(-t.x, -t.y)).rgb * 1.0\n"
    "           + texture(uSrc, vUV + vec2( 0.0, -t.y)).rgb * 2.0\n"
    "           + texture(uSrc, vUV + vec2( t.x, -t.y)).rgb * 1.0\n"
    "           + texture(uSrc, vUV + vec2(-t.x,  0.0)).rgb * 2.0\n"
    "           + texture(uSrc, vUV).rgb                    * 4.0\n"
    "           + texture(uSrc, vUV + vec2( t.x,  0.0)).rgb * 2.0\n"
    "           + texture(uSrc, vUV + vec2(-t.x,  t.y)).rgb * 1.0\n"
    "           + texture(uSrc, vUV + vec2( 0.0,  t.y)).rgb * 2.0\n"
    "           + texture(uSrc, vUV + vec2( t.x,  t.y)).rgb * 1.0;\n"
    "    FragColor = vec4(c / 16.0, 1.0);\n"
    "}\n";

#endif /* SOL_RHI_METAL — the bloom trio */

/* --- the shadow (depth-only) pass (item 9b): render the scene from the light's
   POV, writing only depth into the shadow map. No color work; position only,
   reading just attr 0 of the 12-float vertex. --- */
#ifdef SOL_RHI_METAL

/* Full-fidelity twins: depth-only, the fragment returns void (no color
   attachment on a depth target — the PSO is built with an Invalid color
   format, which is exactly what a void fragment requires). */
static const char *SHADOW_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn { float3 pos [[attribute(0)]]; };\n"
    "struct VU { float4x4 uModel; float4x4 uLightVP; };\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    o.pos = u.uLightVP * (u.uModel * float4(v.pos, 1.0));\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal: the
                                          stored depth = GL's 0.5*ndc+0.5,
                                          so the shadow COMPARE in the PBR
                                          twin stays GLSL-identical */
    "    return o;\n"
    "}\n";

static const char *SHADOW_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "fragment void fmain() {}\n";                  /* depth written automatically */

#else /* GLSL */

static const char *SHADOW_VERTEX_SRC =
    "#version 330 core\n"
    "layout(location = 0) in vec3 aPos;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uLightVP;\n"                               /* light proj * light view */
    "void main() {\n"
    "    gl_Position = uLightVP * uModel * vec4(aPos, 1.0);\n"
    "}\n";

static const char *SHADOW_FRAGMENT_SRC =
    "#version 330 core\n"
    "void main() {}\n";                                      /* depth written automatically */

#endif /* SOL_RHI_METAL — the shadow pair */

/* --- the skinned twins (P4 item 9): the standard VS plus the MATRIX
   PALETTE. Each vertex blends up to four palette entries (linear blend
   skinning); the palette returns vertices in mesh-local space, so uModel
   — the OBJECT's transform — goes on top exactly like any prop's. The
   shadow twin does the same blend into the light's clip space: a running
   fox must not cast a T-posed shadow. uPalette[64] = GL 3.3's guaranteed
   vertex-uniform budget, exactly. Same FRAGMENT_SRC as everything else —
   a skinned surface is lit like any surface. */
#ifdef SOL_RHI_METAL
static const char *SKINNED_VERTEX_SRC =       /* uPalette[64] = 4KB: the uniform
                                       arena's boundary case, by design */
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float3 pos     [[attribute(0)]];\n"
    "    float3 normal  [[attribute(1)]];\n"
    "    float2 uv      [[attribute(2)]];\n"
    "    float4 tangent [[attribute(3)]];\n"
    "    float4 joints  [[attribute(4)]];\n"
    "    float4 weights [[attribute(5)]];\n"
    "};\n"
    "struct VU {\n"
    "    float4x4 uModel;\n"
    "    float4x4 uView;\n"
    "    float4x4 uProj;\n"
    "    float3x3 uNormalMatrix;\n"
    "    float4x4 uPalette[64];\n"
    "};\n"
    "struct VOut {\n"                 /* field names match the core FS's
                                         stage_in — cross-library linking
                                         matches by name */
    "    float4 pos [[position]];\n"
    "    float3 normal;\n"
    "    float3 worldPos;\n"
    "    float2 uv;\n"
    "    float4 tangent;\n"
    "};\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float4x4 skin = v.weights.x * u.uPalette[(int)v.joints.x]\n"
    "                  + v.weights.y * u.uPalette[(int)v.joints.y]\n"
    "                  + v.weights.z * u.uPalette[(int)v.joints.z]\n"
    "                  + v.weights.w * u.uPalette[(int)v.joints.w];\n"
    "    float4 worldPos = u.uModel * (skin * float4(v.pos, 1.0));\n"
    "    o.pos = u.uProj * (u.uView * worldPos);\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    float3x3 skin3 = float3x3(skin[0].xyz, skin[1].xyz, skin[2].xyz);\n"
    "    o.normal = u.uNormalMatrix * (skin3 * v.normal);\n"
    "    o.worldPos = worldPos.xyz;\n"
    "    o.uv = v.uv;\n"
    "    float3x3 lin = float3x3(u.uModel[0].xyz, u.uModel[1].xyz, u.uModel[2].xyz);\n"
    "    o.tangent = float4(lin * v.tangent.xyz, v.tangent.w);\n"
    "    return o;\n"
    "}\n";
static const char *SKINNED_SHADOW_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VIn {\n"
    "    float3 pos     [[attribute(0)]];\n"
    "    float4 joints  [[attribute(4)]];\n"
    "    float4 weights [[attribute(5)]];\n"
    "};\n"
    "struct VU {\n"
    "    float4x4 uModel;\n"
    "    float4x4 uLightVP;\n"
    "    float4x4 uPalette[64];\n"
    "};\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "vertex VOut vmain(VIn v [[stage_in]], constant VU &u [[buffer(2)]]) {\n"
    "    VOut o;\n"
    "    float4x4 skin = v.weights.x * u.uPalette[(int)v.joints.x]\n"
    "                  + v.weights.y * u.uPalette[(int)v.joints.y]\n"
    "                  + v.weights.z * u.uPalette[(int)v.joints.z]\n"
    "                  + v.weights.w * u.uPalette[(int)v.joints.w];\n"
    "    o.pos = u.uLightVP * (u.uModel * (skin * float4(v.pos, 1.0)));\n"
    "    o.pos.z = (o.pos.z + o.pos.w) * 0.5;\n"   /* GL clip z -> Metal */
    "    return o;\n"
    "}\n";
#else /* GLSL */
static const char *SKINNED_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec3 aNormal;\n"
    "layout (location = 2) in vec2 aUV;\n"
    "layout (location = 3) in vec4 aTangent;\n"
    "layout (location = 4) in vec4 aJoints;\n"               /* indices, as floats */
    "layout (location = 5) in vec4 aWeights;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uView;\n"
    "uniform mat4 uProj;\n"
    "uniform mat3 uNormalMatrix;\n"
    "uniform mat4 uPalette[64];\n"
    "out vec3 vNormal;\n"
    "out vec3 vWorldPos;\n"
    "out vec2 vUV;\n"
    "out vec4 vTangent;\n"
    "void main() {\n"
    "    mat4 skin = aWeights.x * uPalette[int(aJoints.x)]\n"
    "              + aWeights.y * uPalette[int(aJoints.y)]\n"
    "              + aWeights.z * uPalette[int(aJoints.z)]\n"
    "              + aWeights.w * uPalette[int(aJoints.w)];\n"
    "    vec4 worldPos = uModel * (skin * vec4(aPos, 1.0));\n"
    "    gl_Position = uProj * uView * worldPos;\n"
    "    vNormal = uNormalMatrix * (mat3(skin) * aNormal);\n"
    "    vWorldPos = worldPos.xyz;\n"
    "    vUV = aUV;\n"
    "    vTangent = vec4(mat3(uModel) * aTangent.xyz, aTangent.w);\n"
    "}\n";
static const char *SKINNED_SHADOW_VERTEX_SRC =
    "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 4) in vec4 aJoints;\n"
    "layout (location = 5) in vec4 aWeights;\n"
    "uniform mat4 uModel;\n"
    "uniform mat4 uLightVP;\n"
    "uniform mat4 uPalette[64];\n"
    "void main() {\n"
    "    mat4 skin = aWeights.x * uPalette[int(aJoints.x)]\n"
    "              + aWeights.y * uPalette[int(aJoints.y)]\n"
    "              + aWeights.z * uPalette[int(aJoints.z)]\n"
    "              + aWeights.w * uPalette[int(aJoints.w)];\n"
    "    gl_Position = uLightVP * (uModel * (skin * vec4(aPos, 1.0)));\n"
    "}\n";
#endif /* SOL_RHI_METAL — the skinned twins */

/* --- debug view (item 9b): show the shadow map full-screen as linearized
   grayscale (near=dark, far=white), so we can confirm the light's-eye depth
   render is correct before 9c samples it. Reuses the fullscreen-triangle VS. --- */
#ifdef SOL_RHI_METAL

static const char *SHADOW_DEBUG_FRAGMENT_SRC =      /* identical math: stored
                                       depth is GL's mapping (the VS remap) */
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "struct FU { float uNear; float uFar; };\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      depth2d<float> uDepth [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float d   = uDepth.sample(s0, v.uv);\n"
    "    float ndc = d * 2.0 - 1.0;\n"
    "    float z   = (2.0 * u.uNear * u.uFar) / (u.uFar + u.uNear - ndc * (u.uFar - u.uNear));\n"
    "    float g   = (z - u.uNear) / (u.uFar - u.uNear);\n"
    "    return float4(float3(g), 1.0);\n"
    "}\n";

#else /* GLSL */

static const char *SHADOW_DEBUG_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uDepth;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    float d   = texture(uDepth, vUV).r;\n"               /* [0,1] nonlinear (perspective) */
    "    float ndc = d * 2.0 - 1.0;\n"
    "    float z   = (2.0 * uNear * uFar) / (uFar + uNear - ndc * (uFar - uNear));\n"  /* view-space dist */
    "    float g   = (z - uNear) / (uFar - uNear);\n"         /* -> [0,1] linear for display */
    "    FragColor = vec4(vec3(g), 1.0);\n"
    "}\n";

#endif /* SOL_RHI_METAL — the shadow inspector */

/* --- the skybox (Phase A2): a fullscreen triangle that samples the
   equirectangular HDR by the per-pixel world-space view direction. Drawn FIRST
   into the HDR target with depth off, so it fills the background; objects then
   draw over it. Writes linear radiance — the post pass tonemaps it like the
   rest of the scene. --- */
#ifdef SOL_RHI_METAL

static const char *SKYBOX_VERTEX_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 ndc; };\n"
    "vertex VOut vmain(uint vid [[vertex_id]]) {\n"
    "    VOut o;\n"
    "    float2 p = float2((float)((vid << 1) & 2), (float)(vid & 2));\n"
    "    o.ndc = p * 2.0 - 1.0;\n"
    "    o.pos = float4(o.ndc, 1.0, 1.0);\n"      /* z=far; already Metal-legal */
    "    return o;\n"
    "}\n";

static const char *SKYBOX_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant float PI = 3.14159265359;\n"
    "struct VOut { float4 pos [[position]]; float2 ndc; };\n"
    "struct FU {\n"
    "    float3 uCamForward;\n"
    "    float3 uCamRight;\n"
    "    float3 uCamUp;\n"
    "    float  uTanHalfFovY;\n"
    "    float  uAspect;\n"
    "};\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texture2d<float> uEquirect [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float3 dir = normalize(u.uCamForward\n"
    "               + v.ndc.x * u.uTanHalfFovY * u.uAspect * u.uCamRight\n"
    "               + v.ndc.y * u.uTanHalfFovY * u.uCamUp);\n"
    "    float uu = atan2(dir.z, dir.x) / (2.0 * PI) + 0.5;\n"
    "    float vv = 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / PI;\n"
    "    float3 c = min(uEquirect.sample(s0, float2(uu, vv)).rgb, float3(60000.0));\n"
    "    return float4(c, 1.0);\n"
    "}\n";

/* --- the on-screen skybox once we have a cubemap (B1): identical to the equirect
   version but samples a samplerCube by the view direction. Switching the live
   skybox to this is the self-check — if the sky looks the same, the equirect->
   cube conversion is correct. --- */
static const char *SKYBOX_CUBE_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; float2 ndc; };\n"
    "struct FU {\n"
    "    float3 uCamForward;\n"
    "    float3 uCamRight;\n"
    "    float3 uCamUp;\n"
    "    float  uTanHalfFovY;\n"
    "    float  uAspect;\n"
    "    float  uLod;\n"
    "};\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texturecube<float> uEnvCube [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float3 dir = normalize(u.uCamForward\n"
    "               + v.ndc.x * u.uTanHalfFovY * u.uAspect * u.uCamRight\n"
    "               + v.ndc.y * u.uTanHalfFovY * u.uCamUp);\n"
    "    return float4(uEnvCube.sample(s0, dir, level(u.uLod)).rgb, 1.0);\n"
    "}\n";

#else /* GLSL */

static const char *SKYBOX_VERTEX_SRC =
    "#version 330 core\n"
    "out vec2 vNdc;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"  /* (0,0)(2,0)(0,2) */
    "    vNdc = p * 2.0 - 1.0;\n"                                    /* pixel NDC, -1..1 */
    "    gl_Position = vec4(vNdc, 1.0, 1.0);\n"                      /* z=far (depth off anyway) */
    "}\n";

static const char *SKYBOX_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vNdc;\n"
    "uniform vec3  uCamForward;\n"
    "uniform vec3  uCamRight;\n"
    "uniform vec3  uCamUp;\n"
    "uniform float uTanHalfFovY;\n"
    "uniform float uAspect;\n"
    "uniform sampler2D uEquirect;\n"
    "out vec4 FragColor;\n"
    "const float PI = 3.14159265359;\n"
    "void main() {\n"
    "    vec3 dir = normalize(uCamForward\n"
    "             + vNdc.x * uTanHalfFovY * uAspect * uCamRight\n"
    "             + vNdc.y * uTanHalfFovY * uCamUp);\n"
    "    float u = atan(dir.z, dir.x) / (2.0 * PI) + 0.5;\n"         /* longitude (wraps) */
    "    float v = 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / PI;\n"      /* latitude (up = sky) */
    "    vec3 c = min(texture(uEquirect, vec2(u, v)).rgb, vec3(60000.0));\n"  /* finite (no inf in 16F) so mips can average the sun (IBL) */
    "    FragColor = vec4(c, 1.0);\n"                                /* linear HDR -> hdr_rt / env cube */
    "}\n";

static const char *SKYBOX_CUBE_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vNdc;\n"
    "uniform vec3  uCamForward;\n"
    "uniform vec3  uCamRight;\n"
    "uniform vec3  uCamUp;\n"
    "uniform float uTanHalfFovY;\n"
    "uniform float uAspect;\n"
    "uniform samplerCube uEnvCube;\n"
    "uniform float uLod;\n"                                  /* 0 = sharp; >0 inspects prefilter mips (C1) */
    "out vec4 FragColor;\n"
    "void main() {\n"
    "    vec3 dir = normalize(uCamForward\n"
    "             + vNdc.x * uTanHalfFovY * uAspect * uCamRight\n"
    "             + vNdc.y * uTanHalfFovY * uCamUp);\n"
    "    FragColor = vec4(textureLod(uEnvCube, dir, uLod).rgb, 1.0);\n"
    "}\n";

#endif /* SOL_RHI_METAL — the skybox family */

/* --- the diffuse irradiance convolution (B2): for each output direction N (a
   surface normal), integrate the cosine-weighted hemisphere of incoming light
   from the env cubemap. Run once per face into a tiny 32^2 cubemap. cos*sin =
   Lambert cosine * solid-angle Jacobian; the pi folds in the 1/pi BRDF so the
   lighting pass is just diffuse = irradiance * albedo. --- */
#ifdef SOL_RHI_METAL
static const char *IRRADIANCE_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant float PI = 3.14159265359;\n"
    "struct VOut { float4 pos [[position]]; float2 ndc; };\n"
    "struct FU {\n"
    "    float3 uCamForward;\n"
    "    float3 uCamRight;\n"
    "    float3 uCamUp;\n"
    "    float  uTanHalfFovY;\n"
    "    float  uAspect;\n"
    "};\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texturecube<float> uEnvCube [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float3 N = normalize(u.uCamForward\n"
    "             + v.ndc.x * u.uTanHalfFovY * u.uAspect * u.uCamRight\n"
    "             + v.ndc.y * u.uTanHalfFovY * u.uCamUp);\n"
    "    float3 up    = abs(N.y) < 0.999 ? float3(0.0,1.0,0.0) : float3(1.0,0.0,0.0);\n"
    "    float3 right = normalize(cross(up, N));\n"
    "    up = cross(N, right);\n"
    "    float3 irradiance = float3(0.0);\n"
    "    float  nrSamples  = 0.0;\n"
    "    float  delta = 0.025;\n"
    "    for (float phi = 0.0; phi < 2.0*PI; phi += delta)\n"
    "        for (float theta = 0.0; theta < 0.5*PI; theta += delta) {\n"
    "            float3 t = float3(sin(theta)*cos(phi), sin(theta)*sin(phi), cos(theta));\n"
    "            float3 w = t.x*right + t.y*up + t.z*N;\n"
    "            float3 radiance = uEnvCube.sample(s0, w, level(4.0)).rgb;\n"
    "            irradiance += radiance * cos(theta) * sin(theta);\n"
    "            nrSamples  += 1.0;\n"
    "        }\n"
    "    return float4(PI * irradiance / nrSamples, 1.0);\n"
    "}\n";
#else /* GLSL */
static const char *IRRADIANCE_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vNdc;\n"
    "uniform vec3  uCamForward;\n"
    "uniform vec3  uCamRight;\n"
    "uniform vec3  uCamUp;\n"
    "uniform float uTanHalfFovY;\n"
    "uniform float uAspect;\n"
    "uniform samplerCube uEnvCube;\n"
    "out vec4 FragColor;\n"
    "const float PI = 3.14159265359;\n"
    "void main() {\n"
    "    vec3 N = normalize(uCamForward\n"
    "           + vNdc.x * uTanHalfFovY * uAspect * uCamRight\n"
    "           + vNdc.y * uTanHalfFovY * uCamUp);\n"
    "    vec3 up    = abs(N.y) < 0.999 ? vec3(0.0,1.0,0.0) : vec3(1.0,0.0,0.0);\n"
    "    vec3 right = normalize(cross(up, N));\n"            /* tangent basis around N */
    "    up = cross(N, right);\n"
    "    vec3  irradiance = vec3(0.0);\n"
    "    float nrSamples  = 0.0;\n"
    "    float delta = 0.025;\n"
    "    for (float phi = 0.0; phi < 2.0*PI; phi += delta)\n"
    "        for (float theta = 0.0; theta < 0.5*PI; theta += delta) {\n"
    "            vec3 t = vec3(sin(theta)*cos(phi), sin(theta)*sin(phi), cos(theta));\n"  /* tangent space */
    "            vec3 w = t.x*right + t.y*up + t.z*N;\n"     /* -> world */
    "            vec3 radiance = textureLod(uEnvCube, w, 4.0).rgb;\n"  /* high mip: sun pre-spread -> warm AND smooth (no firefly step) */
    "            irradiance += radiance * cos(theta) * sin(theta);\n"
    "            nrSamples  += 1.0;\n"
    "        }\n"
    "    FragColor = vec4(PI * irradiance / nrSamples, 1.0);\n"
    "}\n";
#endif /* SOL_RHI_METAL — irradiance */

/* --- specular prefilter convolution (C1): for each output direction (= the
   reflection vector R, assuming V=N=R), GGX-importance-sample the env cube and
   average. Rendered once per mip, each at a higher roughness -> the roughness
   mip chain. Hammersley + GGX inverse-CDF gives samples clustered in the lobe
   (far fewer needed than uniform). The per-sample mip pick (solid-angle ratio)
   pre-blurs bright spots so the sun can't firefly a glossy reflection. --- */
#ifdef SOL_RHI_METAL
static const char *PREFILTER_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant float PI = 3.14159265359;\n"
    "constant float ENV_SIZE = 1024.0;\n"
    "struct VOut { float4 pos [[position]]; float2 ndc; };\n"
    "struct FU {\n"
    "    float3 uCamForward;\n"
    "    float3 uCamRight;\n"
    "    float3 uCamUp;\n"
    "    float  uTanHalfFovY;\n"
    "    float  uAspect;\n"
    "    float  uRoughness;\n"
    "};\n"
    "static float distributionGGX(float NoH, float rough) {\n"
    "    float a = rough*rough; float a2 = a*a;\n"
    "    float d = NoH*NoH*(a2 - 1.0) + 1.0;\n"
    "    return a2 / (PI * d * d);\n"
    "}\n"
    "static float radicalInverse(uint bits) {\n"
    "    bits = (bits << 16u) | (bits >> 16u);\n"
    "    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);\n"
    "    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);\n"
    "    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);\n"
    "    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);\n"
    "    return float(bits) * 2.3283064365386963e-10;\n"
    "}\n"
    "static float2 hammersley(uint i, uint n) { return float2(float(i)/float(n), radicalInverse(i)); }\n"
    "static float3 importanceSampleGGX(float2 Xi, float3 N, float rough) {\n"
    "    float a = rough*rough;\n"
    "    float phi = 2.0*PI*Xi.x;\n"
    "    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0)*Xi.y));\n"
    "    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);\n"
    "    float3 H = float3(sinTheta*cos(phi), sinTheta*sin(phi), cosTheta);\n"
    "    float3 up = abs(N.z) < 0.999 ? float3(0.0,0.0,1.0) : float3(1.0,0.0,0.0);\n"
    "    float3 tangent = normalize(cross(up, N));\n"
    "    float3 bitan = cross(N, tangent);\n"
    "    return normalize(tangent*H.x + bitan*H.y + N*H.z);\n"
    "}\n"
    "fragment float4 fmain(VOut v [[stage_in]], constant FU &u [[buffer(0)]],\n"
    "                      texturecube<float> uEnvCube [[texture(0)]],\n"
    "                      sampler s0 [[sampler(0)]]) {\n"
    "    float3 N = normalize(u.uCamForward\n"
    "             + v.ndc.x * u.uTanHalfFovY * u.uAspect * u.uCamRight\n"
    "             + v.ndc.y * u.uTanHalfFovY * u.uCamUp);\n"
    "    float3 R = N; float3 V = N;\n"
    "    const uint SAMPLES = 1024u;\n"
    "    float3 sum = float3(0.0); float wsum = 0.0;\n"
    "    for (uint i = 0u; i < SAMPLES; i++) {\n"
    "        float2 Xi = hammersley(i, SAMPLES);\n"
    "        float3 H  = importanceSampleGGX(Xi, N, u.uRoughness);\n"
    "        float3 L  = normalize(2.0 * dot(V, H) * H - V);\n"
    "        float NoL = max(dot(N, L), 0.0);\n"
    "        if (NoL > 0.0) {\n"
    "            float NoH = max(dot(N, H), 0.0);\n"
    "            float D   = distributionGGX(NoH, u.uRoughness);\n"
    "            float pdf = D * 0.25 + 0.0001;\n"
    "            float saTexel  = 4.0*PI / (6.0 * ENV_SIZE * ENV_SIZE);\n"
    "            float saSample = 1.0 / (float(SAMPLES) * pdf + 0.0001);\n"
    "            float mip = u.uRoughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);\n"
    "            sum  += uEnvCube.sample(s0, L, level(mip)).rgb * NoL;\n"
    "            wsum += NoL;\n"
    "        }\n"
    "    }\n"
    "    return float4(sum / wsum, 1.0);\n"
    "}\n";
#else /* GLSL */
static const char *PREFILTER_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vNdc;\n"
    "uniform vec3  uCamForward;\n"
    "uniform vec3  uCamRight;\n"
    "uniform vec3  uCamUp;\n"
    "uniform float uTanHalfFovY;\n"
    "uniform float uAspect;\n"
    "uniform samplerCube uEnvCube;\n"
    "uniform float uRoughness;\n"
    "out vec4 FragColor;\n"
    "const float PI = 3.14159265359;\n"
    "const float ENV_SIZE = 1024.0;\n"                       /* env cube face resolution */
    "float distributionGGX(float NoH, float rough) {\n"
    "    float a = rough*rough; float a2 = a*a;\n"
    "    float d = NoH*NoH*(a2 - 1.0) + 1.0;\n"
    "    return a2 / (PI * d * d);\n"
    "}\n"
    "float radicalInverse(uint bits) {\n"                    /* van der Corput (no bitfieldReverse in 330) */
    "    bits = (bits << 16u) | (bits >> 16u);\n"
    "    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);\n"
    "    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);\n"
    "    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);\n"
    "    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);\n"
    "    return float(bits) * 2.3283064365386963e-10;\n"     /* / 2^32 */
    "}\n"
    "vec2 hammersley(uint i, uint n) { return vec2(float(i)/float(n), radicalInverse(i)); }\n"
    "vec3 importanceSampleGGX(vec2 Xi, vec3 N, float rough) {\n"
    "    float a = rough*rough;\n"
    "    float phi = 2.0*PI*Xi.x;\n"
    "    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0)*Xi.y));\n"  /* GGX inverse-CDF */
    "    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);\n"
    "    vec3 H = vec3(sinTheta*cos(phi), sinTheta*sin(phi), cosTheta);\n"   /* tangent space */
    "    vec3 up = abs(N.z) < 0.999 ? vec3(0.0,0.0,1.0) : vec3(1.0,0.0,0.0);\n"
    "    vec3 tangent = normalize(cross(up, N));\n"
    "    vec3 bitan = cross(N, tangent);\n"
    "    return normalize(tangent*H.x + bitan*H.y + N*H.z);\n"
    "}\n"
    "void main() {\n"
    "    vec3 N = normalize(uCamForward\n"
    "           + vNdc.x * uTanHalfFovY * uAspect * uCamRight\n"
    "           + vNdc.y * uTanHalfFovY * uCamUp);\n"
    "    vec3 R = N; vec3 V = N;\n"                           /* split-sum assumption: V=N=R */
    "    const uint SAMPLES = 1024u;\n"
    "    vec3  sum = vec3(0.0); float wsum = 0.0;\n"
    "    for (uint i = 0u; i < SAMPLES; i++) {\n"
    "        vec2 Xi = hammersley(i, SAMPLES);\n"
    "        vec3 H  = importanceSampleGGX(Xi, N, uRoughness);\n"
    "        vec3 L  = normalize(2.0 * dot(V, H) * H - V);\n"  /* reflect V about H */
    "        float NoL = max(dot(N, L), 0.0);\n"
    "        if (NoL > 0.0) {\n"
    "            float NoH = max(dot(N, H), 0.0);\n"
    "            float D   = distributionGGX(NoH, uRoughness);\n"
    "            float pdf = D * 0.25 + 0.0001;\n"            /* HoV==NoH under V=N -> D/4 */
    "            float saTexel  = 4.0*PI / (6.0 * ENV_SIZE * ENV_SIZE);\n"
    "            float saSample = 1.0 / (float(SAMPLES) * pdf + 0.0001);\n"
    "            float mip = uRoughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);\n"
    "            sum  += textureLod(uEnvCube, L, mip).rgb * NoL;\n"
    "            wsum += NoL;\n"
    "        }\n"
    "    }\n"
    "    FragColor = vec4(sum / wsum, 1.0);\n"
    "}\n";
#endif /* SOL_RHI_METAL — prefilter */

/* --- the BRDF integration LUT (C2): the second split-sum factor. For each
   (NoV, roughness) it integrates the specular geometry+Fresnel over the GGX
   lobe, factored so F0 separates -> R = scale on F0, G = bias. A 2D fullscreen
   render; environment-independent (bake once). Uses the same Hammersley + GGX
   importance sampling as C1, but the IBL geometry remap (k = rough^2 / 2). --- */
#ifdef SOL_RHI_METAL
static const char *BRDF_LUT_FRAGMENT_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constant float PI = 3.14159265359;\n"
    "struct VOut { float4 pos [[position]]; float2 uv; };\n"
    "static float radicalInverse(uint bits) {\n"
    "    bits = (bits << 16u) | (bits >> 16u);\n"
    "    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);\n"
    "    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);\n"
    "    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);\n"
    "    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);\n"
    "    return float(bits) * 2.3283064365386963e-10;\n"
    "}\n"
    "static float2 hammersley(uint i, uint n) { return float2(float(i)/float(n), radicalInverse(i)); }\n"
    "static float3 importanceSampleGGX(float2 Xi, float3 N, float rough) {\n"
    "    float a = rough*rough;\n"
    "    float phi = 2.0*PI*Xi.x;\n"
    "    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0)*Xi.y));\n"
    "    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);\n"
    "    float3 H = float3(sinTheta*cos(phi), sinTheta*sin(phi), cosTheta);\n"
    "    float3 up = abs(N.z) < 0.999 ? float3(0.0,0.0,1.0) : float3(1.0,0.0,0.0);\n"
    "    float3 tangent = normalize(cross(up, N));\n"
    "    float3 bitan = cross(N, tangent);\n"
    "    return normalize(tangent*H.x + bitan*H.y + N*H.z);\n"
    "}\n"
    "static float geometrySchlickGGX(float NdotX, float rough) {\n"
    "    float k = (rough*rough) / 2.0;\n"
    "    return NdotX / (NdotX*(1.0 - k) + k);\n"
    "}\n"
    "static float geometrySmith(float3 N, float3 V, float3 L, float rough) {\n"
    "    return geometrySchlickGGX(max(dot(N,V),0.0), rough) * geometrySchlickGGX(max(dot(N,L),0.0), rough);\n"
    "}\n"
    "static float2 integrateBRDF(float NoV, float rough) {\n"
    "    float3 V = float3(sqrt(1.0 - NoV*NoV), 0.0, NoV);\n"
    "    float3 N = float3(0.0, 0.0, 1.0);\n"
    "    float A = 0.0; float B = 0.0;\n"
    "    const uint SAMPLES = 1024u;\n"
    "    for (uint i = 0u; i < SAMPLES; i++) {\n"
    "        float2 Xi = hammersley(i, SAMPLES);\n"
    "        float3 H  = importanceSampleGGX(Xi, N, rough);\n"
    "        float3 L  = normalize(2.0 * dot(V, H) * H - V);\n"
    "        float NoL = max(L.z, 0.0);\n"
    "        float NoH = max(H.z, 0.0);\n"
    "        float VoH = max(dot(V, H), 0.0);\n"
    "        if (NoL > 0.0) {\n"
    "            float G    = geometrySmith(N, V, L, rough);\n"
    "            float Gvis = (G * VoH) / (NoH * NoV);\n"
    "            float Fc   = pow(1.0 - VoH, 5.0);\n"
    "            A += (1.0 - Fc) * Gvis;\n"
    "            B += Fc * Gvis;\n"
    "        }\n"
    "    }\n"
    "    return float2(A, B) / float(SAMPLES);\n"
    "}\n"
    "fragment float4 fmain(VOut v [[stage_in]]) {\n"
    "    return float4(integrateBRDF(max(v.uv.x, 0.001), v.uv.y), 0.0, 1.0);\n"
    "}\n";
#else /* GLSL */
static const char *BRDF_LUT_FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vUV;\n"
    "out vec4 FragColor;\n"
    "const float PI = 3.14159265359;\n"
    "float radicalInverse(uint bits) {\n"
    "    bits = (bits << 16u) | (bits >> 16u);\n"
    "    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);\n"
    "    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);\n"
    "    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);\n"
    "    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);\n"
    "    return float(bits) * 2.3283064365386963e-10;\n"
    "}\n"
    "vec2 hammersley(uint i, uint n) { return vec2(float(i)/float(n), radicalInverse(i)); }\n"
    "vec3 importanceSampleGGX(vec2 Xi, vec3 N, float rough) {\n"
    "    float a = rough*rough;\n"
    "    float phi = 2.0*PI*Xi.x;\n"
    "    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0)*Xi.y));\n"
    "    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);\n"
    "    vec3 H = vec3(sinTheta*cos(phi), sinTheta*sin(phi), cosTheta);\n"
    "    vec3 up = abs(N.z) < 0.999 ? vec3(0.0,0.0,1.0) : vec3(1.0,0.0,0.0);\n"
    "    vec3 tangent = normalize(cross(up, N));\n"
    "    vec3 bitan = cross(N, tangent);\n"
    "    return normalize(tangent*H.x + bitan*H.y + N*H.z);\n"
    "}\n"
    "float geometrySchlickGGX(float NdotX, float rough) {\n"  /* IBL remap: k = rough^2 / 2 */
    "    float k = (rough*rough) / 2.0;\n"
    "    return NdotX / (NdotX*(1.0 - k) + k);\n"
    "}\n"
    "float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {\n"
    "    return geometrySchlickGGX(max(dot(N,V),0.0), rough) * geometrySchlickGGX(max(dot(N,L),0.0), rough);\n"
    "}\n"
    "vec2 integrateBRDF(float NoV, float rough) {\n"
    "    vec3 V = vec3(sqrt(1.0 - NoV*NoV), 0.0, NoV);\n"     /* view in tangent space */
    "    vec3 N = vec3(0.0, 0.0, 1.0);\n"
    "    float A = 0.0; float B = 0.0;\n"
    "    const uint SAMPLES = 1024u;\n"
    "    for (uint i = 0u; i < SAMPLES; i++) {\n"
    "        vec2 Xi = hammersley(i, SAMPLES);\n"
    "        vec3 H  = importanceSampleGGX(Xi, N, rough);\n"
    "        vec3 L  = normalize(2.0 * dot(V, H) * H - V);\n"
    "        float NoL = max(L.z, 0.0);\n"
    "        float NoH = max(H.z, 0.0);\n"
    "        float VoH = max(dot(V, H), 0.0);\n"
    "        if (NoL > 0.0) {\n"
    "            float G    = geometrySmith(N, V, L, rough);\n"
    "            float Gvis = (G * VoH) / (NoH * NoV);\n"
    "            float Fc   = pow(1.0 - VoH, 5.0);\n"          /* Fresnel with F0 factored out */
    "            A += (1.0 - Fc) * Gvis;\n"                    /* scale on F0 */
    "            B += Fc * Gvis;\n"                            /* bias */
    "        }\n"
    "    }\n"
    "    return vec2(A, B) / float(SAMPLES);\n"
    "}\n"
    "void main() {\n"
    "    FragColor = vec4(integrateBRDF(max(vUV.x, 0.001), vUV.y), 0.0, 1.0);\n"  /* guard NoV=0 */
    "}\n";
#endif /* SOL_RHI_METAL — the BRDF LUT */

#define EDIT_BUF_CAP 2048   /* a note's text, while it is being typed */

/* The turning leaf's 2D section (item 9): one struct both the leaf MESH
   and the leaf TEXT evaluate, so paper and ink bend identically. */
typedef struct {
    float alpha, kappa;     /* hinge angle + curl */
    float wA, wR, wL;       /* arc / right-rest / left-rest blend weights */
    float pinch, rise, xf;  /* the resting fan profile */
    float wb;
} LeafShape;

/* P9 item 1: color-grade presets (display-referred). Mode 0 is neutral — exact
   identity, today's image bit-for-bit. '9' cycles. Synthesized, no binary. The
   lut field indexes grade_luts[]; lut_mix 0 bypasses it (exact), 1 = full LUT. */
typedef struct {
    float contrast, saturation, tint_r, tint_g, tint_b, vig_strength, vig_radius;
    int   lut;
    float lut_mix;
} GradePreset;
static const GradePreset GRADE_PRESETS[] = {
    { 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 0.00f, 0.50f, 0, 0.0f },  /* 0 neutral (off) */
    { 1.08f, 1.05f, 1.06f, 1.00f, 0.92f, 0.35f, 0.45f, 0, 0.0f },  /* 1 warm dusk */
    { 1.05f, 0.92f, 0.92f, 0.98f, 1.10f, 0.30f, 0.50f, 0, 0.0f },  /* 2 cool */
    { 1.18f, 0.80f, 1.00f, 1.00f, 1.00f, 0.45f, 0.40f, 0, 0.0f },  /* 3 dramatic */
    { 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 0.00f, 0.50f, 0, 1.0f },  /* 4 LUT identity (passthrough test) */
    { 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 0.00f, 0.50f, 1, 1.0f }   /* 5 filmic (teal/orange split-tone) */
};
static const char *GRADE_PRESET_NAMES[] = {
    "neutral (off)", "warm dusk", "cool", "dramatic", "LUT identity (test)", "filmic"
};
#define GRADE_PRESET_COUNT 6

/* P9 item 1 (LUT half): bake a 16-cube color grade into a 256x16 RGBA8 strip.
   Tile = blue slice (x / 16); within a tile, (x % 16) = red, y = green. The
   shader rebuilds trilinear by hand (two slice taps + a blue lerp) with half-
   texel insets, so GL's REPEAT + mips are sidestepped. look 0 = identity. */
#define GRADE_LUT_N 16

static unsigned char grade_u8(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static RhiTexture build_grade_lut(int look) {
    unsigned char px[GRADE_LUT_N * GRADE_LUT_N * GRADE_LUT_N * 4];
    int N = GRADE_LUT_N, W = N * N;
    int bs, gy, rx;
    for (bs = 0; bs < N; bs++)
        for (gy = 0; gy < N; gy++)
            for (rx = 0; rx < N; rx++) {
                float r  = (float)rx / (float)(N - 1);
                float g  = (float)gy / (float)(N - 1);
                float b  = (float)bs / (float)(N - 1);
                float cr = r, cg = g, cb = b;
                int   o  = (gy * W + bs * N + rx) * 4;
                if (look == 1) {                       /* filmic: shadows cool, highlights warm */
                    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    float t   = lum * lum * (3.0f - 2.0f * lum);   /* smoothstep(0,1,lum) */
                    cr = r * (0.90f + 0.22f * t);
                    cb = b * (1.12f - 0.24f * t);
                }
                px[o + 0] = grade_u8(cr);
                px[o + 1] = grade_u8(cg);
                px[o + 2] = grade_u8(cb);
                px[o + 3] = 255;
            }
    return rhi_create_texture(px, W, N, RHI_TEX_RGBA8);
}

/* P9 item 3: synthesize the decal atlas — left half stains (dark vertical
   streaks, denser under the sill), right half moss (green blotches, denser at
   the foot). Alpha is the mask, color is what it pulls toward. No binary. */
#define DECAL_ATLAS_W 128
#define DECAL_ATLAS_H 128

static float decal_hash(int x, int y, int s) {
    unsigned h = (unsigned)(x * 374761393 + y * 668265263 + s * 2147483647);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

static unsigned char decal_u8(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static RhiTexture build_decal_atlas(void) {
    static unsigned char px[DECAL_ATLAS_W * DECAL_ATLAS_H * 4];
    int x, y, half = DECAL_ATLAS_W / 2;
    for (y = 0; y < DECAL_ATLAS_H; y++) {
        for (x = 0; x < DECAL_ATLAS_W; x++) {
            int   o  = (y * DECAL_ATLAS_W + x) * 4;
            float fy = (float)y / (float)(DECAL_ATLAS_H - 1);   /* 0 bottom .. 1 top */
            float r = 0.0f, g = 0.0f, bl = 0.0f, a;
            if (x < half) {                              /* STAIN: dark streaks */
                float band   = decal_hash(x / 3, 0, 11);
                float n      = 0.6f + 0.4f * decal_hash(x, y / 4, 12);
                a = band * band * n * (0.35f + 0.65f * fy);     /* stronger near top */
            } else {                                     /* MOSS: green blotches */
                int   mx     = x - half;
                float blob   = decal_hash(mx / 5, y / 5, 21);
                float n      = decal_hash(mx, y, 22);
                a = blob * blob * (0.5f + 0.5f * n) * (0.4f + 0.6f * (1.0f - fy));
                r = 0.10f; g = 0.22f; bl = 0.06f;
            }
            px[o + 0] = decal_u8(r);
            px[o + 1] = decal_u8(g);
            px[o + 2] = decal_u8(bl);
            px[o + 3] = decal_u8(a);
        }
    }
    return rhi_create_texture(px, DECAL_ATLAS_W, DECAL_ATLAS_H, RHI_TEX_RGBA8);
}

#define INV_THUMB_CAP 64    /* max items the bag tracks (items[] buffers, paging) */
#define INV_THUMB_SIZE 256  /* per-item thumbnail render-target edge, px */
#define INV_THUMB_POOL 16   /* cached thumbnail render targets (bounds GPU RT slots:
                               only the visible page is built, a few spill cached) */

typedef struct AppState {
    int         fb_width, fb_height;
    Editor      editor;         /* top-down spatial-tree editor (zero = inactive) */
    RhiPipeline pipeline;
    RhiPipeline post_pipeline;  /* fullscreen tonemap/encode pass (item 7b) */
    RhiPipeline glass_pipeline; /* P9 item 2: church_glass — alpha-blend, depth-write-off */
    RhiPipeline decal_pipeline; /* P9 item 3: church_decals — unlit alpha quads */
    RhiPipeline portal_pipeline;/* Portal Material: the energy-pane shader */
    RhiTexture  decal_atlas;    /* P9 item 3: synthesized stain (left) + moss (right) */
    RhiTexture  albedo_tex;     /* decoded page image (item 5b); 0 if load failed */
    Scene       scene;
    sol_u32     box_handle;     /* so update() can animate the box object */
    sol_u32     anchor_handle;  /* the empty the box is parented to */
    sol_u32     page_handle;    /* the readable parchment surface (item 5d) */
    sol_u32     sword_handle;   /* the showcase sword: stood upright, spun in update() */
    sol_u32     sword_precess_handle;  /* invisible anchor the sword orbits (precession) */
    Camera      camera;
    /* offscreen HDR pass (item 7) */
    RhiRenderTarget hdr_rt;          /* scene renders here, then to the window */
    int             rt_width, rt_height;  /* size hdr_rt was built at; recreate on resize */
    float           exposure;        /* HDR exposure (item 7c); '[' / ']' scrub it live */
    int             grade_mode;      /* P9 item 1: 0=neutral(off), then preset slots; '9' cycles */
    RhiTexture      grade_luts[2];   /* P9 item 1: [0]=identity, [1]=filmic split-tone */
    /* the shadow-casting sun (P8 item 6): now a DIRECTIONAL light. light_pos ->
       light_target encodes only its DIRECTION (no position/cone/falloff). The
       inner/outer/intensity fields linger for the scene-format round-trip and
       intensity scaling; the cone is no longer applied. */
    vec3        light_pos;
    vec3        light_target;    /* with light_pos: the sun direction = normalize(target - pos) */
    vec3        light_color;
    float       light_intensity; /* directional: a plain multiplier (no inverse-square) */
    float       light_inner_deg; /* (legacy spot cone; unused by the directional sun) */
    float       light_outer_deg; /* (legacy spot cone; unused by the directional sun) */
    /* cascaded shadow maps (P8 item 6): one depth target per cascade + the
       per-frame fitted light matrices (rebuilt each render from the camera). */
    RhiRenderTarget shadow_rt[SHADOW_CASCADES];
    mat4            cascade_vp[SHADOW_CASCADES];
    /* the shadow-casting spot sconce (P8 item 7): one perspective map + the
       resolved caster, rebuilt each frame (position swayed by the flicker). */
    RhiRenderTarget spot_shadow_rt;
    mat4            spot_vp;
    vec3            spot_pos, spot_dir, spot_color;  /* color premultiplied by intensity*glow */
    float           spot_radius, spot_cos_inner, spot_cos_outer;
    sol_bool        spot_enabled;                    /* 0 = no caster -> today's lighting */
    RhiPipeline shadow_pipeline;       /* the depth-only pass */
    RhiPipeline skinned_pipeline;        /* item 9: palette-blending twins of */
    RhiPipeline skinned_shadow_pipeline; /* the standard + shadow pipelines  */
    RhiPipeline shadow_debug_pipeline; /* fullscreen depth-map inspector */
    sol_bool    show_shadow_map;       /* 'M' toggles the inspector view */
    /* environment / skybox (Phase A): equirectangular HDR, linear radiance */
    RhiTexture  skybox_tex;            /* equirect HDR; 0 if the .hdr failed to load */
    RhiPipeline skybox_pipeline;       /* fullscreen-triangle equirect draw (A2; also the equirect->cube tool) */
    RhiTexture  env_cubemap;           /* equirect baked into a cubemap (B1); 0 if none */
    RhiPipeline skybox_cube_pipeline;  /* on-screen skybox sampling the cubemap (B1) */
    RhiTexture  irradiance_cubemap;    /* diffuse irradiance convolution (B2) */
    RhiPipeline irradiance_pipeline;   /* the convolution pass */
    sol_bool    show_irradiance;       /* 'I' toggles the skybox source: env vs irradiance */
    RhiTexture  prefilter_cubemap;     /* specular prefilter, roughness mip chain (C1) */
    RhiPipeline prefilter_pipeline;    /* the GGX importance-sampling pass */
    sol_bool    show_prefilter;        /* 'P' cycles the prefilter-mip inspector */
    int         prefilter_mip;         /* which roughness level the inspector shows */
    RhiRenderTarget brdf_lut_rt;       /* BRDF integration LUT, 2D RG (C2) */
    RhiPipeline     brdf_lut_pipeline;
    sol_bool    bs_was_down;    /* edge-detect tombstone dismissal (Backspace) */
    sol_bool    textsize_was_down; /* edge-detect +/- note text sizing */
    sol_bool    g_was_down;     /* edge-detect gather-to-workspace (P3 item 6d) */
    sol_bool    night;          /* P8 item 9: the day/night lever's state (` toggles) */
    /* the yardstick (P4 item 2): smoothed CPU-side timings + draw counts.
       Wall clock is vsync-pinned (the documented swap cadence), so the
       numbers that can MOVE are encode/update cost and the draw counts —
       culling and instancing claims point here, not at fps. All in ms. */
    float t_frame, t_cpu, t_update, t_shadow, t_spot_shadow, t_hdr, t_post, t_swap;
    float t_frame_gpu;              /* P8 item 1: whole-frame GPU ms (Metal); < 0 = n/a (GL) */
    int   draws_done, draws_total;  /* scene objects drawn / with geometry */
    float t_text_ms;                /* P9 perf #2 measure: world-text section wall-time */
    int   t_text_blocks, t_text_uploads, t_text_misses; /* wtext blocks / re-uploads / cache misses */
    long  t_text_shape_calls, t_text_shape_glyphs; /* text_shape calls / glyphs shaped per frame */
    /* the spatial index (P4 item 2): world AABBs of everything with
       geometry, build-or-refit on demand (ids compared each refresh —
       same set refits, changed set rebuilds). bvh_ids/boxes are the
       scratch + identity arrays the tree is fed from. */
    Bvh      bvh;
    sol_u32 *bvh_ids;
    Aabb    *bvh_boxes;
    int      bvh_count, bvh_cap;
    unsigned char *vis;          /* per-pass visibility, handle-indexed (piece 4) */
    sol_u32        vis_cap;
    /* doorway-label routes (per-frame CPU spec, Part A): route_all is too costly
       to run every frame, so solve at most ~4x/sec and reuse the result. Labels
       are cosmetic; a room only moves in the editor, where a <=0.25s lag is
       imperceptible. routes_last_t = 0 (zero-init AppState) forces a frame-1 solve. */
    Route   routes[ROUTE_MAX];
    int     route_count;
    double  routes_last_t;
    /* bloom (P4 item 5): the half-res chain — level 0 is half the frame,
       each level half again; recreated with the HDR target on resize */
#define BLOOM_LEVELS 5
    RhiRenderTarget bloom_rt[BLOOM_LEVELS];
    int             bloom_w[BLOOM_LEVELS], bloom_h[BLOOM_LEVELS];
    RhiPipeline     bloom_extract_pipeline, bloom_down_pipeline, bloom_up_pipeline;
    sol_bool        bloom_on;      /* 'K' toggles, for honest A/B */
    /* god-rays (P8 item 3): a half-res raymarch of the spot-light shadow
       volume, composited additively in post like a bloom level */
    RhiRenderTarget godray_rt;
    int             godray_w, godray_h;
    RhiPipeline     godray_pipeline;
    /* soft particles (P8 item 4): a depth copy lets the particle FS read the
       scene depth without a feedback loop on the live depth attachment */
    RhiRenderTarget depthcopy_rt;
    RhiPipeline     copy_pipeline;
    /* SSAO (P8 item 5): a half-res occlusion factor + a bilateral blur,
       post-multiplied into the lit scene */
    RhiRenderTarget ssao_rt, ssao_blur_rt;
    int             ssao_w, ssao_h;
    RhiPipeline     ssao_pipeline, ssao_blur_pipeline;
    /* the meadow (P4 item 3): per-island instanced grass — DERIVED data
       (the arrows pattern at landscape scale), rebuilt on load and mint,
       never serialized */
    RhiPipeline meadow_pipeline;
    RhiBuffer   meadow_vbuf, meadow_ibuf;   /* the one shared tuft */
    RhiBuffer   flower_vbuf, flower_ibuf;   /* the bloom (item 7) */
    MeadowPatch meadow[32];
    int         meadow_count;

    /* instanced ornament (P6 item 10, the item-7 debt; P7 item 4 grew
       it to KINDS): unit meshes through the FULL PBR pipeline + a shadow
       twin. orn_mesh is indexed by ORN_* — baluster, broadleaf, conifer */
    RhiPipeline   ornament_pipeline, ornament_shadow_pipeline;
    Mesh          orn_mesh[ORN_KIND_COUNT];
    OrnamentPatch ornament[128];
    int           ornament_count;
    float         ornament_fp;     /* the scene fingerprint last built */

    /* the FIELD forest (P7 item 5): per-island woods on the ornament
       pipeline; the variant wood meshes are shared (built once). The
       pebble unit (item 6) rides the same pool for scree. */
    Mesh          forest_wood[FOREST_VARIANT_COUNT];
    Mesh          scree_mesh;
    ForestPatch   forest[32];
    int           forest_count;
    /* particles (P4 item 7): the pool is VIEW STATE (the reader-rig
       doctrine) — emitters persist as components, the weather never does.
       One shared unit quad, one instance buffer re-uploaded per frame,
       one additive draw for everything airborne. */
    ParticlePool particles;
    RhiPipeline  part_pipeline;
    RhiBuffer    part_vbuf, part_ibuf, part_inst;
    int          part_count;    /* live count last frame — the HUD's number */

    /* water (P7 item 8): the pond's pipeline + its scrolling ripple
       normal map; ponds are scene objects the scene pass skips */
    RhiPipeline  water_pipeline;
    RhiTexture   water_ripple;

    /* the one wind (P7 item 9): evaluated once per frame at the camera,
       read by the meadow + canopy shaders, particle drift, and audio */
    float        wind_dx, wind_dz, wind_gust;
    /* collision (P4 item 1): derived data like the arrows — rebuilt on load
       and on drag release (walls and paths are draggable props) */
    ColliderSet colliders;
    sol_bool    ghost;          /* 'X': debug no-clip — building wants to pass through walls */
    /* room graph (P3 item 7) */
    sol_u32     current_room;   /* containing room's anchor handle; 0 = outside (derived per frame) */
    sol_u32     portal_debounce;   /* gate just arrived at: ignore until you leave it */
    float       ambient_scale;  /* eased toward the room's ambient (sealed = dim) */
    /* text (P3 item 3) */
    Font       *ui_font;        /* DejaVu Sans, SDF atlas; NULL if load failed */
    Font       *mono_font;      /* DejaVu Sans Mono — code + aligned readouts */
    int         text_inspect;   /* 'T' cycles: 0 off, 1 atlas, 2 type specimen */
    /* mouse-look / cursor state (item 3c/3d) */
    double      mouse_last_x, mouse_last_y;
    int         mouse_skip;        /* swallow N frames of delta after a cursor-mode change */
    sol_bool    tab_was_down;
    double      scroll_accum;      /* scroll events accumulate here, drained per frame */
    /* picking / selection (item 4) */
    sol_u32     selected_handle;   /* 0 = none */
    sol_u32     carried;          /* object being carried; 0 = none */
    sol_u32     plant_room;     /* descent: room you're aiming a folder tablet in; 0 = none */
    int         plant_wall;     /* descent: ROOM_WALL_* the carried folder is aimed at */
    float       plant_off;      /* descent: offset along that wall */
    sol_bool    plant_aim;      /* descent: a valid wall-aim this frame */
    vec3        carry_origin;   /* descent: a folder card's pre-carry local pos (snaps back on drop) */
    sol_bool    file_aim;        /* carried tablet is aimed at furniture this frame */
    sol_u32     file_target;     /* the furniture handle */
    vec3        file_local;      /* the tablet's resting local pos on it */
    quat        file_rot;        /* the resting local orientation */
    sol_bool    place_active;     /* place-furniture preview mode */
    int         place_index;      /* catalog index being previewed */
    float       place_yaw;        /* ghost yaw (radians) */
    Mesh        place_ghost;      /* realized ghost mesh (rebuilt on enter/cycle) */
    sol_u32     place_label_target; /* handle awaiting a bookshelf-label prompt; 0 = none */
    sol_bool    lmb_was_down;
    sol_bool    editor_del_was;    /* edge-detect Delete/Backspace -> disconnect (editor) */
    char        editor_resize_key[160];  /* a resized island's mesh registry key, captured at press */
    sol_bool    editor_resize_keyed;     /* a terrain resize is in flight (rebuild its mesh on commit) */
    sol_bool    show_hud;                 /* the upper-left debug/stats card (default on) */
    double      press_x, press_y;  /* left-press position, for orbit tap-vs-drag */
    /* drag-to-place (P3 item 4) */
    sol_u32     floor_handle;      /* room structure: never draggable (§1.2 is
                                      about PLACED things; the floor is the room) */
    sol_u32     drag_handle;       /* group root being carried; 0 = none */
    vec3        drag_offset;       /* grab offset: pos - first plane hit */
    vec3        drag_start_pos;    /* pos at press (did it really move?) */
    sol_bool    drag_moved;        /* orbit: the slop latch; both: gates the save */
    /* whiteboard carry (P3 item 8): a card can hop onto/off a board mid-drag */
    sol_u32     drag_prev_parent;  /* pre-drag placement, restored if the drop */
    vec3        drag_prev_pos;     /*   is refused (a FILE card snaps home and */
    quat        drag_prev_rot;     /*   pins an ALIAS instead)                 */
    sol_u32     drag_ground_parent;/* parent while off-board (never a board)   */
    float       drag_plane_y;      /* ground-mode constraint height (P4 item 5
                                      fix): the GRAB height, so an elevated
                                      object tracks the cursor at its own
                                      depth — the floor plane under a raised
                                      lantern exaggerated every move */
    sol_u32     drag_board;        /* board hosting the carry; 0 = ground mode */
    float       drag_board_ox;     /* grab offset in board-local XY            */
    float       drag_board_oy;
    sol_u32     resize_board;      /* wall-board being corner-resized; 0 = none */
    vec3        resize_anchor;     /* the fixed (opposite) corner, world        */
    vec3        resize_u;          /* the wall's horizontal in-plane axis       */
    sol_u32     resize_room;       /* the board's parent room (wall clamp)      */
    Mesh        resize_handle_mesh;/* small corner quad; built once on first use */
    Mesh        gate_pane;        /* portal energy pane: a unit quad, built once on first use */
    sol_u32     picture_aim;       /* image card aimed at a wall/board this frame */
    sol_u32     picture_target;    /* the room (wall) or board to parent the picture */
    vec3        picture_local;     /* the picture's local pos under the target */
    quat        picture_rot;       /* the picture's local rotation */
    sol_u32     carry_prev_parent; /* the carried object's pre-pickup parent (restore on plant) */
    quat        carry_prev_rot;    /* and its pre-pickup local rotation */
    /* inventory (the bag): a mesh-less anchor tagged meta["inventory"]; items
       re-parented under it are "stowed" (scene_object_stowed hides them). */
    sol_u32     inv_anchor;     /* cached anchor handle; 0 = not created yet     */
    sol_bool    inv_open;       /* the modal screen is up                        */
    sol_bool    inv_was_open;   /* edge-detect for the cursor release            */
    int         inv_page;       /* current page in the grid                      */
    /* inventory thumbnails: one cached RGBA8 target per stowed item, plus a
       shared HDR scratch + two 1x1 neutral textures for the post-pass reuse. */
    struct { sol_u32 handle; RhiRenderTarget rt; } inv_thumbs[INV_THUMB_POOL];
    int             inv_thumb_count;
    RhiRenderTarget inv_thumb_hdr;   /* shared HDR scratch (RGBA16F)          */
    RhiTexture      inv_white_tex;   /* 1x1 white: neutral AO input           */
    RhiTexture      inv_black_tex;   /* 1x1 black: neutral godray/bloom/depth  */
    sol_u32     move_board;        /* picture being slid along its wall; 0 = none */
    vec3        move_grab;         /* origin - cursor-hit at grab, so it tracks the cursor */
    /* board view: frame a whiteboard head-on, cursor unlocked, to arrange cards.
       Transient UI state (never persisted). bv_t starts at 1.0 (settled). */
    sol_u32  board_view;          /* board being viewed; 0 = not in board view  */
    sol_bool board_view_was;      /* edge-detect for the cursor toggle           */
    double   last_press_t, last_press_x, last_press_y;  /* board-view double-click detect */
    vec3     bv_from_pos,  bv_to_pos;
    float    bv_from_yaw,  bv_to_yaw;
    float    bv_from_pitch, bv_to_pitch;
    float    bv_t;                /* 0..1 eased glide progress; >=1 = settled     */
    float    bv_dir;             /* +1 gliding to the board, -1 gliding back out  */
    vec3     bv_return_pos;       /* pose to restore on exit (where you stood)    */
    float    bv_return_yaw, bv_return_pitch;
    int      hover_corner;        /* resize-corner the pointer is over; -1 = none */
    int      resize_corner;       /* corner grabbed for the active resize (stays lit) */
    vec3     resize_grab;         /* grabbed-corner minus cursor-hit: no jump on grab */
    sol_u32     connect_from;      /* C armed a connection from this card; 0 = idle */
    sol_bool    c_was_down;
    sol_bool    n_was_down;        /* edge-detect spawn-note (N) */
    /* note editing (item 8 piece 5): FOCUS routes the keyboard to TEXT.
       Keys are buttons, chars are text — GLFW separates them (key vs char
       callback, the OS keymap between), and edit_handle is what bridges
       them: while it is set, chars append here and buttons go quiet. */
    sol_u32     edit_handle;       /* note being edited; 0 = none */
    char        edit_buf[EDIT_BUF_CAP];
    int         edit_len;
    int         edit_cursor;      /* byte offset into edit_buf: the caret */
    float       edit_goal_x;      /* remembered note-local x for Up/Down */
    int         edit_sel_anchor;  /* selection's fixed end; span = [min,max) with edit_cursor */
    int         click_seq;        /* shared multi-click counter (1 single, 2 double, 3 triple) */
    sol_bool    edit_dragging;    /* a left-drag is selecting text in the edited note */
    Mesh        caret_mesh;       /* unit caret quad; built once on first use */
    Mesh        folderbook_leaves_mesh; /* unit box for a folder book's white page block; built once */
    Palette     palette;           /* command palette state (palette.h) */
    /* the reader (item 9): VIEW state, never scene state — the open book
       rig lives here, not in the scene graph; nothing about reading
       persists. The source object hides while its book is aloft. */
    int         reader_state;      /* READER_IDLE/RISING/OPEN/RETURNING */
    float       reader_t;          /* 0..1 through the current animation */
    sol_u32     reader_source;     /* what is being read (group root) */
    vec3        reader_rest_pos;   /* the source's resting pose (world) */
    quat        reader_rest_rot;
    vec3        reader_a_pos, reader_b_pos;   /* current animation: a -> b */
    quat        reader_a_rot, reader_b_rot;
    vec3        reader_pos;        /* the pose drawn this frame */
    quat        reader_rot;
    Mesh        reader_cover, reader_block;   /* runtime meshes, per open */
    Material    reader_cover_mat, reader_block_mat;
    float       reader_params[5];  /* open-book w,h,t,board,sq */
    /* the pages (piece 4): the file's text, wrapped ONCE into a heap
       buffer; pages are just LINE RANGES into it, a spread is two */
    char       *reader_text;       /* wrapped; NULL = no content loaded */
    long        reader_text_len;
    int        *reader_line_off;   /* byte offset of each line start */
    int         reader_line_count;
    int         reader_lines_per_page;
    int         reader_spread;     /* current page-pair, 0-based */
    char      **reader_pages;      /* editable codex: page text array */
    int         reader_page_count;
    int         reader_page;       /* current page (the caret's page) */
    sol_bool    reader_editable;   /* a codex opened as a writable notebook */
    float       reader_cam_yaw0, reader_cam_yaw1;     /* swing the look to centre */
    float       reader_cam_pitch0, reader_cam_pitch1; /* the book as it rises */
    const Font *reader_font;       /* mono for code, sans for prose */
    float       reader_px2m;       /* body text scale, meters per font px */
    sol_bool    reader_is_image;       /* this book shows an image, not text */
    Mesh        reader_image_quad;     /* the aspect-fitted right-page quad */
    RhiTexture  reader_image_tex;      /* decoded image; .id==0 if none */
    int         reader_image_w, reader_image_h;  /* source pixel dims */
    sol_bool    arrow_l_was, arrow_r_was;     /* page-turn edges */
    /* the turning leaf (piece 5): an inextensible paper arc swept by the
       hinge angle, bowing opposite the travel (the lag) — strongest
       mid-flight, flat at both ends. Blank parchment; the spread's text
       swaps under it at the midpoint, when the leaf stands edge-on. */
    int         reader_turning;    /* 0 idle, +1 forward, -1 back */
    float       reader_turn_t;
    int         reader_turn_old;   /* the spread we are turning AWAY from */
    Mesh        reader_leaf;       /* rebuilt per frame (curl = vertex map) */
    LeafShape   reader_leaf_shape; /* this frame's section: mesh + ink share it */
    /* the app book (TODO5 slice): an open book whose meta["app"] routes the
       reader to an in-world widget UI instead of page text. */
    int       reader_app;                  /* 0 none, 1 synth */
    sol_bool  reader_app_was;              /* edge tracker for the cursor toggle */
    float     synth_params[SYNTH_PARAMS];  /* the open book's live patch */
    sol_u32   synth_rng;                   /* the "Roll" LCG state */
    WidgetCtx widget_ctx;                  /* this frame's emitted draw-list */
    Mesh      widget_quad;                 /* unit XY quad for widget rects (lazy) */
    /* terrain shading (item 10): set per draw by the scene loop, read by
       draw_mesh — the plot wears the slope/height palette */
    sol_bool    terrain_blend;
    float       terrain_y0, terrain_amp;
    sol_u32     current_terrain;   /* plot underfoot; 0 = none (HUD naming) */
    sol_bool    z_was_down;        /* edge-detect mint-abbey (Z, P6 item 10) */
    sol_bool    plan_on;
    sol_u32     plan_plot;         /* island the overlay was built for */
    Mesh        plan_mesh;         /* DERIVED, never serialized (arrows law) */
    /* board-pages (Board Pages Task 4): folder creation via 'd' in board view */
    vec3        folder_place_local;  /* board-local point captured at 'd' press */
    sol_bool    folder_place_has;    /* 0 = no board hit at press -> use center */
    sol_bool    d_was_down;          /* edge-detect for the 'd' folder key */
    sol_bool    paste_was_down;      /* edge-detect for Cmd+V paste */
    sol_bool    page_prev_was;       /* edge-detect for arrow-cycle (Task 5) */
    sol_bool    page_next_was;
    sol_u32     drop_target_handle;  /* folder under a dragged card (Task 8); 0 = none */
    /* Board multi-select (Board Multi-Select Tasks 2-8) */
    sol_u32     sel[MULTISEL_CAP];   /* multi-select set; <=1 mirrors selected_handle */
    int         sel_count;
    sol_u32     cut[MULTISEL_CAP];   /* cut buffer (Cmd+X): handles marked to MOVE on paste */
    int         cut_count;           /* 0 = nothing cut; GLOBAL — survives board_view_exit */
    sol_bool    cut_was_down;        /* edge-detect for Cmd+X */
    sol_bool    win_color_was;       /* edge-detect for Up/Down window glass cycle */
    sol_bool    win_style_was;       /* edge-detect for Left/Right window shape cycle */
    sol_bool    marquee_active;      /* a marquee gesture is underway (M2) */
    sol_bool    marquee_dragging;    /* moved past the slop -> a real rubber-band (M2) */
    sol_bool    marquee_add;         /* shift held -> union, else replace (M2) */
    double      marquee_x0, marquee_y0, marquee_x1, marquee_y1; /* screen px (M2) */
    float       marquee_lx0, marquee_ly0, marquee_lx1, marquee_ly1; /* board-local rect (M2) */
    float       marquee_px_scale;    /* fb_width/window_width: cursor(pts)->ui(px) for the draw */
    sol_bool    group_drag;          /* dragging the whole set together (M3) */
    vec3        group_prepos[MULTISEL_CAP]; /* per-member pre-drag board-local pos (M3) */
} AppState;

#define READER_IDLE      0
#define READER_RISING    1
#define READER_OPEN      2
#define READER_RETURNING 3
#define READER_RISE_SECS 0.45f
#define READER_TURN_SECS 0.50f
#define READER_LAG_MAX   0.85f    /* tip lag, radians, at mid-turn */
#define READER_FILE_CAP  (256L * 1024L)

/* Union AABB over all of a model's meshes (local space) — for auto-fitting an
   arbitrary-scale imported asset to the room. */
static Aabb union_bounds(const GlbModel *model) {
    Aabb    b;
    sol_u32 m;
    b.min = vec3_make( 1e30f,  1e30f,  1e30f);
    b.max = vec3_make(-1e30f, -1e30f, -1e30f);
    for (m = 0; m < model->count; m++) {
        Aabb e = model->parts[m].mesh.bounds;
        if (e.min.x < b.min.x) b.min.x = e.min.x;
        if (e.min.y < b.min.y) b.min.y = e.min.y;
        if (e.min.z < b.min.z) b.min.z = e.min.z;
        if (e.max.x > b.max.x) b.max.x = e.max.x;
        if (e.max.y > b.max.y) b.max.y = e.max.y;
        if (e.max.z > b.max.z) b.max.z = e.max.z;
    }
    return b;
}

/* Attach a loaded model's parts under `anchor` as DERIVED children: offset
   by -center so the anchor pivots the model in place. Derived objects are
   never serialized (6e) — the file stores the anchor + its glb ref, and
   parts regenerate from the .glb on every load, exactly as procedural
   meshes regenerate from their registry refs. */
static void glb_attach_parts(AppState *state, sol_u32 anchor, const GlbModel *model,
                             const char *path) {
    Aabb    b      = union_bounds(model);
    vec3    center = vec3_scale(vec3_add(b.min, b.max), 0.5f);
    sol_u32 m;
    for (m = 0; m < model->count; m++) {
        char    akey[320];
        sol_u32 h = scene_add(&state->scene, anchor, model->parts[m].mesh,
                              vec3_make(-center.x, -center.y, -center.z),
                              quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_material_set(&state->scene, h, model->parts[m].material);
        scene_meta_set(&state->scene, h, "derived", "1");
        /* the BORROW TICKET (P4 item 4): a part has no mesh_ref to re-derive
           a key from, so it carries its registry key in runtime meta —
           derived objects never serialize, so the format never sees it */
        glb_part_key(path, (int)m, akey);
        scene_meta_set(&state->scene, h, "akey", akey);
    }
}


/* Re-import every glb anchor's parts after a load (6e): anchors persist —
   position, name, relations, the identity — while parts regenerate from
   the file. Skips anchors that already carry derived children. Handles are
   collected first: attaching parts adds objects and can realloc the array. */
static void scene_reimport_glbs(AppState *state) {
    sol_u32 anchors[32];
    sol_u32 i, j, n = 0;
    for (i = 0; i < state->scene.count && n < 32; i++) {
        const SceneObject *o = &state->scene.objects[i];
        if (!scene_meta_get(&state->scene, o->handle, "glb")) continue;
        for (j = 0; j < state->scene.count; j++) {        /* already has parts? */
            const SceneObject *c = &state->scene.objects[j];
            const char *d;
            if (c->parent != o->handle) continue;
            d = scene_meta_get(&state->scene, c->handle, "derived");
            if (d && strcmp(d, "1") == 0) break;
        }
        if (j < state->scene.count) continue;
        anchors[n++] = o->handle;
    }
    for (i = 0; i < n; i++) {
        const char *path = scene_meta_get(&state->scene, anchors[i], "glb");
        GlbModel    model;
        if (!path || !glb_acquire_model(path, &model)) {
            fprintf(stderr, "glb re-import failed: %s — anchor kept, body missing\n",
                    path ? path : "(no path)");
            continue;                       /* the placed anchor outlives its asset */
        }
        glb_attach_parts(state, anchors[i], &model, path);
        glb_free(&model);
    }
}

/* World-space position of an object (its world matrix's translation column),
   falling back to the scene focus if the handle is gone. */
static vec3 object_world_pos(Scene *s, sol_u32 handle) {
    SceneObject *o = scene_get(s, handle);
    if (o) {
        mat4 w = scene_world_matrix(s, o);
        return vec3_make(w.m[12], w.m[13], w.m[14]);
    }
    return vec3_make(0.0f, 0.5f, 0.0f);
}

/* The human label for an object: its "name" meta (own, then its group
   root's), else its content path's basename (a FILE card is named by its
   file), else its mesh ref, else "#handle" into buf (>= 16 bytes). */
static const char *object_label(Scene *s, sol_u32 handle, char *buf) {
    SceneObject *o = scene_get(s, handle);
    const char  *m;
    sol_u32      walk;
    if (!o) return "?";
    m = scene_meta_get(s, handle, "name");
    if (m) return m;
    walk = handle;                              /* a part borrows its group's name */
    while (o && o->parent != 0) {
        walk = o->parent;
        o = scene_get(s, walk);
        m = o ? scene_meta_get(s, walk, "name") : (const char *)0;
        if (m) return m;
    }
    o = scene_get(s, handle);
    if (o->content) {
        const char *slash = strrchr(o->content, '/');
        return slash ? slash + 1 : o->content;
    }
    if (o->mesh_ref) return o->mesh_ref;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    sprintf(buf, "#%u", (unsigned)handle);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    return buf;
}

/* Which room contains point p? (P3 item 7.) Rooms are the §1.10 anchors; a
   room's VOLUME is derived from its shell child's parametric (w,d,h) — no
   mesh test, no event tracking, so the answer stays correct across
   teleports, reloads, and dragged rooms. Smallest containing volume wins
   (nested rooms). Returns the anchor's handle, 0 = not in any room. */
static sol_u32 room_containing(Scene *s, vec3 p) {
    sol_u32 i, best = 0;
    float   best_vol = 1e30f;
    for (i = 0; i < s->count; i++) {
        SceneObject *shell = &s->objects[i];
        vec3         org;
        float        w, d, h, vol;
        mat4         wm;
        if (!shell->mesh_ref || strcmp(shell->mesh_ref, "room") != 0) continue;
        if (!scene_get(s, shell->parent)) continue;        /* a shell needs its anchor */
        w = shell->mesh_param_count > 0 ? shell->mesh_params[0] : 6.0f;
        d = shell->mesh_param_count > 1 ? shell->mesh_params[1] : 4.0f;
        h = shell->mesh_param_count > 2 ? shell->mesh_params[2] : 3.0f;
        wm  = scene_world_matrix(s, shell);
        org = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
        if (p.x < org.x - w * 0.5f || p.x > org.x + w * 0.5f) continue;
        if (p.z < org.z - d * 0.5f || p.z > org.z + d * 0.5f) continue;
        if (p.y < org.y || p.y > org.y + h) continue;
        vol = w * d * h;
        if (vol < best_vol) { best_vol = vol; best = shell->parent; }
    }
    return best;
}

/* The room's ambient level: an explicit "ambient" meta on the anchor wins
   (the builder knows a doorway-sealed room is dim even though its own shell
   has an open side); otherwise derived from the shell's presence flags —
   fully sealed dims the sky's light, anything open stays bright. */
static float room_ambient(Scene *s, sol_u32 anchor) {
    const char *m = scene_meta_get(s, anchor, "ambient");
    sol_u32     i;
    if (m) return (float)strtod(m, (char **)0);
    for (i = 0; i < s->count; i++) {
        SceneObject *shell = &s->objects[i];
        if (shell->parent != anchor || !shell->mesh_ref ||
            strcmp(shell->mesh_ref, "room") != 0) continue;
        if (shell->mesh_param_count >= 8) {
            int k;
            for (k = 3; k < 8; k++) {
                if (shell->mesh_params[k] < 0.5f) return 1.0f;   /* an open side */
            }
        }
        return 0.35f;          /* sealed (flags all present, or defaulted) */
    }
    return 1.0f;
}

/* The GROUP an object belongs to: walk the parent chain upward, but STOP at
   a room anchor — a room is a PLACE things sit in, not a group they belong
   to (P4 item 2: with room shells pick-transparent, clicking the shared
   doorway wall surfaced this — it promoted to the hall anchor and lit the
   whole room + path). For a grouped import this is still the model anchor
   (every part shares one root, even when the import sits inside a room);
   for furniture parented to a room it's the piece itself — which also makes
   walls/paths individually draggable, as the drag rule always intended. */
static sol_u32 group_root(Scene *s, sol_u32 handle) {
    SceneObject *o = scene_get(s, handle);
    while (o && o->parent != 0) {
        SceneObject *par = scene_get(s, o->parent);
        if (scene_meta_get(s, o->parent, "room_type")) break;  /* the room boundary */
        if (par && par->mesh_ref &&
            (furniture_is_shelf(par->mesh_ref) || furniture_is_table(par->mesh_ref) ||
             strcmp(par->mesh_ref, "board") == 0))
            break;                              /* furniture / whiteboard: a PINNED item
                                                   (book/card/picture/note) is its own
                                                   group, only placed on its host */
        handle = o->parent;
        o = scene_get(s, handle);
    }
    return handle;
}

/* The pick ray for the current camera mode: through the crosshair (screen
   centre) in first person, through the cursor in orbit. */
static Ray pick_ray(AppState *st, GLFWwindow *w) {
    int    ww, wh;
    float  aspect, nx = 0.0f, ny = 0.0f;
    double mx, my;
    glfwGetWindowSize(w, &ww, &wh);
    aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
    if ((st->camera.mode == CAMERA_ORBIT || st->board_view != 0) && ww > 0 && wh > 0) {
        glfwGetCursorPos(w, &mx, &my);
        nx = 2.0f * (float)mx / (float)ww - 1.0f;
        ny = 1.0f - 2.0f * (float)my / (float)wh;
    }
    return camera_ray(&st->camera, nx, ny, aspect);
}

/* Pick policy (P4 item 2): room shells and terrain are LAND — places you
   stand, not things you select. Their triangles are pick-TRANSPARENT, so a
   click through a doorway reaches what stands beyond, and a click on a wall
   or a hillside means "nothing" (deselect), as it always has. The engine
   takes this as a callback; scene.c never learns these names. */
static sol_bool pick_skip_land(const Scene *s, const SceneObject *o, void *ctx) {
    (void)s; (void)ctx;
    return o->mesh_ref != NULL && (strcmp(o->mesh_ref, "room") == 0 ||
                                   strcmp(o->mesh_ref, "terrain") == 0);
}

/* Refresh the spatial index from the scene: collect (handle, world AABB)
   for everything with geometry; the SAME set in the same order refits
   (cheap, motion only), a changed set rebuilds (add/remove/load). Called
   on demand by picking — clicks are rare — and per frame once culling
   (piece 4) wants current boxes anyway. */
static void bvh_refresh(AppState *st) {
    Scene   *s = &st->scene;
    int      n = 0;
    sol_u32  i;
    sol_bool same;
    if ((int)s->count > st->bvh_cap) {
        int      ncap = (int)s->count;
        sol_u32 *ni = (sol_u32 *)realloc(st->bvh_ids,   (size_t)ncap * sizeof *ni);
        Aabb    *nb = (Aabb *)realloc(st->bvh_boxes, (size_t)ncap * sizeof *nb);
        if (ni) st->bvh_ids   = ni;
        if (nb) st->bvh_boxes = nb;
        if (!ni || !nb) return;             /* OOM: the stale tree still works */
        st->bvh_cap = ncap;
    }
    same = SOL_TRUE;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->mesh.index_count == 0) continue;
        if (!scene_object_active(s, o->handle)) continue;   /* hidden workspace */
        if (n >= st->bvh_count || st->bvh_ids[n] != o->handle) same = SOL_FALSE;
        st->bvh_ids[n]   = o->handle;
        st->bvh_boxes[n] = aabb_transform(scene_world_matrix(s, o),
                                          o->mesh.bounds);
        n++;
    }
    if (n != st->bvh_count) same = SOL_FALSE;
    if (same) {
        bvh_refit(&st->bvh, st->bvh_boxes, n);
    } else {
        bvh_build(&st->bvh, st->bvh_boxes, st->bvh_ids, n);
        st->bvh_count = n;
    }
}

/* The meadow (P4 item 3 piece 2): every terrain island grows its grass.
   DERIVED data, the arrows pattern at landscape scale: from the island's
   SEED (its identity) a deterministic LCG scatters tufts; each sits at
   terrain_height — the one source of truth, the same function that built
   the hills and stands you on them — and the shader palette's own
   slope/height rules decide where grass grows at all (moss, not crag:
   one author, now three consumers). World positions bake into one static
   instance buffer per island: one draw per island per frame. Rebuilt on
   load and on mint; never serialized. */
#define MEADOW_MAX_TUFTS 12000
#define MEADOW_PER_M2    6.0f

static float meadow_rnd(sol_u32 *rng) {
    *rng = *rng * 1664525u + 1013904223u;
    return (float)((*rng >> 8) & 0xFFFFu) / 65535.0f;
}

/* THE ONE WIND (P7 item 9): a tiny pure field — a prevailing direction
   that slowly wobbles, and a GUST scalar (0..1) that swells and fades.
   Evaluated once per frame at the camera; the meadow + canopy sway,
   particle drift, and the audio wind gain all read it, so the island
   gusts TOGETHER (one gust, four senses — the lantern's law at weather
   scale). The traveling-wave crossing lives in the shaders: they add
   dot(worldPos.xz, dir) to the phase, so a gust front sweeps the field. */
static void wind_at(float t, float x, float z,
                    float *dx, float *dz, float *gust) {
    float ang = 0.7f + 0.40f * sinf(t * 0.07f) + 0.18f * sinf(t * 0.019f);
    float g   = 0.55f * sinf(t * 0.42f + x * 0.03f - z * 0.021f)
              + 0.30f * sinf(t * 0.91f + x * 0.06f)
              + 0.15f * sinf(t * 1.70f - z * 0.05f);
    *dx = cosf(ang);
    *dz = sinf(ang);
    g = 0.5f + 0.5f * g;                 /* into 0..1 */
    *gust = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
}

/* flowers cluster in PATCHES (item 7): a per-cell hash over a coarse
   grid — a cell is "flowery" or not, so blooms gather rather than
   sprinkle. The lane keeps it off the grass scatter's rng stream. */
static float flower_patch(sol_u32 seed, float lx, float lz) {
    int     cx = (int)floorf(lx / 5.0f), cz = (int)floorf(lz / 5.0f);
    sol_u32 h  = seed * 374761393u + 1u;
    h ^= (sol_u32)cx * 668265263u; h ^= h >> 15; h *= 2246822519u;
    h ^= (sol_u32)cz * 3266489917u; h ^= h >> 13; h *= 374761393u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

/* a sparse bright palette, picked per bloom (item 7) */
static void flower_color(sol_u32 *rng, float *r, float *g, float *b) {
    static const float PAL[6][3] = {   /* vivid but <=1: no bloom */
        { 0.92f, 0.18f, 0.14f },   /* red     */
        { 0.96f, 0.86f, 0.20f },   /* yellow  */
        { 0.95f, 0.93f, 0.92f },   /* white   */
        { 0.58f, 0.22f, 0.88f },   /* violet  */
        { 0.96f, 0.52f, 0.14f },   /* orange  */
        { 0.90f, 0.40f, 0.66f }    /* pink    */
    };
    int i = (int)(meadow_rnd(rng) * 6.0f);
    if (i > 5) i = 5;
    *r = PAL[i][0]; *g = PAL[i][1]; *b = PAL[i][2];
}

static void meadow_rebuild(AppState *st) {
    sol_u32 i;
    int     p, total = 0;
    for (p = 0; p < st->meadow_count; p++) {
        rhi_destroy_buffer(st->meadow[p].data);
        if (st->meadow[p].flower_count) rhi_destroy_buffer(st->meadow[p].flowers);
    }
    st->meadow_count = 0;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float   w, d, amp;
        sol_u32 rng;
        mat4    world;
        float  *buf;
        int     target, placed, k;
        if (st->meadow_count >= (int)(sizeof st->meadow / sizeof st->meadow[0]))
            break;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "terrain") != 0) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;   /* hidden workspace */
        w   = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "w");
        d   = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "d");
        amp = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "amp");
        rng = (sol_u32)mesh_ref_param("terrain", o->mesh_params,
                                      o->mesh_param_count, "seed")
                  * 2654435761u + 1u;
        world  = scene_world_matrix(&st->scene, o);
        target = (int)(w * d * MEADOW_PER_M2);
        if (target > MEADOW_MAX_TUFTS) target = MEADOW_MAX_TUFTS;
        buf = (float *)malloc((size_t)target * 8 * sizeof(float));
        if (buf == NULL) continue;
        placed = 0;
        for (k = 0; k < target; k++) {
            float lx, lz, h, sx, sz, slope, r1, r2, r3;
            vec3  wp;
            lx = (meadow_rnd(&rng) - 0.5f) * (w - 1.5f);
            lz = (meadow_rnd(&rng) - 0.5f) * (d - 1.5f);
            h  = terrain_height(o->mesh_params, o->mesh_param_count, lx, lz);
            /* the palette's rules, replayed: full stone by slope 0.45,
               crag country above ~0.78 of the relief — no grass there */
            sx = terrain_height(o->mesh_params, o->mesh_param_count,
                                lx + 0.5f, lz);
            sz = terrain_height(o->mesh_params, o->mesh_param_count,
                                lx, lz + 0.5f);
            slope = sqrtf((sx - h) * (sx - h) + (sz - h) * (sz - h)) * 2.0f;
            if (slope > 0.45f) continue;
            if (amp > 0.0f && h / amp > 0.78f) continue;
            wp = mat4_mul_point(world, vec3_make(lx, h, lz));
            r1 = meadow_rnd(&rng);
            r2 = meadow_rnd(&rng);
            r3 = meadow_rnd(&rng);
            buf[placed * 8 + 0] = wp.x;
            buf[placed * 8 + 1] = wp.y;
            buf[placed * 8 + 2] = wp.z;
            buf[placed * 8 + 3] = 0.16f + 0.20f * r1;       /* tuft size, m */
            buf[placed * 8 + 4] = 0.10f + 0.10f * r2;       /* moss-family greens */
            buf[placed * 8 + 5] = 0.22f + 0.16f * r3;
            buf[placed * 8 + 6] = 0.05f + 0.06f * r2;
            buf[placed * 8 + 7] = 1.0f;
            placed++;
        }
        if (placed > 0) {
            MeadowPatch *mp = &st->meadow[st->meadow_count];
            mp->data   = rhi_create_buffer(RHI_BUFFER_VERTEX, buf,
                             (size_t)placed * 8 * sizeof(float));
            mp->count  = placed;
            mp->island = o->handle;
            mp->flower_count = 0;
            {   /* the flowers (item 7): a sparser scatter, gathered in
                   patches, bright per-bloom tints — the meadow grown up */
                sol_u32 fseed = (sol_u32)mesh_ref_param("terrain",
                                    o->mesh_params, o->mesh_param_count,
                                    "seed") * 2654435761u + 101u;
                int     ftarget = (int)(w * d * 0.5f), fk, fn = 0;
                float  *fbuf;
                if (ftarget > MEADOW_MAX_TUFTS) ftarget = MEADOW_MAX_TUFTS;
                fbuf = (float *)malloc((size_t)(ftarget > 0 ? ftarget : 1)
                                       * 8 * sizeof(float));
                for (fk = 0; fbuf && fk < ftarget; fk++) {
                    float lx, lz, h, sx, sz, slope, cr, cg, cb;
                    vec3  wp;
                    lx = (meadow_rnd(&rng) - 0.5f) * (w - 1.5f);
                    lz = (meadow_rnd(&rng) - 0.5f) * (d - 1.5f);
                    if (flower_patch(fseed, lx, lz) < 0.62f) continue; /* patchy */
                    h  = terrain_height(o->mesh_params, o->mesh_param_count, lx, lz);
                    sx = terrain_height(o->mesh_params, o->mesh_param_count, lx + 0.5f, lz);
                    sz = terrain_height(o->mesh_params, o->mesh_param_count, lx, lz + 0.5f);
                    slope = sqrtf((sx-h)*(sx-h) + (sz-h)*(sz-h)) * 2.0f;
                    if (slope > 0.40f) continue;
                    if (amp > 0.0f && h / amp > 0.70f) continue;
                    wp = mat4_mul_point(world, vec3_make(lx, h, lz));
                    flower_color(&rng, &cr, &cg, &cb);
                    fbuf[fn * 8 + 0] = wp.x;
                    fbuf[fn * 8 + 1] = wp.y;
                    fbuf[fn * 8 + 2] = wp.z;
                    fbuf[fn * 8 + 3] = 0.14f + 0.10f * meadow_rnd(&rng);
                    fbuf[fn * 8 + 4] = cr;
                    fbuf[fn * 8 + 5] = cg;
                    fbuf[fn * 8 + 6] = cb;
                    fbuf[fn * 8 + 7] = 1.0f;
                    fn++;
                }
                if (fbuf && fn > 0) {
                    mp->flowers = rhi_create_buffer(RHI_BUFFER_VERTEX, fbuf,
                                      (size_t)fn * 8 * sizeof(float));
                    mp->flower_count = fn;
                }
                free(fbuf);
            }
            st->meadow_count++;
            total += placed;
        }
        free(buf);
    }
    if (st->meadow_count > 0)
        printf("the meadow: %d island(s), %d tufts + flowers\n",
               st->meadow_count, total);
}

/* The instanced ornament (P6 item 10): every balustrade's balusters as
   one static instance buffer of LOCAL slots — gothic_balusters is the
   one author (the carcass's sill/rail constants ride gothic.h, so the
   copies stand exactly between them). Rebuilt when the fingerprint
   moves; dragging only moves uModel, which the pool never bakes. */
#define ORN_MAX_SLOTS 512
#define ORN_LEAF_SWAY 0.10f   /* rustle amplitude (item 4's wind down-payment) */

/* add one finished patch to the pool; returns SOL_FALSE if it's full */
static sol_bool orn_add(AppState *st, const float *inst, int n, int kind,
                        sol_u32 source, Material mat) {
    OrnamentPatch *op;
    if (n <= 0) return SOL_TRUE;
    if (st->ornament_count >=
        (int)(sizeof st->ornament / sizeof st->ornament[0])) return SOL_FALSE;
    op = &st->ornament[st->ornament_count++];
    op->data     = rhi_create_buffer(RHI_BUFFER_VERTEX, inst,
                                     (size_t)n * 8 * sizeof(float));
    op->count    = n;
    op->kind     = kind;
    op->source   = source;
    op->material = mat;
    return SOL_TRUE;
}

static void ornament_rebuild(AppState *st) {
    int     p, balusters = 0, leaves = 0, trees = 0;
    sol_u32 i;
    for (p = 0; p < st->ornament_count; p++)
        rhi_destroy_buffer(st->ornament[p].data);
    st->ornament_count = 0;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float        inst[ORN_MAX_SLOTS * 8];
        int          n, k, species;
        if (!o->mesh_ref) continue;

        if (strcmp(o->mesh_ref, "balustrade") == 0) {
            float    xs[ORN_MAX_SLOTS], gap;
            float    len = mesh_ref_param("balustrade", o->mesh_params,
                                          o->mesh_param_count, "len");
            float    h   = mesh_ref_param("balustrade", o->mesh_params,
                                          o->mesh_param_count, "h");
            unsigned seed = (unsigned)(mesh_ref_param("balustrade",
                                o->mesh_params, o->mesh_param_count, "seed")
                                + 0.5f);
            float    ruin = mesh_ref_param("balustrade", o->mesh_params,
                                           o->mesh_param_count, "ruin");
            n = gothic_balusters(len, h, seed, ruin, xs, ORN_MAX_SLOTS);
            gap = (h - GOTHIC_BALUSTER_RAIL) - GOTHIC_BALUSTER_SILL;
            if (n <= 0 || gap < 0.2f) continue;
            for (k = 0; k < n; k++) {
                inst[k * 8 + 0] = xs[k];
                inst[k * 8 + 1] = GOTHIC_BALUSTER_SILL;
                inst[k * 8 + 2] = 0.0f;
                inst[k * 8 + 3] = 0.0f;   /* square balusters sit straight */
                inst[k * 8 + 4] = gap;    /* thickness rides the gap */
                inst[k * 8 + 5] = gap;
                inst[k * 8 + 6] = 0.0f;
                inst[k * 8 + 7] = 0.0f;   /* sway amp 0: stone never moves */
            }
            if (!orn_add(st, inst, n, ORN_BALUSTER, o->handle,
                         o->material)) break;
            balusters += n;

        } else if ((species = flora_species(o->mesh_ref)) >= 0) {
            FloraLeaf slots[ORN_MAX_SLOTS];
            Material  mat = material_default();
            int       leaf_kind = flora_leaf_kind(species);
            n = flora_canopy(species, o->mesh_params, o->mesh_param_count,
                             slots, ORN_MAX_SLOTS);
            if (n <= 0) continue;
            for (k = 0; k < n; k++) {
                inst[k * 8 + 0] = slots[k].pos.x;
                inst[k * 8 + 1] = slots[k].pos.y;
                inst[k * 8 + 2] = slots[k].pos.z;
                inst[k * 8 + 3] = slots[k].yaw;
                inst[k * 8 + 4] = slots[k].scale;
                inst[k * 8 + 5] = slots[k].scale;
                inst[k * 8 + 6] = slots[k].phase;
                inst[k * 8 + 7] = ORN_LEAF_SWAY;   /* leaves rustle */
            }
            mat.base_color = flora_leaf_color(species);
            mat.roughness  = 0.85f;
            mat.metallic   = 0.0f;
            if (!orn_add(st, inst, n,
                         leaf_kind == FLORA_LEAF_CONIFER ? ORN_LEAF_CONIFER
                                                         : ORN_LEAF_BROAD,
                         o->handle, mat)) break;
            leaves += n;
            trees++;
        }
    }
    if (balusters || leaves)
        printf("ornament: %d balusters, %d leaves on %d tree(s)\n",
               balusters, leaves, trees);
}

/* the per-frame sync: a cheap fingerprint over (handle, params) of every
   ornament SOURCE (balustrades + trees) — immune to missed call sites
   (mint, L, X, hand-edit all just change the fingerprint) */
static void ornament_sync(AppState *st) {
    sol_u32 i;
    int     j;
    float   fp = 0.0f;
    for (i = 0; i < st->scene.count; i++) {
        const SceneObject *o = &st->scene.objects[i];
        if (!o->mesh_ref) continue;
        if (strcmp(o->mesh_ref, "balustrade") != 0 &&
            flora_species(o->mesh_ref) < 0) continue;
        fp += (float)o->handle * 0.618034f;
        for (j = 0; j < o->mesh_param_count; j++)
            fp += o->mesh_params[j] * (float)(j + 1);
    }
    if (fp != st->ornament_fp) {
        st->ornament_fp = fp;
        ornament_rebuild(st);
    }
}

/* the leaf visit for picking: app policy first, then the shared narrow
   phase; returns the (possibly sharpened) best t so the walk prunes harder */
typedef struct {
    AppState *st;
    Ray       ray;
    sol_u32   hit;
} PalacePickCtx;

static float palace_pick_leaf(sol_u32 handle, float best_t, void *ctx) {
    PalacePickCtx *pc = (PalacePickCtx *)ctx;
    SceneObject   *o  = scene_get(&pc->st->scene, handle);
    if (o == NULL) return best_t;
    if (pick_skip_land(&pc->st->scene, o, NULL)) return best_t;
    if (scene_pick_object(&pc->st->scene, o, pc->ray, best_t, &best_t))
        pc->hit = handle;
    return best_t;
}

/* Side-effect-free pick through an NDC point: the nearest hit's handle (0 =
   none). do_pick adds the selection/focus behavior on top; the drag path
   (item 4) needs just the hit. Broad phase = the BVH (P4 item 2 piece 3);
   semantics identical to the linear scene_pick (pick_test proves it). */
static sol_u32 pick_at(AppState *st, GLFWwindow *w, float ndc_x, float ndc_y, float *t_out) {
    int           ww, wh;
    float         aspect, best;
    PalacePickCtx pc;
    glfwGetWindowSize(w, &ww, &wh);                 /* cursor is in window coords */
    aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
    pc.st  = st;
    pc.ray = camera_ray(&st->camera, ndc_x, ndc_y, aspect);
    pc.hit = 0;
    bvh_refresh(st);                                /* current boxes, this exact frame */
    best = bvh_ray_query(&st->bvh, pc.ray, 1e30f, palace_pick_leaf, &pc);
    if (t_out) *t_out = (pc.hit != 0) ? best : 0.0f;
    return pc.hit;
}

/* The selection UNIT's root, mirroring the drag rule (item 6): a card
   (non-PLAIN kind) selects INDIVIDUALLY — its parent is a room, and
   grouping by root would highlight the whole room; a prop selects as its
   import group (book/candle parts light together). */
static sol_u32 selection_root(Scene *s, sol_u32 handle) {
    SceneObject *o = scene_get(s, handle);
    if (!o) return 0;
    if (o->kind != KIND_PLAIN) return handle;
    if (o->mesh_ref && strcmp(o->mesh_ref, "arrow") == 0)
        return handle;            /* an edge is its own thing: select it, not
                                     the board group it hangs on (item 8) */
    return group_root(s, handle);
}

/* ---- skinned models (P4 item 9): loaded once per path, session-lived.
   An object wears meta skin_glb="Fox.glb"; this registry turns the path
   into mesh + material + skeleton on first sight. The POSE is view state
   (§1.6 for skeletons): sampled fresh from absolute t every frame, never
   stored, never saved — the file records which model, and (piece 3)
   which clip at what speed. */
#define SKINNED_MAX 4
typedef struct {
    char         path[128];
    SkinnedModel model;
    int          state;            /* 0 = empty, 1 = loaded, -1 = failed */
} SkinnedSlot;
static SkinnedSlot g_skinned[SKINNED_MAX];

static SkinnedModel *skinned_get(const char *path) {
    int i;
    for (i = 0; i < SKINNED_MAX; i++) {
        if (g_skinned[i].state != 0 && strcmp(g_skinned[i].path, path) == 0)
            return g_skinned[i].state == 1 ? &g_skinned[i].model
                                           : (SkinnedModel *)0;
    }
    for (i = 0; i < SKINNED_MAX; i++) {
        if (g_skinned[i].state == 0) {
            if (strlen(path) >= sizeof g_skinned[i].path)
                return (SkinnedModel *)0;
            strcpy(g_skinned[i].path, path);
            if (glb_load_skinned(path, &g_skinned[i].model)) {
                g_skinned[i].state = 1;
                printf("skinned model: %s (%d joints, %d clips)\n", path,
                       g_skinned[i].model.rig.skel.joint_count,
                       g_skinned[i].model.rig.clip_count);
                return &g_skinned[i].model;
            }
            g_skinned[i].state = -1;       /* remembered: fail once, quietly after */
            fprintf(stderr, "skinned model failed: %s\n", path);
            return (SkinnedModel *)0;
        }
    }
    return (SkinnedModel *)0;
}

/* the animate component's effective (clip, speed) for an object — the
   renderer is its consumer; absent component = the rest pose (clip -1).
   The prefix+defaults merge, the standing rule. */
static void skin_anim_of(const SceneObject *o, int *clip, float *speed) {
    sol_u32 c;
    *clip  = -1;
    *speed = 1.0f;
    if (o->overlay_clip >= 0) {        /* a behavior's CURRENT gait (§1.6):
                                          the wander brain outranks the
                                          persisted rule, never the file */
        *clip  = o->overlay_clip;
        *speed = o->overlay_speed;
        return;
    }
    for (c = 0; c < o->comp_count; c++) {
        const Component *cp = &o->components[c];
        const float     *def;
        float            eff[2];
        int              n, k;
        if (strcmp(cp->type, "animate") != 0) continue;
        n = component_schema("animate", (const char *const **)0, &def);
        for (k = 0; k < n && k < 2; k++)
            eff[k] = k < cp->param_count ? cp->params[k] : def[k];
        *clip  = (int)eff[0];
        *speed = eff[1];
        return;
    }
}

/* one pose at absolute t -> the palette (deterministic; looping) */
static void skinned_palette_at(const SkinnedModel *sm, int clip, float t,
                               mat4 *pal) {
    vec3 lt[SKEL_MAX_JOINTS], ls[SKEL_MAX_JOINTS];
    quat lr[SKEL_MAX_JOINTS];
    const SkelClip *cl = (clip >= 0 && clip < sm->rig.clip_count)
                       ? &sm->rig.clips[clip] : (const SkelClip *)0;
    skel_pose(&sm->rig.skel, cl, t, SOL_TRUE, lt, lr, ls);
    skel_palette(&sm->rig.skel, lt, lr, ls, pal);
}

/* ---- the palace's voice (P4 item 8 piece 2): minted buffers + the
   producer side of the voice protocol. Slots 0..7 are RESERVED for the
   long-lived loops piece 3 brings (wind); one-shots round-robin the rest
   and may steal a still-playing slot — for short SFX, the oldest sound
   losing its tail beats silence. The generation stamp makes any command
   aimed at a stolen voice a no-op instead of a misfire. */
#define VOICE_ONESHOT_BASE 8

static float   g_snd_blip[SYNTH_RATE];
static int     g_snd_blip_frames = 0;
static sol_u32 g_voice_gen = 0u;
static int     g_voice_rr  = 0;

static void play_oneshot(const float *buf, int frames, float gain, float pan) {
    MixCmd c;
    if (buf == (const float *)0 || frames <= 0) return;
    c.kind   = MIX_CMD_START;
    c.slot   = VOICE_ONESHOT_BASE
             + (g_voice_rr++ % (MIX_VOICES - VOICE_ONESHOT_BASE));
    c.gen    = ++g_voice_gen;
    c.buf    = buf;
    c.frames = frames;
    c.gain   = gain;
    c.pan    = pan;
    c.loop   = 0;
    audio_push(&c);              /* full ring = a dropped blip, never a stall */
}

/* ---- the sound bank (piece 3): every buffer the palace plays, minted at
   startup from presets merged with the optional sounds.stml overrides,
   re-minted by the watcher when that file changes — sound design is a
   text editor and your ears. Buffers are static and session-lived, which
   is what satisfies the mixer's lifetime contract by construction; a
   re-mint rewrites the same arrays in place (a momentarily-playing voice
   reads a blend of old and new samples for one buffer length — the
   audible cost of a live reload, accepted). */
#define SND_STEP_VARIANTS 6
#define SND_LOOP_MAX      (SYNTH_RATE * 4)

static float g_snd_step[SND_STEP_VARIANTS][SYNTH_RATE / 2];
static int   g_snd_step_frames[SND_STEP_VARIANTS];
static float g_snd_whoosh[SYNTH_RATE];
static int   g_snd_whoosh_frames = 0;
static float g_snd_thump[SYNTH_RATE];
static int   g_snd_thump_frames = 0;
static float g_snd_wind[SND_LOOP_MAX];
static int   g_snd_wind_frames = 0;
static float g_snd_crackle[SND_LOOP_MAX];
static int   g_snd_crackle_frames = 0;
static float g_snd_water[SND_LOOP_MAX];           /* P8 item 8: the pond's trickle */
static int   g_snd_water_frames = 0;

/* sounds.stml: <sounds><sound type="step" lpcut="900"/></sounds> — knobs
   by NAME from the synth schema, any subset over the preset (the mesh-ref
   pattern, not the component prefix rule: these params have names). */
#define SND_OVERRIDE_MAX 12
static char  g_snd_over_type[SND_OVERRIDE_MAX][16];
static float g_snd_over_params[SND_OVERRIDE_MAX][SYNTH_PARAMS];
static int   g_snd_over_count = 0;

static const float *snd_params(const char *type) {
    int i;
    for (i = 0; i < g_snd_over_count; i++) {
        if (strcmp(g_snd_over_type[i], type) == 0)
            return g_snd_over_params[i];
    }
    return synth_preset(type);
}

static void load_sound_overrides(void) {
    char     *src;
    StmlNode *root, *top;
    sol_u32   i;
    g_snd_over_count = 0;
    src = fs_read_file("sounds.stml", 64L * 1024L, (long *)0, (int *)0);
    if (src == NULL) return;                       /* no file = pure presets */
    root = stml_parse(src);
    free(src);
    if (root == NULL) {
        fprintf(stderr, "sounds.stml: parse error — using presets\n");
        return;
    }
    top = stml_child(root, "sounds");
    for (i = 0; top && i < top->child_count; i++) {
        StmlNode    *n  = top->children[i];
        const char  *ty = stml_attr(n, "type");
        const float *base;
        int          k;
        if (n->tag == NULL || strcmp(n->tag, "sound") != 0 || ty == NULL)
            continue;
        base = synth_preset(ty);
        if (base == NULL) {
            fprintf(stderr, "sounds.stml: unknown sound type '%s'\n", ty);
            continue;
        }
        if (g_snd_over_count >= SND_OVERRIDE_MAX ||
            strlen(ty) >= sizeof g_snd_over_type[0]) continue;
        strcpy(g_snd_over_type[g_snd_over_count], ty);
        for (k = 0; k < SYNTH_PARAMS; k++) {
            const char *v = stml_attr(n, synth_param_names()[k]);
            g_snd_over_params[g_snd_over_count][k] =
                v ? (float)atof(v) : base[k];
        }
        g_snd_over_count++;
    }
    stml_free(root);
}

static void sound_bank_mint(void) {
    float lp[SYNTH_PARAMS];
    int   i;
    g_snd_blip_frames = synth_render(snd_params("blip"), 7u,
                                     g_snd_blip, SYNTH_RATE);
    for (i = 0; i < SND_STEP_VARIANTS; i++) {       /* jitter pre-minted as
                                                       variants: no two steps
                                                       alike, picked per stride */
        g_snd_step_frames[i] = synth_render(snd_params("step"),
                                            101u + (sol_u32)i,
                                            g_snd_step[i], SYNTH_RATE / 2);
    }
    g_snd_whoosh_frames = synth_render(snd_params("whoosh"), 7u,
                                       g_snd_whoosh, SYNTH_RATE);
    g_snd_thump_frames  = synth_render(snd_params("thump"), 7u,
                                       g_snd_thump, SYNTH_RATE);
    /* the loops render LOOP-SHAPED (no attack/decay — fades belong to the
       mixer's gain) and tail-into-head blended: seamless by construction,
       offline — the "no loop seams" goal without synthesis in the callback */
    memcpy(lp, snd_params("wind"), sizeof lp);
    lp[1] = 0.0f; lp[2] = 3.0f; lp[4] = 0.0f;
    g_snd_wind_frames = synth_render_loop(lp, 7u, g_snd_wind,
                                          SND_LOOP_MAX, SYNTH_RATE / 4);
    memcpy(lp, snd_params("crackle"), sizeof lp);
    lp[1] = 0.0f; lp[4] = 0.0f;
    g_snd_crackle_frames = synth_render_loop(lp, 7u, g_snd_crackle,
                                             SND_LOOP_MAX, SYNTH_RATE / 4);
    memcpy(lp, snd_params("water"), sizeof lp);
    lp[1] = 0.0f; lp[4] = 0.0f;
    g_snd_water_frames = synth_render_loop(lp, 7u, g_snd_water,
                                           SND_LOOP_MAX, SYNTH_RATE / 4);
}

/* ---- the living voices: wind on slot 0, lantern crackles on 1..6, the
   nearest pond's water on 7 (P8 item 8). All allocation logic is
   PRODUCER-side; the consumer only obeys. */
#define VOICE_LANTERN_BASE 1
#define VOICE_LANTERN_MAX  6
#define VOICE_WATER        7

static sol_u32 g_lantern_handle[VOICE_LANTERN_MAX];
static sol_u32 g_lantern_gen[VOICE_LANTERN_MAX];
static sol_u32 g_water_handle = 0u;               /* the pond holding the water voice (P8 item 8) */
static sol_u32 g_water_gen    = 0u;
static float   g_wind_cur = 0.0f, g_wind_sent = -1.0f;
static sol_u32 g_wind_gen = 0u;
static int     g_loop_voices = 0;                /* the HUD's number */
static sol_bool g_muted = SOL_FALSE;             /* global audio mute (palette toggle) */
static float   g_step_acc = 0.0f;
static vec3    g_step_prev;
static int     g_step_prev_ok = 0;
static sol_u32 g_step_rng = 12345u;

/* (re)arm the loops — at startup and after a sounds.stml re-mint changed
   buffer lengths; lantern slots are dropped and re-claimed next frame */
static void audio_loops_restart(void) {
    MixCmd c;
    int    i;
    memset(&c, 0, sizeof c);
    c.kind   = MIX_CMD_START;
    c.slot   = 0;
    c.gen    = ++g_wind_gen;
    c.buf    = g_snd_wind;
    c.frames = g_snd_wind_frames;
    c.gain   = g_wind_cur;
    c.loop   = 1;
    audio_push(&c);
    g_wind_sent = g_wind_cur;
    for (i = 0; i < VOICE_LANTERN_MAX; i++) {
        if (g_lantern_handle[i] != 0u) {
            memset(&c, 0, sizeof c);
            c.kind = MIX_CMD_STOP;
            c.slot = VOICE_LANTERN_BASE + i;
            c.gen  = g_lantern_gen[i];
            audio_push(&c);
            g_lantern_handle[i] = 0u;
        }
    }
}

/* place a world sound in the stereo field (P8 item 8): a windowed
   inverse-square attenuation (the item-5 light law, heard) + a constant-power
   auto-pan from the camera's flattened right vector. Lifted from the lantern
   crackle so any located source — a flame, a pond, a future bell — shares one
   law. The caller applies its own gain shaping (a flame's flicker, a cap). */
static void spatialize(vec3 src, vec3 campos, vec3 right, float radius,
                       float *att, float *pan) {
    vec3  dv = vec3_sub(src, campos);
    float d  = sqrtf(vec3_dot(dv, dv));
    float w  = 1.0f - (d / radius) * (d / radius) * (d / radius) * (d / radius);
    if (w < 0.0f) w = 0.0f;
    *att = (w * w) / (d * d + 1.0f);
    *pan = 0.0f;
    if (d > 0.001f) {
        float p = vec3_dot(vec3_scale(dv, 1.0f / d), right);
        if (p < -1.0f) p = -1.0f;
        if (p >  1.0f) p =  1.0f;
        *pan = p;
    }
}

/* the reverb the LISTENER's environment calls for (P8 item 8): 0 = dry/open
   air, 1 = a small enclosed room, 2 = a church (long + bright). Regular rooms
   come straight from room_containing; a church has no 'room' shell, so a
   generous proximity cylinder around each church anchor stands in for true
   wall-containment — the eased send smooths the threshold either way. */
static int listener_reverb_kind(AppState *st) {
    sol_u32 i;
    vec3    cam = st->camera.pos;
    if (st->current_room != 0) {
        const char *rt = scene_meta_get(&st->scene, st->current_room, "room_type");
        return (rt && strcmp(rt, "church") == 0) ? 2 : 1;
    }
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o  = &st->scene.objects[i];
        const char  *rt = scene_meta_get(&st->scene, o->handle, "room_type");
        mat4         wm;
        vec3         a;
        float        dx, dz;
        if (!rt || strcmp(rt, "church") != 0) continue;
        wm = scene_world_matrix(&st->scene, o);
        a  = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
        dx = cam.x - a.x; dz = cam.z - a.z;
        if (dx * dx + dz * dz > 13.0f * 13.0f) continue;   /* ~the abbey footprint */
        if (cam.y < a.y - 1.5f || cam.y > a.y + 14.0f) continue;
        return 2;
    }
    return 0;
}

/* one global reverb (P8 item 8), eased then pushed over the same ring as the
   wind. {decay, damp, wet} per listener-room kind — kinds-are-presets. */
static float g_rv_decay = 0.5f, g_rv_damp = 0.5f, g_rv_wet = 0.0f;
static float g_rv_decay_sent = -1.0f, g_rv_damp_sent = -1.0f, g_rv_wet_sent = -1.0f;

static void update_audio(AppState *st, float dt) {
    MixCmd c;
    vec3   fwd, right;
    float  target, ease;
    int    i;

    /* wind breathes with containment: outdoors full, indoors a memory —
       the same derived query that dims the ambient dims the air. AND it
       swells with the gust (P7 item 9): the wind you SEE crossing the
       grass is the wind you HEAR rise — one gust, four senses. */
    target = (st->current_room == 0) ? 0.20f : 0.02f;
    target *= 0.6f + 0.8f * st->wind_gust;
    ease   = 1.0f - expf(-dt * 2.5f);
    g_wind_cur += (target - g_wind_cur) * ease;
    if (fabsf(g_wind_cur - g_wind_sent) > 0.003f) {
        memset(&c, 0, sizeof c);
        c.kind = MIX_CMD_SET;
        c.slot = 0;
        c.gen  = g_wind_gen;
        c.gain = g_wind_cur;
        if (audio_push(&c)) g_wind_sent = g_wind_cur;
    }

    /* reverb breathes with the listener's room (P8 item 8): the church rings
       long + bright, a small room is short + dark, open air is dry. Same
       containment that dims the ambient, eased so the tail grows/dries with
       no pop at the threshold, sent only when it moves (drop-never-block). */
    {
        int   k = listener_reverb_kind(st);
        float td = (k == 2) ? 0.92f : (k == 1) ? 0.52f : 0.40f;  /* decay */
        float tm = (k == 2) ? 0.18f : (k == 1) ? 0.55f : 0.50f;  /* damp  */
        float tw = (k == 2) ? 0.50f : (k == 1) ? 0.20f : 0.00f;  /* wet   */
        g_rv_decay += (td - g_rv_decay) * ease;
        g_rv_damp  += (tm - g_rv_damp)  * ease;
        g_rv_wet   += (tw - g_rv_wet)   * ease;
        if (fabsf(g_rv_decay - g_rv_decay_sent) > 0.002f ||
            fabsf(g_rv_damp  - g_rv_damp_sent)  > 0.002f ||
            fabsf(g_rv_wet   - g_rv_wet_sent)   > 0.002f) {
            memset(&c, 0, sizeof c);
            c.kind     = MIX_CMD_REVERB;
            c.rv_decay = g_rv_decay;
            c.rv_damp  = g_rv_damp;
            c.rv_wet   = g_rv_wet;
            if (audio_push(&c)) {
                g_rv_decay_sent = g_rv_decay;
                g_rv_damp_sent  = g_rv_damp;
                g_rv_wet_sent   = g_rv_wet;
            }
        }
    }

    /* the camera's right, for panning the world (flattened: tilting your
       head shouldn't swing the stereo field) */
    fwd = camera_forward(&st->camera);
    fwd.y = 0.0f;
    if (vec3_dot(fwd, fwd) < 1e-6f) fwd = vec3_make(0.0f, 0.0f, -1.0f);
    fwd   = vec3_normalize(fwd);
    right = vec3_make(-fwd.z, 0.0f, fwd.x);

    /* lantern crackles: the nearest few flames each hold a voice. The gain
       rides the SAME flicker the light and the bloom ride (one flame, three
       senses) and the SAME windowed inverse-square the light uses (one
       perceptual law, two senses — item 5's falloff, heard). */
    {
        sol_u32 want[VOICE_LANTERN_MAX];
        float   wd2[VOICE_LANTERN_MAX];
        int     wn = 0;
        sol_u32 k;
        for (k = 0; k < st->scene.count; k++) {
            SceneObject *o  = &st->scene.objects[k];
            const char  *lt = scene_meta_get(&st->scene, o->handle, "light");
            mat4 wm; vec3 p, dv; float d2;
            if (!lt || strcmp(lt, "point") != 0) continue;
            wm = scene_world_matrix(&st->scene, o);
            p  = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
            dv = vec3_sub(p, st->camera.pos);
            d2 = vec3_dot(dv, dv);
            if (wn < VOICE_LANTERN_MAX) {
                want[wn] = o->handle; wd2[wn] = d2; wn++;
            } else {
                int worst = 0, j;
                for (j = 1; j < wn; j++) if (wd2[j] > wd2[worst]) worst = j;
                if (d2 < wd2[worst]) { want[worst] = o->handle; wd2[worst] = d2; }
            }
        }
        for (i = 0; i < VOICE_LANTERN_MAX; i++) {   /* release the unwanted */
            int j, keep = 0;
            if (g_lantern_handle[i] == 0u) continue;
            for (j = 0; j < wn; j++)
                if (want[j] == g_lantern_handle[i]) keep = 1;
            if (scene_get(&st->scene, g_lantern_handle[i]) == NULL) keep = 0;
            if (!keep) {
                memset(&c, 0, sizeof c);
                c.kind = MIX_CMD_STOP;
                c.slot = VOICE_LANTERN_BASE + i;
                c.gen  = g_lantern_gen[i];
                audio_push(&c);
                g_lantern_handle[i] = 0u;
            }
        }
        for (i = 0; i < wn; i++) {                  /* claim for the new */
            int j, held = -1, slot = -1;
            for (j = 0; j < VOICE_LANTERN_MAX; j++) {
                if (g_lantern_handle[j] == want[i]) held = j;
                if (g_lantern_handle[j] == 0u && slot < 0) slot = j;
            }
            if (held < 0 && slot >= 0) {
                memset(&c, 0, sizeof c);
                c.kind   = MIX_CMD_START;
                c.slot   = VOICE_LANTERN_BASE + slot;
                c.gen    = ++g_voice_gen;
                c.buf    = g_snd_crackle;
                c.frames = g_snd_crackle_frames;
                c.gain   = 0.0f;
                c.loop   = 1;
                if (audio_push(&c)) {
                    g_lantern_handle[slot] = want[i];
                    g_lantern_gen[slot]    = c.gen;
                }
            }
        }
        g_loop_voices = 1;                          /* the wind */
        for (i = 0; i < VOICE_LANTERN_MAX; i++) {   /* drive gain + pan */
            SceneObject *o;
            mat4  wm;
            vec3  p;
            float r, att, gain, pan;
            const char *s;
            if (g_lantern_handle[i] == 0u) continue;
            o = scene_get(&st->scene, g_lantern_handle[i]);
            if (o == NULL) continue;
            g_loop_voices++;
            wm = scene_world_matrix(&st->scene, o);
            p  = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
            r  = 9.0f;
            s  = scene_meta_get(&st->scene, o->handle, "light_radius");
            if (s) r = (float)atof(s);
            spatialize(p, st->camera.pos, right, r, &att, &pan);
            gain = 0.9f * att * o->overlay_glow;     /* the flame's flicker, heard */
            if (gain > 0.5f) gain = 0.5f;
            memset(&c, 0, sizeof c);
            c.kind = MIX_CMD_SET;
            c.slot = VOICE_LANTERN_BASE + i;
            c.gen  = g_lantern_gen[i];
            c.gain = gain;
            c.pan  = pan;
            audio_push(&c);
        }

        /* the nearest pond's water (P8 item 8): one located loop on its own
           slot, the spatializer's demo source. Started lazily, then driven
           like a lantern — pans + fades as you walk the tarn. */
        {
            sol_u32 k, near = 0u;
            float   nd2 = 1e30f;
            for (k = 0; k < st->scene.count; k++) {
                SceneObject *o = &st->scene.objects[k];
                mat4 wm; vec3 p; float dx, dz, d2;
                if (!o->mesh_ref || strcmp(o->mesh_ref, "pond") != 0) continue;
                wm = scene_world_matrix(&st->scene, o);
                p  = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
                dx = p.x - st->camera.pos.x; dz = p.z - st->camera.pos.z;
                d2 = dx * dx + dz * dz;
                if (d2 < nd2) { nd2 = d2; near = o->handle; }
            }
            if (near == 0u) {                        /* no pond -> silence the voice */
                if (g_water_handle != 0u) {
                    memset(&c, 0, sizeof c);
                    c.kind = MIX_CMD_STOP;
                    c.slot = VOICE_WATER;
                    c.gen  = g_water_gen;
                    audio_push(&c);
                    g_water_handle = 0u;
                }
            } else {
                SceneObject *o = scene_get(&st->scene, near);
                mat4  wm = scene_world_matrix(&st->scene, o);
                vec3  p  = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
                float att, pan;
                if (g_water_handle != near && g_snd_water_frames > 0) {
                    memset(&c, 0, sizeof c);          /* (re)start on the new pond */
                    c.kind   = MIX_CMD_START;
                    c.slot   = VOICE_WATER;
                    c.gen    = ++g_voice_gen;
                    c.buf    = g_snd_water;
                    c.frames = g_snd_water_frames;
                    c.gain   = 0.0f;
                    c.loop   = 1;
                    if (audio_push(&c)) { g_water_handle = near; g_water_gen = c.gen; }
                }
                if (g_water_handle == near) {
                    spatialize(p, st->camera.pos, right, 14.0f, &att, &pan);
                    g_loop_voices++;
                    memset(&c, 0, sizeof c);
                    c.kind = MIX_CMD_SET;
                    c.slot = VOICE_WATER;
                    c.gen  = g_water_gen;
                    c.gain = 0.7f * att;
                    c.pan  = pan;
                    audio_push(&c);
                }
            }
        }
    }

    /* footsteps from actual ground travel: stride-accumulated, variants
       pre-minted with jittered seeds, silent the moment you stop (item 1
       made "moving" honest; this is that honesty, audible) */
    {
        vec3 pos = st->camera.pos;
        if (g_step_prev_ok && st->camera.mode == CAMERA_WALK) {
            float dx = pos.x - g_step_prev.x;
            float dz = pos.z - g_step_prev.z;
            float dl = sqrtf(dx * dx + dz * dz);
            if (dl < 1.0f) {                        /* a teleport, not a stride */
                g_step_acc += dl;
                if (g_step_acc >= 1.7f) {
                    int v = (int)(particles_rand01(&g_step_rng)
                                  * (float)SND_STEP_VARIANTS);
                    if (v >= SND_STEP_VARIANTS) v = SND_STEP_VARIANTS - 1;
                    play_oneshot(g_snd_step[v], g_snd_step_frames[v],
                                 0.30f, 0.0f);
                    g_step_acc -= 1.7f;
                }
            }
        } else {
            g_step_acc = 0.0f;
        }
        g_step_prev    = pos;
        g_step_prev_ok = 1;
    }
}

/* Cast a pick ray through a screen point (NDC) and select the nearest object,
   reporting its stable handle + nid. In orbit, a hit re-targets the pivot. */
static void do_pick(AppState *st, GLFWwindow *w, float ndc_x, float ndc_y) {
    float   t;
    sol_u32 hit;

    hit = pick_at(st, w, ndc_x, ndc_y, &t);
    if (hit) {                                   /* a window's glass/fill child -> select the window */
        SceneObject *ho = scene_get(&st->scene, hit);
        if (ho && ho->mesh_ref &&
            (strcmp(ho->mesh_ref, "window_glass") == 0 ||
             strcmp(ho->mesh_ref, "window_fill") == 0))
            hit = ho->parent;
    }
    st->selected_handle = hit;
    if (hit) {
        SceneObject *o = scene_get(&st->scene, hit);
        play_oneshot(g_snd_blip, g_snd_blip_frames, 0.22f, 0.0f);  /* piece 2's
                                                       audible proof: selection
                                                       speaks */
        printf("picked: handle %u, nid %s, t=%.2f\n",
               (unsigned)hit, (o && o->nid) ? o->nid : "?", t);
        if (hit == st->page_handle) {                   /* click the page -> read it */
            camera_focus(&st->camera, object_world_pos(&st->scene, st->page_handle),
                         vec3_make(0.0f, 0.0f, 1.0f), 0.6f);   /* page faces +Z, half-height 0.6 */
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  /* first-person look */
            st->mouse_skip = 2;                         /* no jump across the cursor-mode change */
        } else if (st->camera.mode == CAMERA_ORBIT) {   /* "click a shelf to zoom" */
            camera_enter_orbit(&st->camera, object_world_pos(&st->scene, hit));
        }
    } else {
        printf("picked: nothing\n");
    }
}

/* Drag-to-place (item 4). The cursor is a RAY — one constraint short of a
   position — so dragging pins the object to the ray's hit on a plane. The
   plane is CONTEXT (item 8): the FLOOR plane on the ground (y=0 — never a
   plane at the object's own height: through an object at eye level such a
   plane is nearly edge-on to the gaze, and a near-parallel ray sweeps huge
   distances for tiny mouse moves), or a board's face when the cursor is
   over one. The grab offset carries the object's height, so a floor-plane
   drag still slides things horizontally with height preserved. */
#define DRAG_MAX_DIST 60.0f     /* a grazing ray races to the horizon; clamp */

/* Is this object a pinboard? Identity = its registry ref name. */
static sol_bool object_is_board(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (o && o->mesh_ref && strcmp(o->mesh_ref, "board") == 0)
               ? SOL_TRUE : SOL_FALSE;
}

/* The board under the cursor: the nearest FRONT-face plane hit landing within
   the board rectangle. Back/edge-on approaches are skipped — pinning through
   the back would bury the card inside the slab. *out_local = the hit in
   board-local space (bottom-origin: x in [-w/2, w/2], y in [0, h]). */
static sol_u32 board_under_ray(AppState *st, Ray r, vec3 *out_local) {
    sol_u32 i, best = 0;
    float   best_t = DRAG_MAX_DIST;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float bw, bh, bt, t;
        vec3  n, face, local;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) continue;
        bw   = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w");
        bh   = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h");
        bt   = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "t");
        n    = quat_rotate(scene_world_rotation(&st->scene, o->handle),
                           vec3_make(0.0f, 0.0f, 1.0f));
        face = mat4_mul_point(scene_world_matrix(&st->scene, o),
                              vec3_make(0.0f, 0.0f, bt * 0.5f));
        if (vec3_dot(r.dir, n) >= 0.0f) continue;          /* back or edge-on */
        if (!ray_vs_plane(r, face, n, &t) || t >= best_t) continue;
        local = scene_world_to_local(&st->scene, o->handle,
                                     vec3_add(r.origin, vec3_scale(r.dir, t)));
        if (local.x < -bw * 0.5f || local.x > bw * 0.5f) continue;
        if (local.y < 0.0f || local.y > bh) continue;
        best   = o->handle;
        best_t = t;
        if (out_local) *out_local = local;
    }
    return best;
}

/* Project ray `r` onto board `board`'s front-face plane; writes the board-local
   point (UNCLAMPED). Returns SOL_FALSE if the ray misses the plane. */
static sol_bool board_ray_local(AppState *st, sol_u32 board, Ray r, vec3 *out) {
    SceneObject *o = scene_get(&st->scene, board);
    float bt, t;
    vec3  n, face;
    if (!o) return SOL_FALSE;
    bt   = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "t");
    n    = quat_rotate(scene_world_rotation(&st->scene, board),
                       vec3_make(0.0f, 0.0f, 1.0f));
    face = mat4_mul_point(scene_world_matrix(&st->scene, o),
                          vec3_make(0.0f, 0.0f, bt * 0.5f));
    if (!ray_vs_plane(r, face, n, &t)) return SOL_FALSE;
    *out = scene_world_to_local(&st->scene, board,
                                vec3_add(r.origin, vec3_scale(r.dir, t)));
    return SOL_TRUE;
}

/* The board-local footprint of a selectable card (x centered on pos.x, y
   bottom-origin pos.y..pos.y+h). */
static void card_footprint(Scene *s, sol_u32 h, float *x0, float *y0,
                           float *x1, float *y1) {
    SceneObject *o  = scene_get(s, h);
    const char  *mr = (o && o->mesh_ref) ? o->mesh_ref : "card";
    float cw = o ? mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "w") : 0.0f;
    float ch = o ? mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "h") : 0.0f;
    *x0 = o ? o->pos.x - cw * 0.5f : 0.0f;
    *x1 = o ? o->pos.x + cw * 0.5f : 0.0f;
    *y0 = o ? o->pos.y             : 0.0f;
    *y1 = o ? o->pos.y + ch        : 0.0f;
}

/* A pinned card's board-local position: the cursor hit plus the grab offset
   in the board plane, Z pinned so the card's back just clears the face. */
#define BOARD_PIN_EPS 0.003f
static vec3 board_pin_pos(Scene *s, sol_u32 board, sol_u32 card, vec3 local,
                          float ox, float oy) {
    SceneObject *bo   = scene_get(s, board);
    SceneObject *co   = scene_get(s, card);
    const char  *cref = (co && co->mesh_ref) ? co->mesh_ref : "card";
    float bt = bo ? mesh_ref_param("board", bo->mesh_params, bo->mesh_param_count, "t") : 0.05f;
    float ct = co ? mesh_ref_param(cref, co->mesh_params, co->mesh_param_count, "t") : 0.03f;
    return vec3_make(local.x + ox, local.y + oy, bt * 0.5f + ct * 0.5f + BOARD_PIN_EPS);
}

/* ---- arrows: relations made visible (item 8) ----
   An arrow is an OBJECT (child of its board) carrying two `connects` rels —
   the third embodiment of the item-7 edge, after the doorway wall and the
   path slab. Only the rels persist; the geometry below is derived from the
   two cards' current positions and rebuilt whenever they move. */
#define ARROW_INK_W    0.03f     /* shaft width, board-local units */
#define ARROW_PIN_EPS  0.0015f   /* above the board face, beneath the cards */

static sol_bool object_is_arrow(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (o && o->mesh_ref && strcmp(o->mesh_ref, "arrow") == 0)
               ? SOL_TRUE : SOL_FALSE;
}

static sol_bool object_is_walkway(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (o && o->mesh_ref && strcmp(o->mesh_ref, "walkway") == 0)
               ? SOL_TRUE : SOL_FALSE;
}

/* Param t (along a segment leaving a rect's CENTER) where it crosses the
   rect boundary — the 2D slab test, nearer axis wins. Used to clip the
   shaft to the cards' rectangles so the head lands on an edge. */
static float seg_rect_exit(float dx, float dy, float hw, float hh) {
    float adx = dx < 0.0f ? -dx : dx;
    float ady = dy < 0.0f ? -dy : dy;
    float tx  = adx > 1e-9f ? hw / adx : 1e30f;
    float ty  = ady > 1e-9f ? hh / ady : 1e30f;
    return tx < ty ? tx : ty;
}

/* A card's face center + half-extents in board-local space (bottom-origin
   card: center is half a height above its position). */
static void card_rect(Scene *s, SceneObject *c, float *cx, float *cy,
                      float *hw, float *hh) {
    const char *ref = c->mesh_ref ? c->mesh_ref : "card";
    (void)s;
    *hw = 0.5f * mesh_ref_param(ref, c->mesh_params, c->mesh_param_count, "w");
    *hh = 0.5f * mesh_ref_param(ref, c->mesh_params, c->mesh_param_count, "h");
    *cx = c->pos.x;
    *cy = c->pos.y + *hh;
}

/* Re-derive every arrow's geometry from its two connected cards. Called on
   load and whenever a board card moves — the rel is the data, the mesh is
   its picture, so there is no "update the arrow" bookkeeping to go stale.
   A dangling or off-board edge keeps its object but draws nothing. */
static void arrows_rebuild(AppState *st) {
    Scene  *s = &st->scene;
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        SceneObject *ca, *cb;
        sol_u32      ha = 0, hb = 0, j;
        float        ax, ay, ahw, ahh, bx, by, bhw, bhh, dx, dy, t0, t1;
        MeshBuilder  mb;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "arrow") != 0) continue;
        mesh_destroy(&o->mesh);
        for (j = 0; j < o->rel_count; j++) {
            if (strcmp(o->relations[j].type, "connects") != 0) continue;
            if (ha == 0)      ha = o->relations[j].target;
            else if (hb == 0) hb = o->relations[j].target;
        }
        ca = scene_get(s, ha);
        cb = scene_get(s, hb);
        if (!ca || !cb) continue;                         /* dangling: invisible,
                                                             preserved */
        if (ca->parent != o->parent || cb->parent != o->parent) continue;
        card_rect(s, ca, &ax, &ay, &ahw, &ahh);
        card_rect(s, cb, &bx, &by, &bhw, &bhh);
        dx = bx - ax;
        dy = by - ay;
        t0 = seg_rect_exit(dx, dy, ahw, ahh);             /* leave card A...   */
        t1 = 1.0f - seg_rect_exit(dx, dy, bhw, bhh);      /* ...arrive at B    */
        if (!(t0 < t1)) continue;                         /* overlapping cards */
        mb_init(&mb);
        make_arrow(&mb, ax + dx * t0, ay + dy * t0,
                        ax + dx * t1, ay + dy * t1, ARROW_INK_W);
        if (mb.index_count > 0) o->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
    }
}

/* a room's half-extent (max of its shell's w/d, halved) — for edge points */
static float room_half_extent(Scene *s, sol_u32 anchor) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == anchor && o->mesh_ref &&
            strcmp(o->mesh_ref, "room") == 0) {
            float w = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            float d = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            return (w > d ? w : d) * 0.5f;
        }
    }
    return 4.0f;   /* fallback */
}

/* Derive ALL connection geometry from the route author (route.c): each
   walkway becomes an L (or straight) ribbon anchored at its lower door, and
   each room's shell is rebuilt with the doorways its routes pierce. Rooms and
   walkways store no geometry — this runs on load and after every edit, so a
   new root can never leave a sibling's path stale (the take-1 bug). */
/* ---- plank walls overlay (sourced-texture experiment, flagged) ----
   Tiled planks on room inner-wall faces, split around doorways. Render-time;
   built where the doorway openings are already computed (connections_rebuild). */
#define WALL_TILE_M    3.0f      /* meters per texture-repeat (the plank-size knob) */
#define PATH_TILE_M    1.5f      /* meters per marble-repeat on walkways/steps */
#define CURB_W         0.14f     /* walkway trim: curb rail cross-section thickness (m) */
#define CURB_H         0.10f     /* walkway trim: curb height above the deck top (m) */
#define CURB_OVER      0.04f     /* walkway trim: how far the curb juts past the deck edge (m) */
#define DOOR_LINE_T    0.05f     /* doorway wood casing: how far it stands proud into the opening (m) */
#define WALL_EPS       0.01f     /* inward lift off the wall face (anti z-fight) */
#define GABLE_NOTCH_EPS 1e-3f    /* gable-window sill/lintel sliver guard (band gate == reveal gate) */
#define ROOM_FRAME_MAX 128
#define FRAME_COL_T  0.24f       /* corner column cross-section (m) */
#define WOOD_TILE_M  1.0f        /* meters per wood texture-repeat along a beam */
#define FRAME_BEAM_T       0.14f     /* truss beam cross-section (m) */
#define FRAME_PITCH_DEG    35.0f     /* roof / truss pitch (degrees) */
#define FRAME_BENT_SPACING 2.5f      /* meters between scissor-truss bents */
#define FRAME_SCISSOR_FRAC 0.6f      /* lower chord meets the opposite rafter this far up */
#define ROOF_TILE_M  2.0f        /* meters per roof texture-repeat */
#define BOARD_VIEW_GLIDE_S  0.35f   /* seconds for the enter/exit camera glide */
#define BOARD_VIEW_MARGIN   1.10f   /* fill the FOV to the board + a little air */
#define ROUTE_LABEL_REFRESH_S 0.25  /* doorway-label routes resolve at most ~4x/sec */
#define BOARD_DBL_S   0.35   /* max seconds between the two clicks of a double-click */
#define BOARD_DBL_PX  6.0    /* max cursor drift (px) between them */

#define CAMPUS_SUB      72        /* campus tessellation (clamped 2..96 in make_campus) */
#define CAMPUS_TILE_M   2.0f      /* meters per ground-texture repeat (bigger = less tiling) */
#define CAMPUS_HILL_AMP 2.0f      /* hill amplitude between rooms (m) */
#define CAMPUS_SEED     7u        /* campus noise identity */
#define CAMPUS_PAD_GROW  0.6f     /* expand a room pad past its walls (m) */
#define CAMPUS_PAD_SINK  0.15f    /* sink a pad below the floor/deck so the solid covers it (m) */
#define CAMPUS_PATH_HALF 0.85f    /* half-width of a walkway corridor pad (m) */
#define CAMPUS_PATH_STEP 1.2f     /* sample spacing along a walkway leg (m) */
#define CAMPUS_GRASS_PER_M2 2.0f      /* grass density on the campus */
#define CAMPUS_GRASS_MAX    16000
#define CAMPUS_TREE_PER_M2  0.012f    /* tree density */
#define CAMPUS_TREE_MAX     128
#define CAMPUS_GRASS_CLEAR  0.4f      /* keep grass this far off pads (m) */
#define CAMPUS_TREE_CLEAR   1.6f      /* keep trees this far off pads (m) */
#define CAMPUS_SCREE_MAX    400

static Material g_wall_mat;       /* planks; albedo_tex.id == 0 => overlay disabled */
static Material g_dark_wood;      /* timber frame; albedo_tex.id == 0 => no wood */
static Material g_roof_mat;       /* pitched roof; albedo_tex.id == 0 => no roof */
static Material g_path_mat;       /* sandstone walkway deck; albedo_tex.id == 0 => default */
static Material g_campus_mat;      /* rocky campus ground (experiment); id 0 => palette */
static Material g_stone_mat;       /* stone-wall room shell; id 0 => default material */
static Material g_plaster_mat;     /* painted-plaster whiteboard; id 0 => default material */
static Material g_oak_mat;         /* oak-veneer file/folder tablets; id 0 => default material */

typedef struct { sol_u32 handle; Mesh wall, wood, roof, gable, door_floor, door_trim; } RoomFrame;
static RoomFrame g_room_frame[ROOM_FRAME_MAX];
static int       g_room_frame_n = 0;

typedef struct {
    int       enabled;
    vec3      center;             /* world XZ centre of the rectangle (y unused) */
    float     w, d;               /* rectangle size */
    float     y0, amp_range;      /* for the terrain slope/height palette */
    CampusPad pads[CAMPUS_MAX_PADS];
    int       npads;
    Mesh      mesh;
} CampusState;
static CampusState g_campus;       /* derived: the active world's campus, or disabled */

typedef struct {
    RhiBuffer grass;                          int grass_n;
    RhiBuffer wood[FOREST_VARIANT_COUNT];      int wood_n[FOREST_VARIANT_COUNT];
    RhiBuffer canopy[2];                       int canopy_n[2];
    RhiBuffer flowers;                          int flower_n;
    RhiBuffer scree;                            int scree_n;
} CampusFlora;
static CampusFlora g_campus_flora;             /* derived, drawn explicitly when g_campus.enabled */

static void room_frame_flush(void) {
    int i;
    for (i = 0; i < g_room_frame_n; i++) {
        mesh_destroy(&g_room_frame[i].wall);
        mesh_destroy(&g_room_frame[i].wood);
        mesh_destroy(&g_room_frame[i].roof);
        mesh_destroy(&g_room_frame[i].gable);
        mesh_destroy(&g_room_frame[i].door_floor);
        mesh_destroy(&g_room_frame[i].door_trim);
    }
    g_room_frame_n = 0;
}

/* one flat inner-face quad; position-based UVs (u = s/TILE, v = y/TILE) so planks
   tile at constant size and align across doorway gaps. runx: 1 = wall runs along
   X (s=X, face plane at z=f); 0 = runs along Z (s=Z, face plane at x=f). */
static void wall_panel_quad(MeshBuilder *b, int runx, float f, float ns,
                            float slo, float shi, float y0, float y1) {
    float   u0 = slo / WALL_TILE_M, u1 = shi / WALL_TILE_M;
    float   v0 = y0  / WALL_TILE_M, v1 = y1  / WALL_TILE_M;
    sol_u32 a, c, e, g;
    if (runx) {
        a = mb_push_vertex(b, slo, y0, f, 0.0f, 0.0f, ns, u0, v0);
        c = mb_push_vertex(b, shi, y0, f, 0.0f, 0.0f, ns, u1, v0);
        e = mb_push_vertex(b, shi, y1, f, 0.0f, 0.0f, ns, u1, v1);
        g = mb_push_vertex(b, slo, y1, f, 0.0f, 0.0f, ns, u0, v1);
    } else {
        a = mb_push_vertex(b, f, y0, slo, ns, 0.0f, 0.0f, u0, v0);
        c = mb_push_vertex(b, f, y0, shi, ns, 0.0f, 0.0f, u1, v0);
        e = mb_push_vertex(b, f, y1, shi, ns, 0.0f, 0.0f, u1, v1);
        g = mb_push_vertex(b, f, y1, slo, ns, 0.0f, 0.0f, u0, v1);
    }
    mb_push_triangle(b, a, c, e);                  /* consistent winding (engine */
    mb_push_triangle(b, a, e, g);                  /* never culls; normals are set) */
}

/* one wall's inner-face panels: mirror emit_doored_wall's slab sweep (cut the
   veneer as solid minus the UNION of the opening holes), emitting a flat plaster
   quad per remaining y-band instead of a solid box. A gable window (sill >= h)
   leaves the veneer solid; a spanning window's veneer stops at h. The duplication
   with emit_doored_wall (mesh.c) is deliberate — they were always parallel; only
   the emit primitive (wall_panel_quad vs aabb_box) differs. */
static void wall_panels(MeshBuilder *b, int runx, float f, float ns,
                        float s0, float s1, float h,
                        const RoomOpening *ops, int n_ops, int wall_id) {
    float lo[ROOM_MAX_OPENINGS_PER_WALL];
    float hi[ROOM_MAX_OPENINGS_PER_WALL];
    float oy[ROOM_MAX_OPENINGS_PER_WALL];   /* lintel, capped at h */
    float sy[ROOM_MAX_OPENINGS_PER_WALL];   /* sill */
    float xb[2 * ROOM_MAX_OPENINGS_PER_WALL + 2];   /* x-boundaries: wall ends + opening edges */
    int   k = 0, nb = 0, i, j;
    /* gather this wall's openings (skip ones entirely above the wall top) */
    for (i = 0; i < n_ops; i++) {
        float c, hwid;
        if (ops[i].wall != wall_id) continue;
        if (ops[i].sill >= h) continue;             /* gable window: veneer stays solid */
        if (k >= ROOM_MAX_OPENINGS_PER_WALL) break;
        c = ops[i].center; hwid = ops[i].width * 0.5f;
        lo[k] = c - hwid; hi[k] = c + hwid;
        oy[k] = ops[i].height; if (oy[k] > h) oy[k] = h;   /* spanning window: veneer stops at h */
        sy[k] = ops[i].sill;
        if (oy[k] <= sy[k]) continue;               /* degenerate/inverted: skip (don't record k) */
        k++;
    }
    /* x-boundaries: the wall ends + each opening edge, clamped to [s0,s1] */
    xb[nb++] = s0; xb[nb++] = s1;
    for (i = 0; i < k; i++) {
        float a = lo[i], c = hi[i];
        if (a < s0) a = s0; if (a > s1) a = s1;
        if (c < s0) c = s0; if (c > s1) c = s1;
        xb[nb++] = a; xb[nb++] = c;
    }
    for (i = 1; i < nb; i++) {                       /* insertion sort xb ascending */
        float p = xb[i]; j = i - 1;
        while (j >= 0 && xb[j] > p) { xb[j + 1] = xb[j]; j--; }
        xb[j + 1] = p;
    }
    /* each x-slab: cut the union of covering openings' [sill,lintel]; emit solid bands */
    for (j = 0; j + 1 < nb; j++) {
        float xa = xb[j], xc = xb[j + 1], mid;
        float iv_lo[ROOM_MAX_OPENINGS_PER_WALL], iv_hi[ROOM_MAX_OPENINGS_PER_WALL];
        int   ni = 0, p, q;
        float cur_y;
        if (xc - xa < 1e-5f) continue;              /* zero-width (duplicate boundary) */
        mid = (xa + xc) * 0.5f;
        for (i = 0; i < k; i++)
            if (lo[i] <= mid && hi[i] >= mid) { iv_lo[ni] = sy[i]; iv_hi[ni] = oy[i]; ni++; }
        for (p = 1; p < ni; p++) {                  /* sort the covering intervals by lo */
            float a = iv_lo[p], c = iv_hi[p]; q = p - 1;
            while (q >= 0 && iv_lo[q] > a) { iv_lo[q + 1] = iv_lo[q]; iv_hi[q + 1] = iv_hi[q]; q--; }
            iv_lo[q + 1] = a; iv_hi[q + 1] = c;
        }
        cur_y = 0.0f;                               /* emit complement bands within [0,h] */
        for (p = 0; p < ni; p++) {
            if (iv_lo[p] > cur_y) wall_panel_quad(b, runx, f, ns, xa, xc, cur_y, iv_lo[p]);
            if (iv_hi[p] > cur_y) cur_y = iv_hi[p];
        }
        if (cur_y < h) wall_panel_quad(b, runx, f, ns, xa, xc, cur_y, h);   /* top band up to wall top */
    }
}

/* a quad a->b->c->d (CCW from +n) with bilinear UVs: a=(u0,v0) ... c=(u1,v1). */
static void frame_quad(MeshBuilder *mb, vec3 a, vec3 b, vec3 c, vec3 d,
                       vec3 n, float u0, float v0, float u1, float v1) {
    sol_u32 ia, ib, ic, id;
    ia = mb_push_vertex(mb, a.x, a.y, a.z, n.x, n.y, n.z, u0, v0);
    ib = mb_push_vertex(mb, b.x, b.y, b.z, n.x, n.y, n.z, u1, v0);
    ic = mb_push_vertex(mb, c.x, c.y, c.z, n.x, n.y, n.z, u1, v1);
    id = mb_push_vertex(mb, d.x, d.y, d.z, n.x, n.y, n.z, u0, v1);
    mb_push_triangle(mb, ia, ib, ic);
    mb_push_triangle(mb, ia, ic, id);
}

/* map a bent's (along-ridge, across-span, height) to a world point. ridge_along_x:
   1 = ridge runs X so span is Z; 0 = ridge runs Z so span is X. */
static vec3 bent_pt(int ridge_along_x, float along, float span, float y) {
    return ridge_along_x ? vec3_make(along, y, span)
                         : vec3_make(span, y, along);
}

/* a t x t square-section beam swept A->B, wood tiling along its length. */
static void frame_beam(MeshBuilder *mb, vec3 a, vec3 b, float t) {
    vec3  dir, side, vup, refv, s, v;
    vec3  a00, a01, a11, a10, b00, b01, b11, b10;
    float len, hr = t * 0.5f, uL, vT;
    dir = vec3_sub(b, a);
    len = (float)sqrt((double)vec3_dot(dir, dir));
    if (len < 1e-5f) return;
    dir  = vec3_scale(dir, 1.0f / len);
    refv = (fabs((double)dir.y) < 0.99) ? vec3_make(0.0f, 1.0f, 0.0f)
                                        : vec3_make(1.0f, 0.0f, 0.0f);
    side = vec3_normalize(vec3_cross(dir, refv));
    vup  = vec3_normalize(vec3_cross(side, dir));
    s = vec3_scale(side, hr); v = vec3_scale(vup, hr);
    a00 = vec3_sub(vec3_sub(a, s), v); a10 = vec3_sub(vec3_add(a, s), v);
    a11 = vec3_add(vec3_add(a, s), v); a01 = vec3_add(vec3_sub(a, s), v);
    b00 = vec3_sub(vec3_sub(b, s), v); b10 = vec3_sub(vec3_add(b, s), v);
    b11 = vec3_add(vec3_add(b, s), v); b01 = vec3_add(vec3_sub(b, s), v);
    uL = len / WOOD_TILE_M; vT = t / WOOD_TILE_M;
    frame_quad(mb, a10, b10, b11, a11, side,                    0.0f, 0.0f, uL, vT);  /* +side */
    frame_quad(mb, a01, b01, b00, a00, vec3_scale(side, -1.0f), 0.0f, 0.0f, uL, vT);  /* -side */
    frame_quad(mb, a11, b11, b01, a01, vup,                     0.0f, 0.0f, uL, vT);  /* +vup  */
    frame_quad(mb, a00, b00, b10, a10, vec3_scale(vup,  -1.0f), 0.0f, 0.0f, uL, vT);  /* -vup  */
    frame_quad(mb, b10, b00, b01, b11, dir,                     0.0f, 0.0f, vT, vT);  /* +end  */
    frame_quad(mb, a00, a10, a11, a01, vec3_scale(dir,  -1.0f), 0.0f, 0.0f, vT, vT);  /* -end  */
}

/* one triangle a,b,c with per-vertex normal n and explicit UVs (for the gable
   ends, whose UVs are position-based to line up with the wall planks below). */
static void gable_tri(MeshBuilder *mb, vec3 a, vec3 b, vec3 c, vec3 n,
                      float ua, float va, float ub, float vb, float uc, float vc) {
    sol_u32 ia, ib, ic;
    ia = mb_push_vertex(mb, a.x, a.y, a.z, n.x, n.y, n.z, ua, va);
    ib = mb_push_vertex(mb, b.x, b.y, b.z, n.x, n.y, n.z, ub, vb);
    ic = mb_push_vertex(mb, c.x, c.y, c.z, n.x, n.y, n.z, uc, vc);
    mb_push_triangle(mb, ia, ib, ic);
}

/* an axis-aligned solid box (all six faces, explicit outward normals, position-
   based UVs at `tl` scale) — for door casings/trim. Buried faces are occluded;
   abutting the shell back-to-back never z-fights (GL_LESS, first-drawn wins). */
static void trim_box(MeshBuilder *mb, float x0, float x1, float y0, float y1,
                     float z0, float z1, float tl) {
    frame_quad(mb, vec3_make(x0,y0,z1), vec3_make(x1,y0,z1), vec3_make(x1,y1,z1), vec3_make(x0,y1,z1), vec3_make(0,0,1),  x0/tl,y0/tl, x1/tl,y1/tl);  /* +z */
    frame_quad(mb, vec3_make(x1,y0,z0), vec3_make(x0,y0,z0), vec3_make(x0,y1,z0), vec3_make(x1,y1,z0), vec3_make(0,0,-1), x1/tl,y0/tl, x0/tl,y1/tl);  /* -z */
    frame_quad(mb, vec3_make(x1,y0,z1), vec3_make(x1,y0,z0), vec3_make(x1,y1,z0), vec3_make(x1,y1,z1), vec3_make(1,0,0),  z1/tl,y0/tl, z0/tl,y1/tl);  /* +x */
    frame_quad(mb, vec3_make(x0,y0,z0), vec3_make(x0,y0,z1), vec3_make(x0,y1,z1), vec3_make(x0,y1,z0), vec3_make(-1,0,0), z0/tl,y0/tl, z1/tl,y1/tl);  /* -x */
    frame_quad(mb, vec3_make(x0,y1,z1), vec3_make(x1,y1,z1), vec3_make(x1,y1,z0), vec3_make(x0,y1,z0), vec3_make(0,1,0),  x0/tl,z1/tl, x1/tl,z0/tl);  /* +y */
    frame_quad(mb, vec3_make(x0,y0,z0), vec3_make(x1,y0,z0), vec3_make(x1,y0,z1), vec3_make(x0,y0,z1), vec3_make(0,-1,0), x0/tl,z0/tl, x1/tl,z1/tl);  /* -y */
}

/* opening reveal overlays for one opening (the faces of the thick wall you see
   through a doorway or window): a sandstone THRESHOLD quad into mbf, and dark-wood
   JAMB + lintel CASINGS into mbt. The casings are SOLID boxes standing proud into
   the opening (not floating veneer sheets) so there's no gap behind their edges.
   sill > 0 marks a WINDOW: skip the floor threshold, start the side jambs at the
   sill (no posts running to the floor), and add a sill ledge casing across the
   bottom — the mirror of the lintel casing across the top. sill == 0 is a door
   (reaches the floor) and renders exactly as before. */
static void emit_door_reveal(MeshBuilder *mbf, MeshBuilder *mbt, int wall,
                             float center, float width, float oh, float sill,
                             float hw, float hd, float t) {
    float gL  = center - width * 0.5f, gR = center + width * 0.5f;
    float e   = WALL_EPS, lt = DOOR_LINE_T;
    float ft  = PATH_TILE_M, wt = WOOD_TILE_M;   /* sandstone threshold / dark-wood casing */
    float jy0 = (sill > 0.0f) ? sill : 0.0f;     /* jamb bottom: floor for a door, sill for a window */
    if (wall == ROOM_WALL_N || wall == ROOM_WALL_S) {     /* opening spans X, thickness in Z */
        float z0 = (wall == ROOM_WALL_N) ? -hd - t : hd;
        float z1 = (wall == ROOM_WALL_N) ? -hd     : hd + t;
        if (sill <= 0.0f)
            frame_quad(mbf, vec3_make(gL, e, z0), vec3_make(gR, e, z0),        /* threshold (+y) */
                            vec3_make(gR, e, z1), vec3_make(gL, e, z1),
                            vec3_make(0.0f, 1.0f, 0.0f), gL / ft, z0 / ft, gR / ft, z1 / ft);
        trim_box(mbt, gL,      gL + lt, jy0,     oh, z0, z1, wt);   /* left jamb casing */
        trim_box(mbt, gR - lt, gR,      jy0,     oh, z0, z1, wt);   /* right jamb casing */
        trim_box(mbt, gL,      gR,      oh - lt, oh, z0, z1, wt);   /* lintel casing */
        if (sill > 0.0f)
            trim_box(mbt, gL,  gR,      sill, sill + lt, z0, z1, wt);   /* sill ledge casing */
    } else {                                              /* opening spans Z, thickness in X */
        float x0 = (wall == ROOM_WALL_W) ? -hw - t : hw;
        float x1 = (wall == ROOM_WALL_W) ? -hw     : hw + t;
        if (sill <= 0.0f)
            frame_quad(mbf, vec3_make(x0, e, gL), vec3_make(x1, e, gL),        /* threshold (+y) */
                            vec3_make(x1, e, gR), vec3_make(x0, e, gR),
                            vec3_make(0.0f, 1.0f, 0.0f), x0 / ft, gL / ft, x1 / ft, gR / ft);
        trim_box(mbt, x0, x1, jy0,     oh, gL,      gL + lt, wt);   /* jamb casing z=gL */
        trim_box(mbt, x0, x1, jy0,     oh, gR - lt, gR,      wt);   /* jamb casing z=gR */
        trim_box(mbt, x0, x1, oh - lt, oh, gL,      gR,      wt);   /* lintel casing */
        if (sill > 0.0f)
            trim_box(mbt, x0, x1, sill, sill + lt, gL,      gR,      wt);   /* sill ledge casing */
    }
}

/* push one gable-plane vertex: (s,y) -> 3D via bent_pt + offv, position UVs. */
static sol_u32 gable_push(MeshBuilder *mb, int rax, float ge, vec3 offv, vec3 n, float s, float y) {
    vec3 p = vec3_add(bent_pt(rax, ge, s, y), offv);
    return mb_push_vertex(mb, p.x, p.y, p.z, n.x, n.y, n.z, s / WALL_TILE_M, y / WALL_TILE_M);
}

/* one gable end FACE (inner: offv=0,n=nin / outer: offv=off,n=nout): the triangle
   (base [-sh,sh] at hwall, apex (0,ridge_y)) MINUS the notch [s0,s1]x[yb,yt].
   Four regions: bottom band (if yb>hwall), left strip, right strip, top piece (if yt<ridge_y). */
static void gable_face_notched(MeshBuilder *mb, int rax, float ge, vec3 offv, vec3 n,
                               float sh, float hwall, float ridge_y,
                               float s0, float s1, float yb, float yt) {
    float spanH = ridge_y - hwall;
    float hwb   = sh * (ridge_y - yb) / spanH;
    float hwt   = sh * (ridge_y - yt) / spanH;
    sol_u32 a, b, c, d;
    if (yb > hwall + GABLE_NOTCH_EPS) {
        a = gable_push(mb,rax,ge,offv,n, -sh,  hwall);
        b = gable_push(mb,rax,ge,offv,n,  sh,  hwall);
        c = gable_push(mb,rax,ge,offv,n,  hwb, yb);
        d = gable_push(mb,rax,ge,offv,n, -hwb, yb);
        mb_push_triangle(mb,a,b,c); mb_push_triangle(mb,a,c,d);
    }
    a = gable_push(mb,rax,ge,offv,n, -hwb, yb);
    b = gable_push(mb,rax,ge,offv,n,  s0,  yb);
    c = gable_push(mb,rax,ge,offv,n,  s0,  yt);
    d = gable_push(mb,rax,ge,offv,n, -hwt, yt);
    mb_push_triangle(mb,a,b,c); mb_push_triangle(mb,a,c,d);
    a = gable_push(mb,rax,ge,offv,n,  s1,  yb);
    b = gable_push(mb,rax,ge,offv,n,  hwb, yb);
    c = gable_push(mb,rax,ge,offv,n,  hwt, yt);
    d = gable_push(mb,rax,ge,offv,n,  s1,  yt);
    mb_push_triangle(mb,a,b,c); mb_push_triangle(mb,a,c,d);
    if (yt < ridge_y - GABLE_NOTCH_EPS) {
        a = gable_push(mb,rax,ge,offv,n, -hwt, yt);
        b = gable_push(mb,rax,ge,offv,n,  hwt, yt);
        c = gable_push(mb,rax,ge,offv,n,  0.0f, ridge_y);
        mb_push_triangle(mb,a,b,c);
    }
}

/* the notch's inner hole walls (solid gable material), each spanning inner->outer
   (0->off) so you don't see through the slab. Normals point INTO the hole.
   `has_bottom` adds the bottom wall (an interior/gable-only notch; a spanning
   notch's bottom meets the wall hole at hwall). */
static void gable_notch_reveal(MeshBuilder *mb, int rax, float ge, vec3 off,
                               float s0, float s1, float yb, float yt, int has_bottom) {
    vec3 su = rax ? vec3_make(0.0f,0.0f,1.0f) : vec3_make(1.0f,0.0f,0.0f);  /* +s direction */
    { vec3 ia = bent_pt(rax,ge,s0,yb), ib = bent_pt(rax,ge,s0,yt);          /* left s=s0 (faces +s) */
      frame_quad(mb, ia, ib, vec3_add(ib,off), vec3_add(ia,off), su, 0.0f,0.0f, 1.0f,1.0f); }
    { vec3 ia = bent_pt(rax,ge,s1,yt), ib = bent_pt(rax,ge,s1,yb);          /* right s=s1 (faces -s) */
      frame_quad(mb, ia, ib, vec3_add(ib,off), vec3_add(ia,off), vec3_scale(su,-1.0f), 0.0f,0.0f, 1.0f,1.0f); }
    { vec3 ia = bent_pt(rax,ge,s0,yt), ib = bent_pt(rax,ge,s1,yt);          /* top y=yt (faces -y) */
      frame_quad(mb, ia, ib, vec3_add(ib,off), vec3_add(ia,off), vec3_make(0.0f,-1.0f,0.0f), 0.0f,0.0f, 1.0f,1.0f); }
    if (has_bottom) {                                                       /* bottom y=yb (faces +y) */
      vec3 ia = bent_pt(rax,ge,s1,yb), ib = bent_pt(rax,ge,s0,yb);
      frame_quad(mb, ia, ib, vec3_add(ib,off), vec3_add(ia,off), vec3_make(0.0f,1.0f,0.0f), 0.0f,0.0f, 1.0f,1.0f); }
}

/* build a RoomFrame (wall + timber) for a room shell from its openings and
   store it by handle (replacing any prior entry). no-op if planks disabled. */
static void room_frame_build(SceneObject *shell, const RoomOpening *ops, int no) {
    MeshBuilder mb;
    Mesh        wall, wood, roof, gable, door_floor, door_trim;
    float       w, d, h, hw, hd;
    int         i;
    int         rax;
    float       rlen, along_h, sh, dy, ridge_y;
    if (g_wall_mat.albedo_tex.id == 0 && g_dark_wood.albedo_tex.id == 0 &&
        g_roof_mat.albedo_tex.id == 0 && g_path_mat.albedo_tex.id == 0) return;
    w  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
    d  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
    h  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
    hw = w * 0.5f; hd = d * 0.5f;
    memset(&wall, 0, sizeof wall);
    memset(&wood, 0, sizeof wood);
    memset(&roof,  0, sizeof roof);
    memset(&gable, 0, sizeof gable);
    memset(&door_floor, 0, sizeof door_floor);
    memset(&door_trim,  0, sizeof door_trim);
    rax     = (w >= d) ? 1 : 0;     /* 1 = ridge along X (span = Z) */
    rlen    = rax ? w : d;          /* ridge length */
    along_h = rax ? hw : hd;        /* half the ridge length */
    sh      = rax ? hd : hw;        /* half-span (eave -> centre) */
    dy      = sh * (float)tan((double)sol_radians(FRAME_PITCH_DEG));
    ridge_y = h + dy;               /* ridge peak above the floor */
    if (g_wall_mat.albedo_tex.id != 0) {
        mb_init(&mb);
        if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f)
            wall_panels(&mb, 1, -hd + WALL_EPS,  1.0f, -hw, hw, h, ops, no, ROOM_WALL_N);
        if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f)
            wall_panels(&mb, 1,  hd - WALL_EPS, -1.0f, -hw, hw, h, ops, no, ROOM_WALL_S);
        if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f)
            wall_panels(&mb, 0,  hw - WALL_EPS, -1.0f, -hd, hd, h, ops, no, ROOM_WALL_E);
        if (mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f)
            wall_panels(&mb, 0, -hw + WALL_EPS,  1.0f, -hd, hd, h, ops, no, ROOM_WALL_W);
        if (mb.index_count > 0) wall = mesh_from_builder(&mb);
        mb_free(&mb);
    }
    if (g_dark_wood.albedo_tex.id != 0) {
        float ci = FRAME_COL_T * 0.5f + 0.02f;   /* inset just inside the walls */
        float cx = hw - ci, cz = hd - ci;
        mb_init(&mb);
        frame_beam(&mb, vec3_make(-cx, 0.0f, -cz), vec3_make(-cx, h, -cz), FRAME_COL_T);
        frame_beam(&mb, vec3_make( cx, 0.0f, -cz), vec3_make( cx, h, -cz), FRAME_COL_T);
        frame_beam(&mb, vec3_make( cx, 0.0f,  cz), vec3_make( cx, h,  cz), FRAME_COL_T);
        frame_beam(&mb, vec3_make(-cx, 0.0f,  cz), vec3_make(-cx, h,  cz), FRAME_COL_T);
        /* scissor trusses: bents spaced along the ridge axis (the longer dim) */
        {
            float f       = FRAME_SCISSOR_FRAC;
            float cross_y = h + dy * f / (2.0f - f);  /* where the lower chords cross */
            int   bents   = (int)(rlen / FRAME_BENT_SPACING + 0.5f);
            int   bi;
            if (bents < 2) bents = 2;
            for (bi = 0; bi < bents; bi++) {
                float al    = -along_h + rlen * ((float)bi + 0.5f) / (float)bents;
                vec3  eaveL = bent_pt(rax, al, -sh,  h);
                vec3  eaveR = bent_pt(rax, al,  sh,  h);
                vec3  apex  = bent_pt(rax, al, 0.0f, ridge_y);
                frame_beam(&mb, eaveL, apex, FRAME_BEAM_T);                                            /* rafter L */
                frame_beam(&mb, eaveR, apex, FRAME_BEAM_T);                                            /* rafter R */
                frame_beam(&mb, eaveL, bent_pt(rax, al,  sh * (1.0f - f), h + dy * f), FRAME_BEAM_T);  /* scissor L */
                frame_beam(&mb, eaveR, bent_pt(rax, al, -sh * (1.0f - f), h + dy * f), FRAME_BEAM_T);  /* scissor R */
                frame_beam(&mb, bent_pt(rax, al, 0.0f, cross_y), apex, FRAME_BEAM_T);                  /* king post */
            }
        }
        if (mb.index_count > 0) wood = mesh_from_builder(&mb);
        mb_free(&mb);
    }
    if (g_roof_mat.albedo_tex.id != 0) {
        float slope_l = (float)sqrt((double)(sh * sh + dy * dy));
        float uL = rlen / ROOF_TILE_M, uS = slope_l / ROOF_TILE_M;
        int   side;
        mb_init(&mb);
        for (side = 0; side < 2; side++) {
            float sg  = side ? -1.0f : 1.0f;
            vec3  en  = bent_pt(rax, -along_h, sg * sh, h);        /* eave near */
            vec3  ef  = bent_pt(rax,  along_h, sg * sh, h);        /* eave far  */
            vec3  rdf = bent_pt(rax,  along_h, 0.0f,    ridge_y);  /* ridge far */
            vec3  rdn = bent_pt(rax, -along_h, 0.0f,    ridge_y);  /* ridge near*/
            vec3  nrm = vec3_normalize(bent_pt(rax, 0.0f, sg * dy, sh));
            frame_quad(&mb, en, ef, rdf, rdn, nrm, 0.0f, 0.0f, uL, uS);
        }
        if (mb.index_count > 0) roof = mesh_from_builder(&mb);
        mb_free(&mb);
    }
    if (g_roof_mat.albedo_tex.id != 0 && g_wall_mat.albedo_tex.id != 0) {
        int gi;
        mb_init(&mb);
        for (gi = 0; gi < 2; gi++) {
            float ge   = gi ? along_h : -along_h;      /* this ridge end (inner-face plane) */
            float gout = gi ? 1.0f : -1.0f;            /* outward along the ridge axis */
            vec3  off  = bent_pt(rax, gout * ROUTE_WALL_T, 0.0f, 0.0f);  /* thickness, extruded outward */
            vec3  nout = vec3_normalize(bent_pt(rax, gout, 0.0f, 0.0f)); /* faces the island exterior */
            vec3  nin  = vec3_scale(nout, -1.0f);                        /* faces the hall interior */
            vec3  eL   = bent_pt(rax, ge, -sh,  h);
            vec3  eR   = bent_pt(rax, ge,  sh,  h);
            vec3  ap   = bent_pt(rax, ge, 0.0f, ridge_y);
            vec3  eLo  = vec3_add(eL, off);
            vec3  eRo  = vec3_add(eR, off);
            vec3  apo  = vec3_add(ap, off);
            /* a gable wall WITH thickness, like make_room_doored's walls: emit the
               two EXPOSED faces -- inner (flush with the end wall, lit for the hall)
               and outer (off by the wall thickness, lit for the exterior). offset so
               they never z-fight; each lit for its own side, so the gable reads right
               from inside the hall and from outside -- no backface culling needed. */
            {
                int   gwall = rax ? (gi ? ROOM_WALL_E : ROOM_WALL_W)
                                  : (gi ? ROOM_WALL_S : ROOM_WALL_N);
                int   oi, found = -1;
                float s0 = 0.0f, s1 = 0.0f, yb = 0.0f, yt = 0.0f;
                for (oi = 0; oi < no; oi++)
                    if (ops[oi].wall == gwall && ops[oi].height > h + 1e-3f) { found = oi; break; }
                if (found >= 0) {
                    float cen = ops[found].center, hwid = ops[found].width * 0.5f;
                    float hwt;
                    s0 = cen - hwid; s1 = cen + hwid;
                    yb = ops[found].sill;   if (yb < h)       yb = h;
                    yt = ops[found].height; if (yt > ridge_y) yt = ridge_y;
                    hwt = sh * (ridge_y - yt) / (ridge_y - h);   /* fit width to the triangle at yt */
                    if (s0 < -hwt) s0 = -hwt;
                    if (s1 >  hwt) s1 =  hwt;
                    if (s1 - s0 < 0.05f || yt - yb < 0.05f) found = -1;   /* degenerate -> solid */
                }
                if (found >= 0) {
                    gable_face_notched(&mb, rax, ge, vec3_make(0.0f,0.0f,0.0f), nin,
                                       sh, h, ridge_y, s0, s1, yb, yt);
                    gable_face_notched(&mb, rax, ge, off, nout,
                                       sh, h, ridge_y, s0, s1, yb, yt);
                    gable_notch_reveal(&mb, rax, ge, off, s0, s1, yb, yt, yb > h + GABLE_NOTCH_EPS);
                } else {
                    gable_tri(&mb, eL, eR, ap, nin,            /* inner face (toward the hall) */
                              -sh / WALL_TILE_M, h / WALL_TILE_M,
                               sh / WALL_TILE_M, h / WALL_TILE_M,
                              0.0f,              ridge_y / WALL_TILE_M);
                    gable_tri(&mb, eRo, eLo, apo, nout,        /* outer face (reversed winding) */
                               sh / WALL_TILE_M, h / WALL_TILE_M,
                              -sh / WALL_TILE_M, h / WALL_TILE_M,
                              0.0f,              ridge_y / WALL_TILE_M);
                }
            }
            {   /* cap the two sloped top edges (the rake) so the slab isn't hollow
                   where it stands proud of the roof; bottom edge sits on the wall
                   top, so it stays hidden like the walls' unexposed faces. */
                float raflen = (float)sqrt((double)(sh * sh + dy * dy));
                float ue = raflen / WALL_TILE_M, ve = ROUTE_WALL_T / WALL_TILE_M;
                vec3  nrp = vec3_normalize(bent_pt(rax, 0.0f,  dy, sh));  /* +span rake */
                vec3  nrn = vec3_normalize(bent_pt(rax, 0.0f, -dy, sh));  /* -span rake */
                frame_quad(&mb, eR, ap, apo, eRo, nrp, 0.0f, 0.0f, ue, ve);
                frame_quad(&mb, ap, eL, eLo, apo, nrn, 0.0f, 0.0f, ue, ve);
            }
        }
        if (mb.index_count > 0) gable = mesh_from_builder(&mb);
        mb_free(&mb);
    }
    if (g_path_mat.albedo_tex.id != 0 || g_dark_wood.albedo_tex.id != 0) {
        MeshBuilder mbf, mbt;     /* threshold (sandstone) + jambs/lintel (dark wood) */
        int         oi;
        mb_init(&mbf);
        mb_init(&mbt);
        for (oi = 0; oi < no; oi++)
            emit_door_reveal(&mbf, &mbt, ops[oi].wall, ops[oi].center,
                             ops[oi].width, ops[oi].height, ops[oi].sill,
                             hw, hd, ROUTE_WALL_T);
        if (mbf.index_count > 0) door_floor = mesh_from_builder(&mbf);
        if (mbt.index_count > 0) door_trim  = mesh_from_builder(&mbt);
        mb_free(&mbf);
        mb_free(&mbt);
    }
    for (i = 0; i < g_room_frame_n; i++)
        if (g_room_frame[i].handle == shell->handle) {
            mesh_destroy(&g_room_frame[i].wall);
            mesh_destroy(&g_room_frame[i].wood);
            g_room_frame[i].wall = wall;
            g_room_frame[i].wood = wood;
            mesh_destroy(&g_room_frame[i].roof);
            mesh_destroy(&g_room_frame[i].gable);
            g_room_frame[i].roof = roof;
            g_room_frame[i].gable = gable;
            mesh_destroy(&g_room_frame[i].door_floor);
            mesh_destroy(&g_room_frame[i].door_trim);
            g_room_frame[i].door_floor = door_floor;
            g_room_frame[i].door_trim  = door_trim;
            return;
        }
    if (g_room_frame_n >= ROOM_FRAME_MAX) {
        static int warned = 0;
        if (!warned) { printf("room frame: cache full (%d rooms)\n",
                              ROOM_FRAME_MAX); warned = 1; }
        mesh_destroy(&wall); mesh_destroy(&wood);
        mesh_destroy(&roof); mesh_destroy(&gable);
        mesh_destroy(&door_floor); mesh_destroy(&door_trim);
        return;
    }
    g_room_frame[g_room_frame_n].handle = shell->handle;
    g_room_frame[g_room_frame_n].wall   = wall;
    g_room_frame[g_room_frame_n].wood   = wood;
    g_room_frame[g_room_frame_n].roof   = roof;
    g_room_frame[g_room_frame_n].gable  = gable;
    g_room_frame[g_room_frame_n].door_floor = door_floor;
    g_room_frame[g_room_frame_n].door_trim  = door_trim;
    g_room_frame_n++;
}

static RoomFrame *room_frame_get(sol_u32 handle) {
    int i;
    for (i = 0; i < g_room_frame_n; i++)
        if (g_room_frame[i].handle == handle) return &g_room_frame[i];
    return (RoomFrame *)0;
}

/* dark-wood curb trim for marble walkways: a derived mesh per walkway (its own
   material, so it can't ride the deck's mesh), cached by walkway handle exactly
   like RoomFrame. flush on the full rebuild; replace-by-handle on the incremental
   one. The draw gates on the deck being non-empty, so a stale entry from a now-
   invalid walkway simply isn't drawn (and the next full flush clears it). */
typedef struct { sol_u32 handle; Mesh trim; } WalkwayTrim;
static WalkwayTrim g_walk_trim[ROUTE_MAX];
static int         g_walk_trim_n = 0;

static void walk_trim_flush(void) {
    int i;
    for (i = 0; i < g_walk_trim_n; i++) mesh_destroy(&g_walk_trim[i].trim);
    g_walk_trim_n = 0;
}

static WalkwayTrim *walk_trim_get(sol_u32 handle) {
    int i;
    for (i = 0; i < g_walk_trim_n; i++)
        if (g_walk_trim[i].handle == handle) return &g_walk_trim[i];
    return (WalkwayTrim *)0;
}

static void walk_trim_store(sol_u32 handle, Mesh trim) {
    int i;
    for (i = 0; i < g_walk_trim_n; i++)
        if (g_walk_trim[i].handle == handle) {
            mesh_destroy(&g_walk_trim[i].trim);
            g_walk_trim[i].trim = trim;
            return;
        }
    if (g_walk_trim_n >= ROUTE_MAX) { mesh_destroy(&trim); return; }
    g_walk_trim[g_walk_trim_n].handle = handle;
    g_walk_trim[g_walk_trim_n].trim   = trim;
    g_walk_trim_n++;
}

/* fill g_campus.pads from the active world's ROOM anchors (not islands), and
   return the footprint bounding box via lo/hi. Returns the pad count. */
/* store one pad into g_campus.pads (capped) and grow the bbox by its footprint. */
static void campus_add_pad(int *n, float cx, float cz, float hw, float hd, float fy,
                           float *minx, float *maxx, float *minz, float *maxz) {
    if (*n >= CAMPUS_MAX_PADS) return;
    g_campus.pads[*n].cx = cx; g_campus.pads[*n].cz = cz;
    g_campus.pads[*n].hw = hw; g_campus.pads[*n].hd = hd;
    g_campus.pads[*n].floor_y = fy;
    if (*n == 0) { *minx = cx - hw; *maxx = cx + hw; *minz = cz - hd; *maxz = cz + hd; }
    else {
        if (cx - hw < *minx) *minx = cx - hw;
        if (cx + hw > *maxx) *maxx = cx + hw;
        if (cz - hd < *minz) *minz = cz - hd;
        if (cz + hd > *maxz) *maxz = cz + hd;
    }
    (*n)++;
}

/* fill g_campus.pads from the active world's room footprints (grown past the
   walls, sunk under the floor) AND a corridor of pads sampled along each
   walkway route (sunk under the deck), returning the footprint bbox via lo/hi. */
static int campus_gather_pads(AppState *st, const Route *routes, int nroutes,
                              vec3 *lo, vec3 *hi) {
    Scene  *s = &st->scene;
    int     n = 0, ri;
    sol_u32 i;
    float   minx = 0, maxx = 0, minz = 0, maxz = 0;
    /* room pads: grown past the walls, sunk under the floor */
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        if (o->mesh_ref) continue;
        rt = scene_meta_get(s, o->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        if (!scene_object_active(s, o->handle)) continue;
        r = editor_room_rect(s, o->handle);
        campus_add_pad(&n, r.cx, r.cz,
                       r.hw + ROUTE_WALL_T + CAMPUS_PAD_GROW,
                       r.hd + ROUTE_WALL_T + CAMPUS_PAD_GROW,
                       r.floor_y - CAMPUS_PAD_SINK,
                       &minx, &maxx, &minz, &maxz);
    }
    /* walkway corridor pads: sample each valid route's two legs (door_lo ->
       corner -> door_hi), one pad per step, sunk under the deck. */
    for (ri = 0; ri < nroutes; ri++) {
        const Route *rt = &routes[ri];
        vec3         pts[3];
        int          leg;
        if (!rt->valid) continue;
        pts[0] = rt->door_lo; pts[1] = rt->corner; pts[2] = rt->door_hi;
        for (leg = 0; leg < 2; leg++) {
            vec3  a = pts[leg], b = pts[leg + 1];
            float dx = b.x - a.x, dz = b.z - a.z;
            float len = (float)sqrt((double)(dx * dx + dz * dz));
            int   k = (int)(len / CAMPUS_PATH_STEP) + 1, sgi;
            if (len < 0.05f) continue;                 /* degenerate leg (straight path) */
            if (k > 64) k = 64;
            for (sgi = 0; sgi <= k; sgi++) {
                float t  = (float)sgi / (float)k;
                float px = a.x + dx * t, pz = a.z + dz * t;
                float py = a.y + (b.y - a.y) * t;
                campus_add_pad(&n, px, pz, CAMPUS_PATH_HALF, CAMPUS_PATH_HALF,
                               py - CAMPUS_PAD_SINK, &minx, &maxx, &minz, &maxz);
            }
        }
    }
    *lo = vec3_make(minx, 0.0f, minz);
    *hi = vec3_make(maxx, 0.0f, maxz);
    return n;
}

static void campus_flora_rebuild(AppState *st);  /* scatters campus grass + trees; defined below */

/* (re)build the active world's campus terrain into g_campus, or disable it.
   Derived: called at the end of connections_rebuild (every structural change). */
static void campus_rebuild(AppState *st, const Route *routes, int nroutes) {
    const char *wsname = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    sol_u32     anchor = workspace_anchor_find(&st->scene, wsname);
    const char *flag   = anchor ? scene_meta_get(&st->scene, anchor, "campus") : (const char *)0;
    vec3        lo, hi;
    float       margin, miny, maxy;
    int         k;
    MeshBuilder mb;
    mesh_destroy(&g_campus.mesh);
    g_campus.enabled = (flag && strcmp(flag, "1") == 0);
    g_campus.npads   = 0;
    if (!g_campus.enabled) return;
    g_campus.npads = campus_gather_pads(st, routes, nroutes, &lo, &hi);
    if (g_campus.npads == 0) { g_campus.enabled = 0; return; }    /* nothing to ground */
    /* rubber-band rectangle: footprint bbox + a margin proportional to its size
       (so rooms stay inside the un-faded rim zone). */
    margin = 0.15f * ((hi.x - lo.x) > (hi.z - lo.z) ? (hi.x - lo.x) : (hi.z - lo.z));
    if (margin < 8.0f) margin = 8.0f;
    g_campus.center = vec3_make((lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f);
    g_campus.w = (hi.x - lo.x) + 2.0f * margin;
    g_campus.d = (hi.z - lo.z) + 2.0f * margin;
    /* pads to campus-local (centre at origin); track the height range for the palette */
    miny = maxy = g_campus.pads[0].floor_y;
    for (k = 0; k < g_campus.npads; k++) {
        g_campus.pads[k].cx -= g_campus.center.x;
        g_campus.pads[k].cz -= g_campus.center.z;
        if (g_campus.pads[k].floor_y < miny) miny = g_campus.pads[k].floor_y;
        if (g_campus.pads[k].floor_y > maxy) maxy = g_campus.pads[k].floor_y;
    }
    g_campus.y0        = miny;
    g_campus.amp_range = (maxy - miny) + CAMPUS_HILL_AMP;
    if (g_campus.amp_range < 0.001f) g_campus.amp_range = 0.001f;
    mb_init(&mb);
    make_campus(&mb, g_campus.pads, g_campus.npads, g_campus.w, g_campus.d,
                CAMPUS_SUB, CAMPUS_HILL_AMP, CAMPUS_SEED);
    mb_scale_uvs(&mb, 1.0f / CAMPUS_TILE_M);   /* larger ground tiles, less repetition */
    if (mb.index_count > 0) g_campus.mesh = mesh_from_builder(&mb);
    mb_free(&mb);
}

#define ROOM_OPENINGS_CAP 32   /* doors + windows per room, across all walls */

/* a window's wall index, from meta["wall"] ("0".."3"); default N. */
static int window_wall(Scene *s, sol_u32 handle) {
    const char *m = scene_meta_get(s, handle, "wall");
    int v = m ? atoi(m) : 0;
    if (v < 0 || v > 3) v = 0;
    return v;
}

/* append every active child window of `room` to ops[] as a RoomOpening.
   A window is center-origin and parented to the room, so its room-local pos
   gives the along-wall center (x for N/S, z for E/W), the opening bottom
   (pos.y - h/2 = sill) and top (pos.y + h/2 = lintel). The sill clamps to the
   floor but the lintel stays independent so the top doesn't inflate. */
static void room_append_windows(Scene *s, sol_u32 room, RoomOpening *ops,
                                int *no, int max) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        float w, h, sill, lintel;
        int   wall;
        if (o->parent != room || !o->mesh_ref ||
            strcmp(o->mesh_ref, "window") != 0) continue;
        if (!scene_object_active(s, o->handle)) continue;
        if (*no >= max) break;
        w      = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "w");
        h      = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "h");
        wall   = window_wall(s, o->handle);
        sill   = o->pos.y - h * 0.5f;
        lintel = o->pos.y + h * 0.5f;
        if (sill < 0.0f) sill = 0.0f;
        ops[*no].wall   = wall;
        ops[*no].center = (wall == ROOM_WALL_N || wall == ROOM_WALL_S) ? o->pos.x : o->pos.z;
        ops[*no].width  = w;
        ops[*no].height = lintel;   /* was sill + h */
        ops[*no].sill   = sill;
        (*no)++;
    }
}

static void connections_rebuild(AppState *st) {
    Scene  *s = &st->scene;
    Route   routes[ROUTE_MAX];
    int     n = route_all(s, routes, ROUTE_MAX), i;
    sol_u32 j;

    room_frame_flush();   /* full pass is authoritative: drop stranded entries, repopulate below */
    walk_trim_flush();    /* same for the walkway curb trim */

    /* 1. walkways: anchor at the lower door, mesh in local coords */
    for (i = 0; i < n; i++) {
        Route       *r = &routes[i];
        SceneObject *o = scene_get(s, r->walkway);
        MeshBuilder  mb;
        if (!o) continue;
        mesh_destroy(&o->mesh);
        if (!r->valid) continue;
        o->pos = r->door_lo;
        o->pos.y -= ROUTE_DECK_DROP;   /* sit just under the floors: no threshold z-fight */
        o->rot = quat_identity();
        mb_init(&mb);
        make_walkway_L(&mb,
                       r->corner.x - r->door_lo.x, r->corner.z - r->door_lo.z,
                       r->corner.y - r->door_lo.y,
                       r->door_hi.x - r->door_lo.x, r->door_hi.z - r->door_lo.z,
                       r->door_hi.y - r->door_lo.y,
                       ROUTE_DECK_W, ROUTE_DECK_T);
        mb_scale_uvs(&mb, 1.0f / PATH_TILE_M);   /* marble tile size on the walkway */
        if (mb.index_count > 0) o->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
        {   /* dark-wood curb trim: its own mesh (own material), cached by handle */
            MeshBuilder tb;
            Mesh        tm;
            mb_init(&tb);
            make_walkway_trim(&tb,
                              r->corner.x - r->door_lo.x, r->corner.z - r->door_lo.z,
                              r->corner.y - r->door_lo.y,
                              r->door_hi.x - r->door_lo.x, r->door_hi.z - r->door_lo.z,
                              r->door_hi.y - r->door_lo.y,
                              ROUTE_DECK_W, CURB_W, CURB_H, CURB_OVER, ROUTE_DECK_T);
            memset(&tm, 0, sizeof tm);
            if (tb.index_count > 0) tm = mesh_from_builder(&tb);
            mb_free(&tb);
            walk_trim_store(o->handle, tm);
        }
    }

    /* 2. rooms: rebuild each shell child with its doorways */
    for (j = 0; j < s->count; j++) {
        SceneObject *room = &s->objects[j];
        const char  *rt;
        sol_u32      k;
        if (room->mesh_ref) continue;                 /* room parents are empties */
        rt = scene_meta_get(s, room->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        if (!scene_object_active(s, room->handle)) continue;   /* hidden workspace */
        for (k = 0; k < s->count; k++) {
            SceneObject *shell = &s->objects[k];
            RoomOpening  ops[ROOM_OPENINGS_CAP];
            int          no;
            float        w, d, h;
            MeshBuilder  mb;
            if (shell->parent != room->handle || !shell->mesh_ref ||
                strcmp(shell->mesh_ref, "room") != 0) continue;
            w = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
            d = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
            h = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
            no = route_room_openings_in(routes, n, s, room->handle, ops, ROOM_OPENINGS_CAP);
            room_append_windows(s, room->handle, ops, &no, ROOM_OPENINGS_CAP);
            mesh_destroy(&shell->mesh);
            mb_init(&mb);
            make_room_doored(&mb, w, d, h, ROUTE_WALL_T,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f,
                             (g_dark_wood.albedo_tex.id == 0 &&
                              mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ceil") > 0.5f),
                             ops, no);
            if (mb.index_count > 0) shell->mesh = mesh_from_builder(&mb);
            mb_free(&mb);
            room_frame_build(shell, ops, no);   /* the room's timber frame */
        }
    }

    campus_rebuild(st, routes, n);   /* derived: the active world's grounding terrain */
    campus_flora_rebuild(st);   /* the campus's grass + trees follow it */
}

/* Incident rebuild for the LIVE editor drag: re-thread only the walkways that
   touch `focus`, and rebuild only the shells of the rooms those walkways touch —
   a handful of meshes, not the whole graph. route_all is cheap now; this keeps
   the per-frame GPU buffer churn small. The full connections_rebuild runs once
   on release for the authoritative result (+ colliders + save). */
static void connections_rebuild_focus(AppState *st, sol_u32 focus) {
    Scene  *s = &st->scene;
    Route   routes[ROUTE_MAX];
    int     n, i;
    sol_u32 j;
    sol_u32 touched[ROUTE_MAX * 2];
    int     nt = 0;
    if (focus == 0) { connections_rebuild(st); return; }   /* full pass solves on its own */
    n = route_all(s, routes, ROUTE_MAX);

    /* walkways incident to focus (collect the rooms they touch) */
    for (i = 0; i < n; i++) {
        Route       *r = &routes[i];
        SceneObject *o = scene_get(s, r->walkway);
        MeshBuilder  mb;
        if (!o) continue;
        if (r->room_lo != focus && r->room_hi != focus) continue;
        if (nt < ROUTE_MAX * 2) touched[nt++] = r->room_lo;
        if (nt < ROUTE_MAX * 2) touched[nt++] = r->room_hi;
        mesh_destroy(&o->mesh);
        if (!r->valid) continue;
        o->pos = r->door_lo;
        o->pos.y -= ROUTE_DECK_DROP;
        o->rot = quat_identity();
        mb_init(&mb);
        make_walkway_L(&mb,
                       r->corner.x - r->door_lo.x, r->corner.z - r->door_lo.z,
                       r->corner.y - r->door_lo.y,
                       r->door_hi.x - r->door_lo.x, r->door_hi.z - r->door_lo.z,
                       r->door_hi.y - r->door_lo.y,
                       ROUTE_DECK_W, ROUTE_DECK_T);
        mb_scale_uvs(&mb, 1.0f / PATH_TILE_M);   /* marble tile size on the walkway */
        if (mb.index_count > 0) o->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
        {   /* dark-wood curb trim: rebuilt with the deck, replace-by-handle */
            MeshBuilder tb;
            Mesh        tm;
            mb_init(&tb);
            make_walkway_trim(&tb,
                              r->corner.x - r->door_lo.x, r->corner.z - r->door_lo.z,
                              r->corner.y - r->door_lo.y,
                              r->door_hi.x - r->door_lo.x, r->door_hi.z - r->door_lo.z,
                              r->door_hi.y - r->door_lo.y,
                              ROUTE_DECK_W, CURB_W, CURB_H, CURB_OVER, ROUTE_DECK_T);
            memset(&tm, 0, sizeof tm);
            if (tb.index_count > 0) tm = mesh_from_builder(&tb);
            mb_free(&tb);
            walk_trim_store(o->handle, tm);
        }
    }
    if (nt < ROUTE_MAX * 2) touched[nt++] = focus;

    /* shells of the touched rooms only */
    for (j = 0; j < s->count; j++) {
        SceneObject *room = &s->objects[j];
        const char  *rt;
        sol_u32      k;
        int          ti, hit = 0;
        if (room->mesh_ref) continue;
        rt = scene_meta_get(s, room->handle, "room_type");
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        for (ti = 0; ti < nt; ti++)
            if (touched[ti] == room->handle) { hit = 1; break; }
        if (!hit) continue;
        for (k = 0; k < s->count; k++) {
            SceneObject *shell = &s->objects[k];
            RoomOpening  ops[ROOM_OPENINGS_CAP];
            int          no;
            float        w, d, h;
            MeshBuilder  mb;
            if (shell->parent != room->handle || !shell->mesh_ref ||
                strcmp(shell->mesh_ref, "room") != 0) continue;
            w = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
            d = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
            h = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
            no = route_room_openings_in(routes, n, s, room->handle, ops, ROOM_OPENINGS_CAP);
            room_append_windows(s, room->handle, ops, &no, ROOM_OPENINGS_CAP);
            mesh_destroy(&shell->mesh);
            mb_init(&mb);
            make_room_doored(&mb, w, d, h, ROUTE_WALL_T,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f,
                             mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f,
                             (g_dark_wood.albedo_tex.id == 0 &&
                              mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ceil") > 0.5f),
                             ops, no);
            if (mb.index_count > 0) shell->mesh = mesh_from_builder(&mb);
            mb_free(&mb);
            room_frame_build(shell, ops, no);   /* the room's timber frame */
        }
    }
}

/* Make the edge object itself: two rels, ink material, geometry derived. */
static sol_u32 arrow_create(AppState *st, sol_u32 board, sol_u32 from, sol_u32 to) {
    SceneObject *bo = scene_get(&st->scene, board);
    float bt = bo ? mesh_ref_param("board", bo->mesh_params, bo->mesh_param_count, "t")
                  : 0.05f;
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    sol_u32 h;
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, board, empty,
                  vec3_make(0.0f, 0.0f, bt * 0.5f + ARROW_PIN_EPS),
                  quat_identity(), one);
    scene_rel_add(&st->scene, h, "connects", from);
    scene_rel_add(&st->scene, h, "connects", to);
    scene_meta_set(&st->scene, h, "name", "arrow");
    scene_mesh_ref_set(&st->scene, h, "arrow");
    {   /* inherit the board's active page so the arrow lands on the page you're
           viewing, not the root (the law: anything pinned to a board must tag
           active_page or it leaks to "/"). Read by handle = realloc-safe. */
        const char *ap = scene_meta_get(&st->scene, board, "active_page");
        scene_meta_set(&st->scene, h, "page", ap ? ap : "/");
    }
    {
        SceneObject *ao = scene_get(&st->scene, h);
        if (ao) {
            Material m   = material_default();
            m.base_color = vec3_make(0.10f, 0.10f, 0.12f);   /* ink */
            m.roughness  = 0.90f;
            ao->material = m;
        }
    }
    arrows_rebuild(st);
    return h;
}

/* The armed connection (C): a press on a second card of the same board
   births the arrow. One-shot — any press disarms, hit or miss. */
static sol_bool try_connect(AppState *st, sol_u32 hit) {
    SceneObject *from, *to;
    sol_u32      src = st->connect_from;
    if (src == 0) return SOL_FALSE;
    st->connect_from = 0;
    from = scene_get(&st->scene, src);
    to   = scene_get(&st->scene, hit);
    if (!from || !to || hit == src) { printf("connect: cancelled\n"); return SOL_FALSE; }
    if (to->kind != KIND_ALIAS && to->kind != KIND_NOTE) {
        printf("connect: cancelled (not a board card)\n");
        return SOL_FALSE;
    }
    if (to->parent != from->parent) {
        printf("connect: cancelled (not on the same board)\n");
        return SOL_FALSE;
    }
    arrow_create(st, from->parent, src, hit);
    scene_save(&st->scene, "scene.stml");
    printf("connected — the rel persists; the arrow is its picture\n");
    return SOL_TRUE;
}

/* Begin carrying: the GROUP ROOT moves (dragging a part would tear it off
   its import), and only root-level objects for PLAIN props. Cards move
   individually wherever they sit — the rotated-parent boundary item 4 noted
   is built now (scene_world_to_local), so a card on a vertical board begins
   its drag in the board's own plane. The grab offset keeps the object under
   the grip point instead of snapping its origin to the cursor. */
static void drag_begin(AppState *st, GLFWwindow *w, sol_u32 hit) {
    SceneObject *ho = scene_get(&st->scene, hit);
    SceneObject *o;
    sol_u32      target;
    vec3         wpos;
    Ray          r;
    float        t;

    if (!ho) return;
    if (ho->kind != KIND_PLAIN ||
        (ho->mesh_ref &&
         (strcmp(ho->mesh_ref, "folderbook") == 0 ||
          strcmp(ho->mesh_ref, "picture") == 0))) {
        target = hit;       /* cards (item 6), folder-link books, AND pictures move
                               INDIVIDUALLY, though parented to a room or board.
                               A picture only reaches drag_begin as a GROUP-drag
                               anchor — a lone one slides via picture_move_pick. */
    } else {
        if (object_is_arrow(&st->scene, hit)) return;   /* derived geometry: an
                                                           arrow follows its cards,
                                                           it is never dragged */
        if (object_is_walkway(&st->scene, hit)) return;   /* derived geometry */
        target = group_root(&st->scene, hit);    /* props move as their group */
        if (target == st->floor_handle) return;  /* the floor is the room, not a card */
        if (scene_meta_get(&st->scene, target, "room_type")) return;   /* architecture */
        o = scene_get(&st->scene, target);
        if (!o || o->parent != 0) return;        /* child PROPS stay grouped */
    }
    o = scene_get(&st->scene, target);
    if (!o) return;

    /* remember the pre-drag placement: a refused drop restores it (a FILE
       card snaps home when its drop pins an alias), and pulling a card off
       a board needs the board's OWN parent as the landing frame */
    st->drag_prev_parent   = o->parent;
    st->drag_prev_pos      = o->pos;
    st->drag_prev_rot      = o->rot;
    st->drag_ground_parent = o->parent;
    st->drag_board         = 0;
    st->drag_board_ox      = 0.0f;
    st->drag_board_oy      = 0.0f;
    if (object_is_board(&st->scene, o->parent)) {
        SceneObject *bo = scene_get(&st->scene, o->parent);
        st->drag_ground_parent = bo ? bo->parent : 0;
    }

    r = pick_ray(st, w);
    if (object_is_board(&st->scene, o->parent)) {
        /* begin in board mode: the constraint plane is the board's face */
        vec3 local;
        if (board_under_ray(st, r, &local) == o->parent) {
            st->drag_board     = o->parent;
            st->drag_board_ox  = o->pos.x - local.x;
            st->drag_board_oy  = o->pos.y - local.y;
            st->drag_handle    = target;
            st->drag_offset    = vec3_make(0.0f, 0.0f, 0.0f);
            st->drag_start_pos = object_world_pos(&st->scene, target);
            st->drag_moved     = SOL_FALSE;
            return;
        }
        /* edge grab beside the face: fall through to the ground plane */
    }
    wpos = object_world_pos(&st->scene, target);
    /* the constraint plane sits at the OBJECT's height (not the floor): the
       object then stays under the cursor at its own depth — dragging the
       floor plane under a raised object amplified every move by the depth
       ratio. The old eye-height pathology (a near-parallel plane racing to
       the horizon) is fenced by DRAG_MAX_DIST: an edge-on drag refuses
       instead of racing. */
    st->drag_plane_y = wpos.y;
    if (!ray_vs_plane(r, vec3_make(0.0f, wpos.y, 0.0f), vec3_make(0.0f, 1.0f, 0.0f), &t) ||
        t > DRAG_MAX_DIST)
        return;                                  /* plane edge-on or out of reach:
                                                    look up/down a little to drag */
    st->drag_handle    = target;
    st->drag_offset    = vec3_sub(wpos, vec3_add(r.origin, vec3_scale(r.dir, t)));
    st->drag_start_pos = wpos;                   /* world, throughout */
    st->drag_moved     = SOL_FALSE;
}

/* Begin dragging the whole selection together: snapshot every member's board-
   local position, then drag the anchor on the normal path; the update applies
   the anchor's delta to the rest. */
static void group_drag_begin(AppState *st, GLFWwindow *w) {
    int i;
    for (i = 0; i < st->sel_count; i++) {
        SceneObject *o = scene_get(&st->scene, st->sel[i]);
        st->group_prepos[i] = o ? o->pos : vec3_make(0.0f, 0.0f, 0.0f);
    }
    st->group_drag = SOL_TRUE;
    drag_begin(st, w, st->selected_handle);     /* the anchor leads */
}

/* Scroll arrives only as events (no poll), so it needs a callback; the window
   user-pointer bridges back to our state. read_input drains scroll_accum. */
static void on_scroll(GLFWwindow *w, double xoff, double yoff) {
    AppState *st = (AppState *)glfwGetWindowUserPointer(w);
    (void)xoff;
    if (st) st->scroll_accum += yoff;
}

/* Poll GLFW into a CameraInput (the platform layer; camera.c stays GLFW-free).
   Movement/look are level-triggered (held keys); the mode toggle is edge-
   triggered so it fires once per press. */
static void scene_resolve_meshes(Scene *s);    /* defined with init_scene below */
static void hdr_reload(AppState *st, const char *path);  /* the abbey key
                                          switches the sky (defined with
                                          the watcher below) */
static void apply_time_of_day(AppState *st);   /* P8 item 9: `-toggle's sky + sun */
static void apply_kind_materials(Scene *s);    /* likewise */
static void folderbook_materialize(Scene *s);  /* re-apply the leather cover (textures aren't serialized) */
static int  rescan_mirrors(AppState *st);      /* likewise */
static sol_bool load_palace(AppState *st);     /* likewise */
static void world_rebuild(AppState *st);       /* workspace-switch re-derive; load_palace inlines its OWN tail */
static void adopt_legacy_motion(AppState *st); /* P4 item 6: motion becomes data */
static void note_edit_end(AppState *st);       /* defined with on_key below */
static void place_confirm(AppState *st);       /* Furniture: Task 8 fills this in */
static void place_set_label(AppState *st, const char *label); /* Furniture: bookshelf label callback */

/* The codex mint's tiny LCG (item 9): varied-but-PERSISTENT books — the
   drawn parameters land in the parts' mesh attrs, so a minted book keeps
   its build forever; the generator is only consulted at mint time. */
static unsigned g_mint_rng = 0;
static float mint_range(float lo, float hi) {
    g_mint_rng = g_mint_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)((g_mint_rng >> 16) & 0x7FFF) / 32767.0f;
}

/* ---- the mesh asset registry (P4 item 4): shared shapes, owned once ----
   The store is the ONLY destroyer of registered meshes; objects borrow.
   File-static like the mint rng — main.c is the composition layer, and
   every consumer (resolve, release, the death sites, the HUD) lives here. */
static AssetStore g_mesh_assets;

static void mesh_asset_destroy(void *payload, void *user) {
    (void)user;
    mesh_destroy((Mesh *)payload);   /* GPU buffers + retained CpuGeom together */
}

/* The texture store (P4 item 4 piece 3): key = "t|<path>|s/l" — the same
   file as COLOR (sRGB-decoded by the sampler) and as DATA (raw) are two
   different GPU objects, so colorspace is part of identity. Policy is
   deliberately relaxed vs meshes: textures get registry IDENTITY (dedup +
   the watcher's reload hook) but SESSION lifetime — strict release waits
   for a consumer that actually churns textures; meshes churn at every L. */
static AssetStore g_tex_assets;

static void tex_asset_destroy(void *payload, void *user) {
    (void)user;
    rhi_destroy_texture(*(RhiTexture *)payload);
}

static void tex_asset_key(const char *path, sol_bool srgb, char *buf) {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    sprintf(buf, "t|%s|%c", path, srgb ? 's' : 'l');
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

/* ---- synthesized materials through the registry (texture side-quest) ----
   One entry per (kind, knob prefix) holding all three maps — albedo rides
   sRGB, normal/ORM ride linear (colorspace stays part of identity, the
   "t|" lesson). Key = "x|<kind>|<prefix over SCHEMA defaults>": the
   materials.stml layer is a render-time VOICE, never identity (the
   sounds.stml lesson) — editing it re-renders pixels behind the same
   handles, and nobody rebinds. A knob explicitly set to its schema
   default is identity-equal to leaving it unset (defaults-are-identity,
   the mesh doctrine); the file layer then voices both alike. The payload
   carries kind + prefix so the watcher re-renders without parsing keys. */
typedef struct {
    RhiTexture albedo, normal, orm;
    int        kind;
    float      prefix[TEXGEN_PARAMS];
    int        count;
} TexgenSet;

static AssetStore g_texgen_assets;

static void texgen_asset_destroy(void *payload, void *user) {
    TexgenSet *t = (TexgenSet *)payload;
    (void)user;
    rhi_destroy_texture(t->albedo);
    rhi_destroy_texture(t->normal);
    rhi_destroy_texture(t->orm);
}

/* materials.stml: <materials><stone depth="0.8"/></materials> — knobs by
   NAME over the kind defaults (the sounds.stml pattern verbatim; the tag
   IS the kind). Layering: schema defaults < materials.stml < the
   object's own <tex> knobs. */
static float g_mat_over[TEXGEN_KIND_COUNT][TEXGEN_PARAMS];
static int   g_mat_over_on[TEXGEN_KIND_COUNT];

static void load_material_overrides(void) {
    char     *src;
    StmlNode *root, *top;
    sol_u32   i;
    memset(g_mat_over_on, 0, sizeof g_mat_over_on);
    src = fs_read_file("materials.stml", 64L * 1024L, (long *)0, (int *)0);
    if (src == NULL) return;                   /* no file = pure presets */
    root = stml_parse(src);
    free(src);
    if (root == NULL) {
        fprintf(stderr, "materials.stml: parse error — using presets\n");
        return;
    }
    top = stml_child(root, "materials");
    for (i = 0; top && i < top->child_count; i++) {
        StmlNode          *n    = top->children[i];
        int                kind = n->tag ? texgen_kind(n->tag) : -1;
        const char *const *names;
        const float       *defs;
        int                k;
        if (kind < 0) continue;
        texgen_schema(kind, &names, &defs);
        for (k = 0; k < TEXGEN_PARAMS; k++) {
            const char *v = stml_attr(n, names[k]);
            g_mat_over[kind][k] = v ? (float)atof(v) : defs[k];
        }
        g_mat_over_on[kind] = 1;
    }
    stml_free(root);
}

/* the full knob vector one render sees: the object's explicit prefix,
   then the kind's current voice (file override or schema default) */
static void texgen_compose(int kind, const float *prefix, int count,
                           float *out) {
    const float *defs;
    int k;
    texgen_schema(kind, (const char *const **)0, &defs);
    for (k = 0; k < TEXGEN_PARAMS; k++)
        out[k] = (k < count) ? prefix[k]
               : (g_mat_over_on[kind] ? g_mat_over[kind][k] : defs[k]);
}

/* render the three maps for one knob vector and upload them; SOL_FALSE
   leaves outputs untouched (fail-open, the watcher's rule) */
static sol_bool texgen_mint(int kind, const float *knobs,
                            RhiTexture *albedo, RhiTexture *normal,
                            RhiTexture *orm) {
    size_t         bytes = (size_t)TEXGEN_SIZE * TEXGEN_SIZE * 4;
    unsigned char *ab, *nb, *ob;
    sol_bool       ok;
    ab = (unsigned char *)malloc(bytes);
    nb = (unsigned char *)malloc(bytes);
    ob = (unsigned char *)malloc(bytes);
    ok = (ab && nb && ob)
       ? texgen_render(kind, knobs, TEXGEN_PARAMS, ab, nb, ob)
       : SOL_FALSE;
    if (ok) {
        *albedo = rhi_create_texture(ab, TEXGEN_SIZE, TEXGEN_SIZE, RHI_TEX_SRGB8);
        *normal = rhi_create_texture(nb, TEXGEN_SIZE, TEXGEN_SIZE, RHI_TEX_RGBA8);
        *orm    = rhi_create_texture(ob, TEXGEN_SIZE, TEXGEN_SIZE, RHI_TEX_RGBA8);
    }
    free(ab); free(nb); free(ob);
    return ok;
}

/* the re-voice (a changed materials.stml): every live set re-renders
   with its own prefix over the NEW voice — in place, same handles */
static void texgen_revoice_visit(const char *key, void *payload, void *user) {
    TexgenSet     *t     = (TexgenSet *)payload;
    size_t         bytes = (size_t)TEXGEN_SIZE * TEXGEN_SIZE * 4;
    float          knobs[TEXGEN_PARAMS];
    unsigned char *ab, *nb, *ob;
    (void)key; (void)user;
    texgen_compose(t->kind, t->prefix, t->count, knobs);
    ab = (unsigned char *)malloc(bytes);
    nb = (unsigned char *)malloc(bytes);
    ob = (unsigned char *)malloc(bytes);
    if (ab && nb && ob &&
        texgen_render(t->kind, knobs, TEXGEN_PARAMS, ab, nb, ob)) {
        rhi_update_texture(t->albedo, ab, TEXGEN_SIZE, TEXGEN_SIZE, RHI_TEX_SRGB8);
        rhi_update_texture(t->normal, nb, TEXGEN_SIZE, TEXGEN_SIZE, RHI_TEX_RGBA8);
        rhi_update_texture(t->orm,    ob, TEXGEN_SIZE, TEXGEN_SIZE, RHI_TEX_RGBA8);
    }
    free(ab); free(nb); free(ob);
}

/* ================== the FIELD forest (P7 item 5) ================== */

#define FOREST_MAX_TREES   48
#define FOREST_PER_M2      0.018f
#define FOREST_VAR_SLOTS   512        /* canopy clusters cached per variant */
#define FOREST_CANOPY_CAP  8192       /* leaf clusters per island per kind  */
#define FOREST_FOOTPRINT   2.5f       /* clear band around a whole church   */
#define FOREST_RECLAIM     6.0f       /* how far ruin pulls the band in     */
#define FOREST_LEAF_SWAY   0.10f

/* a variant: a species at a size, with its OWN seed (distinct trees that
   still SHARE a mesh — instancing needs that). Per-placement yaw/scale
   jitter does the rest. */
typedef struct { int species; float scale; float seed; float density; }
        ForestVariant;
#define FOREST_TREE_VARIANTS 5    /* the first 5 are trees; index 5 = shrub */
static const ForestVariant FOREST_VARIANTS[FOREST_VARIANT_COUNT] = {
    { FLORA_OAK,     1.00f, 11.0f, 0.75f },
    { FLORA_OAK,     0.78f, 23.0f, 0.70f },
    { FLORA_BIRCH,   0.95f, 31.0f, 0.60f },
    { FLORA_PINE,    1.10f, 41.0f, 0.65f },
    { FLORA_CYPRESS, 0.85f, 53.0f, 0.80f },
    { FLORA_SHRUB,   1.00f, 67.0f, 0.85f }   /* undergrowth (item 7) */
};

/* built-once variant caches: the wood mesh lives in AppState, the canopy
   slot layout + the shared bark here (file-static, the mint-rng idiom) */
static FloraLeaf g_var_slots[FOREST_VARIANT_COUNT][FOREST_VAR_SLOTS];
static int       g_var_slot_count[FOREST_VARIANT_COUNT];
static RhiTexture g_bark_albedo, g_bark_normal, g_bark_orm;
static RhiTexture g_scree_albedo, g_scree_normal, g_scree_orm;
static int       g_forest_built = 0;

/* the variant's 10-param prefix: silhouette defaults at the variant's
   seed/size, leaf_density from the variant */
static void forest_variant_params(int v, float *p) {
    const float *defs;
    int k;
    flora_schema(FOREST_VARIANTS[v].species,
                 (const char *const **)0, &defs);
    for (k = 0; k < 10; k++) p[k] = defs[k];
    p[0] = FOREST_VARIANTS[v].seed;
    p[2] = defs[2] * FOREST_VARIANTS[v].scale;   /* height by the size knob */
    p[9] = FOREST_VARIANTS[v].density;
}

/* build the wood mesh + cache the canopy slots for every variant, and
   synthesize the one bark texture every forest trunk shares. Once. */
static void forest_build_variants(AppState *st) {
    int v;
    if (g_forest_built) return;
    for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
        MeshBuilder mb;
        float       p[10];
        forest_variant_params(v, p);
        mb_init(&mb);
        flora_tree_wood(&mb, FOREST_VARIANTS[v].species, p, 10);
        st->forest_wood[v] = mesh_from_builder(&mb);
        mb_free(&mb);
        g_var_slot_count[v] = flora_canopy(FOREST_VARIANTS[v].species,
                                           p, 10, g_var_slots[v],
                                           FOREST_VAR_SLOTS);
    }
    {   /* the unit pebble for scree (item 6) */
        MeshBuilder mb;
        mb_init(&mb);
        rock_pebble_unit(&mb);
        st->scree_mesh = mesh_from_builder(&mb);
        mb_free(&mb);
    }
    {   /* one bark set for the whole wood (a hero tree near you wears its
           own; the forest is backdrop — one texture is plenty) */
        float bk[TEXGEN_PARAMS];
        const float *bd;
        int k;
        texgen_schema(TEXGEN_BARK, (const char *const **)0, &bd);
        for (k = 0; k < TEXGEN_PARAMS; k++) bk[k] = bd[k];
        texgen_mint(TEXGEN_BARK, bk, &g_bark_albedo, &g_bark_normal,
                    &g_bark_orm);
    }
    {   /* one COURSE-FREE stone set for the scree (the boulder texture,
           shared) — granular rock, no masonry grid */
        float sk[TEXGEN_PARAMS];
        const float *sd;
        int k;
        texgen_schema(TEXGEN_STONE, (const char *const **)0, &sd);
        for (k = 0; k < TEXGEN_PARAMS; k++) sk[k] = sd[k];
        sk[2] = 0.0f;          /* course = 0: no grid, just stone */
        sk[5] = 0.7f;          /* depth: rougher granular relief */
        texgen_mint(TEXGEN_STONE, sk, &g_scree_albedo, &g_scree_normal,
                    &g_scree_orm);
    }
    g_forest_built = 1;
}

/* the scree's stone material (shared course-free maps) */
static Material forest_scree_material(void) {
    Material m = material_default();
    m.base_color = vec3_make(1.0f, 1.0f, 1.0f);
    m.roughness  = 1.0f;
    m.metallic   = 1.0f;
    m.albedo_tex = g_scree_albedo;
    m.normal_tex = g_scree_normal;
    m.mr_tex     = g_scree_orm;
    m.ao_tex     = g_scree_orm;
    return m;
}

/* the forest's bark material (shared maps; white base so they show) */
static Material forest_bark_material(void) {
    Material m = material_default();
    m.base_color = vec3_make(1.0f, 1.0f, 1.0f);
    m.roughness  = 1.0f;
    m.metallic   = 1.0f;
    m.albedo_tex = g_bark_albedo;
    m.normal_tex = g_bark_normal;
    m.mr_tex     = g_bark_orm;
    m.ao_tex     = g_bark_orm;
    return m;
}

/* find the church anchored on this island (the plot meta links them);
   returns its handle + fills the plan, or 0 */
static sol_u32 forest_island_church(AppState *st, const char *isl_nid,
                                    ChurchPlan *cp) {
    sol_u32 i;
    if (!isl_nid) return 0;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *a = &st->scene.objects[i];
        const char  *rt, *pl;
        sol_u32      c;
        float        params[10];
        int          j, last = -1;
        rt = scene_meta_get(&st->scene, a->handle, "room_type");
        pl = scene_meta_get(&st->scene, a->handle, "plot");
        if (!rt || strcmp(rt, "church") != 0) continue;
        if (!pl || strcmp(pl, isl_nid) != 0) continue;
        /* a church anchor's params live on its stone child */
        for (c = 0; c < st->scene.count; c++) {
            SceneObject *ch = &st->scene.objects[c];
            if (ch->parent != a->handle || !ch->mesh_ref) continue;
            if (strcmp(ch->mesh_ref, "church_stone") != 0) continue;
            for (j = 0; j < ch->mesh_param_count && j < 10; j++) {
                params[j] = ch->mesh_params[j];
                last = j;
            }
            church_plan(cp, params, last + 1);
            return a->handle;
        }
    }
    return 0;
}

static float forest_rnd(sol_u32 *rng) {
    *rng = *rng * 1664525u + 1013904223u;
    return (float)((*rng >> 8) & 0xFFFFu) / 65535.0f;
}

/* place one plant (a tree or a shrub) of variant v at (lx,h,lz): one
   wood instance + its cached canopy clusters, transformed. The wood and
   canopy accumulators are the caller's per-island arrays. */
static void forest_place(int v, float lx, float lz, float h, float yaw,
                         float sc, float *wood[], int wn[],
                         float *can[], int cn[]) {
    int   lk = flora_leaf_kind(FOREST_VARIANTS[v].species)
                   == FLORA_LEAF_CONIFER ? 1 : 0;
    float c = cosf(yaw), s = sinf(yaw);
    int   j;
    wood[v][wn[v] * 8 + 0] = lx;
    wood[v][wn[v] * 8 + 1] = h;
    wood[v][wn[v] * 8 + 2] = lz;
    wood[v][wn[v] * 8 + 3] = yaw;
    wood[v][wn[v] * 8 + 4] = sc;
    wood[v][wn[v] * 8 + 5] = sc;
    wood[v][wn[v] * 8 + 6] = 0.0f;
    wood[v][wn[v] * 8 + 7] = 0.0f;       /* wood doesn't sway */
    wn[v]++;
    for (j = 0; j < g_var_slot_count[v] && cn[lk] < FOREST_CANOPY_CAP; j++) {
        FloraLeaf *sl = &g_var_slots[v][j];
        float spx = sc * sl->pos.x, spy = sc * sl->pos.y, spz = sc * sl->pos.z;
        int   ci = cn[lk];
        can[lk][ci * 8 + 0] = lx + (c * spx + s * spz);
        can[lk][ci * 8 + 1] = h + spy;
        can[lk][ci * 8 + 2] = lz + (-s * spx + c * spz);
        can[lk][ci * 8 + 3] = yaw + sl->yaw;
        can[lk][ci * 8 + 4] = sc * sl->scale;
        can[lk][ci * 8 + 5] = sc * sl->scale;
        can[lk][ci * 8 + 6] = sl->phase;
        can[lk][ci * 8 + 7] = FOREST_LEAF_SWAY;
        cn[lk]++;
    }
}

static void forest_rebuild(AppState *st) {
    sol_u32 i;
    int     p, v, total = 0;
    if (!g_forest_built) return;
    for (p = 0; p < st->forest_count; p++) {     /* free the old buffers */
        for (v = 0; v < FOREST_VARIANT_COUNT; v++)
            if (st->forest[p].wood_count[v])
                rhi_destroy_buffer(st->forest[p].wood[v]);
        if (st->forest[p].canopy_count[0]) rhi_destroy_buffer(st->forest[p].canopy[0]);
        if (st->forest[p].canopy_count[1]) rhi_destroy_buffer(st->forest[p].canopy[1]);
        if (st->forest[p].scree_count)     rhi_destroy_buffer(st->forest[p].scree);
    }
    st->forest_count = 0;

    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float        w, d, amp, ruin = 0.0f;
        sol_u32      rng, anchor = 0;
        mat4         world;
        ChurchPlan   cp;
        float       *wood[FOREST_VARIANT_COUNT];
        float       *can[2];
        int          wn[FOREST_VARIANT_COUNT], cn[2];
        int          target, darts, placed, k, ok;
        ForestPatch *fp;

        if (st->forest_count >= (int)(sizeof st->forest / sizeof st->forest[0]))
            break;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "terrain") != 0) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;   /* hidden workspace */
        w   = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "w");
        d   = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "d");
        amp = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "amp");
        rng = (sol_u32)mesh_ref_param("terrain", o->mesh_params,
                                      o->mesh_param_count, "seed")
                  * 2246822519u + 3266489917u;
        world  = scene_world_matrix(&st->scene, o);
        anchor = forest_island_church(st, o->nid, &cp);
        if (anchor) ruin = cp.ruin;

        target = (int)(w * d * FOREST_PER_M2);
        if (target > FOREST_MAX_TREES) target = FOREST_MAX_TREES;
        if (target < 1) continue;

        ok = 1;
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
            wood[v] = (float *)malloc((size_t)target * 8 * sizeof(float));
            wn[v] = 0;
            if (!wood[v]) ok = 0;
        }
        can[0] = (float *)malloc((size_t)FOREST_CANOPY_CAP * 8 * sizeof(float));
        can[1] = (float *)malloc((size_t)FOREST_CANOPY_CAP * 8 * sizeof(float));
        cn[0] = cn[1] = 0;
        if (!can[0] || !can[1]) ok = 0;
        if (!ok) {
            for (v = 0; v < FOREST_VARIANT_COUNT; v++) free(wood[v]);
            free(can[0]); free(can[1]);
            continue;
        }

        {   /* TWO-PHASE SCATTER (item 7): trees first, then shrubs reading
               the trees — undergrowth shelters under the canopy (plan-as-
               author chained, one pass feeding the next) */
            float tree_xz[FOREST_MAX_TREES * 2];
            int   ntree = 0, stgt, sdarts, splaced;

            placed = 0;
            for (darts = 0; darts < target * 6 && placed < target; darts++) {
                float lx, lz, h, sx, sz, slope, yaw, sc;
                lx = (forest_rnd(&rng) - 0.5f) * (w - 2.0f);
                lz = (forest_rnd(&rng) - 0.5f) * (d - 2.0f);
                h  = terrain_height(o->mesh_params, o->mesh_param_count, lx, lz);
                sx = terrain_height(o->mesh_params, o->mesh_param_count, lx + 0.5f, lz);
                sz = terrain_height(o->mesh_params, o->mesh_param_count, lx, lz + 0.5f);
                slope = sqrtf((sx - h) * (sx - h) + (sz - h) * (sz - h)) * 2.0f;
                if (slope > 0.40f) continue;                  /* crag: bare */
                if (amp > 0.0f && h / amp > 0.74f) continue;  /* peaks: bare */
                if (anchor) {   /* keep clear of a standing church; the band
                                   shrinks as it falls — the wood reclaims */
                    vec3  wp = mat4_mul_point(world, vec3_make(lx, h, lz));
                    vec3  al = scene_world_to_local(&st->scene, anchor, wp);
                    float margin = FOREST_FOOTPRINT - ruin * FOREST_RECLAIM;
                    if (church_occupies(&cp, al.x, al.z, margin)) continue;
                }
                v   = (int)(forest_rnd(&rng) * (float)FOREST_TREE_VARIANTS);
                if (v >= FOREST_TREE_VARIANTS) v = FOREST_TREE_VARIANTS - 1;
                yaw = forest_rnd(&rng) * 6.2831853f;
                sc  = FOREST_VARIANTS[v].scale * (0.82f + 0.36f * forest_rnd(&rng));
                forest_place(v, lx, lz, h, yaw, sc, wood, wn, can, cn);
                tree_xz[ntree * 2 + 0] = lx;
                tree_xz[ntree * 2 + 1] = lz;
                ntree++;
                placed++;
            }

            /* phase 2: shrubs, biased UNDER the canopy — accept near a
               tree, rarely in the open (woodland understory) */
            stgt = (int)((float)placed * 1.6f);
            if (stgt > target) stgt = target;
            splaced = 0;
            for (sdarts = 0; sdarts < stgt * 8 && splaced < stgt; sdarts++) {
                float lx, lz, h, sx, sz, slope, yaw, sc, near2 = 1e30f;
                int   ti;
                lx = (forest_rnd(&rng) - 0.5f) * (w - 2.0f);
                lz = (forest_rnd(&rng) - 0.5f) * (d - 2.0f);
                h  = terrain_height(o->mesh_params, o->mesh_param_count, lx, lz);
                sx = terrain_height(o->mesh_params, o->mesh_param_count, lx + 0.5f, lz);
                sz = terrain_height(o->mesh_params, o->mesh_param_count, lx, lz + 0.5f);
                slope = sqrtf((sx - h) * (sx - h) + (sz - h) * (sz - h)) * 2.0f;
                if (slope > 0.45f) continue;
                if (amp > 0.0f && h / amp > 0.78f) continue;
                if (anchor) {
                    vec3  wp = mat4_mul_point(world, vec3_make(lx, h, lz));
                    vec3  al = scene_world_to_local(&st->scene, anchor, wp);
                    float margin = FOREST_FOOTPRINT - ruin * FOREST_RECLAIM;
                    if (church_occupies(&cp, al.x, al.z, margin)) continue;
                }
                for (ti = 0; ti < ntree; ti++) {
                    float ddx = lx - tree_xz[ti * 2], ddz = lz - tree_xz[ti * 2 + 1];
                    float d2 = ddx * ddx + ddz * ddz;
                    if (d2 < near2) near2 = d2;
                }
                /* under a crown (< 3.2 m) accept; else a sparse few */
                if (near2 > 3.2f * 3.2f && forest_rnd(&rng) > 0.18f) continue;
                yaw = forest_rnd(&rng) * 6.2831853f;
                sc  = FOREST_VARIANTS[5].scale * (0.7f + 0.5f * forest_rnd(&rng));
                forest_place(5, lx, lz, h, yaw, sc, wood, wn, can, cn);
                splaced++;
            }
            placed += splaced;
        }

        fp = &st->forest[st->forest_count];
        fp->island = o->handle;
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
            fp->wood_count[v] = wn[v];
            fp->wood_mat[v]   = forest_bark_material();
            if (wn[v] > 0)
                fp->wood[v] = rhi_create_buffer(RHI_BUFFER_VERTEX, wood[v],
                                  (size_t)wn[v] * 8 * sizeof(float));
            free(wood[v]);
        }
        for (k = 0; k < 2; k++) {
            fp->canopy_count[k] = cn[k];
            if (cn[k] > 0)
                fp->canopy[k] = rhi_create_buffer(RHI_BUFFER_VERTEX, can[k],
                                    (size_t)cn[k] * 8 * sizeof(float));
            free(can[k]);
        }

        {   /* FIELD scree (item 6): small pebbles scattered EVERYWHERE —
               no tree gates (gravel lives on crag and near ruins alike),
               ghost (you don't trip on it). A separate denser throw. */
            int    starget = (int)(w * d * 0.045f), sn = 0, sk;
            float *scr;
            if (starget > 160) starget = 160;
            scr = (float *)malloc((size_t)(starget > 0 ? starget : 1)
                                  * 8 * sizeof(float));
            if (scr) {
                for (sk = 0; sk < starget; sk++) {
                    float lx, lz, h, ps;
                    lx = (forest_rnd(&rng) - 0.5f) * (w - 1.0f);
                    lz = (forest_rnd(&rng) - 0.5f) * (d - 1.0f);
                    h  = terrain_height(o->mesh_params, o->mesh_param_count,
                                        lx, lz);
                    ps = 0.10f + 0.28f * forest_rnd(&rng);   /* pebble size */
                    scr[sn * 8 + 0] = lx;
                    scr[sn * 8 + 1] = h + ps * 0.35f;        /* half-sunk */
                    scr[sn * 8 + 2] = lz;
                    scr[sn * 8 + 3] = forest_rnd(&rng) * 6.2831853f;
                    scr[sn * 8 + 4] = ps;
                    scr[sn * 8 + 5] = ps;
                    scr[sn * 8 + 6] = 0.0f;
                    scr[sn * 8 + 7] = 0.0f;                  /* stone: no sway */
                    sn++;
                }
                fp->scree_count = sn;
                if (sn > 0)
                    fp->scree = rhi_create_buffer(RHI_BUFFER_VERTEX, scr,
                                    (size_t)sn * 8 * sizeof(float));
                free(scr);
            } else {
                fp->scree_count = 0;
            }
        }

        st->forest_count++;
        total += placed;
    }
    if (st->forest_count > 0)
        printf("the forest: %d island(s), %d trees + scree\n",
               st->forest_count, total);
}

static void campus_flora_free(void) {
    int v;
    if (g_campus_flora.grass_n) rhi_destroy_buffer(g_campus_flora.grass);
    for (v = 0; v < FOREST_VARIANT_COUNT; v++)
        if (g_campus_flora.wood_n[v]) rhi_destroy_buffer(g_campus_flora.wood[v]);
    if (g_campus_flora.canopy_n[0]) rhi_destroy_buffer(g_campus_flora.canopy[0]);
    if (g_campus_flora.canopy_n[1]) rhi_destroy_buffer(g_campus_flora.canopy[1]);
    if (g_campus_flora.flower_n) rhi_destroy_buffer(g_campus_flora.flowers);
    if (g_campus_flora.scree_n)  rhi_destroy_buffer(g_campus_flora.scree);
    memset(&g_campus_flora, 0, sizeof g_campus_flora);
}

/* scatter grass + trees over the active campus, sampling campus_height and
   skipping the pads (rooms + walkway corridors). Reuses the meadow tuft format
   and forest_place; drawn explicitly (campus is a derived global). No-op when off. */
static void campus_flora_rebuild(AppState *st) {
    float   cx, cz, w, d, amp;
    int     target, placed, k;
    sol_u32 rng;
    float  *buf;
    (void)st;
    campus_flora_free();
    if (!g_campus.enabled || g_campus.npads == 0) return;
    cx = g_campus.center.x; cz = g_campus.center.z;
    w = g_campus.w; d = g_campus.d; amp = CAMPUS_HILL_AMP;

    /* --- grass (world-space; reuse the meadow tuft format/colors) --- */
    rng    = CAMPUS_SEED * 2654435761u + 7u;
    target = (int)(w * d * CAMPUS_GRASS_PER_M2);
    if (target > CAMPUS_GRASS_MAX) target = CAMPUS_GRASS_MAX;
    buf = (float *)malloc((size_t)(target > 0 ? target : 1) * 8 * sizeof(float));
    placed = 0;
    for (k = 0; buf && k < target; k++) {
        float lx, lz, h, sx, sz, slope, r1, r2, r3;
        lx = (meadow_rnd(&rng) - 0.5f) * (w - 1.5f);
        lz = (meadow_rnd(&rng) - 0.5f) * (d - 1.5f);
        if (campus_point_blocked(g_campus.pads, g_campus.npads, lx, lz, CAMPUS_GRASS_CLEAR)) continue;
        h  = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz);
        sx = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx + 0.5f, lz);
        sz = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz + 0.5f);
        slope = (float)sqrt((double)((sx - h) * (sx - h) + (sz - h) * (sz - h))) * 2.0f;
        if (slope > 0.5f) continue;                       /* steep hillside: bare */
        r1 = meadow_rnd(&rng); r2 = meadow_rnd(&rng); r3 = meadow_rnd(&rng);
        buf[placed * 8 + 0] = cx + lx;
        buf[placed * 8 + 1] = h;
        buf[placed * 8 + 2] = cz + lz;
        buf[placed * 8 + 3] = 0.16f + 0.20f * r1;
        buf[placed * 8 + 4] = 0.10f + 0.10f * r2;
        buf[placed * 8 + 5] = 0.22f + 0.16f * r3;
        buf[placed * 8 + 6] = 0.05f + 0.06f * r2;
        buf[placed * 8 + 7] = 1.0f;
        placed++;
    }
    if (buf && placed > 0) {
        g_campus_flora.grass = rhi_create_buffer(RHI_BUFFER_VERTEX, buf,
                                   (size_t)placed * 8 * sizeof(float));
        g_campus_flora.grass_n = placed;
    }
    free(buf);

    /* --- flowers (world-space; reuse flower_patch clustering + flower_color) --- */
    {
        sol_u32 fseed = CAMPUS_SEED * 2654435761u + 101u;
        sol_u32 frng  = CAMPUS_SEED * 2654435761u + 23u;
        int     ftarget = (int)(w * d * 0.5f), fk, fn = 0;
        float  *fbuf;
        if (ftarget > CAMPUS_GRASS_MAX) ftarget = CAMPUS_GRASS_MAX;
        fbuf = (float *)malloc((size_t)(ftarget > 0 ? ftarget : 1) * 8 * sizeof(float));
        for (fk = 0; fbuf && fk < ftarget; fk++) {
            float lx, lz, h, sx, sz, slope, cr, cg, cb;
            lx = (meadow_rnd(&frng) - 0.5f) * (w - 1.5f);
            lz = (meadow_rnd(&frng) - 0.5f) * (d - 1.5f);
            if (flower_patch(fseed, lx, lz) < 0.62f) continue;
            if (campus_point_blocked(g_campus.pads, g_campus.npads, lx, lz, CAMPUS_GRASS_CLEAR)) continue;
            h  = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz);
            sx = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx + 0.5f, lz);
            sz = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz + 0.5f);
            slope = (float)sqrt((double)((sx - h) * (sx - h) + (sz - h) * (sz - h))) * 2.0f;
            if (slope > 0.4f) continue;
            flower_color(&frng, &cr, &cg, &cb);
            fbuf[fn * 8 + 0] = cx + lx; fbuf[fn * 8 + 1] = h; fbuf[fn * 8 + 2] = cz + lz;
            fbuf[fn * 8 + 3] = 0.14f + 0.10f * meadow_rnd(&frng);
            fbuf[fn * 8 + 4] = cr; fbuf[fn * 8 + 5] = cg; fbuf[fn * 8 + 6] = cb; fbuf[fn * 8 + 7] = 1.0f;
            fn++;
        }
        if (fbuf && fn > 0) {
            g_campus_flora.flowers = rhi_create_buffer(RHI_BUFFER_VERTEX, fbuf, (size_t)fn * 8 * sizeof(float));
            g_campus_flora.flower_n = fn;
        }
        free(fbuf);
    }

    /* --- trees (local-to-centre via forest_place; needs the canopy slots) --- */
    if (g_forest_built) {
        float *wood[FOREST_VARIANT_COUNT];
        float *can[2];
        int    wn[FOREST_VARIANT_COUNT], cn[2], v, ok = 1, darts;
        float  tree_xz[CAMPUS_TREE_MAX * 2];
        int    ntree = 0;
        rng    = CAMPUS_SEED * 2246822519u + 13u;
        target = (int)(w * d * CAMPUS_TREE_PER_M2);
        if (target > CAMPUS_TREE_MAX) target = CAMPUS_TREE_MAX;
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
            wood[v] = (float *)malloc((size_t)(target > 0 ? target : 1) * 8 * sizeof(float));
            wn[v] = 0;
            if (!wood[v]) ok = 0;
        }
        can[0] = (float *)malloc((size_t)FOREST_CANOPY_CAP * 8 * sizeof(float));
        can[1] = (float *)malloc((size_t)FOREST_CANOPY_CAP * 8 * sizeof(float));
        cn[0] = cn[1] = 0;
        if (!can[0] || !can[1]) ok = 0;
        if (ok && target > 0) {
            placed = 0;
            for (darts = 0; darts < target * 6 && placed < target; darts++) {
                float lx, lz, h, sx, sz, slope, yaw, sc;
                lx = (forest_rnd(&rng) - 0.5f) * (w - 2.0f);
                lz = (forest_rnd(&rng) - 0.5f) * (d - 2.0f);
                if (campus_point_blocked(g_campus.pads, g_campus.npads, lx, lz, CAMPUS_TREE_CLEAR)) continue;
                h  = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz);
                sx = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx + 0.5f, lz);
                sz = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz + 0.5f);
                slope = (float)sqrt((double)((sx - h) * (sx - h) + (sz - h) * (sz - h))) * 2.0f;
                if (slope > 0.45f) continue;
                v   = (int)(forest_rnd(&rng) * (float)FOREST_TREE_VARIANTS);
                if (v >= FOREST_TREE_VARIANTS) v = FOREST_TREE_VARIANTS - 1;
                yaw = forest_rnd(&rng) * 6.2831853f;
                sc  = FOREST_VARIANTS[v].scale * (0.82f + 0.36f * forest_rnd(&rng));
                forest_place(v, lx, lz, h, yaw, sc, wood, wn, can, cn);
                if (ntree < CAMPUS_TREE_MAX) {
                    tree_xz[ntree * 2 + 0] = lx;
                    tree_xz[ntree * 2 + 1] = lz;
                    ntree++;
                }
                placed++;
            }
            {   /* shrubs (variant 5), biased UNDER the canopy like the islands */
                int stgt = (int)((float)placed * 1.6f), sdarts, splaced = 0;
                if (stgt > target) stgt = target;   /* wood[5] is sized `target` */
                for (sdarts = 0; sdarts < stgt * 8 && splaced < stgt; sdarts++) {
                    float lx, lz, h, sx, sz, slope, yaw, sc, near2 = 1e30f;
                    int   ti;
                    lx = (forest_rnd(&rng) - 0.5f) * (w - 2.0f);
                    lz = (forest_rnd(&rng) - 0.5f) * (d - 2.0f);
                    if (campus_point_blocked(g_campus.pads, g_campus.npads, lx, lz, CAMPUS_GRASS_CLEAR)) continue;
                    h  = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz);
                    sx = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx + 0.5f, lz);
                    sz = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz + 0.5f);
                    slope = (float)sqrt((double)((sx - h) * (sx - h) + (sz - h) * (sz - h))) * 2.0f;
                    if (slope > 0.45f) continue;
                    for (ti = 0; ti < ntree; ti++) {
                        float ddx = lx - tree_xz[ti * 2], ddz = lz - tree_xz[ti * 2 + 1];
                        float d2 = ddx * ddx + ddz * ddz;
                        if (d2 < near2) near2 = d2;
                    }
                    if (near2 > 3.2f * 3.2f && forest_rnd(&rng) > 0.18f) continue;  /* under crown, sparse open */
                    yaw = forest_rnd(&rng) * 6.2831853f;
                    sc  = FOREST_VARIANTS[5].scale * (0.7f + 0.5f * forest_rnd(&rng));
                    forest_place(5, lx, lz, h, yaw, sc, wood, wn, can, cn);
                    splaced++;
                }
            }
            for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
                g_campus_flora.wood_n[v] = wn[v];
                if (wn[v] > 0)
                    g_campus_flora.wood[v] = rhi_create_buffer(RHI_BUFFER_VERTEX,
                                                 wood[v], (size_t)wn[v] * 8 * sizeof(float));
            }
            for (k = 0; k < 2; k++) {
                g_campus_flora.canopy_n[k] = cn[k];
                if (cn[k] > 0)
                    g_campus_flora.canopy[k] = rhi_create_buffer(RHI_BUFFER_VERTEX,
                                                   can[k], (size_t)cn[k] * 8 * sizeof(float));
            }
        }
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) free(wood[v]);
        free(can[0]); free(can[1]);
    }

    /* --- scree (pebbles, local-to-centre; reuse the scree mesh) --- */
    {
        sol_u32 srng = CAMPUS_SEED * 2246822519u + 29u;
        int     starget = (int)(w * d * 0.045f), sn = 0, sk;
        float  *scr;
        if (starget > CAMPUS_SCREE_MAX) starget = CAMPUS_SCREE_MAX;
        scr = (float *)malloc((size_t)(starget > 0 ? starget : 1) * 8 * sizeof(float));
        for (sk = 0; scr && sk < starget; sk++) {
            float lx, lz, h, ps;
            lx = (forest_rnd(&srng) - 0.5f) * (w - 1.0f);
            lz = (forest_rnd(&srng) - 0.5f) * (d - 1.0f);
            if (campus_point_blocked(g_campus.pads, g_campus.npads, lx, lz, CAMPUS_GRASS_CLEAR)) continue;
            h  = campus_height(g_campus.pads, g_campus.npads, w, d, amp, CAMPUS_SEED, lx, lz);
            ps = 0.10f + 0.28f * forest_rnd(&srng);
            scr[sn * 8 + 0] = lx; scr[sn * 8 + 1] = h + ps * 0.35f; scr[sn * 8 + 2] = lz;
            scr[sn * 8 + 3] = forest_rnd(&srng) * 6.2831853f;
            scr[sn * 8 + 4] = ps; scr[sn * 8 + 5] = ps; scr[sn * 8 + 6] = 0.0f; scr[sn * 8 + 7] = 0.0f;
            sn++;
        }
        if (scr && sn > 0) {
            g_campus_flora.scree = rhi_create_buffer(RHI_BUFFER_VERTEX, scr, (size_t)sn * 8 * sizeof(float));
            g_campus_flora.scree_n = sn;
        }
        free(scr);
    }
}

/* ---- the watcher (P4 item 4 piece 3c): hot reload by mtime ----
   A small watch list polled every half second. Textures re-decode through
   rhi_update_texture — same handle, new pixels, nobody rebinds; a changed
   glb re-imports its anchors through the registry; the .hdr re-runs the
   IBL bakes. FAIL-OPEN: a torn mid-save read keeps the old contents and
   retries next poll (the mtime is recorded only after a good reload). */
typedef enum { WATCH_TEX_SRGB = 0, WATCH_GLB, WATCH_HDR, WATCH_SND,
               WATCH_MAT } WatchKind;
typedef struct {
    char       path[200];
    long       mtime;
    int        kind;
    RhiTexture tex;          /* WATCH_TEX_*: the in-place handle */
} WatchEntry;
#define WATCH_MAX 32
static WatchEntry g_watch[WATCH_MAX];
static int        g_watch_count;
static double     g_watch_last;

static void watch_add(const char *path, int kind, RhiTexture tex) {
    WatchEntry *w;
    int         i;
    if (g_watch_count >= WATCH_MAX) return;
    if (strlen(path) >= sizeof g_watch[0].path) return;
    for (i = 0; i < g_watch_count; i++)
        if (strcmp(g_watch[i].path, path) == 0) return;   /* already watched */
    w = &g_watch[g_watch_count++];
    strcpy(w->path, path);
    w->kind  = kind;
    w->tex   = tex;
    w->mtime = fs_mtime(path);
}

/* ---- glb models through the registry (P4 item 4 piece 3) ----
   Parts are keyed "g|<path>|<i>" with whole GlbPart payloads — mesh AND
   material, so a memo hit reconstructs the model without the file: the
   material's texture handles ride inside the payload. The MEMO solves the
   bootstrap problem (you can't acquire part 2 of a file you haven't parsed
   — its part count lives inside it): parse once per session, remember the
   count, borrow by index forever after. Two anchors of one sword now share
   one set of buffers — the cross-anchor dedup imports never had. */
static AssetStore g_glbpart_assets;

static void glb_part_destroy(void *payload, void *user) {
    (void)user;
    mesh_destroy(&((GlbPart *)payload)->mesh);  /* textures are session-owned */
}

#define GLB_MEMO_MAX 32
typedef struct { char path[200]; int parts; } GlbMemo;
static GlbMemo g_glb_memo[GLB_MEMO_MAX];
static int     g_glb_memo_count;

static void glb_memo_remove(const char *path) {
    int i;
    for (i = 0; i < g_glb_memo_count; i++) {
        if (strcmp(g_glb_memo[i].path, path) == 0) {
            g_glb_memo[i] = g_glb_memo[--g_glb_memo_count];
            return;
        }
    }
}

static void glb_part_key(const char *path, int index, char *buf) {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    sprintf(buf, "g|%s|%d", path, index);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

/* register a model's embedded textures ("t|<path>#<n>|g", discovery order)
   so the watcher can find them; session lifetime like every texture */
static void glb_register_textures(const char *path, const GlbModel *model) {
    sol_u32 seen[64];
    int     nseen = 0, named = 0;
    sol_u32 m;
    for (m = 0; m < model->count; m++) {
        RhiTexture slots[4];
        int        si, k;
        slots[0] = model->parts[m].material.albedo_tex;
        slots[1] = model->parts[m].material.mr_tex;
        slots[2] = model->parts[m].material.ao_tex;
        slots[3] = model->parts[m].material.normal_tex;
        for (si = 0; si < 4; si++) {
            char key[320];
            if (slots[si].id == 0) continue;
            for (k = 0; k < nseen; k++)
                if (seen[k] == slots[si].id) break;
            if (k < nseen || nseen >= 64) continue;
            seen[nseen++] = slots[si].id;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            sprintf(key, "t|%s#%d|g", path, named);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
            named++;
            asset_store_add(&g_tex_assets, key, &slots[si], sizeof slots[si]);
        }
    }
}

/* The acquire-or-parse handshake for a whole model. On a memo hit, every
   part is borrowed from the store and the file never opens; on first sight
   the file parses once and everything registers. Either way the caller owns
   only the parts ARRAY (glb_free), never the GPU resources. */
static sol_bool glb_acquire_model(const char *path, GlbModel *out) {
    char key[320];
    int  i, memo = -1;
    for (i = 0; i < g_glb_memo_count; i++) {
        if (strcmp(g_glb_memo[i].path, path) == 0) { memo = i; break; }
    }
    if (memo >= 0) {
        int n = g_glb_memo[memo].parts;
        out->parts = (GlbPart *)malloc((size_t)n * sizeof(GlbPart));
        out->count = (sol_u32)n;
        if (out->parts == NULL) return SOL_FALSE;
        for (i = 0; i < n; i++) {
            glb_part_key(path, i, key);
            if (!asset_acquire(&g_glbpart_assets, key,
                               &out->parts[i], sizeof(GlbPart))) {
                fprintf(stderr, "glb registry: '%s' part %d missing — memo stale?\n",
                        path, i);
                free(out->parts);
                return SOL_FALSE;
            }
        }
        return SOL_TRUE;
    }
    if (!glb_load(path, out)) return SOL_FALSE;
    for (i = 0; i < (int)out->count; i++) {
        glb_part_key(path, i, key);
        asset_store_add(&g_glbpart_assets, key, &out->parts[i], sizeof(GlbPart));
    }
    glb_register_textures(path, out);
    if (g_glb_memo_count < GLB_MEMO_MAX && strlen(path) < sizeof g_glb_memo[0].path) {
        strcpy(g_glb_memo[g_glb_memo_count].path, path);
        g_glb_memo[g_glb_memo_count].parts = (int)out->count;
        g_glb_memo_count++;
    }
    {
        RhiTexture none;
        none.id = 0;
        watch_add(path, WATCH_GLB, none);      /* edits to the file re-import */
    }
    return SOL_TRUE;
}

/* The registry key for an object's geometry: the ref + its EFFECTIVE
   parameters — the file's prefix merged with the schema defaults, so an old
   3-param room and its explicit full-param twin read as THE SAME SHAPE
   (params are identity, and defaults are part of it). SOL_FALSE = not
   registry material: no ref, an unknown ref, or an arrow (scene-derived,
   arrows_rebuild owns those). Key budget: "m|" + ref + 8 params x ~15
   chars < 140 — callers pass char[160]. */
static sol_bool mesh_asset_key(const SceneObject *o, char *buf) {
    const char *const *names;
    const float       *defaults;
    int                n, i;
    size_t             len;
    if (!o->mesh_ref) return SOL_FALSE;
    if (strcmp(o->mesh_ref, "arrow") == 0) return SOL_FALSE;
    /* room + walkway are scene-derived: connections_rebuild owns their meshes
       (destroys + rebuilds them each pass, like arrows_rebuild does arrows).
       They must NEVER be shared through the asset store, or that mesh_destroy
       frees a buffer the store still references — double-freeing it and, via
       buffer-id reuse, blanking an unrelated object (the disappearing room). */
    if (strcmp(o->mesh_ref, "room") == 0 ||
        strcmp(o->mesh_ref, "walkway") == 0) return SOL_FALSE;
    n = mesh_ref_schema(o->mesh_ref, &names, &defaults);
    if (n < 0) return SOL_FALSE;
    (void)defaults;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    len = (size_t)sprintf(buf, "m|%s", o->mesh_ref);
    for (i = 0; i < n; i++) {
        float v = mesh_ref_param(o->mesh_ref, o->mesh_params,
                                 o->mesh_param_count, names[i]);
        len += (size_t)sprintf(buf + len, "|%.9g", (double)v);
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    return SOL_TRUE;
}

/* The registry key for an object's synthesized material: kind + the
   explicit prefix merged with SCHEMA defaults — the same defaults-are-
   identity rule as meshes, and deliberately blind to materials.stml
   (a voice change must not re-key the world). Callers pass char[200]. */
static sol_bool texgen_asset_key(const SceneObject *o, char *buf) {
    const float *defs;
    int          kind, k;
    size_t       len;
    if (!o->tex_ref) return SOL_FALSE;
    kind = texgen_kind(o->tex_ref);
    if (kind < 0) return SOL_FALSE;
    texgen_schema(kind, (const char *const **)0, &defs);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    len = (size_t)sprintf(buf, "x|%s", o->tex_ref);
    for (k = 0; k < TEXGEN_PARAMS; k++) {
        float v = (k < o->tex_param_count) ? o->tex_params[k] : defs[k];
        len += (size_t)sprintf(buf + len, "|%.9g", (double)v);
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    return SOL_TRUE;
}

/* The mirror walk of scene_resolve_meshes: registry meshes go BACK to the
   store (each object carries everything its release needs — the key
   re-derives from ref + params); non-registry meshes — arrows, glb import
   parts — are uniquely owned per object today and die outright with their
   scene. With both handled, the L-reload leak (tolerated since P3 item 1)
   is retired. */
static void scene_release_meshes(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        char        key[160];
        char        xkey[200];
        const char *akey;
        if (o->tex_ref && o->material.albedo_tex.id &&
            texgen_asset_key(o, xkey)) {   /* synthesized maps: same mirror —
                                              the key re-derives, the store
                                              destroys at zero */
            asset_release(&g_texgen_assets, xkey);
            o->material.albedo_tex.id = 0;
            o->material.normal_tex.id = 0;
            o->material.mr_tex.id     = 0;
            o->material.ao_tex.id     = 0;
        }
        if (o->mesh.index_count == 0) continue;
        akey = scene_meta_get(s, o->handle, "akey");
        if (akey) {                                /* a glb part: read its ticket */
            asset_release(&g_glbpart_assets, akey);
            memset(&o->mesh, 0, sizeof o->mesh);
        } else if (mesh_asset_key(o, key)) {
            asset_release(&g_mesh_assets, key);
            memset(&o->mesh, 0, sizeof o->mesh);   /* borrowed: just forget it */
        } else {
            mesh_destroy(&o->mesh);                /* owned (arrows): destroy it */
        }
    }
}

/* The ground under a world point (item 10): the highest terrain plot
   beneath it — within a step-up tolerance, so a plot floating overhead
   doesn't claim you — else the world floor at 0. terrain_height is the
   SAME function the mesh was built from, and the chain inverse handles
   placed/rotated plots; the camera's walk settle targets this + eye
   height, so the doorway-threshold glide becomes hill-climbing and
   rim-stepping for free. */
static float ground_under(AppState *st, vec3 p, sol_u32 *out_plot) {
    Scene  *s = &st->scene;
    float   best = 0.0f;
    sol_u32 i, best_plot = 0;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        float w, d, h, gy;
        vec3  local;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "terrain") != 0) continue;
        local = scene_world_to_local(s, o->handle, p);
        w = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "w");
        d = mesh_ref_param("terrain", o->mesh_params, o->mesh_param_count, "d");
        if (local.x < -w * 0.5f || local.x > w * 0.5f ||
            local.z < -d * 0.5f || local.z > d * 0.5f)
            continue;
        h  = terrain_height(o->mesh_params, o->mesh_param_count,
                            local.x, local.z);
        gy = mat4_mul_point(scene_world_matrix(s, o),
                            vec3_make(local.x, h, local.z)).y;
        if (gy <= p.y + COLLIDE_STEP_UP && gy > best) {   /* the treaty constant:
                                       what ground may claim, walls must not
                                       resist — one number, two authorities */
            best      = gy;
            best_plot = o->handle;
        }
    }
    if (g_campus.enabled) {                              /* the derived campus ground */
        float lx = p.x - g_campus.center.x;
        float lz = p.z - g_campus.center.z;
        float hw = g_campus.w * 0.5f, hd = g_campus.d * 0.5f;
        if (lx >= -hw && lx <= hw && lz >= -hd && lz <= hd) {
            float gy = campus_height(g_campus.pads, g_campus.npads, g_campus.w,
                                     g_campus.d, CAMPUS_HILL_AMP, CAMPUS_SEED, lx, lz);
            if (gy <= p.y + COLLIDE_STEP_UP && gy > best) { best = gy; best_plot = 0; }
        }
    }
    if (out_plot) *out_plot = best_plot;
    return best;
}

/* the wander component's outlet: ground_under behind a void* — creatures'
   feet ride the same ground law the camera walks by */
static float wander_ground(void *ctx, vec3 p, sol_u32 *plot) {
    return ground_under((AppState *)ctx, p, plot);
}

/* The ground at a mint point ahead of you: islands and architecture
   claim spawn heights exactly the way they claim your feet — the same
   two reads the camera's ground line makes each frame (terrain via
   ground_under, pavement/steps/stumps via collide_stand), gated by the
   step treaty at YOUR height. Flat world answers 0, so the old mints
   keep their old meaning off-island. */
static float mint_ground(AppState *st, vec3 pos) {
    vec3  probe = pos, sfeet;
    float g, gs;
    probe.y = st->camera.pos.y;            /* what you could step onto */
    g = ground_under(st, probe, (sol_u32 *)0);
    sfeet = probe;
    sfeet.y -= CAMERA_EYE_HEIGHT;
    gs = collide_stand(&st->colliders, sfeet, COLLIDE_RADIUS);
    if (gs > g) g = gs;
    return g;
}

/* ---- the floor-plan overlay (P6 item 3) ----
   The plan made visible before one stone exists: J toggles, the island
   underfoot is the plot (w, d, seed straight from its terrain ref — the
   island IS the commission), and the overlay is DERIVED geometry on the
   arrows law: never serialized, rebuilt when the standing island
   changes, drawn in the island's local frame so the plan rides it. */

static void plan_overlay_drop(AppState *st) {
    if (st->plan_mesh.vbuffer.id) mesh_destroy(&st->plan_mesh);
    memset(&st->plan_mesh, 0, sizeof st->plan_mesh);
    st->plan_plot = 0;
}

/* plan frame -> island local: church_plan works east-along-the-longer-
   dimension; a deeper-than-wide plot swaps the axes */
static void plan_to_local(const ChurchPlan *cp, float px, float pz,
                          float *lx, float *lz) {
    if (cp->swapped) { *lx = pz; *lz = px; }
    else             { *lx = px; *lz = pz; }
}

/* one chalk line: a thin flat quad on the overlay plane, up normal */
static void plan_seg(MeshBuilder *mb, const ChurchPlan *cp,
                     float x0, float z0, float x1, float z1,
                     float y, float wd) {
    float ax, az, bx, bz, dx, dz, l, px, pz;
    sol_u32 a, b2, c, d;
    plan_to_local(cp, x0, z0, &ax, &az);
    plan_to_local(cp, x1, z1, &bx, &bz);
    dx = bx - ax; dz = bz - az;
    l  = sqrtf(dx * dx + dz * dz);
    if (l < 1e-6f) return;
    px = -dz / l * 0.5f * wd; pz = dx / l * 0.5f * wd;
    a  = mb_push_vertex(mb, ax + px, y, az + pz, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    b2 = mb_push_vertex(mb, ax - px, y, az - pz, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    c  = mb_push_vertex(mb, bx - px, y, bz - pz, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f);
    d  = mb_push_vertex(mb, bx + px, y, bz + pz, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    mb_push_triangle(mb, a, b2, c);
    mb_push_triangle(mb, a, c, d);
}

static void plan_mark(MeshBuilder *mb, const ChurchPlan *cp,
                      float x, float z, float y, float s) {
    plan_seg(mb, cp, x - 0.5f * s, z, x + 0.5f * s, z, y, s);
}

static void plan_overlay_build(AppState *st, SceneObject *isl) {
    float       params[8];
    ChurchPlan  cp;
    MeshBuilder mb;
    float       datum = 0.0f, y, x, z, hwid;
    int         i, j;

    params[0] = mesh_ref_param("terrain", isl->mesh_params,
                               isl->mesh_param_count, "w");
    params[1] = mesh_ref_param("terrain", isl->mesh_params,
                               isl->mesh_param_count, "d");
    params[2] = mesh_ref_param("terrain", isl->mesh_params,
                               isl->mesh_param_count, "seed");
    params[3] = -1.0f; params[4] = 0.0f; params[5] = 1.0f;
    params[6] = 1.0f;  params[7] = 0.0f;
    church_plan(&cp, params, 8);

    /* the datum — item 9's law previewed: the building stands at the
       HIGHEST pier station's ground (it must nowhere float) */
    for (i = 0; i <= cp.nbays; i++)
        for (j = 0; j <= PIER_ROW_N_WALL; j++) {
            float lx, lz, h;
            if (!plan_pier(&cp, i, j, &x, &z)) continue;
            plan_to_local(&cp, x, z, &lx, &lz);
            h = terrain_height(isl->mesh_params, isl->mesh_param_count, lx, lz);
            if (h > datum) datum = h;
        }
    for (i = 0; i < 6; i++) {
        float lx, lz, h;
        if (!plan_apse_pier(&cp, i, &x, &z)) continue;
        plan_to_local(&cp, x, z, &lx, &lz);
        h = terrain_height(isl->mesh_params, isl->mesh_param_count, lx, lz);
        if (h > datum) datum = h;
    }
    y = datum + 0.15f;

    mb_init(&mb);
    hwid = 0.5f * cp.nave_w + cp.aisle_w;

    {   /* the usable rect: where the margin reserves buttress depth */
        float ux0 = -0.5f * cp.plot_l + cp.margin;
        float ux1 =  0.5f * cp.plot_l - cp.margin;
        float uz  =  0.5f * cp.plot_w - cp.margin;
        plan_seg(&mb, &cp, ux0, -uz, ux1, -uz, y, 0.05f);
        plan_seg(&mb, &cp, ux0,  uz, ux1,  uz, y, 0.05f);
        plan_seg(&mb, &cp, ux0, -uz, ux0,  uz, y, 0.05f);
        plan_seg(&mb, &cp, ux1, -uz, ux1,  uz, y, 0.05f);
    }
    /* the body: west front, the apse mouth chord, the long walls */
    plan_seg(&mb, &cp, cp.west_x, -hwid, cp.west_x,  hwid, y, 0.12f);
    plan_seg(&mb, &cp, cp.east_x, -hwid, cp.east_x,  hwid, y, 0.12f);
    plan_seg(&mb, &cp, cp.west_x, -hwid, cp.east_x, -hwid, y, 0.12f);
    plan_seg(&mb, &cp, cp.west_x,  hwid, cp.east_x,  hwid, y, 0.12f);
    if (cp.aisles) {                                       /* the arcades */
        plan_seg(&mb, &cp, cp.west_x + cp.tower_d, -0.5f * cp.nave_w,
                 cp.east_x, -0.5f * cp.nave_w, y, 0.07f);
        plan_seg(&mb, &cp, cp.west_x + cp.tower_d,  0.5f * cp.nave_w,
                 cp.east_x,  0.5f * cp.nave_w, y, 0.07f);
    }
    for (i = 0; i <= cp.nbays; i++) {       /* bay stations + the piers */
        if (plan_pier(&cp, i, PIER_ROW_S_WALL, &x, &z))
            plan_seg(&mb, &cp, x, -hwid, x, hwid, y, 0.04f);
        for (j = 0; j <= PIER_ROW_N_WALL; j++)
            if (plan_pier(&cp, i, j, &x, &z))
                plan_mark(&mb, &cp, x, z, y + 0.02f, 0.45f);
    }
    if (cp.apse_sides == 5) {               /* the chevet */
        float px0, pz0, px1, pz1;
        for (i = 0; i < 5; i++) {
            plan_apse_pier(&cp, i,     &px0, &pz0);
            plan_apse_pier(&cp, i + 1, &px1, &pz1);
            plan_seg(&mb, &cp, px0, pz0, px1, pz1, y, 0.12f);
            plan_mark(&mb, &cp, px0, pz0, y + 0.02f, 0.45f);
        }
        plan_apse_pier(&cp, 5, &px0, &pz0);
        plan_mark(&mb, &cp, px0, pz0, y + 0.02f, 0.45f);
    }
    for (i = 0; i < cp.nbays; i++) {        /* the windows' rhythm */
        GothicOpening o;
        plan_opening(&cp, WALL_AISLE_S, i, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW)
            plan_seg(&mb, &cp, o.cx - 0.5f * o.w, -hwid + 0.3f,
                     o.cx + 0.5f * o.w, -hwid + 0.3f, y, 0.1f);
        plan_opening(&cp, WALL_AISLE_N, i, &o);
        if (o.kind == GOTHIC_OPEN_WINDOW)
            plan_seg(&mb, &cp, o.cx - 0.5f * o.w, hwid - 0.3f,
                     o.cx + 0.5f * o.w, hwid - 0.3f, y, 0.1f);
    }
    {                                        /* the portal */
        GothicOpening o;
        plan_opening(&cp, WALL_WEST, 0, &o);
        if (o.kind == GOTHIC_OPEN_DOOR)
            plan_seg(&mb, &cp, cp.west_x + 0.3f, o.cx - 0.5f * o.w,
                     cp.west_x + 0.3f, o.cx + 0.5f * o.w, y, 0.18f);
    }
    if (cp.tower)                            /* the tower's inner wall */
        plan_seg(&mb, &cp, cp.west_x + cp.tower_d, -hwid,
                 cp.west_x + cp.tower_d, hwid, y, 0.10f);

    plan_overlay_drop(st);
    st->plan_mesh = mesh_from_builder(&mb);
    st->plan_plot = isl->handle;
    {
        const char *nm = scene_meta_get(&st->scene, isl->handle, "name");
        printf("plan: %s on %s — %d bays%s%s, nave %.1fm\n",
               cp.style == CHURCH_CHAPEL ? "a chapel" :
               cp.style == CHURCH_HALL ? "a hall church" : "a basilica",
               nm ? nm : "the island", cp.nbays,
               cp.apse_sides ? ", apsed" : "", cp.tower ? ", towered" : "",
               (double)cp.nave_w);
    }
}

/* ---- the reader (item 9 piece 3) ----
   Skyrim-shaped: the book rises from its resting place and faces you on
   the slerp arc, holding a lectern tilt just under the sightline. All of
   it is VIEW state — the rig's meshes live in AppState and die when the
   book lands; the scene only ever knows the source object's resting TRS. */

/* The codex's cover child under a group root; 0 if not a codex. */
static sol_u32 codex_cover_child(Scene *s, sol_u32 root) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->parent == root && o->mesh_ref &&
            strcmp(o->mesh_ref, "book_cover") == 0)
            return o->handle;
    }
    return 0;
}

/* mono for code, sans for prose — by extension; extensionless files
   (Makefile, .gitignore-style) read as config, so mono. */
static sol_bool reader_wants_mono(const AppState *st, const char *path) {
    static const char *code[] = { "c", "h", "sh", "py", "js", "ts", "json",
                                  "yml", "yaml", "toml", "mk", "stml",
                                  (const char *)0 };
    const char *dot, *base;
    int i;
    if (!st->mono_font || !path) return SOL_FALSE;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot  = strrchr(base, '.');
    if (!dot || dot == base) return SOL_TRUE;
    for (i = 0; code[i]; i++)
        if (strcmp(dot + 1, code[i]) == 0) return SOL_TRUE;
    return SOL_FALSE;
}

/* png/jpg/jpeg open AS a picture on the page, not as typeset text. */
static sol_bool reader_is_image_path(const char *path) {
    static const char *img[] = { "png", "jpg", "jpeg", (const char *)0 };
    const char *dot, *base;
    char        ext[8];
    int         i;
    if (!path) return SOL_FALSE;
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot  = strrchr(base, '.');
    if (!dot || dot == base) return SOL_FALSE;
    for (i = 0; dot[i + 1] != '\0' && i < (int)sizeof ext - 1; i++) {
        char c = dot[i + 1];                       /* lowercase for the compare */
        ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    ext[i] = '\0';
    for (i = 0; img[i] != (const char *)0; i++)
        if (strcmp(ext, img[i]) == 0) return SOL_TRUE;
    return SOL_FALSE;
}

static void reader_free_text(AppState *st) {
    free(st->reader_text);
    free(st->reader_line_off);
    st->reader_text       = (char *)0;
    st->reader_line_off   = (int *)0;
    st->reader_line_count = 0;
    st->reader_text_len   = 0;
    st->reader_spread     = 0;
}

static void reader_free_image(AppState *st) {
    if (st->reader_image_tex.id) rhi_destroy_texture(st->reader_image_tex);
    st->reader_image_tex.id = 0;
    mesh_destroy(&st->reader_image_quad);
    st->reader_image_w = 0;
    st->reader_image_h = 0;
    st->reader_is_image = SOL_FALSE;
}

/* Load + typeset the source's content (piece 4): read (capped — a partial
   read is honestly marked), expand tabs to spaces (text_shape skips
   unbaked glyphs, so a raw \t would advance ZERO), drop \r, wrap ONCE to
   the page field at the body scale, then index the line starts. From here
   pagination is arithmetic: a page is a line range, a spread is two. */
static void reader_load_content(AppState *st, const char *path) {
    const float *bp = st->reader_params;
    float wb, zh, mg, xf, field_w, field_h;
    long  rawlen = 0, i, n, cap;
    int   truncated = 0, line;
    char *raw, *clean;

    reader_free_text(st);
    reader_free_image(st);
    st->reader_font = reader_wants_mono(st, path) ? st->mono_font : st->ui_font;
    if (!st->reader_font) return;

    wb = bp[0] - bp[4];
    zh = bp[1] * 0.5f - bp[4];
    mg = wb * 0.06f;
    xf = wb * BOOK_GUTTER_FRAC;
    field_w = wb - xf - 2.0f * mg;
    field_h = 2.0f * zh - 2.0f * mg;
    /* IMAGE BRANCH: a picture file opens AS a picture — decode it, size the
       page quad to its aspect, and skip typesetting. Any decode/upload failure
       falls through to the text path below (which renders a graceful message). */
    if (reader_is_image_path(path)) {
        Image img;
        if (image_load(path, &img)) {
            st->reader_image_tex = rhi_create_texture(img.pixels, img.w, img.h,
                                                      RHI_TEX_SRGB8);
            st->reader_image_w = img.w;
            st->reader_image_h = img.h;
            image_free(&img);
            if (st->reader_image_tex.id) {
                float       qw, qh;
                MeshBuilder mb;
                image_fit_box(st->reader_image_w, st->reader_image_h,
                              field_w, field_h, &qw, &qh);
                mb_init(&mb);
                make_page(&mb, qw, qh);
                st->reader_image_quad = mesh_from_builder(&mb);
                mb_free(&mb);
                st->reader_is_image = SOL_TRUE;
                return;                            /* image ready; no typeset */
            }
            st->reader_image_w = 0;                /* upload failed: drop dims */
            st->reader_image_h = 0;
        }
        /* image_load failed -> fall through to the text fallback below */
    }
    st->reader_px2m = (bp[1] * 0.022f) / font_line_height(st->reader_font);
    st->reader_lines_per_page = (int)(field_h /
        (font_line_height(st->reader_font) * st->reader_px2m));
    if (st->reader_lines_per_page < 1) st->reader_lines_per_page = 1;

    raw = path ? fs_read_file(path, READER_FILE_CAP, &rawlen, &truncated)
               : (char *)0;
    if (!raw) {
        const char *ph = path ? "(the file would not open)"
                              : "(an empty codex - no file is bound to it)";
        raw = (char *)malloc(strlen(ph) + 1);
        if (!raw) return;
        strcpy(raw, ph);
        rawlen    = (long)strlen(raw);
        truncated = 0;
    }

    clean = (char *)malloc((size_t)rawlen * 4 + 64);
    if (!clean) { free(raw); return; }
    n = 0;
    for (i = 0; i < rawlen; i++) {
        char ch = raw[i];
        if (ch == '\t') {
            clean[n++] = ' '; clean[n++] = ' ';   /* tab = 2 spaces (Fran's call) */
        } else if (ch != '\r') {
            clean[n++] = ch;
        }
    }
    if (truncated) {
        const char *mark = "\n\n[... truncated ...]";
        memcpy(clean + n, mark, strlen(mark));
        n += (long)strlen(mark);
    }
    clean[n] = '\0';
    free(raw);

    cap = n * 2 + 256;                        /* wrapping only adds newlines */
    st->reader_text = (char *)malloc((size_t)cap);
    if (!st->reader_text) { free(clean); return; }
    st->reader_line_count = text_wrap(st->reader_font, clean, st->reader_px2m,
                                      field_w, st->reader_text, (int)cap);
    free(clean);
    if (st->reader_line_count <= 0) { reader_free_text(st); return; }
    st->reader_text_len = (long)strlen(st->reader_text);
    st->reader_line_off = (int *)malloc(sizeof(int) *
                                        (size_t)(st->reader_line_count + 1));
    if (!st->reader_line_off) { reader_free_text(st); return; }
    line = 0;
    st->reader_line_off[line++] = 0;
    for (i = 0; i < st->reader_text_len && line < st->reader_line_count; i++)
        if (st->reader_text[i] == '\n')
            st->reader_line_off[line++] = (int)i + 1;
    st->reader_line_count     = line;          /* trust the scan */
    st->reader_line_off[line] = (int)st->reader_text_len + 1;  /* sentinel */
}

static char *reader_strdup(const char *s) {
    size_t n = strlen(s ? s : "");
    char  *d = (char *)malloc(n + 1);
    if (d) { memcpy(d, s ? s : "", n); d[n] = '\0'; }
    return d;
}

static void reader_free_pages(AppState *st) {
    int i;
    if (st->reader_pages) {
        for (i = 0; i < st->reader_page_count; i++) free(st->reader_pages[i]);
        free(st->reader_pages);
    }
    st->reader_pages      = (char **)0;
    st->reader_page_count = 0;
    st->reader_page       = 0;
}

/* load page0..page{N-1} from the codex anchor's meta; a fresh book = 1 blank page */
static void reader_load_pages(AppState *st, sol_u32 root) {
    const char *pc = scene_meta_get(&st->scene, root, "pagecount");
    int n = pc ? atoi(pc) : 0, i;
    char key[16];
    reader_free_pages(st);
    if (n < 1) n = 1;
    if (n > 4096) n = 4096;
    st->reader_pages = (char **)malloc(sizeof(char *) * (size_t)n);
    if (!st->reader_pages) return;
    for (i = 0; i < n; i++) {
        const char *t;
        snprintf(key, sizeof key, "page%d", i);
        t = scene_meta_get(&st->scene, root, key);
        st->reader_pages[i] = reader_strdup(t ? t : "");
    }
    st->reader_page_count = n;
    st->reader_page       = 0;
}

static void reader_save_pages(AppState *st) {
    char key[16], cbuf[16];
    int  i;
    if (!st->reader_editable || !st->reader_pages || st->reader_source == 0) return;
    snprintf(cbuf, sizeof cbuf, "%d", st->reader_page_count);
    scene_meta_set(&st->scene, st->reader_source, "pagecount", cbuf);
    for (i = 0; i < st->reader_page_count; i++) {
        snprintf(key, sizeof key, "page%d", i);
        scene_meta_set(&st->scene, st->reader_source, key,
                       st->reader_pages[i] ? st->reader_pages[i] : "");
    }
}

static float reader_field_w(const AppState *st) {
    const float *bp = st->reader_params;
    float wb = bp[0] - bp[4];
    return wb - wb * BOOK_GUTTER_FRAC - 2.0f * (wb * 0.06f);
}

/* Rebuild reader_text from the page array so the EXISTING page/leaf render works:
   each page is wrapped to field_w, then capped + padded to exactly L lines and
   concatenated. The current page shows a trailing caret while open. Frees the old
   reader_text/line_off INLINE (not reader_free_text — that resets reader_spread). */
static void reader_pack_pages(AppState *st) {
    const float *bp = st->reader_params;
    float zh = bp[1] * 0.5f - bp[4];
    float field_h, field_w = reader_field_w(st);
    int   L, pg, c, line, total_lines;
    long  cap, used;
    char *out;
    int  *offs;
    if (!st->ui_font || st->reader_page_count <= 0) return;
    st->reader_font = st->ui_font;
    st->reader_px2m = (bp[1] * 0.022f) / font_line_height(st->reader_font);
    field_h = 2.0f * zh - 2.0f * (bp[0] - bp[4]) * 0.06f;
    L = (int)(field_h / (font_line_height(st->reader_font) * st->reader_px2m));
    if (L < 1) L = 1;
    st->reader_lines_per_page = L;
    total_lines = st->reader_page_count * L;
    cap  = (long)total_lines * 512L + 256L;
    out  = (char *)malloc((size_t)cap);
    offs = (int *)malloc(sizeof(int) * (size_t)(total_lines + 1));
    if (!out || !offs) { free(out); free(offs); return; }
    used = 0; line = 0;
    for (pg = 0; pg < st->reader_page_count; pg++) {
        char        wbuf[8192];
        char        caretp[8192];
        const char *src = st->reader_pages[pg] ? st->reader_pages[pg] : "";
        const char *p;
        int         nlines;
        if (pg == st->reader_page && st->reader_editable &&
            st->reader_state != READER_RETURNING) {
            size_t sl = strlen(src);
            if (sl > sizeof(caretp) - 2) sl = sizeof(caretp) - 2;
            memcpy(caretp, src, sl); caretp[sl] = '_'; caretp[sl + 1] = '\0';
            src = caretp;
        }
        nlines = text_wrap(st->reader_font, src, st->reader_px2m, field_w,
                           wbuf, (int)sizeof wbuf);
        if (nlines < 1) { wbuf[0] = '\0'; }
        p = wbuf;
        for (c = 0; c < L; c++) {
            offs[line++] = (int)used;
            if (c < nlines) {
                while (*p && *p != '\n' && used < cap - 2) out[used++] = *p++;
                if (*p == '\n') p++;
            }
            out[used++] = '\n';
        }
    }
    out[used] = '\0';
    offs[line] = (int)used + 1;        /* sentinel */
    free(st->reader_text);             /* inline free: keep reader_spread */
    free(st->reader_line_off);
    st->reader_text       = out;
    st->reader_text_len   = used;
    st->reader_line_off   = offs;
    st->reader_line_count = total_lines;
}

static sol_bool reader_is_editing(AppState *st) {
    return (sol_bool)(st->reader_editable && st->reader_state == READER_OPEN);
}

/* would the current page wrap to more than L lines? (capacity guard) */
static int reader_page_full(AppState *st, const char *text) {
    char wbuf[8192];
    int  lines = text_wrap(st->reader_font, text, st->reader_px2m,
                           reader_field_w(st), wbuf, (int)sizeof wbuf);
    return lines > st->reader_lines_per_page;
}

static void reader_page_append(AppState *st, const char *enc, int n) {
    char  *pg, *grown;
    size_t len;
    if (!st->reader_pages || st->reader_page >= st->reader_page_count) return;
    pg  = st->reader_pages[st->reader_page];
    len = pg ? strlen(pg) : 0;
    grown = (char *)malloc(len + (size_t)n + 1);
    if (!grown) return;
    if (pg) memcpy(grown, pg, len);
    memcpy(grown + len, enc, (size_t)n);
    grown[len + n] = '\0';
    if (reader_page_full(st, grown)) { free(grown); return; }  /* page is full */
    free(pg);
    st->reader_pages[st->reader_page] = grown;
    reader_pack_pages(st);
}

static void reader_page_backspace(AppState *st) {
    char  *pg;
    size_t len;
    if (!st->reader_pages || st->reader_page >= st->reader_page_count) return;
    pg  = st->reader_pages[st->reader_page];
    len = pg ? strlen(pg) : 0;
    if (len == 0) return;
    len--;
    while (len > 0 && ((unsigned char)pg[len] & 0xC0u) == 0x80u) len--;
    pg[len] = '\0';
    reader_pack_pages(st);
}

static void reader_page_newline(AppState *st) {
    char nl = '\n';
    reader_page_append(st, &nl, 1);            /* respects capacity */
}

/* single-page navigation while editing: move the caret one page; flipping past
   the last page appends a blank page; play the leaf turn only when the SPREAD
   changes (so left<->right within a spread just moves the caret). */
static void reader_edit_flip(AppState *st, int dir) {
    int old_spread = st->reader_spread;
    if (st->reader_turning != 0) return;       /* one leaf in flight */
    if (dir > 0) {
        if (st->reader_page + 1 >= st->reader_page_count) {
            char **grown = (char **)realloc(st->reader_pages,
                sizeof(char *) * (size_t)(st->reader_page_count + 1));
            if (!grown) return;
            st->reader_pages = grown;
            st->reader_pages[st->reader_page_count] = reader_strdup("");
            st->reader_page_count++;
        }
        st->reader_page++;
    } else {
        if (st->reader_page == 0) return;
        st->reader_page--;
    }
    st->reader_spread = st->reader_page / 2;
    if (st->reader_spread != old_spread) {
        st->reader_turn_old = old_spread;
        st->reader_turning  = (st->reader_spread > old_spread) ? 1 : -1;
        st->reader_turn_t   = 0.0f;
        play_oneshot(g_snd_whoosh, g_snd_whoosh_frames, 0.30f, 0.0f);
    }
    reader_pack_pages(st);
}

/* The open book's flat page-plane transform: wtext_block draws ink on its z=0
   plane (x right, y up, page-local meters). Shared by the page render and the
   app-book cursor hit-test. Optionally returns the page rect metrics. */
static mat4 reader_page_matrix(const AppState *st, float *out_wb, float *out_zh,
                               float *out_xf, float *out_mg) {
    const float *bp    = st->reader_params;
    float        wb    = bp[0] - bp[4];
    float        zh    = bp[1] * 0.5f - bp[4];
    float        stack = (bp[2] - 2.0f * bp[3]) * 0.5f;
    float        xf, fy, mg;
    mat4         bm, page;
    if (stack < 0.004f) stack = 0.004f;
    xf = wb * BOOK_GUTTER_FRAC;
    fy = bp[3] + stack + 0.0012f;
    mg = wb * 0.06f;
    bm = mat4_from_trs(st->reader_pos, st->reader_rot, vec3_make(1.0f, 1.0f, 1.0f));
    page = mat4_mul(bm, mat4_mul(mat4_translate(vec3_make(0.0f, fy, 0.0f)),
               quat_to_mat4(quat_from_axis_angle(
                   vec3_make(1.0f, 0.0f, 0.0f), sol_radians(-90.0f)))));
    if (out_wb) *out_wb = wb;
    if (out_zh) *out_zh = zh;
    if (out_xf) *out_xf = xf;
    if (out_mg) *out_mg = mg;
    return page;
}

/* load the open synth book's patch: start from the "blip" preset, then apply
   the curated-knob overrides stored in meta["synth"] (space-separated floats in
   curated-knob order). Absent/short meta -> the bare preset. */
static void synth_book_load(AppState *st, sol_u32 root) {
    const float *pre = synth_preset("blip");
    const char  *m;
    int          i, k = app_synth_knob_count();
    for (i = 0; i < SYNTH_PARAMS; i++)
        st->synth_params[i] = pre ? pre[i] : 0.0f;
    m = scene_meta_get(&st->scene, root, "synth");
    if (m) {
        /* the format string and v[] are sized to app_synth_knob_count()==4; update both if curated knobs are added */
        float v[8];
        int   got = sscanf(m, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
        if (got == k)
            for (i = 0; i < k; i++)
                st->synth_params[app_synth_knob_param(i)] = v[i];
    }
    /* Knuth multiplicative-hash constant: mixes root -> a per-book rng seed */
    st->synth_rng = 0x9E3779B9u ^ (sol_u32)root;
}

/* serialize the curated knobs back into meta["synth"] and save the scene. */
static void synth_book_save(AppState *st, sol_u32 root) {
    char buf[128];
    int  i, n = 0, k = app_synth_knob_count();
    for (i = 0; i < k; i++) {
        if (n >= (int)sizeof buf) break;
        n += snprintf(buf + n, sizeof buf - (size_t)n, (i ? " %.4f" : "%.4f"),
                      (double)st->synth_params[app_synth_knob_param(i)]);
    }
    scene_meta_set(&st->scene, root, "synth", buf);
    scene_save(&st->scene, "scene.stml");
}

/* a pick ray through the OS cursor (app-book mode frees the cursor; the normal
   pick_ray only reads the cursor in ORBIT, so the app book needs its own). */
static Ray cursor_ray(AppState *st, GLFWwindow *w) {
    int    ww, wh;
    float  aspect, nx, ny;
    double mx, my;
    glfwGetWindowSize(w, &ww, &wh);
    aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
    glfwGetCursorPos(w, &mx, &my);
    nx = (ww > 0) ? 2.0f * (float)mx / (float)ww - 1.0f : 0.0f;
    ny = (wh > 0) ? 1.0f - 2.0f * (float)my / (float)wh : 0.0f;
    return camera_ray(&st->camera, nx, ny, aspect);
}

/* cursor -> the open page's z=0 plane -> page-local 2D meters. `in` (the return)
   is true when the hit lands within the book's page rect [-wb,wb] x [-zh,zh]. */
static sol_bool page_under_cursor(AppState *st, GLFWwindow *w, mat4 page,
                                  float wb, float zh, float *px, float *py) {
    Ray   r;
    vec3  o, zp, n, hit, loc;
    float t;
    mat4  inv;
    r   = cursor_ray(st, w);
    o   = mat4_mul_point(page, vec3_make(0.0f, 0.0f, 0.0f));
    zp  = mat4_mul_point(page, vec3_make(0.0f, 0.0f, 1.0f));
    n   = vec3_normalize(vec3_sub(zp, o));
    if (!ray_vs_plane(r, o, n, &t)) return SOL_FALSE;
    hit = vec3_add(r.origin, vec3_scale(r.dir, t));
    inv = mat4_inverse(page);
    loc = mat4_mul_point(inv, hit);
    *px = loc.x;
    *py = loc.y;
    return (sol_bool)(loc.x >= -wb && loc.x <= wb &&
                      loc.y >= -zh && loc.y <= zh);
}

/* an app book is OPEN and routes to the widget UI. */
static sol_bool reader_is_app(const AppState *st) {
    return (sol_bool)(st->reader_app != 0 && st->reader_state == READER_OPEN);
}

static void widget_quad_build(AppState *st) {
    MeshBuilder mb;
    mb_init(&mb);
    make_page(&mb, 1.0f, 1.0f);          /* centered unit XY quad, z=0 */
    st->widget_quad = mesh_from_builder(&mb);
    mb_free(&mb);
}

/* a fixed audition seed (deterministic) + a comfortable preview level */
#define SYNTH_PLAY_SEED 1u
#define SYNTH_PLAY_GAIN 0.6f

static void synth_book_play(AppState *st) {
    /* play_oneshot REFERENCES the buffer (it does not copy — see the sound-bank
       lifetime note), so each play needs a buffer that outlives its voice. There
       are (MIX_VOICES - VOICE_ONESHOT_BASE) oneshot voices; a ring that size
       guarantees a slot's previous voice has been recycled before we reuse it. */
    static float buf[MIX_VOICES - VOICE_ONESHOT_BASE][SYNTH_RATE];
    static int   slot = 0;
    int          n;
    n = synth_render(st->synth_params, SYNTH_PLAY_SEED, buf[slot], SYNTH_RATE);
    if (n > 0) play_oneshot(buf[slot], n, SYNTH_PLAY_GAIN, 0.0f);
    slot = (slot + 1) % (MIX_VOICES - VOICE_ONESHOT_BASE);
}

/* run the widget UI for the open synth book: hit-test the cursor against the
   page, lay out the page, service Sound/Roll. The emitted draw-list lives in
   st->widget_ctx for the render pass (Task 7) to walk. */
static void synth_book_input(AppState *st, GLFWwindow *w, sol_bool lmb) {
    mat4        page;
    float       wb, zh, xf, mg, px = 0.0f, py = 0.0f;
    sol_bool    in;
    SynthAction act;
    if (st->widget_quad.index_count == 0) widget_quad_build(st);
    page = reader_page_matrix(st, &wb, &zh, &xf, &mg);
    in   = page_under_cursor(st, w, page, wb, zh, &px, &py);
    widget_begin(&st->widget_ctx, px, py, in, lmb);
    act = app_synth_page(&st->widget_ctx, st->synth_params,
                         xf + mg, zh - mg,
                         (wb - mg) - (xf + mg), 2.0f * (zh - mg));
    widget_end(&st->widget_ctx);
    if (act == SYNTH_ACT_PLAY) {
        synth_book_play(st);
    } else if (act == SYNTH_ACT_ROLL) {
        app_synth_roll(st->synth_params, &st->synth_rng);
        synth_book_play(st);
    }
}

/* the camera pose that frames a board head-on (defined with the board-view
   machinery below; forward-declared so the reader can re-frame the board when a
   book opened from board view closes) */
static int board_view_pose(AppState *st, sol_u32 board, CameraPose *out);

/* Open `handle` as a book. A codex rises as ITSELF (its own build and
   leather, read from the cover child's params — the open refs share the
   closed schema's prefix); a FILE/ALIAS card rises as the default codex:
   every file opens as a book. Anything else is a no-op. */
static void reader_open(AppState *st, sol_u32 handle) {
    Scene       *s = &st->scene;
    SceneObject *o = scene_get(s, handle);
    sol_u32      root, cover;
    const char *const *names;
    const float       *defs;
    MeshBuilder  mb;
    vec3         f;
    float        dist;
    int          k;
    char         lbuf[16];

    if (!o || st->reader_state != READER_IDLE) return;
    root  = (o->kind == KIND_PLAIN) ? group_root(s, handle) : handle;
    cover = codex_cover_child(s, root);
    if (cover == 0 && o->kind != KIND_FILE && o->kind != KIND_ALIAS) return;

    if (mesh_ref_schema("book_open_block", &names, &defs) != 5) return;
    for (k = 0; k < 5; k++) st->reader_params[k] = defs[k];
    if (cover != 0) {
        SceneObject *co = scene_get(s, cover);
        for (k = 0; k < 5; k++)
            st->reader_params[k] = mesh_ref_param("book_cover",
                co->mesh_params, co->mesh_param_count, names[k]);
        st->reader_cover_mat = co->material;
    } else {
        st->reader_cover_mat = material_default();
        st->reader_cover_mat.base_color = vec3_make(0.36f, 0.22f, 0.13f);
        st->reader_cover_mat.roughness  = 0.65f;
    }
    st->reader_block_mat = material_default();
    st->reader_block_mat.base_color = vec3_make(0.90f, 0.86f, 0.74f);
    st->reader_block_mat.roughness  = 0.92f;

    mesh_destroy(&st->reader_cover);
    mesh_destroy(&st->reader_block);
    mb_init(&mb);
    if (mesh_ref_build("book_open_cover", st->reader_params, 5, &mb))
        st->reader_cover = mesh_from_builder(&mb);
    mb_free(&mb);
    mb_init(&mb);
    if (mesh_ref_build("book_open_block", st->reader_params, 5, &mb))
        st->reader_block = mesh_from_builder(&mb);
    mb_free(&mb);

    /* poses: rest = the source's own; read = held on the lectern tilt a
       touch below the sightline, far enough to frame the whole spread */
    st->reader_rest_pos = object_world_pos(s, root);
    st->reader_rest_rot = scene_world_rotation(s, root);
    f = camera_forward(&st->camera);
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f = vec3_normalize(f);
    dist = 0.45f + st->reader_params[0] * 0.9f;
    st->reader_b_pos = vec3_add(st->camera.pos, vec3_scale(f, dist));
    st->reader_b_pos.y -= 0.22f;
    st->reader_b_rot = quat_mul(
        quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), atan2f(-f.x, -f.z)),
        quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), sol_radians(68.0f)));
    st->reader_a_pos  = st->reader_rest_pos;
    st->reader_a_rot  = st->reader_rest_rot;
    st->reader_pos    = st->reader_a_pos;
    st->reader_rot    = st->reader_a_rot;
    st->reader_source = root;
    st->reader_state  = READER_RISING;
    st->reader_t      = 0.0f;
    if (cover != 0) {                          /* a codex: an editable notebook */
        vec3  cd  = vec3_sub(st->reader_b_pos, st->camera.pos);
        float clen = (float)sqrt((double)vec3_dot(cd, cd));
        st->reader_editable = SOL_TRUE;
        reader_load_pages(st, root);
        reader_pack_pages(st);
        /* swing the camera to centre the held book (the book is placed along the
           LEVEL forward, so a look up/down would leave it off-screen) */
        st->reader_cam_yaw0   = st->camera.yaw;
        st->reader_cam_pitch0 = st->camera.pitch;
        st->reader_cam_yaw1   = st->camera.yaw;
        st->reader_cam_pitch1 = st->camera.pitch;
        if (clen > 1e-4f) {
            vec3 dir = vec3_scale(cd, 1.0f / clen);
            st->reader_cam_yaw1   = (float)atan2((double)dir.z, (double)dir.x);
            st->reader_cam_pitch1 = (float)asin((double)dir.y);
        }
    } else {
        st->reader_editable = SOL_FALSE;
        reader_load_content(st, o->content);   /* a file/alias card: read-only */
    }
    {
        const char *app = scene_meta_get(s, root, "app");
        st->reader_app = (app && strcmp(app, "synth") == 0) ? 1 : 0;
        if (st->reader_app) {
            st->reader_editable = SOL_FALSE;   /* an app book types nothing */
            synth_book_load(st, root);
            memset(&st->widget_ctx, 0, sizeof st->widget_ctx);   /* no stale active_id across sessions */
        }
    }
    st->reader_page   = 0;
    st->reader_spread = 0;
    /* In board view the camera is pinned to the board's frame, so the reader's
       own swing (reader_update, gated to walk/fly) never runs. Drive the
       board-view glide instead: rotate in place to centre the held book. The
       matching re-frame-the-board glide fires from reader_close. */
    if (st->board_view != 0) {
        vec3  cd   = vec3_sub(st->reader_b_pos, st->camera.pos);
        float clen = (float)sqrt((double)vec3_dot(cd, cd));
        st->bv_from_pos   = st->bv_to_pos   = st->camera.pos;   /* rotate, don't move */
        st->bv_from_yaw   = st->bv_to_yaw   = st->camera.yaw;
        st->bv_from_pitch = st->bv_to_pitch = st->camera.pitch;
        if (clen > 1e-4f) {
            vec3 dir = vec3_scale(cd, 1.0f / clen);
            st->bv_to_yaw   = (float)atan2((double)dir.z, (double)dir.x);
            st->bv_to_pitch = (float)asin((double)dir.y);
        }
        st->bv_t   = 0.0f;
        st->bv_dir = 1.0f;
    }
    printf("reading '%s' — Esc or click to put it back; left/right turn pages\n",
           object_label(s, root, lbuf));
}

static void reader_build_leaf(AppState *st, float alpha, float lag);  /* below */

/* Send the book home — from wherever it is, even mid-rise. */
static void reader_close(AppState *st) {
    if (st->reader_state == READER_IDLE || st->reader_state == READER_RETURNING)
        return;
    /* if the book was opened from board view, glide the camera back onto the
       board frame as the book flies home (mirror of reader_open's focus glide) */
    if (st->board_view != 0) {
        CameraPose p;
        if (board_view_pose(st, st->board_view, &p)) {
            st->bv_from_pos   = st->camera.pos;   st->bv_to_pos   = p.pos;
            st->bv_from_yaw   = st->camera.yaw;   st->bv_to_yaw   = p.yaw;
            st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = p.pitch;
            st->bv_t   = 0.0f;
            st->bv_dir = 1.0f;
        }
    }
    if (st->reader_app) {
        synth_book_save(st, st->reader_source);
        reader_free_pages(st);   /* pages were loaded as a codex, but reader_editable=FALSE skips the normal free */
        st->reader_app = 0;
    }
    if (st->reader_editable) {
        reader_save_pages(st);
        scene_save(&st->scene, "scene.stml");
        reader_free_pages(st);
        st->reader_editable = SOL_FALSE;
    }
    st->reader_a_pos = st->reader_pos;
    st->reader_a_rot = st->reader_rot;
    st->reader_b_pos = st->reader_rest_pos;
    st->reader_b_rot = st->reader_rest_rot;
    st->reader_t     = 0.0f;
    st->reader_state = READER_RETURNING;
    st->reader_turning = 0;                    /* a turn dies with the close */
    mesh_destroy(&st->reader_leaf);
}

/* Per-frame: ease along the current arc (position lerp, rotation slerp,
   both under one smoothstep so they arrive together), and sweep the
   turning leaf while the book is open. */
static void reader_update(AppState *st, float dt) {
    float s;
    if (st->reader_state == READER_OPEN && st->reader_turning != 0) {
        st->reader_turn_t += dt / READER_TURN_SECS;
        if (st->reader_turn_t >= 1.0f) {
            st->reader_turning = 0;
            mesh_destroy(&st->reader_leaf);
        } else {
            float e     = sol_smoothstep(st->reader_turn_t);
            float alpha = (st->reader_turning > 0) ? SOL_PI * e
                                                   : SOL_PI * (1.0f - e);
            float lag   = READER_LAG_MAX * sinf(SOL_PI * e)
                        * (float)st->reader_turning;
            reader_build_leaf(st, alpha, lag);
        }
    }
    if (st->reader_state == READER_IDLE || st->reader_state == READER_OPEN)
        return;
    st->reader_t += dt / READER_RISE_SECS;
    if (st->reader_t > 1.0f) st->reader_t = 1.0f;
    s = sol_smoothstep(st->reader_t);
    st->reader_pos = vec3_add(st->reader_a_pos,
                     vec3_scale(vec3_sub(st->reader_b_pos, st->reader_a_pos), s));
    st->reader_rot = quat_slerp(st->reader_a_rot, st->reader_b_rot, s);
    /* an editable OR app book swings the look to centre as it rises (runs
       AFTER camera_update, so it overrides this frame's mouse-look) */
    if (st->reader_state == READER_RISING &&
        (st->reader_editable || st->reader_app) && st->board_view == 0 &&
        (st->camera.mode == CAMERA_WALK || st->camera.mode == CAMERA_FLY)) {
        float yd = st->reader_cam_yaw1 - st->reader_cam_yaw0;
        while (yd >  SOL_PI) yd -= 2.0f * SOL_PI;
        while (yd < -SOL_PI) yd += 2.0f * SOL_PI;
        st->camera.yaw   = st->reader_cam_yaw0 + yd * s;
        st->camera.pitch = st->reader_cam_pitch0 +
                           (st->reader_cam_pitch1 - st->reader_cam_pitch0) * s;
    }
    if (st->reader_t >= 1.0f) {
        if (st->reader_state == READER_RISING) {
            st->reader_state = READER_OPEN;
        } else {                                   /* landed: the rig dies */
            play_oneshot(g_snd_thump, g_snd_thump_frames, 0.45f, 0.0f);
            st->reader_state   = READER_IDLE;
            st->reader_source  = 0;
            st->reader_turning = 0;
            mesh_destroy(&st->reader_cover);
            mesh_destroy(&st->reader_block);
            mesh_destroy(&st->reader_leaf);
            reader_free_text(st);
            reader_free_image(st);
        }
    }
}

/* The turning leaf at hinge angle `alpha` with tip-lag `lag` (radians;
   sign = travel direction). Paper is INEXTENSIBLE, so the flying shape is
   a constant-curvature arc preserving arclength: the tangent angle
   marches psi(s) = alpha - kappa*s from the spine outward, position is
   its integral — at kappa -> 0 it collapses to the rigid hinge. The leaf
   is SEWN at the gutter pinch, and near either end of the sweep its shape
   BLENDS into the resting page-fan profile (right page at alpha=0, the
   mirrored left at alpha=pi), so it peels off and lands seamlessly
   instead of popping in at field height (Fran's catch). Both faces are
   emitted a paper-thickness apart. Book-local coordinates. */
#define LEAF_SEG 16

/* the leaf section at arclength s -> book-local (x, y): blended arc + rest */
static void leaf_eval(const LeafShape *L, float s, float *ou, float *ov) {
    float ua, va, vr;
    if (L->kappa > -1e-4f && L->kappa < 1e-4f) {     /* rigid hinge limit */
        ua = s * cosf(L->alpha);
        va = s * sinf(L->alpha);
    } else {
        ua = (sinf(L->alpha) - sinf(L->alpha - L->kappa * s)) / L->kappa;
        va = (cosf(L->alpha - L->kappa * s) - cosf(L->alpha)) / L->kappa;
    }
    vr  = L->rise * sol_smoothstep(s / L->xf);       /* the resting profile */
    *ou = L->wA * ua + (L->wR - L->wL) * s;
    *ov = L->pinch + L->wA * va + (L->wR + L->wL) * vr;
}

/* WtextBend over the leaf: positive text-x rides the recto (right-page
   layout); NEGATIVE x mirrors through the spine for the verso (left-page
   layout) — and the mirrored parameterization flips the tangent, which
   flips the normal, which puts the lift on the verso's own side. Output
   frame: the hinge page matrix's (x, z) = book x, height above pinch. */
static void leaf_bend(float x, void *user, float *bx, float *bz,
                      float *tx, float *tz) {
    const LeafShape *L = (const LeafShape *)user;
    float s = (x < 0.0f) ? -x : x;
    float u0, v0, u1, v1, dx, dz, nl;
    leaf_eval(L, s, &u0, &v0);
    leaf_eval(L, s + 0.002f, &u1, &v1);
    *bx = u0;
    *bz = v0 - L->pinch;
    dx = u1 - u0;
    dz = v1 - v0;
    if (x < 0.0f) { dx = -dx; dz = -dz; }
    nl = sqrtf(dx * dx + dz * dz);
    if (nl < 1e-9f) { *tx = 1.0f; *tz = 0.0f; return; }
    *tx = dx / nl;
    *tz = dz / nl;
}

static void reader_build_leaf(AppState *st, float alpha, float lag) {
    const float *bp = st->reader_params;
    LeafShape   *L  = &st->reader_leaf_shape;
    float wb    = bp[0] - bp[4];
    float zh    = bp[1] * 0.5f - bp[4];
    float stack = (bp[2] - 2.0f * bp[3]) * 0.5f;
    float blend;
    float u[LEAF_SEG + 1], v[LEAF_SEG + 1];
    int   i;
    MeshBuilder mb;

    if (stack < 0.004f) stack = 0.004f;
    L->alpha = alpha;
    L->kappa = lag / wb;
    L->pinch = bp[3] + stack * 0.25f + 0.0012f;  /* the sewing line + a hair */
    L->rise  = stack * 0.75f;                    /* pinch -> flat field */
    L->xf    = wb * BOOK_GUTTER_FRAC;
    L->wb    = wb;

    blend = SOL_PI * 0.22f;                      /* the peel/land window */
    L->wR = 1.0f - alpha / blend;
    if (L->wR < 0.0f) L->wR = 0.0f;
    L->wL = 1.0f - (SOL_PI - alpha) / blend;
    if (L->wL < 0.0f) L->wL = 0.0f;
    L->wA = 1.0f - L->wR - L->wL;

    for (i = 0; i <= LEAF_SEG; i++) {
        float q = (float)i / (float)LEAF_SEG;
        float s = wb * (0.45f * q + 0.55f * q * q);   /* denser at the spine */
        leaf_eval(L, s, &u[i], &v[i]);
    }

    mesh_destroy(&st->reader_leaf);
    mb_init(&mb);
    for (i = 0; i < LEAF_SEG; i++) {
        float du = u[i + 1] - u[i], dv = v[i + 1] - v[i];
        float nl = sqrtf(du * du + dv * dv);
        float nx = (nl > 1e-9f) ? -dv / nl : 0.0f;   /* left of travel: the
                                                        recto side at alpha=0 */
        float ny = (nl > 1e-9f) ?  du / nl : 1.0f;
        float off = 0.0005f;                         /* half paper thickness */
        float u0  = (float)i / (float)LEAF_SEG;
        float u1  = (float)(i + 1) / (float)LEAF_SEG;
        sol_u32 a, b2, c, d;
        /* recto */
        a  = mb_push_vertex(&mb, u[i]   + nx * off, v[i]   + ny * off, -zh,
                            nx, ny, 0.0f, u0, 0.0f);
        b2 = mb_push_vertex(&mb, u[i]   + nx * off, v[i]   + ny * off,  zh,
                            nx, ny, 0.0f, u0, 1.0f);
        c  = mb_push_vertex(&mb, u[i+1] + nx * off, v[i+1] + ny * off,  zh,
                            nx, ny, 0.0f, u1, 1.0f);
        d  = mb_push_vertex(&mb, u[i+1] + nx * off, v[i+1] + ny * off, -zh,
                            nx, ny, 0.0f, u1, 0.0f);
        mb_push_triangle(&mb, a, b2, c);
        mb_push_triangle(&mb, a, c, d);
        /* verso */
        a  = mb_push_vertex(&mb, u[i]   - nx * off, v[i]   - ny * off,  zh,
                            -nx, -ny, 0.0f, u0, 1.0f);
        b2 = mb_push_vertex(&mb, u[i]   - nx * off, v[i]   - ny * off, -zh,
                            -nx, -ny, 0.0f, u0, 0.0f);
        c  = mb_push_vertex(&mb, u[i+1] - nx * off, v[i+1] - ny * off, -zh,
                            -nx, -ny, 0.0f, u1, 0.0f);
        d  = mb_push_vertex(&mb, u[i+1] - nx * off, v[i+1] - ny * off,  zh,
                            -nx, -ny, 0.0f, u1, 1.0f);
        mb_push_triangle(&mb, a, b2, c);
        mb_push_triangle(&mb, a, c, d);
    }
    st->reader_leaf = mesh_from_builder(&mb);
    mb_free(&mb);
}

/* One page of the spread: lines [page*L, page*L+L), drawn by temporarily
   terminating the wrapped buffer at the range end (single-threaded; the
   sentinel offset makes the last page uniform). Already wrapped — wtext
   gets wrap_w 0. */
static void reader_draw_page(AppState *st, mat4 vp, mat4 page_m, int page,
                             float x_left, float top_y) {
    int  L = st->reader_lines_per_page;
    int  first = page * L, last, a, b2;
    char saved;
    if (!st->reader_text || first >= st->reader_line_count) return;
    last = first + L;
    if (last > st->reader_line_count) last = st->reader_line_count;
    a  = st->reader_line_off[first];
    b2 = st->reader_line_off[last] - 1;
    saved = st->reader_text[b2];
    st->reader_text[b2] = '\0';
    wtext_block(st->reader_font, vp, page_m, st->reader_text + a,
                x_left, top_y, st->reader_px2m, 0.0f, 0.13f, 0.10f, 0.08f);
    st->reader_text[b2] = saved;
}

/* the same page range, bent over the turning leaf (the ink rides the
   paper: leaf_bend evaluates the SAME section the mesh was built from) */
static void reader_draw_page_bent(AppState *st, mat4 vp, mat4 hinge_m,
                                  int page, float x_left, float top_y,
                                  float lift) {
    int  L = st->reader_lines_per_page;
    int  first = page * L, last, a, b2;
    char saved;
    if (!st->reader_text || first >= st->reader_line_count) return;
    last = first + L;
    if (last > st->reader_line_count) last = st->reader_line_count;
    a  = st->reader_line_off[first];
    b2 = st->reader_line_off[last] - 1;
    saved = st->reader_text[b2];
    st->reader_text[b2] = '\0';
    wtext_block_bent(st->reader_font, vp, hinge_m, st->reader_text + a,
                     x_left, top_y, st->reader_px2m,
                     leaf_bend, &st->reader_leaf_shape, lift,
                     0.13f, 0.10f, 0.08f);
    st->reader_text[b2] = saved;
}

/* ---- Command registry (palette spec) ----------------------------------------
   One row per discrete, edge-triggered command. read_input() polls each row's
   key and the palette dispatches the same run() — one author, two consumers. */

static void cmd_toggle_bloom(AppState *st) {
    st->bloom_on = !st->bloom_on;
    printf("bloom %s\n", st->bloom_on ? "on" : "off");
}

static void cmd_toggle_mute(AppState *st) {
    (void)st;
    g_muted = !g_muted;
    audio_set_muted(g_muted);
    printf("audio %s\n", g_muted ? "muted" : "unmuted");
}

/* F toggles walk/fly in first person (precondition: not in orbit) */
static sol_bool can_toggle_fly(AppState *st) {
    return st->camera.mode != CAMERA_ORBIT;
}
static void cmd_toggle_fly(AppState *st) {
    st->camera.mode = (st->camera.mode == CAMERA_WALK) ? CAMERA_FLY : CAMERA_WALK;
}

/* X toggles ghost — debug no-clip (P4 item 1; fly collides by decision,
   so building and inspection need an explicit way through walls) */
static void cmd_toggle_ghost(AppState *st) {
    st->ghost = !st->ghost;
    printf("ghost %s\n", st->ghost ? "ON — collision off"
                                   : "off — the world pushes back");
}

/* M toggles the shadow-map inspector (item 9b) */
static void cmd_toggle_shadowmap(AppState *st) {
    st->show_shadow_map = !st->show_shadow_map;
}

/* ` (backtick) flips day <-> night: swaps the sky/IBL + the sun (P8 item 9) */
static void cmd_toggle_daynight(AppState *st) {
    st->night = !st->night;
    apply_time_of_day(st);
}

/* 9 cycles the color grade preset (P9 item 1): neutral(off) -> warm -> cool -> dramatic */
static void cmd_cycle_grade(AppState *st) {
    st->grade_mode = (st->grade_mode + 1) % GRADE_PRESET_COUNT;
    printf("grade: %s\n", GRADE_PRESET_NAMES[st->grade_mode]);
}

/* I toggles the skybox source: env cubemap vs irradiance map (B2) */
static sol_bool can_toggle_irradiance(AppState *st) {
    return st->irradiance_cubemap.id != 0;
}
static void cmd_toggle_irradiance(AppState *st) {
    st->show_irradiance = !st->show_irradiance;
}

/* P cycles the prefilter inspector: off -> roughness 0 -> .. -> 1 -> off (C1) */
static sol_bool can_cycle_prefilter(AppState *st) {
    return st->prefilter_cubemap.id != 0;
}
static void cmd_cycle_prefilter(AppState *st) {
    if (!st->show_prefilter) { st->show_prefilter = SOL_TRUE; st->prefilter_mip = 0; }
    else {
        st->prefilter_mip++;
        if (st->prefilter_mip >= PREFILTER_MIPS) st->show_prefilter = SOL_FALSE;
    }
}

/* T cycles the text inspectors (P3 item 3): off -> SDF atlas -> specimen */
static sol_bool can_cycle_textinspect(AppState *st) {
    return st->ui_font != NULL;
}
static void cmd_cycle_textinspect(AppState *st) {
    st->text_inspect = (st->text_inspect + 1) % 3;
}

/* J toggles the floor-plan overlay (drops the overlay mesh when it goes off) */
static void cmd_toggle_floorplan(AppState *st) {
    st->plan_on = !st->plan_on;
    if (!st->plan_on) plan_overlay_drop(st);
}

/* R reconciles every mirror to its disk (item 6c): new files tray up unplaced,
   vanished files tombstone, returned files resurrect. Changes are real -> saved. */
static void cmd_rescan_mirrors(AppState *st) {
    int total = rescan_mirrors(st);
    if (total > 0) scene_save(&st->scene, "scene.stml");
    printf("rescan: %d change(s)\n", total);
}

/* L reverts to the palace on disk mid-session (the manual "reload my save" key) */
static void cmd_reload_scene(AppState *st) {
    if (load_palace(st))
        printf("reloaded scene.stml: %u objects\n", (unsigned)st->scene.count);
    else
        fprintf(stderr, "scene.stml did not load — keeping the live scene\n");
}

/* B spawns a whiteboard facing you (item 8): plain furniture that
   persists like everything else; drag ALIAS/NOTE cards onto its face. */
static void cmd_mint_whiteboard(AppState *st) {
    Mesh    empty;
    vec3    f = camera_forward(&st->camera);
    vec3    pos, one;
    float   yaw;
    sol_u32 h;
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f   = vec3_normalize(f);
    pos = vec3_add(st->camera.pos, vec3_scale(f, 2.2f));
    pos.y = mint_ground(st, pos) + 0.9f;  /* bottom-origin: face
                                      center ~1.5 above YOUR ground —
                                      islands lift the board too */
    yaw = atan2f(-f.x, -f.z);      /* board +Z looks back at you */
    one = vec3_make(1.0f, 1.0f, 1.0f);
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos,
                  quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw), one);
    scene_mesh_ref_set(&st->scene, h, "board");
    scene_meta_set(&st->scene, h, "name", "board");
    scene_meta_set(&st->scene, h, "workspace",
                   st->scene.active_ws[0] ? st->scene.active_ws : "home");
    scene_resolve_meshes(&st->scene);
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("board #%u spawned — drag cards onto it\n", (unsigned)h);
}

/* V mints a CODEX in front of you (item 9): a procedural bound book —
   cover + page block as a small GROUP, each part wearing its own
   material. Proportions draw from real binding ranges and PERSIST
   (they live in the parts' mesh attrs); press it a few times for a
   shelf of individuals. */
static void cmd_mint_codex(AppState *st) {
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    float   p[8];
    vec3    f, pos;
    float   yaw;
    sol_u32 anchor, part;
    int     leather;
    if (g_mint_rng == 0) g_mint_rng = (unsigned)time((time_t *)0) | 1u;
    /* 2x real codex sizes — true-scale books read tiny in the world
       (Fran's call after the first shelf) */
    p[1] = mint_range(0.36f, 0.68f);             /* h: octavo..folio   */
    p[0] = p[1] / mint_range(1.35f, 1.60f);      /* w from real ratios */
    p[2] = mint_range(0.05f, 0.15f);             /* t (leaf count)     */
    p[3] = mint_range(0.010f, 0.018f);           /* board thickness    */
    p[4] = mint_range(0.006f, 0.012f);           /* squares            */
    p[5] = mint_range(0.40f, 1.00f);             /* spine roundness    */
    p[6] = (float)(int)mint_range(3.0f, 5.99f);  /* raised bands       */
    p[7] = (mint_range(0.0f, 1.0f) < 0.30f) ? 1.0f : 0.0f;   /* clasp */
    leather = (int)mint_range(0.0f, 2.99f);
    f = camera_forward(&st->camera);
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f   = vec3_normalize(f);
    pos = vec3_add(st->camera.pos, vec3_scale(f, 1.6f));
    pos.y = mint_ground(st, pos)
          + p[1] * 0.5f;                  /* standing: height spans +-h/2 */
    yaw = atan2f(-f.z, f.x);              /* the spine (-x) faces you */
    memset(&empty, 0, sizeof empty);
    anchor = scene_add(&st->scene, 0, empty, pos,
                       quat_mul(quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw),
                                quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f),
                                                     sol_radians(-90.0f))),
                       one);
    scene_meta_set(&st->scene, anchor, "name", "codex");
    scene_meta_set(&st->scene, anchor, "workspace",
                   st->scene.active_ws[0] ? st->scene.active_ws : "home");
    part = scene_add(&st->scene, anchor, empty,
                     vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
    scene_mesh_ref_set(&st->scene, part, "book_cover");
    scene_mesh_params_set(&st->scene, part, p, 8);
    {
        SceneObject *po = scene_get(&st->scene, part);
        if (po) {
            Material m = material_default();
            m.base_color = (leather == 0) ? vec3_make(0.36f, 0.22f, 0.13f)
                         : (leather == 1) ? vec3_make(0.34f, 0.12f, 0.10f)
                                          : vec3_make(0.14f, 0.22f, 0.15f);
            m.roughness = 0.65f;
            po->material = m;
        }
    }
    part = scene_add(&st->scene, anchor, empty,
                     vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
    scene_mesh_ref_set(&st->scene, part, "book_block");
    scene_mesh_params_set(&st->scene, part, p, 5);
    {
        SceneObject *po = scene_get(&st->scene, part);
        if (po) {
            Material m = material_default();
            m.base_color = vec3_make(0.88f, 0.84f, 0.72f);   /* cream leaves */
            m.roughness  = 0.92f;
            po->material = m;
        }
    }
    scene_resolve_meshes(&st->scene);
    st->selected_handle = anchor;
    scene_save(&st->scene, "scene.stml");
    printf("codex minted: %.0fx%.0fmm, %d bands, %s spine%s\n",
           (double)(p[0] * 1000.0f), (double)(p[1] * 1000.0f), (int)p[6],
           p[5] > 0.7f ? "round" : "shallow",
           p[7] > 0.5f ? ", clasped" : "");
}

/* ---- board pages: folder books (Board Pages Task 4) ---- */

/* Is this object a folder book pinned to a board? */
static sol_bool object_is_folder(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (o && o->mesh_ref && strcmp(o->mesh_ref, "folderbook") == 0)
               ? SOL_TRUE : SOL_FALSE;
}

/* A board card that drag-to-file can move between pages: notes and pictures. */
static sol_bool is_fileable_card(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    if (!o) return SOL_FALSE;
    if (o->kind == KIND_NOTE) return SOL_TRUE;
    if (o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0) return SOL_TRUE;
    return SOL_FALSE;
}

/* A board-child note/picture/folder is multi-selectable. */
static sol_bool object_is_selectable(Scene *s, sol_u32 h) {
    SceneObject *o   = scene_get(s, h);
    SceneObject *par;
    if (!o || o->parent == 0) return SOL_FALSE;
    par = scene_get(s, o->parent);
    if (!par || !par->mesh_ref || strcmp(par->mesh_ref, "board") != 0) return SOL_FALSE;
    if (o->kind == KIND_NOTE) return SOL_TRUE;
    if (o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0)    return SOL_TRUE;
    if (o->mesh_ref && strcmp(o->mesh_ref, "folderbook") == 0) return SOL_TRUE;
    return SOL_FALSE;
}

/* selection-set wrappers that keep selected_handle (the anchor) in sync:
   sel_count==0 -> selected_handle==0; sel_count==1 -> selected_handle==sel[0]. */
static void sel_clear(AppState *st) {
    st->sel_count = 0;
    st->selected_handle = 0;
}
static void sel_set_single(AppState *st, sol_u32 h) {
    st->sel[0] = h;
    st->sel_count = 1;
    st->selected_handle = h;
}
static void sel_toggle_h(AppState *st, sol_u32 h) {
    sol_bool now;
    /* at cap and not already present: can't add -> leave the set/anchor alone */
    if (st->sel_count >= MULTISEL_CAP &&
        !msel_contains(st->sel, st->sel_count, h))
        return;
    now = msel_toggle(st->sel, &st->sel_count, MULTISEL_CAP, h);
    st->selected_handle = now ? h
                        : (st->sel_count ? st->sel[st->sel_count - 1] : 0);
}

/* Tag a board-pinned card with its board's CURRENT page, so a card created or
   dropped while viewing a sub-page belongs to that page, not the root "/".
   No-op unless `handle` is a direct child of a "board". */
static void board_card_tag_page(AppState *st, sol_u32 handle) {
    SceneObject *o = scene_get(&st->scene, handle);
    SceneObject *par;
    const char  *ap;
    if (!o || o->parent == 0) return;
    par = scene_get(&st->scene, o->parent);
    if (!par || !par->mesh_ref || strcmp(par->mesh_ref, "board") != 0) return;
    ap = scene_meta_get(&st->scene, o->parent, "active_page");
    scene_meta_set(&st->scene, handle, "page", ap ? ap : "/");
}

/* The folder pinned on `board` nearest board-local point `bl`, within a
   small radius; 0 if none. Used by the drag-to-file drop test. */
static sol_u32 folder_at_board_point(AppState *st, sol_u32 board, vec3 bl) {
    sol_u32 i, best = 0;
    float   bestd = 1e30f;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float fw, fh, cx, cy, dx, dy, mx, my, dd;
        if (o->parent != board) continue;
        if (!object_is_folder(&st->scene, o->handle)) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;  /* on this page */
        /* the book footprint: x is centered on pos.x, y spans pos.y..pos.y+h
           (bottom-origin mesh). Test against its visual CENTER + a small margin
           so the whole book is a drop target (and it scales with the book). */
        fw = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "w");
        fh = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "h");
        cx = o->pos.x;
        cy = o->pos.y + fh * 0.5f;
        dx = bl.x - cx;
        dy = bl.y - cy;
        mx = fw * 0.5f + 0.04f;
        my = fh * 0.5f + 0.04f;
        if (dx < -mx || dx > mx || dy < -my || dy > my) continue;  /* outside the book */
        dd = dx * dx + dy * dy;                 /* nearest center among overlaps */
        if (dd < bestd) { bestd = dd; best = o->handle; }
    }
    return best;
}

/* Gather the board's navigable page list into out[cap][PAGE_SLUG_CAP].
   Prefers the ordered "pages" meta (creation order); falls back to
   emergent child-tag collection (natural-sorted) for un-migrated boards. */
static int board_pages(AppState *st, sol_u32 board,
                       char out[][PAGE_SLUG_CAP], int cap) {
    const char *stored = scene_meta_get(&st->scene, board, "pages");
    const char *active = scene_meta_get(&st->scene, board, "active_page");
    const char *raw[BOARD_PAGE_MAX];
    int         n = 0;
    sol_u32     i;
    if (stored && stored[0])
        return boardpage_list(stored, active, out, cap); /* ordered, creation order */
    /* un-migrated / page-less board: emergent collection, natural-sorted */
    for (i = 0; i < st->scene.count && n < BOARD_PAGE_MAX; i++) {
        SceneObject *o = &st->scene.objects[i];
        const char  *pg;
        if (o->parent != board) continue;
        pg = scene_meta_get(&st->scene, o->handle, "page");
        if (pg) raw[n++] = pg;
    }
    return boardpage_collect(raw, n, active, out, cap);
}

/* Seed "pages" for any board lacking it, from its emergent page list (natural-
   sorted), so empty-page persistence becomes real for pre-feature boards. No
   save here -- the next scene_save persists it; re-running is idempotent. */
static void boards_migrate_pages(AppState *st) {
    Scene  *s = &st->scene;
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        char  pages[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
        char  buf[BOARD_PAGE_MAX * PAGE_SLUG_CAP];
        int   np;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) continue;
        if (scene_meta_get(s, o->handle, "pages")) continue; /* already migrated */
        np = board_pages(st, o->handle, pages, BOARD_PAGE_MAX); /* emergent, natural-sorted */
        if (np <= 1) continue;     /* only "/" -> nothing to store */
        boardpage_serialize(pages, np, buf, (int)sizeof buf);
        if (buf[0]) scene_meta_set(s, o->handle, "pages", buf);
    }
}

/* Append slug to the board's ordered "pages" meta if not already listed.
   "/" is implicit and never stored. */
static void board_page_register(AppState *st, sol_u32 board, const char *slug) {
    const char *cur;
    char        buf[BOARD_PAGE_MAX * PAGE_SLUG_CAP];
    if (!slug || !slug[0] || strcmp(slug, "/") == 0) return;
    cur = scene_meta_get(&st->scene, board, "pages");
    if (cur && boardpage_contains(cur, slug)) return;
    if (cur && cur[0]) snprintf(buf, sizeof buf, "%s %s", cur, slug);
    else               snprintf(buf, sizeof buf, "%s", slug);
    scene_meta_set(&st->scene, board, "pages", buf);
}

/* Step the focused board's active_page through its page list by `dir`
   (+1 next, -1 prev), wrapping. '/' is always reachable. */
static void cycle_page(AppState *st, int dir) {
    sol_u32     board = st->board_view;
    char        pages[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    int         np, i, cur, ni;
    const char *active;
    cur = 0;
    if (board == 0) return;
    np = board_pages(st, board, pages, BOARD_PAGE_MAX);
    if (np <= 1) return;
    active = scene_meta_get(&st->scene, board, "active_page");
    if (!active) active = "/";
    for (i = 0; i < np; i++)
        if (strcmp(pages[i], active) == 0) { cur = i; break; }
    ni = (cur + dir + np) % np;
    scene_meta_set(&st->scene, board, "active_page", pages[ni]);
    scene_save(&st->scene, "scene.stml");
}

/* Shift+Right in board view: create a fresh page (/page-N, the next free
   number) and navigate to it. If a selection exists, MOVE it onto the new page
   (each card keeps its board-local position) and stay selected. The page is
   registered in the board's ordered "pages" meta, so it persists in creation
   order even while empty. */
static void board_new_page(AppState *st) {
    sol_u32 board = st->board_view;
    char    pages[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    char    slug[PAGE_SLUG_CAP];
    int     np, i, n;
    if (board == 0) return;
    np = board_pages(st, board, pages, BOARD_PAGE_MAX);
    for (n = 1; ; n++) {                          /* smallest free /page-N */
        sol_bool taken = SOL_FALSE;
        snprintf(slug, sizeof slug, "/page-%d", n);
        for (i = 0; i < np; i++)
            if (strcmp(pages[i], slug) == 0) { taken = SOL_TRUE; break; }
        if (!taken) break;
    }
    board_page_register(st, board, slug);          /* persist the page in creation order */
    for (i = 0; i < st->sel_count; i++)           /* move the selection, layout kept */
        scene_meta_set(&st->scene, st->sel[i], "page", slug);
    scene_meta_set(&st->scene, board, "active_page", slug);
    scene_save(&st->scene, "scene.stml");
    printf("new page %s%s\n", slug, st->sel_count ? " (moved selection)" : "");
}

/* Does the target page already carry a folder linking back to `link`? */
static sol_bool folder_backlink_exists(AppState *st, sol_u32 board,
                                       const char *page, const char *link) {
    sol_u32 i;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        const char  *op;
        const char  *ol;
        if (o->parent != board) continue;
        if (!object_is_folder(&st->scene, o->handle)) continue;
        op = scene_meta_get(&st->scene, o->handle, "page");
        ol = scene_meta_get(&st->scene, o->handle, "link");
        if (op && ol && strcmp(op, page) == 0 && strcmp(ol, link) == 0)
            return SOL_TRUE;
    }
    return SOL_FALSE;
}

/* Board-local point from fractions of the board's own dimensions:
   fx in [-0.5, 0.5] maps to [-w/2, w/2], fy in [0,1] maps to [0,h]. */
static vec3 board_local_frac(AppState *st, sol_u32 board, float fx, float fy) {
    SceneObject *o  = scene_get(&st->scene, board);
    float        bw = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w") : 1.8f;
    float        bh = o ? mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h") : 1.2f;
    return vec3_make(fx * bw, fy * bh, 0.0f);
}

/* Add a randomized folder book to `board` on `page`, linking to `link`,
   pinned at board-local hit point `blocal`. Returns the handle. */
static sol_u32 add_folder(AppState *st, sol_u32 board, const char *page,
                           const char *link, vec3 blocal) {
    Mesh         empty;
    vec3         one = vec3_make(1.0f, 1.0f, 1.0f);
    float        p[4];
    float        blue;
    sol_u32      h;
    SceneObject *o;
    if (g_mint_rng == 0) g_mint_rng = (unsigned)time((time_t *)0) | 1u;
    p[0] = mint_range(0.33f, 0.48f);             /* w (3x scale) */
    p[1] = mint_range(0.51f, 0.78f);             /* h (3x scale) */
    p[2] = mint_range(0.12f, 0.21f);             /* d (3x scale) */
    p[3] = (float)(int)mint_range(3.0f, 5.99f);  /* bands */
    blue = mint_range(0.0f, 1.0f);               /* shade: deep navy .. brighter blue */
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, board, empty, vec3_make(0.0f, 0.0f, 0.0f),
                  quat_identity(), one);
    scene_kind_set(&st->scene, h, KIND_PLAIN);
    scene_mesh_ref_set(&st->scene, h, "folderbook");
    scene_mesh_params_set(&st->scene, h, p, 4);
    scene_meta_set(&st->scene, h, "page", page);
    scene_meta_set(&st->scene, h, "link", link);
    scene_resolve_meshes(&st->scene);
    /* re-fetch by handle: scene_add above may have realloced the objects array */
    o = scene_get(&st->scene, h);
    if (o) {
        Material m = material_default();
        m.base_color = vec3_make(0.08f + 0.14f * blue,   /* always blue, */
                                 0.16f + 0.22f * blue,   /* varied shade  */
                                 0.42f + 0.34f * blue);
        m.roughness = 0.6f;
        o->material = m;
        o->pos = board_pin_pos(&st->scene, board, h, blocal,
                               0.0f, -0.5f * p[1]);  /* center on the point */
    }
    folderbook_materialize(&st->scene);   /* dress the new folder in leather (keeps its blue) */
    return h;
}

/* palette_prompt callback: slugify the name, create a forward folder on the
   current page and an idempotent backlink folder on the target page. */
static void create_folder_from_name(AppState *st, const char *typed) {
    sol_u32     board = st->board_view;
    char        target[PAGE_SLUG_CAP];
    const char *src_raw;
    char        src_buf[PAGE_SLUG_CAP];
    vec3        fwd_local;
    vec3        back_local;
    char        pages[BOARD_PAGE_MAX][PAGE_SLUG_CAP];
    int         np;
    int         i;
    sol_bool    exists = SOL_FALSE;
    if (board == 0) return;
    if (boardpage_slugify(typed, target, sizeof target) <= 1) {
        /* slug collapsed to "/": a deliberate "/" links to the ROOT page;
           an empty / no-slug-chars entry cancels */
        const char *tr = typed ? typed : "";
        while (*tr == ' ' || *tr == '\t') tr++;
        if (tr[0] != '/') return;          /* cancel */
        /* else target is already "/" -> fall through as a root link */
    }
    src_raw = scene_meta_get(&st->scene, board, "active_page");
    if (!src_raw) src_raw = "/";
    /* copy src before any scene mutation: the raw pointer aliases the board's
       meta storage, which could be freed if active_page were ever re-set mid-flight */
    strncpy(src_buf, src_raw, PAGE_SLUG_CAP - 1);
    src_buf[PAGE_SLUG_CAP - 1] = '\0';
    if (strcmp(target, src_buf) == 0) {    /* self-link: ignore */
        printf("folder: already on %s\n", target);
        return;
    }
    np = board_pages(st, board, pages, BOARD_PAGE_MAX);
    for (i = 0; i < np; i++)
        if (strcmp(pages[i], target) == 0) { exists = SOL_TRUE; break; }
    fwd_local  = st->folder_place_has ? st->folder_place_local
                                      : board_local_frac(st, board, 0.0f, 0.6f);
    back_local = board_local_frac(st, board, -0.32f, 0.85f);  /* top-left */
    /* forward folder on the current page */
    st->selected_handle = add_folder(st, board, src_buf, target, fwd_local);
    /* backlink on the target page -- idempotent */
    if (!folder_backlink_exists(st, board, target, src_buf))
        (void)add_folder(st, board, target, src_buf, back_local);
    st->folder_place_has = SOL_FALSE;
    board_page_register(st, board, target);   /* a folder-linked page persists + orders */
    scene_save(&st->scene, "scene.stml");
    printf("folder %s -> %s%s\n", src_buf, target,
           exists ? " (link to existing)" : " (new page)");
}

/* A synth book (TODO5): an ordinary codex tagged as an app. Reuses the whole
   codex mint (cover + page block + workspace tag), then stamps meta["app"] so
   the reader routes it to the widget UI. Unknown apps degrade to plain books. */
static void cmd_mint_synth(AppState *st) {
    sol_u32 h;
    cmd_mint_codex(st);                 /* mints a codex, sets selected_handle, saves */
    h = st->selected_handle;
    if (h != 0) {
        scene_meta_set(&st->scene, h, "name", "synth book");
        scene_meta_set(&st->scene, h, "app", "synth");
        scene_save(&st->scene, "scene.stml");
        printf("synth book minted - read it (look at it, press R) to open the synth\n");
    }
}

/* tag a freshly minted root into the current workspace (absent => the base
   "home", matching every other prop mint) so scenery lands in the world you
   are standing in, not the default one. */
static void mint_tag_ws(AppState *st, sol_u32 h) {
    if (h != 0)
        scene_meta_set(&st->scene, h, "workspace",
                       st->scene.active_ws[0] ? st->scene.active_ws : "home");
}

/* ---- cut & paste board cards (Cmd+X / Cmd+V) ---------------------------
   A cut MARKS a set of board-card handles without removing them; paste MOVES
   them onto the board you're viewing, on its active page. The cut buffer is
   GLOBAL (not cleared on board_view_exit) so a paste can cross boards. */

static sol_bool handle_is_cut(const AppState *st, sol_u32 h) {
    int i;
    for (i = 0; i < st->cut_count; i++)
        if (st->cut[i] == h) return SOL_TRUE;
    return SOL_FALSE;
}

/* Move one board card onto (board, page), preserving its board-local layout.
   Same board: just retag the page (position is already board-local). Cross
   board: re-parent, re-pin onto the new face (keep local x/y, recompute the
   pin z from the new board's thickness), retag the page, and re-tag workspace
   so it joins the world you're viewing. NOTE: scene_meta_set may realloc the
   object array — set o->parent/o->pos FIRST and never deref o afterwards. */
static void card_move_to_page(AppState *st, sol_u32 handle,
                              sol_u32 board, const char *page) {
    SceneObject *o = scene_get(&st->scene, handle);
    if (!o || board == 0) return;
    if (o->parent != board) {                /* cross-board: re-parent + re-pin + re-world */
        vec3 local = vec3_make(o->pos.x, o->pos.y, 0.0f);
        o->parent = board;
        o->pos    = board_pin_pos(&st->scene, board, handle, local, 0.0f, 0.0f);
        scene_meta_set(&st->scene, handle, "page", page);   /* may realloc; o now stale */
        mint_tag_ws(st, handle);                            /* inherit the target board's world */
        return;
    }
    scene_meta_set(&st->scene, handle, "page", page);       /* same board: just retag the page */
}

/* Cmd+V when a cut is pending: move every cut card to the viewed board's
   active page, consume the cut (cards un-dim), deselect, and persist. */
static void cmd_paste_cut(AppState *st) {
    sol_u32     board = st->board_view;
    char        page[PAGE_SLUG_CAP];
    const char *ap;
    int         i, n;
    if (board == 0 || st->cut_count == 0) return;
    ap = scene_meta_get(&st->scene, board, "active_page");
    /* copy the slug out before any scene_meta_set realloc invalidates the meta ptr */
    snprintf(page, sizeof page, "%s", (ap && ap[0]) ? ap : "/");
    n = st->cut_count;
    for (i = 0; i < n; i++)
        card_move_to_page(st, st->cut[i], board, page);
    st->cut_count = 0;                        /* consume the cut: cards un-dim */
    sel_clear(st);                            /* nothing selected after a paste */
    scene_save(&st->scene, "scene.stml");
    printf("pasted %d card(s) to %s\n", n, page);
}

/* Cmd+X: copy the selection into the cut buffer (cards stay, render dimmed).
   With an EMPTY selection it clears the cut — the explicit "cancel" gesture. */
static void cmd_cut_selection(AppState *st) {
    int i;
    if (st->board_view == 0) return;
    if (st->sel_count == 0) {                 /* cut nothing = cancel the pending cut */
        st->cut_count = 0;
        printf("cut cleared\n");
        return;
    }
    for (i = 0; i < st->sel_count; i++) st->cut[i] = st->sel[i];
    st->cut_count = st->sel_count;
    printf("cut %d card(s)\n", st->cut_count);
    sel_clear(st);    /* deselect: the cut now lives on the clipboard (cards stay
                         dimmed via handle_is_cut), so LEFT/RIGHT navigate pages
                         freely to the paste target and no stale resize handles
                         ghost-render over a page the cut card no longer sits on */
}

/* palette wrappers + availability predicates */
static void cmd_paste_cards(AppState *st) { cmd_paste_cut(st); }

static sol_bool can_cut_selection(AppState *st) {
    return (sol_bool)(st->board_view != 0 && st->sel_count > 0);
}
static sol_bool can_paste_cards(AppState *st) {
    return (sol_bool)(st->board_view != 0 && st->cut_count > 0);
}

/* H mints a terrain ISLAND ahead of you, at your floor level (item 10):
   press it while flying and the island FLOATS there — vertical
   placement for free. room_type meta makes it LAND (architecture is
   never draggable); the seed makes it THIS island forever. */
/* toggle the active world's persistent campus flag, then re-derive. */
static void cmd_toggle_campus(AppState *st) {
    const char *wsname = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    sol_u32     anchor = workspace_anchor_add(&st->scene, wsname);   /* find-or-create */
    const char *cur    = scene_meta_get(&st->scene, anchor, "campus");
    int         on     = !(cur && strcmp(cur, "1") == 0);
    scene_meta_set(&st->scene, anchor, "campus", on ? "1" : "0");
    scene_save(&st->scene, "scene.stml");
    world_rebuild(st);                                  /* re-derive incl. campus_rebuild */
    printf("campus %s for world '%s'\n", on ? "on" : "off", wsname);
}

static void cmd_mint_island(AppState *st) {
    static const char *isle[] = { "the heath", "the tor", "the fell",
                                  "the moor", "the downs", "the crag" };
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    float   p[5];
    vec3    f, pos;
    sol_u32 h;
    int     ni;
    if (g_mint_rng == 0) g_mint_rng = (unsigned)time((time_t *)0) | 1u;
    p[0] = mint_range(24.0f, 44.0f);             /* w */
    p[1] = mint_range(24.0f, 44.0f);             /* d */
    p[2] = 56.0f;                                /* sub */
    p[3] = mint_range(1.5f, 3.5f);               /* relief */
    p[4] = (float)(int)mint_range(1.0f, 9999.0f);/* the identity */
    ni   = (int)mint_range(0.0f, 5.99f);
    f = camera_forward(&st->camera);
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f   = vec3_normalize(f);
    pos = vec3_add(st->camera.pos, vec3_scale(f, p[0] * 0.5f + 6.0f));
    pos.y = st->camera.pos.y - CAMERA_EYE_HEIGHT;  /* your floor level */
    if (pos.y < 0.05f) pos.y = 0.0f;               /* grounded: exactly */
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos, quat_identity(), one);
    scene_mesh_ref_set(&st->scene, h, "terrain");
    scene_mesh_params_set(&st->scene, h, p, 5);
    scene_meta_set(&st->scene, h, "name", isle[ni]);
    scene_meta_set(&st->scene, h, "room_type", "terrain");
    mint_tag_ws(st, h);                          /* land in the active world */
    {
        SceneObject *to = scene_get(&st->scene, h);
        if (to) {
            Material m = material_default();
            m.base_color = vec3_make(0.35f, 0.40f, 0.28f);  /* the shader
                                               palette overrides this */
            m.roughness  = 0.95f;
            to->material = m;
        }
    }
    /* the WILD ISLAND dressing (P7 item 10): a hero tree, a couple
       of erratics, and a pond IF the island has a real hollow —
       so a minted island feels inhabited without a church (the
       forest, flowers, scree and wind grow themselves). Children
       of the island, in its LOCAL frame. */
    {
        float lowest = 1e30f, lowx = 0.0f, lowz = 0.0f, rim = -1e30f;
        int   gx, gz;
#define WILD_PLANT(ref, lx, lz, prm, np, texkind, nm)                     \
        do {                                                       \
            float wy_ = terrain_height(p, 5, (lx), (lz));          \
            sol_u32 w_ = scene_add(&st->scene, h, empty,           \
                          vec3_make((lx), wy_, (lz)),              \
                          quat_identity(), one);                   \
            scene_mesh_ref_set(&st->scene, w_, (ref));             \
            if ((np) > 0)                                          \
                scene_mesh_params_set(&st->scene, w_, (prm), (np));\
            scene_meta_set(&st->scene, w_, "name", (nm));          \
            {   SceneObject *wo_ = scene_get(&st->scene, w_);      \
                if (wo_) { Material wm_ = material_default();      \
                    wm_.base_color = vec3_make(1.0f, 1.0f, 1.0f);  \
                    wm_.roughness = 1.0f; wm_.metallic = 1.0f;     \
                    wo_->material = wm_; } }                       \
            scene_tex_ref_set(&st->scene, w_, (texkind));          \
        } while (0)
        /* scan for the lowest spot (the hollow) and the rim */
        for (gz = -3; gz <= 3; gz++)
            for (gx = -3; gx <= 3; gx++) {
                float sx = (float)gx / 3.0f * 0.4f * p[0];
                float sz = (float)gz / 3.0f * 0.4f * p[1];
                float gh = terrain_height(p, 5, sx, sz);
                if (gh < lowest) { lowest = gh; lowx = sx; lowz = sz; }
                if (gh > rim) rim = gh;
            }
        {   /* a lone tree off-center, species by seed */
            static const char *sp4[4] = { "oak", "pine", "birch", "cypress" };
            float tp[3];
            tp[0] = p[4] + 17.0f; tp[1] = 1.1f; tp[2] = 7.0f;
            WILD_PLANT(sp4[(int)p[4] & 3], -0.18f * p[0], 0.16f * p[1],
                       tp, 3, "bark", "the lone tree");
        }
        {   /* two erratics */
            float bp[3];
            bp[0] = 0.9f; bp[1] = p[4] + 3.0f; bp[2] = 0.0f;
            WILD_PLANT("boulder", 0.22f * p[0], -0.20f * p[1], bp, 3,
                       "stone", "erratic");
            bp[0] = 0.6f; bp[1] = p[4] + 9.0f; bp[2] = 0.6f;
            WILD_PLANT("boulder", 0.30f * p[0], -0.10f * p[1], bp, 3,
                       "stone", "erratic");
        }
        if (rim - lowest > 1.2f) {   /* a real dip → a pond */
            float pp[3];
            sol_u32 pondh = scene_add(&st->scene, h, empty,
                          vec3_make(lowx, lowest + 0.2f, lowz),
                          quat_identity(), one);
            scene_mesh_ref_set(&st->scene, pondh, "pond");
            pp[0] = 3.5f; pp[1] = 1.5f; pp[2] = p[4];
            scene_mesh_params_set(&st->scene, pondh, pp, 3);
            scene_meta_set(&st->scene, pondh, "name", "tarn");
        }
#undef WILD_PLANT
    }

    scene_resolve_meshes(&st->scene);
    meadow_rebuild(st);                  /* a new island, new grass */
    forest_rebuild(st);                  /* and its forest */
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("%s rises: %.0fx%.0fm, relief %.1fm, seed %d%s\n",
           isle[ni], (double)p[0], (double)p[1], (double)p[3],
           (int)p[4], pos.y > 0.05f ? " (floating)" : "");
}

/* U mints THE CHURCH on the island underfoot (P6 item 9): the
   J-overlay's commission, built. The datum is the MAX of the pier
   samples (ruled: the building must nowhere float); the group is
   one anchor + four members sharing the island's identity, yawed
   a quarter-turn when the plot is deeper than wide. room_type
   makes it architecture — never draggable. */
static sol_bool can_mint_church(AppState *st) {
    return st->current_terrain != 0;
}
static void cmd_mint_church(AppState *st) {
    SceneObject *isl = scene_get(&st->scene, st->current_terrain);
    sol_u32 had = 0;
    if (isl && isl->nid) {     /* a church here already? then U
                                  is the RUIN DIAL: walk the
                                  ladder 0 -> .3 -> .6 -> .9 -> 0 */
        sol_u32 ai;
        for (ai = 0; ai < st->scene.count && !had; ai++) {
            SceneObject *a = &st->scene.objects[ai];
            const char *rt = scene_meta_get(&st->scene, a->handle,
                                            "room_type");
            const char *pl = scene_meta_get(&st->scene, a->handle,
                                            "plot");
            if (rt && strcmp(rt, "church") == 0 &&
                pl && strcmp(pl, isl->nid) == 0)
                had = a->handle;
        }
    }
    if (had) {
        float r = 0.0f, nr;
        sol_u32 ci;
        for (ci = 0; ci < st->scene.count; ci++) {
            SceneObject *c = &st->scene.objects[ci];
            if (c->parent != had || !c->mesh_ref) continue;
            if (c->mesh_param_count >= 5) r = c->mesh_params[4];
            break;
        }
        nr = r < 0.15f ? 0.3f : r < 0.45f ? 0.6f :
             r < 0.75f ? 0.9f : 0.0f;
        for (ci = 0; ci < st->scene.count; ci++) {
            SceneObject *c = &st->scene.objects[ci];
            float np[5];
            char  okey[160];
            if (c->parent != had || !c->mesh_ref) continue;
            if (strncmp(c->mesh_ref, "church_", 7) != 0) continue;
            /* params are the mesh's IDENTITY (P4 item 4): give
               the old shape back to the registry BEFORE the
               params change, or resolve will see a mesh already
               present and keep the old stones standing */
            if (c->mesh.index_count != 0 &&
                mesh_asset_key(c, okey)) {
                asset_release(&g_mesh_assets, okey);
                memset(&c->mesh, 0, sizeof c->mesh);
            }
            np[0] = c->mesh_params[0];
            np[1] = c->mesh_params[1];
            np[2] = c->mesh_params[2];
            np[3] = -1.0f;            /* style: still derived  */
            np[4] = nr;
            scene_mesh_params_set(&st->scene, c->handle, np, 5);
        }
        scene_resolve_meshes(&st->scene);
        collide_rebuild(&st->colliders, &st->scene);
        scene_save(&st->scene, "scene.stml");
        printf(nr > 0.0f
                   ? "the church decays: ruin %.1f\n"
                   : "the church stands whole again\n", nr);
    } else if (isl && isl->mesh_ref &&
        strcmp(isl->mesh_ref, "terrain") == 0) {
        float cw = mesh_ref_param("terrain", isl->mesh_params,
                                  isl->mesh_param_count, "w");
        float cd = mesh_ref_param("terrain", isl->mesh_params,
                                  isl->mesh_param_count, "d");
        float cseed = mesh_ref_param("terrain", isl->mesh_params,
                                     isl->mesh_param_count, "seed");
        float params[3];
        ChurchPlan cp;
        float datum = 0.0f, x, z;
        int   i2, j2;
        Mesh  empty;
        vec3  one = vec3_make(1.0f, 1.0f, 1.0f);
        quat  rot;
        sol_u32 anchor;
        params[0] = cw; params[1] = cd; params[2] = cseed;
        church_plan(&cp, params, 3);
        for (i2 = 0; i2 <= cp.nbays; i2++)        /* the datum */
            for (j2 = 0; j2 <= PIER_ROW_N_WALL; j2++) {
                float lx, lz, hgt;
                if (!plan_pier(&cp, i2, j2, &x, &z)) continue;
                plan_to_local(&cp, x, z, &lx, &lz);
                hgt = terrain_height(isl->mesh_params,
                                     isl->mesh_param_count, lx, lz);
                if (hgt > datum) datum = hgt;
            }
        for (i2 = 0; i2 < 6; i2++) {
            float lx, lz, hgt;
            if (!plan_apse_pier(&cp, i2, &x, &z)) continue;
            plan_to_local(&cp, x, z, &lx, &lz);
            hgt = terrain_height(isl->mesh_params,
                                 isl->mesh_param_count, lx, lz);
            if (hgt > datum) datum = hgt;
        }
        rot = cp.swapped
            ? quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f),
                                   -0.5f * (float)SOL_PI)
            : quat_identity();
        memset(&empty, 0, sizeof empty);
        anchor = scene_add(&st->scene, 0, empty,
                           vec3_add(isl->pos,
                                    vec3_make(0.0f, datum, 0.0f)),
                           rot, one);
        scene_meta_set(&st->scene, anchor, "room_type", "church");
        mint_tag_ws(st, anchor);                 /* land in the active world */
        if (isl->nid)            /* the dial finds it next press */
            scene_meta_set(&st->scene, anchor, "plot", isl->nid);
        {
            const char *nm = scene_meta_get(&st->scene, isl->handle,
                                            "name");
            char title[96];
            snprintf(title, sizeof title, "the church of %s",
                     nm ? nm : "the island");
            scene_meta_set(&st->scene, anchor, "name", title);
        }
        {
            static const char *refs[5] = {
                "church_stone", "church_glass",
                "church_roof",  "church_floor",
                "church_decals"
            };
            int r2;
            for (r2 = 0; r2 < 5; r2++) {
                sol_u32 ch = scene_add(&st->scene, anchor, empty,
                                       vec3_make(0, 0, 0),
                                       quat_identity(), one);
                scene_mesh_ref_set(&st->scene, ch, refs[r2]);
                scene_mesh_params_set(&st->scene, ch, params, 3);
                {
                    SceneObject *co = scene_get(&st->scene, ch);
                    if (co) {
                        Material mm = material_default();
                        if (r2 == 0) {
                            /* synthesized stone (texture side-quest):
                               the maps carry tone, the factors step
                               aside (the shader multiplies them in) */
                            mm.base_color = vec3_make(1.0f, 1.0f, 1.0f);
                            mm.roughness  = 1.0f;
                            mm.metallic   = 1.0f;
                        } else if (r2 == 1) {
                            mm.base_color = vec3_make(0.02f, 0.025f, 0.045f);
                            mm.roughness  = 0.08f;
                            /* past 1.0 on purpose: that's what bloom
                               bites on — windows radiate, not paint */
                            mm.emissive   = vec3_make(1.8f, 1.12f, 0.5f);
                        } else if (r2 == 2) {
                            mm.base_color = vec3_make(0.30f, 0.32f, 0.36f);
                            mm.roughness  = 0.55f;
                        } else {
                            mm.base_color = vec3_make(1.0f, 1.0f, 1.0f);
                            mm.roughness  = 1.0f;
                            mm.metallic   = 1.0f;
                        }
                        co->material = mm;
                    }
                    if (r2 == 0) {
                        scene_tex_ref_set(&st->scene, ch, "stone");
                    } else if (r2 == 3) {
                        /* the floor wears flagstones: its own seed,
                           paver-scale courses (knobs 0..3) */
                        float fp[4];
                        fp[0] = cseed + 7.0f;
                        fp[1] = 2.4f; fp[2] = 0.8f; fp[3] = 0.8f;
                        scene_tex_ref_set(&st->scene, ch, "stone");
                        scene_tex_params_set(&st->scene, ch, fp, 4);
                    }
                }
            }
        }
        scene_resolve_meshes(&st->scene);
        collide_rebuild(&st->colliders, &st->scene);
        scene_save(&st->scene, "scene.stml");
        printf("a church rises on the island (datum %.2f%s)\n",
               datum, cp.swapped ? ", turned to the long axis" : "");
    }
}

/* O mints a LANTERN (P4 item 5): an ordinary draggable prop whose light
   is META — the light rides the object's transform, so carrying the
   lantern carries its pool of warmth. The body is the shared unit box
   (one more borrower of "m|box"); piece 2 gives it an emissive heart. */
static void cmd_mint_lantern(AppState *st) {
    Mesh    empty;
    vec3    f, pos;
    sol_u32 h;
    f = camera_forward(&st->camera);
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f   = vec3_normalize(f);
    pos = vec3_add(st->camera.pos, vec3_scale(f, 1.5f));
    pos.y = st->camera.pos.y - 0.45f;     /* held at hand height */
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos, quat_identity(),
                  vec3_make(0.16f, 0.16f, 0.16f));
    scene_mesh_ref_set(&st->scene, h, "box");
    scene_meta_set(&st->scene, h, "name", "lantern");
    mint_tag_ws(st, h);                          /* land in the active world */
    scene_meta_set(&st->scene, h, "light", "point");
    scene_meta_set(&st->scene, h, "light_color", "1.0 0.72 0.42");
    scene_meta_set(&st->scene, h, "light_intensity", "14");
    scene_meta_set(&st->scene, h, "light_radius", "9");
    scene_component_add(&st->scene, h, "flicker", (const float *)0, 0);
    {
        /* sparks (item 7): the lantern's other voice — a few embers
           a second, rising off the flame and REDDENING as they die.
           Birth rgb > 1 on purpose: each ember is bloom's customer,
           exactly like the emissive heart below. */
        static const float SPARKS[24] = {
            7.0f, 1.4f,                   /* rate, life */
            0.0f, 0.6f, 0.0f,             /* velocity: rising */
            0.12f, 0.15f, 0.12f,          /* spread: a loose plume */
            0.05f, 0.05f, 0.05f,          /* born AT the flame */
            0.012f, 0.004f,               /* shrinking as they cool */
            2.2f, 1.10f, 0.35f, 0.9f,     /* white-gold birth (HDR) */
            1.2f, 0.25f, 0.08f, 0.0f,     /* ember-red death */
            0.0f, -0.25f, 0.0f            /* gravity wins in the end */
        };
        scene_component_add(&st->scene, h, "emit", SPARKS, 24);
    }
    {
        SceneObject *lo = scene_get(&st->scene, h);
        if (lo) {
            Material m = material_default();
            m.base_color = vec3_make(0.95f, 0.78f, 0.50f);
            m.emissive   = vec3_make(1.60f, 0.95f, 0.45f);  /* the heart
                                               (P4 i5 p2): > 1.0 on
                                               purpose — bloom's first
                                               customer in piece 3 */
            m.roughness  = 0.40f;
            lo->material = m;
        }
    }
    scene_resolve_meshes(&st->scene);
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("a lantern is lit — drag it; the light goes along\n");
}

/* Q mints a POND (P7 item 8): a disc of water dropped ahead, its
   surface settled to the HOLLOW the ground makes there — the spec's
   "the mint samples terrain_height minima." Flat ground gives a
   shallow puddle; a dip gives a real pond. */
static void cmd_mint_pond(AppState *st) {
    Mesh    empty;
    vec3    f, ctr;
    float   r = 5.0f, lowest, params[3];
    int     a;
    sol_u32 h;
    f = camera_forward(&st->camera);
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f   = vec3_normalize(f);
    ctr = vec3_add(st->camera.pos, vec3_scale(f, r + 2.0f));
    /* find the hollow: sample the ground at the center + a ring,
       the surface settles a little above the lowest point */
    lowest = mint_ground(st, ctr);
    for (a = 0; a < 8; a++) {
        float ang = (float)a / 8.0f * 6.2831853f;
        vec3  s = ctr;
        float g;
        s.x += cosf(ang) * r * 0.7f;
        s.z += sinf(ang) * r * 0.7f;
        g = mint_ground(st, s);
        if (g < lowest) lowest = g;
    }
    ctr.y = lowest + 0.25f;          /* the water surface */
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, ctr, quat_identity(),
                  vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, h, "pond");
    params[0] = r; params[1] = 2.0f; params[2] = 7.0f;
    scene_mesh_params_set(&st->scene, h, params, 3);
    scene_meta_set(&st->scene, h, "name", "pond");
    mint_tag_ws(st, h);                          /* land in the active world */
    scene_resolve_meshes(&st->scene);
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("a pond fills the hollow (surface %.2f)\n", ctr.y);
}

/* E mints a DUST EMITTER (P4 item 7): a small dim marker block whose
   emit component fills the air around it with drifting motes. The
   component is attached BARE — the schema defaults ARE dust, so the
   file gets a single <component type="emit"/> line. The marker is an
   ordinary prop: pick it, drag it, the shaft of dust moves along. */
static void cmd_mint_dust(AppState *st) {
    Mesh    empty;
    vec3    f, pos;
    sol_u32 h;
    f = camera_forward(&st->camera);
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f   = vec3_normalize(f);
    pos = vec3_add(st->camera.pos, vec3_scale(f, 2.0f));
    pos.y = st->camera.pos.y - 0.25f;     /* the shaft's heart, chest-high */
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos, quat_identity(),
                  vec3_make(0.05f, 0.05f, 0.05f));
    scene_mesh_ref_set(&st->scene, h, "box");
    scene_meta_set(&st->scene, h, "name", "dust");
    mint_tag_ws(st, h);                          /* land in the active world */
    scene_component_add(&st->scene, h, "emit", (const float *)0, 0);
    {
        SceneObject *eo = scene_get(&st->scene, h);
        if (eo) {
            Material m = material_default();
            m.base_color = vec3_make(0.35f, 0.33f, 0.30f);
            m.roughness  = 0.9f;
            eo->material = m;
        }
    }
    scene_resolve_meshes(&st->scene);
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("dust hangs in the air — drag the little block to move the shaft\n");
}

/* Y mints a FOX (P4 item 9 + the wander sidequest): a rigged glb on an
   empty anchor — meta skin_glb names the file, the skeleton poses it
   fresh every frame, and the wander brain gives it somewhere to be.
   Scale 0.007 because the fox is authored in centimeters (~155 long).
   Not pickable in v1 (an empty has no AABB). */
static void cmd_mint_fox(AppState *st) {
    Mesh    empty;
    vec3    f, pos;
    sol_u32 h;
    f = camera_forward(&st->camera);
    f.y = 0.0f;
    if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
    f   = vec3_normalize(f);
    pos = vec3_add(st->camera.pos, vec3_scale(f, 2.5f));
    pos.y = st->camera.pos.y - CAMERA_EYE_HEIGHT;     /* on the floor */
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos, quat_identity(),
                  vec3_make(0.007f, 0.007f, 0.007f));
    scene_meta_set(&st->scene, h, "name", "fox");
    mint_tag_ws(st, h);                          /* land in the active world */
    scene_meta_set(&st->scene, h, "skin_glb", "Fox.glb");
    scene_component_add(&st->scene, h, "animate", (const float *)0, 0);
                               /* the persisted rule: clip 0 if it
                                  ever stops wandering */
    scene_component_add(&st->scene, h, "wander", (const float *)0, 0);
                               /* bare = a gentle meanderer; delete
                                  the line + L and it stands its
                                  ground */
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("a fox arrives — it has somewhere to be\n");
}

/* ---- Carry: pick up / place the selected object (palette spec) -------------- */

#define CARRY_HOLD_DIST  1.6f    /* how far in front the held object floats */
#define CARRY_HOLD_DROP  0.35f   /* and a little below the crosshair */
#define CARRY_MAX_REACH  6.0f    /* aim-ray ground hit beyond this -> fixed drop */
#define CARRY_DROP_DIST  2.0f    /* fixed horizontal drop distance fallback */

/* What carry actually grabs for `hit`, mirroring drag_begin's gate (main.c:4158):
   non-PLAIN cards move individually; KIND_PLAIN props move as their free-standing
   group root (not an arrow, not the floor, not architecture). 0 = not movable. */
static sol_u32 carry_target(AppState *st, sol_u32 hit) {
    SceneObject *ho = scene_get(&st->scene, hit);
    if (!ho) return 0;
    if (ho->kind != KIND_PLAIN) return hit;   /* cards carry individually, even if parented (matches drag_begin) */
    if (object_is_arrow(&st->scene, hit)) return 0;
    if (object_is_walkway(&st->scene, hit)) return 0;
    {
        sol_u32      target = group_root(&st->scene, hit);
        SceneObject *o;
        if (target == st->floor_handle) return 0;
        if (scene_meta_get(&st->scene, target, "room_type")) return 0;
        o = scene_get(&st->scene, target);
        if (!o) return 0;
        if (codex_cover_child(&st->scene, target) != 0) return target;  /* a book
                                          carries even when shelved (like cards) */
        if (o->parent != 0) return 0;
        return target;
    }
}

static sol_bool can_carry_toggle(AppState *st) {
    if (st->carried != 0) return SOL_TRUE;                 /* always can put down */
    return carry_target(st, st->selected_handle) != 0;     /* else need a movable pick */
}

/* World point a placed object lands at: where the camera-forward ray meets the
   ground under the crosshair, clamped to CARRY_MAX_REACH; falls back to a fixed
   reach straight ahead when you aim up or past the clamp. (At an exact straight
   up/down look the horizontal forward would be zero; the camera's +-89 deg pitch
   clamp keeps it non-zero, so the object still lands a reach ahead rather than at
   your feet.) Y snapped to the exact ground/collider height via mint_ground. */
static vec3 carry_place_point(AppState *st) {
    Ray   r;
    vec3  fwd = camera_forward(&st->camera);
    vec3  place, horiz;
    float t;
    r.origin = st->camera.pos;
    r.dir    = fwd;
    if (ray_vs_plane(r, vec3_make(0.0f, st->camera.ground_y, 0.0f),
                     vec3_make(0.0f, 1.0f, 0.0f), &t)
        && t > 0.1f && t < CARRY_MAX_REACH) {
        place = vec3_add(r.origin, vec3_scale(fwd, t));
    } else {
        horiz   = fwd;
        horiz.y = 0.0f;
        horiz   = vec3_normalize(horiz);
        place   = vec3_add(r.origin, vec3_scale(horiz, CARRY_DROP_DIST));
    }
    place.y = mint_ground(st, place);
    return place;
}

#define SHELF_MAX_ITEMS 64

/* a filed item on a shelf = a codex anchor or a card (NOT the shelf's own mesh). */
static int shelf_is_filed_item(AppState *st, sol_u32 h) {
    SceneObject *o = scene_get(&st->scene, h);
    if (!o) return 0;
    if (codex_cover_child(&st->scene, h) != 0) return 1;
    return (int)(o->mesh_ref && strcmp(o->mesh_ref, "card") == 0);
}

/* the item's shelf footprint: along_w = thickness (packs along the shelf),
   depth = width (into the shelf), height = h. A codex reads its cover child. */
static void shelf_item_dims(AppState *st, sol_u32 handle,
                            float *along_w, float *depth, float *height) {
    sol_u32      cov = codex_cover_child(&st->scene, handle);
    SceneObject *o   = scene_get(&st->scene, cov != 0 ? cov : handle);
    const char  *ref = (cov != 0) ? "book_cover"
                     : (o && o->mesh_ref) ? o->mesh_ref : "card";
    const float *p   = o ? o->mesh_params : (const float *)0;
    int          pc  = o ? o->mesh_param_count : 0;
    *along_w = mesh_ref_param(ref, p, pc, "t");
    *depth   = mesh_ref_param(ref, p, pc, "w");
    *height  = mesh_ref_param(ref, p, pc, "h");
}

/* the LOCAL pos `item` should take on `furniture` for layout x/row `rx`/`row`:
   depth-aware z (spine flush at the front), per-item vertical anchor. */
static vec3 shelf_item_pos(AppState *st, sol_u32 furniture, sol_u32 item,
                           float rx, int row) {
    SceneObject *fo = scene_get(&st->scene, furniture);
    float aw, dp, ht, d, ry;
    vec3  r;
    d  = (fo && fo->mesh_param_count > 2) ? fo->mesh_params[2] : 0.3f;
    ry = furniture_shelf_row_y(fo ? fo->mesh_params : (const float *)0,
                               fo ? fo->mesh_param_count : 0, row);
    shelf_item_dims(st, item, &aw, &dp, &ht);
    r.x = rx;
    r.y = ry + ((codex_cover_child(&st->scene, item) != 0) ? ht * 0.5f : 0.0f);
    r.z = (d * 0.5f + 0.04f) - dp * 0.5f;   /* spine sits proud of the front edge */
    return r;
}

/* Re-flow every filed item on `furniture` tight, left-to-right, no gaps. */
static void shelf_repack(AppState *st, sol_u32 furniture) {
    sol_u32      items[SHELF_MAX_ITEMS];
    float        widths[SHELF_MAX_ITEMS], xs[SHELF_MAX_ITEMS];
    int          rows[SHELF_MAX_ITEMS];
    int          n = 0, i, j;
    SceneObject *fo = scene_get(&st->scene, furniture);
    if (!fo || !fo->mesh_ref || !furniture_is_shelf(fo->mesh_ref)) return;
    for (i = 0; i < (int)st->scene.count && n < SHELF_MAX_ITEMS; i++) {
        SceneObject *o = &st->scene.objects[i];
        if (o->parent != furniture) continue;
        if (!shelf_is_filed_item(st, o->handle)) continue;
        items[n++] = o->handle;
    }
    /* stable fill order: higher row (bigger y) first, then left (smaller x);
       a just-dropped item placed at the append x sorts to the end. */
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            SceneObject *a = scene_get(&st->scene, items[i]);
            SceneObject *b = scene_get(&st->scene, items[j]);
            int swap = 0;
            if (a && b) {
                if (b->pos.y > a->pos.y + 0.01f) swap = 1;
                else if (fabs((double)(b->pos.y - a->pos.y)) <= 0.01 &&
                         b->pos.x < a->pos.x) swap = 1;
            }
            if (swap) { sol_u32 t = items[i]; items[i] = items[j]; items[j] = t; }
        }
    for (i = 0; i < n; i++) {
        float aw, dp, ht;
        shelf_item_dims(st, items[i], &aw, &dp, &ht);
        widths[i] = aw;
    }
    furniture_shelf_layout(fo->mesh_params, fo->mesh_param_count, widths, n, xs, rows);
    for (i = 0; i < n; i++) {
        SceneObject *o = scene_get(&st->scene, items[i]);
        if (o) o->pos = shelf_item_pos(st, furniture, items[i], xs[i], rows[i]);
    }
}

/* where a carried item WOULD land if filed now (append at the end of the pack) —
   for the live preview. */
static vec3 shelf_append_local(AppState *st, sol_u32 furniture, sol_u32 carried) {
    float        widths[SHELF_MAX_ITEMS + 1], xs[SHELF_MAX_ITEMS + 1];
    int          rows[SHELF_MAX_ITEMS + 1];
    int          n = 0, i;
    SceneObject *fo = scene_get(&st->scene, furniture);
    float        aw, dp, ht;
    if (!fo) { vec3 z = { 0.0f, 0.0f, 0.0f }; return z; }
    for (i = 0; i < (int)st->scene.count && n < SHELF_MAX_ITEMS; i++) {
        SceneObject *o = &st->scene.objects[i];
        if (o->parent != furniture || o->handle == carried) continue;
        if (!shelf_is_filed_item(st, o->handle)) continue;
        shelf_item_dims(st, o->handle, &aw, &dp, &ht);
        widths[n++] = aw;
    }
    shelf_item_dims(st, carried, &aw, &dp, &ht);
    widths[n] = aw;
    furniture_shelf_layout(fo->mesh_params, fo->mesh_param_count, widths, n + 1, xs, rows);
    return shelf_item_pos(st, furniture, carried, xs[n], rows[n]);
}

/* ---- the inventory bag (Inventory feature) ----------------------------- */

/* The bag anchor's handle, found by its meta["inventory"] tag and cached. 0 if
   no bag exists yet. Resolving by tag (not only the cached field) is what makes
   a bag persisted in scene.stml visible right after load, before any stow. */
static sol_u32 inventory_anchor_find(AppState *st) {
    int i;
    if (st->inv_anchor != 0 && scene_get(&st->scene, st->inv_anchor) != 0)
        return st->inv_anchor;
    for (i = 0; i < (int)st->scene.count; i++) {
        sol_u32 h = st->scene.objects[i].handle;
        if (scene_meta_get(&st->scene, h, "inventory")) { st->inv_anchor = h; return h; }
    }
    st->inv_anchor = 0;
    return 0;
}

/* The bag's anchor, find-or-create: a mesh-less object at the world root tagged
   meta["inventory"]. NOTE: creating it calls scene_add, which can realloc
   s->objects — callers must re-fetch any SceneObject* AFTER calling this. */
static sol_u32 inventory_anchor(AppState *st) {
    Mesh    empty;
    vec3    z   = vec3_make(0.0f, 0.0f, 0.0f);
    quat    q   = quat_identity();
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    sol_u32 h   = inventory_anchor_find(st);
    if (h != 0) return h;
    memset(&empty, 0, sizeof empty);                        /* else create one */
    st->inv_anchor = scene_add(&st->scene, 0, empty, z, q, one);
    scene_meta_set(&st->scene, st->inv_anchor, "inventory", "1");
    return st->inv_anchor;
}

/* Gather the bag's direct children (one entry per stowed item — a card, a
   note, a picture, or a codex anchor). Returns the count (<= cap). */
static int inventory_collect(AppState *st, sol_u32 *out, int cap) {
    int i, n = 0;
    sol_u32 anchor = inventory_anchor_find(st);
    if (anchor == 0) return 0;
    for (i = 0; i < (int)st->scene.count && n < cap; i++) {
        if (st->scene.objects[i].parent == anchor)
            out[n++] = st->scene.objects[i].handle;
    }
    return n;
}

/* The "world changed" rebuild after a stow/take: meshes, materials, colliders,
   pick BVH, edges, and a save. (Mirrors the load tail + the descend path.) */
static void inventory_commit(AppState *st) {
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    collide_rebuild(&st->colliders, &st->scene);
    bvh_refresh(st);
    arrows_rebuild(st);
    connections_rebuild(st);
    scene_save(&st->scene, "scene.stml");
}

/* Stow the carried item: re-parent it under the bag anchor and drop the carry
   state. The item becomes hidden (scene_object_stowed) on the next frame. */
static void inventory_stow(AppState *st) {
    sol_u32      item = st->carried, anchor;
    SceneObject *o;
    if (item == 0) return;
    anchor = inventory_anchor(st);          /* may scene_add -> re-fetch below */
    o = scene_get(&st->scene, item);
    if (!o) { st->carried = 0; return; }
    o->parent = anchor;
    o->pos    = vec3_make(0.0f, 0.0f, 0.0f);
    o->rot    = quat_identity();
    st->carried     = 0;
    st->plant_aim   = SOL_FALSE;
    st->file_aim    = SOL_FALSE;
    st->picture_aim = SOL_FALSE;
    inventory_commit(st);
    printf("stowed an item\n");
}

/* An image card (mesh_ref "card" with an image content) is the reusable
   "picture" the spec means: the carry/place path (the picture branch in
   cmd_carry_toggle) already HANGS a copy on the wall and snaps the card back
   to its pre-carry parent — so taking one with carry_prev_parent = the bag
   makes a wall-mount return it to the bag automatically. (Identified exactly
   as the mount path does, main.c:7531.) */
static sol_bool is_image_card(const SceneObject *o) {
    return (sol_bool)(o && o->mesh_ref && strcmp(o->mesh_ref, "card") == 0 &&
                      o->kind != KIND_FOLDER &&
                      o->content && reader_is_image_path(o->content));
}

/* Take an item from the bag into the hands: re-parent it to the world (so it
   is visible while carried) and make it the carried object. An image card
   gets carry_prev_parent = the bag, so a wall/board mount returns it to the
   bag (the reusable-picture rule); every other item is unique. Closes the
   screen. */
static void inventory_take(AppState *st, sol_u32 item) {
    SceneObject *o = scene_get(&st->scene, item);
    vec3         w;
    sol_bool     img;
    quat         orot;
    if (!o) return;
    w    = carry_place_point(st);           /* a point in front of the camera */
    img  = is_image_card(o);
    orot = o->rot;
    o->parent = 0;                          /* into the world / your hands */
    o->pos    = w;
    st->carried = item;
    /* the bag is GLOBAL: an item taken out JOINS the current workspace (so it
       is visible and places here, not in the world it was stowed from). Tagged
       like every other mint (active_ws, or "home" at the base world). */
    scene_meta_set(&st->scene, item, "workspace",
                   st->scene.active_ws[0] ? st->scene.active_ws : "home");
    if (img) {                              /* a wall-mount returns it to the bag */
        st->carry_prev_parent = inventory_anchor(st);   /* anchor exists (we took from it) */
        st->carry_origin      = vec3_make(0.0f, 0.0f, 0.0f);
        st->carry_prev_rot    = quat_identity();
    } else {
        st->carry_prev_parent = 0;
        st->carry_origin      = w;
        st->carry_prev_rot    = orot;
    }
    st->inv_open = SOL_FALSE;
    inventory_commit(st);
    printf("took an item from the bag\n");
}

/* Spawn a "picture" of `content` (an image path) parented to `parent` at local
   `pos`/`rot`, sized to the image's aspect (fit into a 1.6 x 1.2 box). Returns
   the new handle. Shared by the carry-drop and click-drag-drop paths so both
   build an identical picture. */
static sol_u32 spawn_image_picture(AppState *st, sol_u32 parent,
                                   vec3 pos, quat rot, const char *content) {
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    float   pw  = 1.2f, ph = 0.9f, pp[3];
    Image   img;
    sol_u32 a;
    memset(&empty, 0, sizeof empty);
    if (content && image_load(content, &img)) {     /* size the frame to the image's aspect */
        image_fit_box(img.w, img.h, 1.6f, 1.2f, &pw, &ph);
        image_free(&img);
    }
    a = scene_add(&st->scene, parent, empty, pos, rot, one);
    scene_mesh_ref_set(&st->scene, a, "picture");
    if (content) scene_content_set(&st->scene, a, content);
    pp[0] = pw; pp[1] = ph; pp[2] = 0.03f;
    scene_mesh_params_set(&st->scene, a, pp, 3);
    scene_resolve_meshes(&st->scene);               /* builds the mesh + loads the albedo */
    apply_kind_materials(&st->scene);               /* skips KIND_PLAIN -> keeps the image */
    board_card_tag_page(st, a);                     /* lands on the page you're viewing */
    return a;
}

/* E: put down what you're carrying, else pick up the selected movable object.
   A carried FOLDER, dropped while aiming at a wall, OPENS the folder as a real
   sub-room (door + walkway + its contents) — and the folder card snaps back to
   its own spot in the room rather than being placed (folders are tray-resident
   navigation, not props). A non-folder places on the ground as before. */
static void cmd_carry_toggle(AppState *st) {
    if (st->carried != 0) {
        SceneObject *o = scene_get(&st->scene, st->carried);
        if (o) {
            const char *cname = o->content;   /* capture before descend_plant reallocs s->objects */
            if (o->kind == KIND_FOLDER) {
                o->pos = st->carry_origin;    /* the folder stays home (restore pre-carry pos) */
                if (st->plant_aim) {
                    sol_u32 pv = descend_plant(&st->scene, st->plant_room, st->carried,
                                               st->plant_wall, st->plant_off);
                    if (pv != 0) {
                        const char *sp = scene_meta_get(&st->scene, pv, "source_path");
                        if (sp) room_mirror_scan(&st->scene, pv, sp);   /* fill the new room now */
                        scene_resolve_meshes(&st->scene);
                        apply_kind_materials(&st->scene);
                        connections_rebuild(st);
                        collide_rebuild(&st->colliders, &st->scene);
                        printf("opened '%s' as a room\n", cname ? cname : "?");
                    }
                }
                scene_save(&st->scene, "scene.stml");
            } else if (st->picture_aim && st->picture_target != 0 &&
                       scene_get(&st->scene, st->picture_target) != 0) {
                const char *path   = o->content;      /* heap ptr survives scene_add */
                float       defh   = mesh_ref_param("picture", (const float *)0, 0, "h");
                vec3        plocal = st->picture_local;
                sol_u32     a;
                o->parent = st->carry_prev_parent;    /* the image card RETURNS to where it was */
                o->pos    = st->carry_origin;          /*   (e.g. back onto its shelf) */
                o->rot    = st->carry_prev_rot;
                a = spawn_image_picture(st, st->picture_target, plocal, st->picture_rot, path);
                {   /* keep the previewed CENTRE as the height changes from the
                       default to the image's aspect (the preview pinned at defh) */
                    SceneObject *ao = scene_get(&st->scene, a);
                    if (ao) {
                        float ph = mesh_ref_param("picture", ao->mesh_params,
                                                  ao->mesh_param_count, "h");
                        ao->pos.y += (defh - ph) * 0.5f;
                    }
                }
                scene_save(&st->scene, "scene.stml");
                printf("hung a picture\n");
            } else if (st->file_aim && st->file_target != 0 &&
                       scene_get(&st->scene, st->file_target) != 0) {
                o->parent = st->file_target;          /* re-parent: the furniture owns it now */
                o->pos    = st->file_local;           /* furniture-local resting pos */
                o->rot    = st->file_rot;
                {   /* a shelf re-flows all its items tight after the drop */
                    SceneObject *ft = scene_get(&st->scene, st->file_target);
                    if (ft && ft->mesh_ref && furniture_is_shelf(ft->mesh_ref))
                        shelf_repack(st, st->file_target);
                }
                scene_resolve_meshes(&st->scene);
                apply_kind_materials(&st->scene);
                scene_save(&st->scene, "scene.stml");
                st->selected_handle = 0;              /* deselect: a placed item drops its
                                                         highlight, so it reads as PLACED,
                                                         not still being preview-hovered */
                printf("filed onto furniture\n");
            } else {
                vec3    w = carry_place_point(st);
                sol_u32 ccov = codex_cover_child(&st->scene, st->carried);
                if (ccov != 0) {            /* a codex stands on its base: its
                                               anchor origin is the book's CENTRE,
                                               so lift by half its height */
                    SceneObject *cco = scene_get(&st->scene, ccov);
                    if (cco)
                        w.y += 0.5f * mesh_ref_param("book_cover", cco->mesh_params,
                                                     cco->mesh_param_count, "h");
                }
                o->pos = scene_world_to_local(&st->scene, o->parent, w);
                scene_save(&st->scene, "scene.stml");
            }
        }
        st->carried     = 0;
        st->plant_aim   = SOL_FALSE;
        st->file_aim    = SOL_FALSE;
        st->picture_aim = SOL_FALSE;
    } else {
        sol_u32 t = carry_target(st, st->selected_handle);
        if (t != 0) {
            SceneObject *co = scene_get(&st->scene, t);
            st->carried  = t;
            st->place_yaw = 0.0f;                  /* a fresh grab starts un-spun;
                                                      ',' / '.' rotate from here */
            if (co) {
                st->carry_origin      = co->pos;   /* remember its spot, to snap it back */
                st->carry_prev_parent = co->parent; /* and its parent (e.g. a shelf) + rot */
                st->carry_prev_rot    = co->rot;
                {
                    SceneObject *par = scene_get(&st->scene, co->parent);
                    sol_bool on_furn = (sol_bool)(par && par->mesh_ref &&
                        (furniture_is_table(par->mesh_ref) || furniture_is_shelf(par->mesh_ref)));
                    sol_bool mounted = (sol_bool)(co->parent != 0 && co->mesh_ref &&
                        (strcmp(co->mesh_ref, "board") == 0 ||
                         strcmp(co->mesh_ref, "picture") == 0));
                    if (on_furn || mounted) {
                        vec3     wp  = object_world_pos(&st->scene, t);  /* world pos before detach */
                        sol_u32  src = co->parent;                       /* the shelf, if any */
                        sol_bool was_shelf = (sol_bool)(par && par->mesh_ref &&
                                                        furniture_is_shelf(par->mesh_ref));
                        co->parent = 0;                                 /* leave the wall/furniture */
                        co->pos    = wp;
                        mint_tag_ws(st, t);                             /* join the active world: a
                                                                           now parent-less item would
                                                                           resolve to "home" and be
                                                                           HIDDEN in a named workspace */
                        if (was_shelf) shelf_repack(st, src);           /* reflow the rest */
                        {   /* un-shelve/un-mount the orientation: a filed card is
                               spine-out and a mounted picture is surface-facing, so
                               hold it FACE-ON, turned to face you (like a fresh card)
                               — else you see only its thin edge in front of you */
                            vec3  f = camera_forward(&st->camera);
                            float yaw;
                            f.y = 0.0f;
                            if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
                            yaw = (float)atan2((double)(-f.x), (double)(-f.z));
                            co  = scene_get(&st->scene, t);             /* re-fetch (be safe) */
                            if (co) {
                                co->rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw);
                                st->carry_prev_rot = co->rot;
                            }
                        }
                    }
                }
            }
        }
    }
}

/* Per-frame: float the carried object in front of the camera. Called right after
   update() (so it runs before components_update reads poses). A carried FOLDER
   tablet instead snaps flat to the wall you're aiming at (descent planting). */
/* a board mounted on a wall = mesh "board" whose parent carries room_type. */
static sol_bool board_is_mounted(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    SceneObject *par;
    if (!o || !o->mesh_ref ||
        (strcmp(o->mesh_ref, "board") != 0 && strcmp(o->mesh_ref, "picture") != 0))
        return SOL_FALSE;
    if (o->parent == 0) return SOL_FALSE;
    par = scene_get(s, o->parent);
    return (sol_bool)(par != 0 && scene_meta_get(s, par->handle, "room_type") != 0);
}

/* a "picture" parented to a whiteboard (not a wall) — resizable in board-local
   space, the third resizable kind alongside wall-mounts and free notes. */
static sol_bool picture_on_board(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (sol_bool)(o && o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0 &&
                      object_is_board(s, o->parent));
}

/* a "window" parented to a room wall — selectable/slidable/resizable like a
   wall picture, but CENTER-origin (its pos is the hole centre). The move/resize
   branches reconcile that to the picture machinery's bottom-origin assumption by
   treating the window as a board of the OUTER frame size (w+2fw)x(h+2fw) whose
   bottom-centre is pos - (0, h/2 + fw, 0). */
static sol_bool window_on_wall(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (sol_bool)(o && o->mesh_ref && strcmp(o->mesh_ref, "window") == 0);
}

/* yaw of a mounted board (its facing). */
static float board_yaw(Scene *s, sol_u32 h) {
    quat q;
    if (!scene_get(s, h)) return 0.0f;
    q = scene_world_rotation(s, h);   /* takes a handle, not a SceneObject* */
    return 2.0f * (float)atan2((double)q.y, (double)q.w);
}

/* world corners (out[4]) + the wall horizontal axis (*out_u) of a mounted board. */
static void board_world_corners(Scene *s, sol_u32 h, vec3 out[4], vec3 *out_u) {
    SceneObject *o   = scene_get(s, h);
    const char  *mr  = (o && o->mesh_ref) ? o->mesh_ref : "board";
    vec3  p   = object_world_pos(s, h);
    float w   = o ? mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "w") : 1.8f;
    float ht  = o ? mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "h") : 1.2f;
    float yaw = board_yaw(s, h);
    vec3  u   = vec3_make((float)cos((double)yaw), 0.0f, -(float)sin((double)yaw));
    if (o && o->mesh_ref && strcmp(o->mesh_ref, "window") == 0) {
        /* center-origin: present the OUTER frame as a bottom-origin board so the
           corner handles wrap the whole window, not the inner opening. */
        float fw = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "fw");
        p.y -= ht * 0.5f + fw;         /* centre -> outer bottom-centre */
        w   += 2.0f * fw;
        ht  += 2.0f * fw;
    }
    board_corners(p, w, ht, u, out);
    if (out_u) *out_u = u;
}

/* Index (0..3) of the selected board/note corner handle the pick ray currently
   passes near, or -1 if none. Shared by the grab (resize_corner_pick) and the
   hover highlight. Optionally returns the 4 world corners + the wall axis. */
static int resize_corner_at(AppState *st, GLFWwindow *w, vec3 cor_out[4], vec3 *u_out) {
    Ray   ray = pick_ray(st, w);
    vec3  cor[4], u;
    int   i, best = -1;
    float bestd = 0.18f;                       /* grab radius (m) */
    ray.dir = vec3_normalize(ray.dir);
    board_world_corners(&st->scene, st->selected_handle, cor, &u);
    for (i = 0; i < 4; i++) {
        vec3  rel   = vec3_sub(cor[i], ray.origin);
        float along = vec3_dot(rel, ray.dir);
        vec3  perp;
        float d;
        if (along <= 0.0f) continue;           /* behind the camera */
        perp = vec3_sub(rel, vec3_scale(ray.dir, along));
        d    = (float)sqrt((double)vec3_dot(perp, perp));
        if (d < bestd) { bestd = d; best = i; }
    }
    if (cor_out) { cor_out[0] = cor[0]; cor_out[1] = cor[1];
                   cor_out[2] = cor[2]; cor_out[3] = cor[3]; }
    if (u_out) *u_out = u;
    return best;
}

/* On a press: if the crosshair ray passes near a corner handle of the selected
   mounted board, begin a resize with the OPPOSITE corner anchored. 1 on a grab. */
static int resize_corner_pick(AppState *st, GLFWwindow *w) {
    vec3  cor[4], u;
    int   best = resize_corner_at(st, w, cor, &u);
    SceneObject *o = scene_get(&st->scene, st->selected_handle);
    if (best < 0) return 0;
    st->resize_board  = st->selected_handle;
    st->resize_anchor = cor[(best + 2) % 4];   /* the opposite corner */
    st->resize_u      = u;
    st->resize_room   = o ? o->parent : 0;
    st->resize_corner = best;                  /* grabbed corner: stays lit while dragging */
    {   /* no-jump grab: remember (grabbed corner - cursor hit) on the card plane,
           so the drag tracks (hit + offset) and the first frame reproduces the
           corner exactly (you clicked up to the grab radius off it). */
        Ray   ray = pick_ray(st, w);
        float yaw = board_yaw(&st->scene, st->selected_handle);
        vec3  n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
        float tt;
        ray.dir = vec3_normalize(ray.dir);
        st->resize_grab = vec3_make(0.0f, 0.0f, 0.0f);
        if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
            vec3 hit = vec3_add(ray.origin, vec3_scale(ray.dir, tt));
            st->resize_grab = vec3_sub(cor[best], hit);
        }
    }
    return 1;
}

/* On a press on a mounted PICTURE's body (no corner grabbed): begin sliding it
   along its wall. Returns 1 on a grab. */
static int picture_move_pick(AppState *st, GLFWwindow *w) {
    Ray   ray = pick_ray(st, w);
    SceneObject *o = scene_get(&st->scene, st->selected_handle);
    float yaw, tt;
    vec3  n, p0, h0;
    /* slide a mounted board OR a picture (a body click — resize_corner_pick
       already claimed it if the click was on a corner). A picture slides on its
       wall or whiteboard; a board slides only when wall-mounted (a free-standing
       board must stay carry-able, not glue itself to a phantom wall). */
    if (!o || !o->mesh_ref) return 0;
    if (strcmp(o->mesh_ref, "picture") != 0 &&
        strcmp(o->mesh_ref, "window")  != 0) {     /* a window slides on its wall too */
        if (strcmp(o->mesh_ref, "board") != 0 ||
            !board_is_mounted(&st->scene, st->selected_handle))
            return 0;
    }
    yaw = board_yaw(&st->scene, st->selected_handle);
    n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
    p0  = object_world_pos(&st->scene, st->selected_handle);  /* origin (bottom-center) */
    ray.dir = vec3_normalize(ray.dir);
    if (!ray_vs_plane(ray, p0, n, &tt) || tt <= 0.0f) return 0;
    h0  = vec3_add(ray.origin, vec3_scale(ray.dir, tt));
    st->move_board  = st->selected_handle;
    st->move_grab   = vec3_sub(p0, h0);          /* keep the grab point under the cursor */
    st->resize_u    = vec3_make((float)cos((double)yaw), 0.0f, -(float)sin((double)yaw));
    st->resize_room = o->parent;
    return 1;
}

/* room interior height from its "room" shell (default 3.0 if none). */
static float room_interior_height(Scene *s, sol_u32 room) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *c = &s->objects[i];
        if (c->parent == room && c->mesh_ref && strcmp(c->mesh_ref, "room") == 0)
            return mesh_ref_param("room", c->mesh_params, c->mesh_param_count, "h");
    }
    return 4.5f;
}

/* The mountable region of a room wall: the rectangle (floor..wall_top, full
   half-span) plus, on the two ridge-END walls, the roof gable triangle above it.
   `normal_x` = this wall's inward normal points along X (the E/W walls). The
   gable is on the walls perpendicular to the ridge, which runs along the longer
   dimension (room_frame_build's `rax`). */
typedef struct { float floor_y, wall_top, apex_y, half_span; int is_gable; } WallMount;

static WallMount wall_gable_geom(RoomRect r, float ih, int normal_x) {
    WallMount m;
    int   rax  = (r.hw >= r.hd) ? 1 : 0;        /* ridge runs along the longer dim */
    float span = normal_x ? r.hd : r.hw;        /* this wall's run half-length */
    float dy   = span * (float)tan((double)sol_radians(FRAME_PITCH_DEG));
    m.floor_y   = r.floor_y;
    m.wall_top  = r.floor_y + ih;
    m.half_span = span;
    m.is_gable  = (normal_x == rax);            /* ridge-end walls carry the gable */
    m.apex_y    = m.is_gable ? m.wall_top + dy : m.wall_top;
    return m;
}

/* Clamp a flat wall object's centre (run = along-wall offset from the wall's
   centre, cy = vertical centre) to the wall, plus the gable triangle above it on
   ridge-end walls. As the object rises into the triangle its along-wall room
   shrinks toward the apex, so it never pokes out past the sloped rake. */
static void wall_clamp_run_cy(WallMount m, float w_half, float h_half,
                              float *run, float *cy) {
    float top, avail, lim;
    if (*cy < m.floor_y + h_half) *cy = m.floor_y + h_half;     /* bottom on the floor */
    if (m.is_gable && m.apex_y > m.wall_top) {
        float spanh  = m.apex_y - m.wall_top;
        float topmax = m.apex_y - w_half * spanh / m.half_span; /* highest the top fits */
        if (topmax > m.apex_y) topmax = m.apex_y;
        if (*cy + h_half > topmax) *cy = topmax - h_half;
    } else if (*cy + h_half > m.wall_top) {
        *cy = m.wall_top - h_half;                              /* top under the eave */
    }
    top = *cy + h_half;
    if (m.is_gable && top > m.wall_top) {
        float spanh = m.apex_y - m.wall_top;
        avail = m.half_span * (m.apex_y - top) / spanh;         /* triangle half-width at top */
        if (avail < 0.0f) avail = 0.0f;
    } else {
        avail = m.half_span;
    }
    lim = avail - w_half;
    if (lim < 0.0f) lim = 0.0f;
    if (*run >  lim) *run =  lim;
    if (*run < -lim) *run = -lim;
}

/* After descend_wall_mount has picked a wall + rectangular centre, lift the
   centre into the gable triangle if the crosshair aims there (ridge-end walls
   only; eave walls keep descend's result untouched). */
static void wall_mount_gable(Scene *s, sol_u32 room, int wall, Ray ray,
                             float w_half, float h_half, float t, vec3 *center) {
    RoomRect  r  = editor_room_rect(s, room);
    float     ih = room_interior_height(s, room);
    int       normal_x = (wall == ROOM_WALL_E || wall == ROOM_WALL_W);
    WallMount m  = wall_gable_geom(r, ih, normal_x);
    vec3      pt, n, hit;
    int       runx;
    float     tt, run, cy;
    if (!m.is_gable) return;                    /* eave wall: nothing above the eave */
    if (wall == ROOM_WALL_N)      { pt = vec3_make(r.cx, r.floor_y, r.cz - r.hd); n = vec3_make(0.0f,0.0f, 1.0f); runx = 1; }
    else if (wall == ROOM_WALL_S) { pt = vec3_make(r.cx, r.floor_y, r.cz + r.hd); n = vec3_make(0.0f,0.0f,-1.0f); runx = 1; }
    else if (wall == ROOM_WALL_E) { pt = vec3_make(r.cx + r.hw, r.floor_y, r.cz); n = vec3_make(-1.0f,0.0f,0.0f); runx = 0; }
    else                          { pt = vec3_make(r.cx - r.hw, r.floor_y, r.cz); n = vec3_make( 1.0f,0.0f,0.0f); runx = 0; }
    if (!ray_vs_plane(ray, pt, n, &tt) || tt <= 0.05f) return;
    hit = vec3_add(ray.origin, vec3_scale(ray.dir, tt));
    run = runx ? (hit.x - r.cx) : (hit.z - r.cz);
    cy  = hit.y;
    wall_clamp_run_cy(m, w_half, h_half, &run, &cy);
    if (runx) { center->x = r.cx + run; center->z = pt.z; }
    else      { center->z = r.cz + run; center->x = pt.x; }
    center->x += n.x * (t * 0.5f);              /* keep the back flush, proud by t/2 */
    center->z += n.z * (t * 0.5f);
    center->y  = cy;
}

/* The camera pose that frames `board` head-on: centred, square to the surface,
   pulled back to fill the FOV. Shared by board_view_enter and the reader's
   return-to-board glide. Returns 0 (leaving *out untouched) if the handle isn't
   a board. */
static int board_view_pose(AppState *st, sol_u32 board, CameraPose *out) {
    SceneObject *o = scene_get(&st->scene, board);
    vec3  cor[4], center, normal;
    float yaw, half_w, half_h, aspect;
    if (!o || !o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) return 0;
    board_world_corners(&st->scene, board, cor, NULL);
    center = vec3_scale(vec3_add(vec3_add(cor[0], cor[1]),
                                 vec3_add(cor[2], cor[3])), 0.25f);
    yaw    = board_yaw(&st->scene, board);
    normal = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
    half_w = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "w") * 0.5f;
    half_h = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h") * 0.5f;
    aspect = (st->fb_height > 0) ? (float)st->fb_width / (float)st->fb_height : 1.7778f;
    *out   = camera_frame_pose(center, normal, half_w, half_h,
                               st->camera.fov, aspect, BOARD_VIEW_MARGIN);
    return 1;
}

/* Enter board view: frame the selected whiteboard head-on and begin the glide.
   Returns 0 (and does nothing) if the selection isn't a board, board view is
   already active, or another mode owns the keyboard/cursor. */
static int board_view_enter(AppState *st) {
    SceneObject *o = scene_get(&st->scene, st->selected_handle);
    CameraPose pose;
    if (st->board_view != 0) return 0;
    if (!o || !o->mesh_ref || strcmp(o->mesh_ref, "board") != 0) return 0;
    if (st->carried != 0 || st->place_active || st->editor.active ||
        st->palette.open || st->inv_open || st->edit_handle != 0 ||
        st->reader_state != READER_IDLE) return 0;
    if (!board_view_pose(st, st->selected_handle, &pose)) return 0;
    st->bv_return_pos   = st->camera.pos;
    st->bv_return_yaw   = st->camera.yaw;
    st->bv_return_pitch = st->camera.pitch;
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = pose.pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = pose.yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = pose.pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = 1.0f;
    st->board_view = st->selected_handle;
    return 1;
}

/* Leave board view: glide the camera back to the stored return pose. Safe to
   call when already out. */
static void board_view_exit(AppState *st) {
    if (st->board_view == 0) return;
    st->bv_from_pos = st->camera.pos;   st->bv_to_pos = st->bv_return_pos;
    st->bv_from_yaw = st->camera.yaw;   st->bv_to_yaw = st->bv_return_yaw;
    st->bv_from_pitch = st->camera.pitch; st->bv_to_pitch = st->bv_return_pitch;
    st->bv_t   = 0.0f;
    st->bv_dir = -1.0f;
    st->board_view = 0;
    st->drop_target_handle = 0;      /* drop no stale drag-to-file target (Task 8) */
    st->marquee_active   = SOL_FALSE;   /* cancel any in-flight marquee */
    st->marquee_dragging = SOL_FALSE;
    st->group_drag       = SOL_FALSE;   /* cancel any in-flight group drag (M3) */
    sel_clear(st);                      /* multi-select is board-local: don't leak the
                                           set (or anchor) across a board exit */
}

/* Advance the board-view camera glide (runs AFTER camera_update so it overrides
   it). Also bails out of board view if the viewed board was deleted. */
static void board_view_update(AppState *st, float dt) {
    float e, dyaw;
    if (st->board_view != 0 && scene_get(&st->scene, st->board_view) == 0)
        board_view_exit(st);                 /* board vanished: glide back out */
    if (st->bv_t >= 1.0f) return;            /* settled: nothing to animate */
    st->bv_t += dt / BOARD_VIEW_GLIDE_S;
    if (st->bv_t > 1.0f) st->bv_t = 1.0f;
    e = sol_smoothstep(st->bv_t);
    st->camera.pos = vec3_add(st->bv_from_pos,
                     vec3_scale(vec3_sub(st->bv_to_pos, st->bv_from_pos), e));
    dyaw = st->bv_to_yaw - st->bv_from_yaw;
    while (dyaw >  SOL_PI) dyaw -= 2.0f * SOL_PI;   /* shortest arc */
    while (dyaw < -SOL_PI) dyaw += 2.0f * SOL_PI;
    st->camera.yaw   = st->bv_from_yaw + dyaw * e;
    st->camera.pitch = st->bv_from_pitch +
                       (st->bv_to_pitch - st->bv_from_pitch) * e;
}

/* a note's body text size in metres-per-line; absent meta => the default,
   clamped to the editable range. */
/* New-note default card: landscape and roomier than the portrait file/folder
   card (the shared "card" registry default is 0.35 x 0.5). Notes set these
   explicitly at spawn so other card kinds are unaffected. */
#define NOTE_CARD_W 0.90f
#define NOTE_CARD_H 0.55f
#define NOTE_CARD_T 0.03f

static float note_text_size(Scene *s, sol_u32 h) {
    const char *v = scene_meta_get(s, h, "text_size");
    float ts = v ? (float)atof(v) : 0.119f;   /* new-note default: 60% toward the max */
    if (ts < 0.015f) ts = 0.015f;
    if (ts > 0.180f) ts = 0.180f;
    return ts;
}

/* THE authority for a note's height + vertical position: fit the card to its
   wrapped text at the current width and size, top-anchored (the top edge stays
   put, the card grows downward), but never shorter than meta["min_h"]. Rebuilds
   the registry-shared "card" mesh only when the height actually changes; width
   and horizontal position are left untouched. */
static void note_autosize(AppState *st, sol_u32 h) {
    SceneObject *o = scene_get(&st->scene, h);
    const char  *txt, *mv;
    char         wbuf[4096];
    float        cw, h0, ct, ts, lh, px2m, usable, content_h, min_h, new_h;
    int          lines;
    if (!o || o->kind != KIND_NOTE || !st->ui_font) return;
    if (!o->mesh_ref || strcmp(o->mesh_ref, "card") != 0) return;
    cw = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
    h0 = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "h");
    ct = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "t");
    ts = note_text_size(&st->scene, h);
    lh = font_line_height(st->ui_font);
    px2m = (lh > 0.0f) ? ts / lh : ts;
    usable = cw - 3.0f * 0.025f;   /* match the render: left pad 2*0.025 (doubled) + right 0.025 */
    txt = scene_meta_get(&st->scene, h, "text");
    lines = (txt && txt[0] && usable > 0.0f)
          ? text_wrap(st->ui_font, txt, px2m, usable, wbuf, (int)sizeof wbuf)
          : 1;
    if (lines < 1) lines = 1;
    content_h = (float)lines * ts + 3.0f * 0.025f;          /* top 2*0.025 (doubled) + bottom 0.025 */
    mv = scene_meta_get(&st->scene, h, "min_h");
    min_h = mv ? (float)atof(mv) : 0.5f;                    /* default card height */
    new_h = (content_h > min_h) ? content_h : min_h;
    if (new_h < 0.05f) new_h = 0.05f;
    if (new_h > h0 - 0.001f && new_h < h0 + 0.001f) return; /* unchanged: no rebuild */
    {   /* top-anchored rebuild: capture the world top-centre, then keep it fixed */
        mat4 M       = scene_world_matrix(&st->scene, o);
        vec3 top_w   = mat4_mul_point(M, vec3_make(0.0f, h0, 0.0f));
        char oldkey[160];
        sol_bool keyed = mesh_asset_key(o, oldkey);
        float p3[3];
        p3[0] = cw; p3[1] = new_h; p3[2] = ct;
        scene_mesh_params_set(&st->scene, h, p3, 3);
        if (keyed) asset_release(&g_mesh_assets, oldkey);
        o = scene_get(&st->scene, h);
        if (o) {
            vec3 nb = vec3_make(top_w.x, top_w.y - new_h, top_w.z);
            memset(&o->mesh, 0, sizeof o->mesh);
            o->pos = scene_world_to_local(&st->scene, o->parent, nb);
        }
        scene_resolve_meshes(&st->scene);
    }
}

/* a note card can be corner-resized (free-standing or pinned, unlike the
   wall-only board path). */
static sol_bool note_resizable(Scene *s, sol_u32 h) {
    SceneObject *o = scene_get(s, h);
    return (sol_bool)(o && o->kind == KIND_NOTE &&
                      o->mesh_ref && strcmp(o->mesh_ref, "card") == 0);
}

static void carry_update(AppState *st) {
    SceneObject *o;
    vec3         fwd, hold;
    if (st->carried == 0) return;
    o = scene_get(&st->scene, st->carried);
    if (!o) { st->carried = 0; return; }                   /* it vanished */
    st->plant_aim   = SOL_FALSE;   /* reset all three aim flags up-front so an */
    st->picture_aim = 0;           /* early return from ANY block below can't   */
    st->file_aim    = SOL_FALSE;   /* leave a stale flag the drop path acts on  */
    if (o->kind == KIND_FOLDER) {
        sol_u32 room = descend_room_at(&st->scene, st->camera.pos);
        if (room != 0) {
            RoomRect r = editor_room_rect(&st->scene, room);
            Ray   ray;
            int   wall;
            float off;
            ray.origin = st->camera.pos;
            ray.dir    = camera_forward(&st->camera);
            if (descend_wall_aim(r, ray, ROUTE_DOOR_H, &wall, &off)) {
                vec3 wpt = descend_door_point(r, wall, off);
                wpt.y += 1.0f;                              /* hover at ~door height */
                o->pos = scene_world_to_local(&st->scene, o->parent, wpt);
                st->plant_room = room; st->plant_wall = wall;
                st->plant_off = off;  st->plant_aim = SOL_TRUE;
                return;
            }
        }
    }
    /* Filing onto furniture is tried BEFORE wall/whiteboard mounting: a shelf or
       table standing in front of a wall should win the aim. descend_wall_mount
       (below) can't see furniture, so checking the wall first would mount on the
       wall behind the shelf — the same reason the whiteboard is checked first. */
    {
        sol_bool carry_is_card  = (sol_bool)(o->mesh_ref &&
            strcmp(o->mesh_ref, "card") == 0 && o->kind != KIND_FOLDER);
        sol_bool carry_is_codex = (sol_bool)(codex_cover_child(&st->scene, st->carried) != 0);
        if (carry_is_card || carry_is_codex) {
        sol_u32 fi;
        for (fi = 0; fi < st->scene.count; fi++) {
            SceneObject *f = &st->scene.objects[fi];
            vec3  fpos, loc; float fyaw; quat fq;
            Ray   ray;
            if (!f->mesh_ref) continue;
            if (!furniture_is_table(f->mesh_ref) && !furniture_is_shelf(f->mesh_ref)) continue;
            if (!scene_object_active(&st->scene, f->handle)) continue;
            fpos = object_world_pos(&st->scene, f->handle);
            fq   = f->rot;
            fyaw = 2.0f * (float)atan2((double)fq.y, (double)fq.w);
            ray.origin = st->camera.pos;
            ray.dir    = camera_forward(&st->camera);
            if (!furniture_surface_aim(f->mesh_ref, f->mesh_params, f->mesh_param_count,
                                       fpos, fyaw, ray, &loc)) continue;
            st->file_aim    = SOL_TRUE;
            st->file_target = f->handle;
            if (furniture_is_shelf(f->mesh_ref)) {
                /* variable-width packing computes the append position (depth +
                   vertical anchor handled inside); FURNITURE-LOCAL spine-out rot. */
                st->file_local = shelf_append_local(st, f->handle, st->carried);
                st->file_rot   = carry_is_codex
                    ? quat_mul(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f)),
                               quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f)))
                    : quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f)); /* spine edge-out */
            } else if (carry_is_codex) {
                st->file_local = furniture_table_point(f->mesh_params, f->mesh_param_count, loc);
                st->file_rot   = quat_mul(
                    quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), sol_radians(90.0f)),
                    quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f)));
            } else {
                st->file_local = furniture_table_point(f->mesh_params, f->mesh_param_count, loc);
                st->file_rot   = quat_mul(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), st->place_yaw),
                                          quat_from_axis_angle(vec3_make(1.0f,0.0f,0.0f), sol_radians(-90.0f))); /* flat + carry yaw */
            }
            /* preview: hover the carried tablet at the resting spot (furniture-local -> world) */
            {
                mat4 fm  = scene_world_matrix(&st->scene, f);
                vec3 wp  = mat4_mul_point(fm, st->file_local);
                quat fwr = scene_world_rotation(&st->scene, f->handle);
                quat wr  = quat_mul(fwr, st->file_rot);   /* the dropped WORLD rot */
                o->pos = scene_world_to_local(&st->scene, o->parent, wp);
                o->rot = (o->parent != 0)
                       ? quat_mul(quat_conjugate(scene_world_rotation(&st->scene, o->parent)), wr)
                       : wr;   /* compose the shelf yaw — match the drop, not file_rot-as-world */
            }
            return;
        }
        }
    }
    /* image card not resting on furniture: mount it as a picture — on a
       whiteboard under the aim, else on the wall behind. */
    if (o->mesh_ref && strcmp(o->mesh_ref, "card") == 0 && o->kind != KIND_FOLDER &&
        o->content && reader_is_image_path(o->content)) {
        float   pw = mesh_ref_param("picture", (const float *)0, 0, "w");
        float   ph = mesh_ref_param("picture", (const float *)0, 0, "h");
        float   pt = mesh_ref_param("picture", (const float *)0, 0, "t");
        Ray     ray;
        vec3    bloc;
        sol_u32 board, room;
        ray.origin = st->camera.pos;
        ray.dir    = camera_forward(&st->camera);
        board = board_under_ray(st, ray, &bloc);       /* a WHITEBOARD in the way wins
                                                          (the wall test below ignores it,
                                                          so it would mount behind it) */
        if (board != 0) {
            vec3 lp = board_pin_pos(&st->scene, board, st->carried, bloc, 0.0f, -0.5f * ph);
            mat4 bm = scene_world_matrix(&st->scene, scene_get(&st->scene, board));
            vec3 wp = mat4_mul_point(bm, lp);
            st->picture_aim    = 1;
            st->picture_target = board;
            st->picture_rot    = quat_identity();
            st->picture_local  = lp;
            o->pos = scene_world_to_local(&st->scene, o->parent, wp);
            o->rot = scene_world_rotation(&st->scene, board);
            return;
        }
        room = descend_room_at(&st->scene, st->camera.pos);
        if (room != 0) {                               /* else aim at a WALL -> mount */
            RoomRect r = editor_room_rect(&st->scene, room);
            int   wall;
            vec3  center;
            float ceil_y = r.floor_y + room_interior_height(&st->scene, room);
            if (descend_wall_mount(r, ray, ceil_y, pw * 0.5f, ph * 0.5f, pt,
                                   &wall, &center)) {
                static const float wyaw[4] = { 0.0f, -90.0f, 180.0f, 90.0f };
                vec3 P;
                wall_mount_gable(&st->scene, room, wall, ray,
                                 pw * 0.5f, ph * 0.5f, pt, &center);
                P = vec3_make(center.x, center.y - ph * 0.5f, center.z);
                st->picture_aim    = 1;
                st->picture_target = room;
                st->picture_rot    = quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f),
                                                          sol_radians(wyaw[wall]));
                st->picture_local  = scene_world_to_local(&st->scene, room, P);
                o->pos = scene_world_to_local(&st->scene, o->parent, P);
                o->rot = st->picture_rot;
                return;
            }
        }
    }
    if (o->mesh_ref && (strcmp(o->mesh_ref, "board") == 0 ||
                        strcmp(o->mesh_ref, "picture") == 0)) {
        const char *mr   = o->mesh_ref;            /* re-mount a board OR a picture */
        sol_u32 room = descend_room_at(&st->scene, st->camera.pos);
        if (room != 0) {
            RoomRect r = editor_room_rect(&st->scene, room);
            Ray     ray;
            int     wall;
            vec3    center;
            float   bw, bh, bt, rh;
            sol_u32 ci;
            bw = mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "w");
            bh = mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "h");
            bt = mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "t");
            rh = 4.5f;                                  /* room interior height (default) */
            for (ci = 0; ci < st->scene.count; ci++) {
                SceneObject *c = &st->scene.objects[ci];
                if (c->parent == room && c->mesh_ref &&
                    strcmp(c->mesh_ref, "room") == 0) {
                    rh = mesh_ref_param("room", c->mesh_params, c->mesh_param_count, "h");
                    break;
                }
            }
            ray.origin = st->camera.pos;
            ray.dir    = camera_forward(&st->camera);
            if (descend_wall_mount(r, ray, r.floor_y + rh, bw * 0.5f, bh * 0.5f, bt,
                                   &wall, &center)) {
                static const float wall_yaw[4] = { 0.0f, -90.0f, 180.0f, 90.0f };
                vec3 P;
                wall_mount_gable(&st->scene, room, wall, ray,
                                 bw * 0.5f, bh * 0.5f, bt, &center);
                P = vec3_make(center.x, center.y - bh * 0.5f, center.z);
                st->file_aim    = SOL_TRUE;
                st->file_target = room;
                st->file_rot    = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f),
                                                       sol_radians(wall_yaw[wall]));
                st->file_local  = scene_world_to_local(&st->scene, room, P);
                o->pos = scene_world_to_local(&st->scene, o->parent, P);
                o->rot = st->file_rot;
                return;
            }
        }
    }
    fwd     = camera_forward(&st->camera);
    hold    = vec3_add(st->camera.pos, vec3_scale(fwd, CARRY_HOLD_DIST));
    hold.y -= CARRY_HOLD_DROP;
    o->pos  = scene_world_to_local(&st->scene, o->parent, hold);
    /* free-float carry: ',' / '.' spin the held object around vertical (place_yaw)
       from its pick-up orientation, and the angle persists when it's dropped. */
    o->rot  = quat_mul(quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), st->place_yaw),
                       st->carry_prev_rot);
}

#define HOME_FLOOR_Y 12.0f   /* the home room floats this high in the sky */

#define ROOT_RING_SLOTS 8    /* evenly-spaced placement slots per ring around home */

/* Does a 2*half x 2*half footprint centered at `c` overlap an existing room
   that is close in Y? (XZ AABB overlap AND within one room-height in Y is a
   real collision; far apart in Y is fine — the walkway just becomes stairs.) */
static sol_bool root_spot_occupied(AppState *st, vec3 c, float half) {
    sol_u32 i;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o  = &st->scene.objects[i];
        const char  *rt = scene_meta_get(&st->scene, o->handle, "room_type");
        float        e;
        vec3         p;
        if (!rt) continue;
        if (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0) continue;
        e = room_half_extent(&st->scene, o->handle);
        p = object_world_pos(&st->scene, o->handle);
        if ((c.y > p.y ? c.y - p.y : p.y - c.y) >= 5.0f) continue;   /* clear in Y (room height 4.5 + 0.5 gap) */
        if (c.x + half < p.x - e || c.x - half > p.x + e) continue;  /* clear in X */
        if (c.z + half < p.z - e || c.z - half > p.z + e) continue;  /* clear in Z */
        return SOL_TRUE;                                             /* overlaps */
    }
    return SOL_FALSE;
}

/* The palette-prompt callback for "New root...": build a floating mirror room
   for `path`, east of home, and fill it with that directory's file/folder cards.
   Reaching it is by fly (F) for now; Phase 3 generates the walkway. */
static void create_root_from_path(AppState *st, const char *path) {
    Mesh       empty = {0};
    sol_u32    home = 0, root, shell, i;
    int        mirror_count = 0, changed;
    float      room_p[8];
    vec3       home_pos, pos;
    Material   stone = material_default();
    const char *slash, *name;
    const char *ws;

    if (path == NULL || path[0] == '\0') return;
    ws = st->scene.active_ws[0] ? st->scene.active_ws : "home";

    /* find the home room + count existing mirror rooms (placement spreads east) */
    for (i = 0; i < st->scene.count; i++) {
        sol_u32     h  = st->scene.objects[i].handle;
        const char *rt = scene_meta_get(&st->scene, h, "room_type");
        if (!rt) continue;
        if (strcmp(workspace_of(&st->scene, h), ws) != 0) continue;   /* this workspace only */
        if      (strcmp(rt, "home")   == 0) home = h;
        else if (strcmp(rt, "mirror") == 0) mirror_count++;
    }
    home_pos = (home != 0) ? object_world_pos(&st->scene, home)
                           : vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    {
        int   slot  = mirror_count % ROOT_RING_SLOTS;   /* slot within the ring */
        int   turn  = mirror_count / ROOT_RING_SLOTS;   /* outer rings as it fills */
        float ang   = (float)slot * (6.2831853f / (float)ROOT_RING_SLOTS);
        float r     = 16.0f + (float)turn * 12.0f;
        int   guard = 0;
        pos = vec3_make(home_pos.x + r * (float)cos((double)ang),
                        home_pos.y,
                        home_pos.z + r * (float)sin((double)ang));
        while (root_spot_occupied(st, pos, 5.0f) && guard < 20) {
            pos.y += 5.0f;                          /* go vertical to clear */
            guard++;
        }
    }

    slash = strrchr(path, '/');
    name  = (slash && slash[1]) ? slash + 1 : path;

    /* the root: a floating mirror room (open-topped 10x10) */
    root = scene_add(&st->scene, 0, empty, pos, quat_identity(),
                     vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(&st->scene, root, "room_type",   "mirror");
    scene_meta_set(&st->scene, root, "source_path", path);
    scene_meta_set(&st->scene, root, "name",        name);
    scene_meta_set(&st->scene, root, "workspace",   ws);

    shell = scene_add(&st->scene, root, empty, vec3_make(0.0f, 0.0f, 0.0f),
                      quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, shell, "room");
    room_p[0] = 10.0f; room_p[1] = 10.0f; room_p[2] = 4.5f;   /* timber halls: tall */
    room_p[3] = 1.0f;  room_p[4] = 1.0f;  room_p[5] = 1.0f;  room_p[6] = 1.0f;
    room_p[7] = 0.0f;
    scene_mesh_params_set(&st->scene, shell, room_p, 8);
    stone.base_color = vec3_make(0.55f, 0.53f, 0.50f);
    stone.roughness  = 0.92f;
    scene_material_set(&st->scene, shell, stone);

    changed = room_mirror_scan(&st->scene, root, path);   /* files -> cards */

    if (home != 0) {
        sol_u32 wk = scene_add(&st->scene, 0, empty, vec3_make(0.0f, 0.0f, 0.0f),
                               quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&st->scene, wk, "walkway");
        scene_rel_add(&st->scene, wk, "connects", home);
        scene_rel_add(&st->scene, wk, "connects", root);
        scene_meta_set(&st->scene, wk, "workspace", ws);
    }

    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    scene_save(&st->scene, "scene.stml");

    if (changed < 0)
        printf("new root: couldn't open '%s' (empty room created)\n", path);
    else
        printf("new root '%s': %d item(s)\n", name, changed);
}

/* Palette command: prompt for a directory path, then build a root room for it. */
static void cmd_new_root(AppState *st) {
    palette_prompt(&st->palette, "root path", create_root_from_path);
}

/* (re)build the translucent ghost mesh for the current catalog item. */
static void place_realize_ghost(AppState *st) {
    MeshBuilder mb;
    mesh_destroy(&st->place_ghost);
    mb_init(&mb);
    if (mesh_ref_build(furniture_catalog_name(st->place_index), (const float *)0, 0, &mb))
        st->place_ghost = mesh_from_builder(&mb);
    mb_free(&mb);
}

/* palette_prompt callback: store the typed line as the bookshelf's label. */
static void place_set_label(AppState *st, const char *label) {
    if (st->place_label_target != 0 && label && label[0])
        scene_meta_set(&st->scene, st->place_label_target, "label", label);
    st->place_label_target = 0;
    scene_save(&st->scene, "scene.stml");
}

/* drop the previewed furniture as a real object, tagged into the active
   workspace; a bookshelf then prompts for its label. */
static void place_confirm(AppState *st) {
    Mesh     empty;
    vec3     pos = carry_place_point(st);
    quat     rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), st->place_yaw);
    const char *kind = furniture_catalog_name(st->place_index);
    sol_u32  h;
    memset(&empty, 0, sizeof empty);
    h = scene_add(&st->scene, 0, empty, pos, rot, vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&st->scene, h, kind);
    scene_meta_set(&st->scene, h, "name", kind);
    scene_meta_set(&st->scene, h, "workspace",
                   st->scene.active_ws[0] ? st->scene.active_ws : "home");
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    st->place_active = SOL_FALSE;
    mesh_destroy(&st->place_ghost);
    if (furniture_is_shelf(kind)) {
        st->place_label_target = h;
        palette_prompt(&st->palette, "bookshelf label", place_set_label);
    } else {
        scene_save(&st->scene, "scene.stml");
    }
    printf("placed %s\n", kind);
}

static void cmd_place_furniture(AppState *st) {
    st->place_active = SOL_TRUE;
    st->place_index  = 0;
    st->place_yaw    = 0.0f;
    place_realize_ghost(st);
    printf("place mode: [ ] cycle, , . rotate, Enter place, Esc cancel\n");
}

/* the world position + facing for an outbound gate placed in front of the
   player. The gate faces the player (yaw + PI) so you walk INTO it. */
static void outbound_gate_placement(AppState *st, vec3 *pos, float *yaw) {
    vec3  f = camera_forward(&st->camera);
    float fl = (float)sqrt((double)(f.x * f.x + f.z * f.z));
    vec3  fwd = (fl > 1e-4f) ? vec3_make(f.x / fl, 0.0f, f.z / fl)
                             : vec3_make(0.0f, 0.0f, 1.0f);
    pos->x = st->camera.pos.x + fwd.x * 3.0f;
    pos->z = st->camera.pos.z + fwd.z * 3.0f;
    pos->y = st->camera.pos.y - CAMERA_EYE_HEIGHT;     /* floor level */
    *yaw   = st->camera.yaw + (float)SOL_PI;            /* face the player */
}

/* "New workspace <name>": a fresh empty home world + a linked gate pair. */
static void create_workspace_from_name(AppState *st, const char *name) {
    vec3  gpos, hroom;
    float gyaw;
    const char *cur = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    if (!name || name[0] == '\0') { printf("workspace: empty name\n"); return; }
    if (workspace_anchor_find(&st->scene, name) != 0 ||
        strcmp(name, cur) == 0) { printf("workspace '%s' already exists\n", name); return; }
    workspace_anchor_add(&st->scene, name);
    /* the new world's home room — at the same origin as the base home. The
       overlap is harmless: only the active workspace is ever shown/collided,
       so the filter keeps the two worlds from ever being seen together. */
    hroom = vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    workspace_add_home_room(&st->scene, name, hroom);
    outbound_gate_placement(st, &gpos, &gyaw);
    /* return gate sits just inside the new home room, facing into it */
    workspace_link(&st->scene, cur, gpos, gyaw,
                   name, vec3_make(hroom.x, hroom.y, hroom.z + 3.0f), 0.0f);
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    scene_save(&st->scene, "scene.stml");
    printf("new workspace '%s' — step through the gate\n", name);
}

static void cmd_new_workspace(AppState *st) {
    palette_prompt(&st->palette, "new workspace name", create_workspace_from_name);
}

/* "Portal to <name>": a gate to an EXISTING workspace. */
static void portal_to_named(AppState *st, const char *name) {
    vec3  gpos, hroom; float gyaw;
    const char *cur = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    if (!name || name[0] == '\0') return;
    if (workspace_anchor_find(&st->scene, name) == 0 && strcmp(name, "home") != 0) {
        printf("no workspace '%s'\n", name); return;
    }
    if (strcmp(name, cur) == 0) { printf("already in '%s'\n", name); return; }
    outbound_gate_placement(st, &gpos, &gyaw);
    /* the return gate lands near the target's home room (or origin if unknown) */
    {
        sol_u32 hr = 0, i;
        for (i = 0; i < st->scene.count; i++) {
            if (strcmp(workspace_of(&st->scene, st->scene.objects[i].handle), name) == 0 &&
                scene_meta_get(&st->scene, st->scene.objects[i].handle, "room_type")) {
                hr = st->scene.objects[i].handle; break;
            }
        }
        hroom = hr ? object_world_pos(&st->scene, hr) : vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    }
    workspace_link(&st->scene, cur, gpos, gyaw,
                   name, vec3_make(hroom.x, hroom.y, hroom.z + 3.0f), 0.0f);
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    scene_save(&st->scene, "scene.stml");
    printf("portal to '%s' opened\n", name);
}

static void cmd_portal_to(AppState *st) {
    palette_prompt(&st->palette, "portal to workspace", portal_to_named);
}

/* frame all rooms for the editor's entry vantage: centroid + farthest-room
   radius (with margin). */
static void editor_frame_rooms(AppState *st, vec3 *center, float *radius) {
    sol_u32 i;
    int     n = 0;
    vec3    c = vec3_make(0.0f, 0.0f, 0.0f);
    float   maxd = 0.0f;
    for (i = 0; i < st->scene.count; i++) {
        sol_u32 h = st->scene.objects[i].handle;
        if (!scene_meta_get(&st->scene, h, "room_type")) continue;
        c = vec3_add(c, object_world_pos(&st->scene, h));
        n++;
    }
    if (n > 0) c = vec3_scale(c, 1.0f / (float)n);
    else       c = vec3_make(0.0f, HOME_FLOOR_Y, 0.0f);
    for (i = 0; i < st->scene.count; i++) {
        sol_u32 h = st->scene.objects[i].handle;
        vec3    p;
        float   dx, dz, dd;
        if (!scene_meta_get(&st->scene, h, "room_type")) continue;
        p  = object_world_pos(&st->scene, h);
        dx = p.x - c.x; dz = p.z - c.z;
        dd = (float)sqrt((double)(dx * dx + dz * dz));
        if (dd > maxd) maxd = dd;
    }
    *center = c;
    *radius = maxd + 14.0f;
}

/* G / palette: toggle the top-down editor. The camera enter/exit + cursor mode
   happen in read_input on the active edge (it has the GLFWwindow). */
static void cmd_toggle_editor(AppState *st) {
    st->editor.active = (sol_bool)!st->editor.active;
}

static sol_bool can_disconnect(AppState *st) {
    return (sol_bool)(st->editor.active && st->editor.selected_wk != 0);
}

/* palette: remove the selected walkway. */
static void cmd_disconnect(AppState *st) {
    editor_delete_selected(&st->editor, &st->scene);
}

/* I / palette: open the inventory screen (the bag). */
static void cmd_inventory_open(AppState *st) {
    st->inv_page = 0;
    st->inv_open = SOL_TRUE;
}

/* palette: show/hide the upper-left stats card. */
static void cmd_toggle_hud(AppState *st) {
    st->show_hud = (sol_bool)!st->show_hud;
}

/* palette (top-down): drop a FREE room — a "mirror" room with NO source_path,
   so it is inert to the filesystem (no scan/sync) yet a first-class editor
   footprint (move/resize/connect). Lands on the floor plane under the screen
   centre, then drag it into place. */
static void cmd_add_room(AppState *st) {
    float       aspect = (st->fb_height > 0) ? (float)st->fb_width / (float)st->fb_height : 1.0f;
    Ray         r   = camera_ray(&st->camera, 0.0f, 0.0f, aspect);
    float       t, fy = HOME_FLOOR_Y;
    vec3        pos, zero = vec3_make(0.0f, 0.0f, 0.0f), one = vec3_make(1.0f, 1.0f, 1.0f);
    quat        qid = quat_identity();
    Mesh        empty;
    sol_u32     room, shell;
    float       p[8];
    const char *ws = st->scene.active_ws[0] ? st->scene.active_ws : "home";
    if (ray_vs_plane(r, vec3_make(0.0f, fy, 0.0f), vec3_make(0.0f, 1.0f, 0.0f), &t) && t > 0.0f)
        pos = vec3_add(r.origin, vec3_scale(r.dir, t));
    else
        pos = vec3_add(st->camera.pos, vec3_scale(camera_forward(&st->camera), 6.0f));
    pos.y = fy;
    memset(&empty, 0, sizeof empty);
    room = scene_add(&st->scene, 0, empty, pos, qid, one);
    scene_meta_set(&st->scene, room, "room_type", "mirror");  /* mirrors nothing = a free room */
    scene_meta_set(&st->scene, room, "name", "room");
    scene_meta_set(&st->scene, room, "workspace", ws);
    shell = scene_add(&st->scene, room, empty, zero, qid, one);  /* room invalidated; use handles */
    scene_mesh_ref_set(&st->scene, shell, "room");
    p[0] = 8.0f; p[1] = 8.0f; p[2] = 4.5f; p[3] = 1.0f;   /* timber halls: tall */
    p[4] = 1.0f; p[5] = 1.0f; p[6] = 1.0f; p[7] = 0.0f;
    scene_mesh_params_set(&st->scene, shell, p, 8);
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    bvh_refresh(st);
    scene_save(&st->scene, "scene.stml");
    printf("added a free room\n");
}

/* --- window placement -------------------------------------------- */

#define WINDOW_DEF_W    1.2f
#define WINDOW_DEF_H    1.4f
#define WINDOW_DEF_SILL 0.9f
/* WINDOW_FRAME_W lives in mesh.h (shared with make_window_glass) */

/* Rebuild one room's shell + frame, re-collecting its door openings (via a
   self-contained route_all pass) and any child window objects. Mirrors the
   connections_rebuild block exactly — same flag expressions, same ceiling
   guard condition. */
static void room_rebuild_one(AppState *st, sol_u32 room) {
    Scene  *s = &st->scene;
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *shell = &s->objects[i];
        RoomOpening  ops[ROOM_OPENINGS_CAP];
        int          no;
        float        w, d, h;
        MeshBuilder  mb;
        if (shell->parent != room || !shell->mesh_ref ||
            strcmp(shell->mesh_ref, "room") != 0) continue;
        w  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "w");
        d  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "d");
        h  = mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "h");
        no = route_room_openings(s, room, ops, ROOM_OPENINGS_CAP);
        room_append_windows(s, room, ops, &no, ROOM_OPENINGS_CAP);
        mesh_destroy(&shell->mesh);
        mb_init(&mb);
        make_room_doored(&mb, w, d, h, ROUTE_WALL_T,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "wn") > 0.5f,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "we") > 0.5f,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ws") > 0.5f,
                         mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ww") > 0.5f,
                         (g_dark_wood.albedo_tex.id == 0 &&
                          mesh_ref_param("room", shell->mesh_params, shell->mesh_param_count, "ceil") > 0.5f),
                         ops, no);
        if (mb.index_count > 0) shell->mesh = mesh_from_builder(&mb);
        mb_free(&mb);
        room_frame_build(shell, ops, no);
        break;
    }
}

/* Place a window on the wall in front of the camera (nearest room wall the
   look-ray hits). The object is parented to the room, center-origin, with
   the window local +z pointing into the room interior. */
static void cmd_place_window(AppState *st) {
    Scene   *s = &st->scene;
    Ray      ray;
    sol_u32  best_room = 0, h = 0;
    int      best_wall = 0;
    vec3     best_center;
    float    best_t = 1e30f;
    sol_u32  i;
    float    hw = WINDOW_DEF_W * 0.5f + WINDOW_FRAME_W;
    float    hh = WINDOW_DEF_H * 0.5f + WINDOW_FRAME_W;

    best_center = vec3_make(0.0f, 0.0f, 0.0f);
    ray.origin  = st->camera.pos;
    ray.dir     = camera_forward(&st->camera);

    for (i = 0; i < s->count; i++) {
        SceneObject *room = &s->objects[i];
        const char  *rt;
        RoomRect     r;
        float        ceil_y, tt;
        int          wall;
        vec3         center;
        vec3         diff;
        rt = scene_meta_get(s, room->handle, "room_type");
        if (!rt || (strcmp(rt, "home") != 0 && strcmp(rt, "mirror") != 0)) continue;
        if (!scene_object_active(s, room->handle)) continue;
        r      = editor_room_rect(s, room->handle);
        ceil_y = r.floor_y + room_interior_height(s, room->handle);
        if (!descend_wall_mount(r, ray, ceil_y, hw, hh, 0.0f, &wall, &center)) continue;
        diff = vec3_sub(center, ray.origin);
        tt   = vec3_dot(diff, diff);   /* squared dist — monotone, fine for min */
        if (tt < best_t) {
            best_t      = tt;
            best_room   = room->handle;
            best_wall   = wall;
            best_center = center;
        }
    }
    if (best_room == 0) { printf("no wall in front to place a window\n"); return; }

    /* descend_wall_mount (t=0) lands the centre on the INTERIOR wall plane, so the
       frame straddles it half-proud into the room. Push the centre OUTWARD (away
       from the interior, into the wall slab) by half the wall thickness so the
       0.20-thick frame centres in the slab. Outward = -inward; inward normals
       (descend_wall_mount): N +z, E -x, S -z, W +x. The along-wall coordinate
       (x for N/S, z for E/W) is untouched, so the shell hole position is unchanged. */
    switch (best_wall) {
        case ROOM_WALL_N: best_center.z -= ROUTE_WALL_T * 0.5f; break;  /* outward -z */
        case ROOM_WALL_E: best_center.x += ROUTE_WALL_T * 0.5f; break;  /* outward +x */
        case ROOM_WALL_S: best_center.z += ROUTE_WALL_T * 0.5f; break;  /* outward +z */
        case ROOM_WALL_W: best_center.x -= ROUTE_WALL_T * 0.5f; break;  /* outward -x */
        default: break;
    }

    {
        static const float wyaw[4] = { 0.0f, -90.0f, 180.0f, 90.0f };
        RoomRect br = editor_room_rect(s, best_room);
        quat  rot   = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f),
                                           sol_radians(wyaw[best_wall]));
        vec3  one   = vec3_make(1.0f, 1.0f, 1.0f);
        vec3  lp;
        Mesh  empty;
        float p[5];
        char  wbuf[8];
        /* keep the aim-derived along-wall X/Z; snap only Y to the default sill
           so the window center sits at floor + sill + h/2 (matches
           room_append_windows' sill = pos.y - h/2). User drags to adjust. */
        best_center.y = br.floor_y + WINDOW_DEF_SILL + WINDOW_DEF_H * 0.5f;
        lp = scene_world_to_local(s, best_room, best_center);
        memset(&empty, 0, sizeof empty);
        p[0] = WINDOW_DEF_W;
        p[1] = WINDOW_DEF_H;
        p[2] = ROUTE_WALL_T;
        p[3] = WINDOW_FRAME_W;
        p[4] = 0.0f;                     /* style: plain */
        h = scene_add(s, best_room, empty, lp, rot, one);
        scene_kind_set(s, h, KIND_PLAIN);
        scene_mesh_ref_set(s, h, "window");
        scene_mesh_params_set(s, h, p, 5);
        snprintf(wbuf, sizeof wbuf, "%d", best_wall);
        scene_meta_set(s, h, "wall", wbuf);
        scene_meta_set(s, h, "glass", "none");
        mint_tag_ws(st, h);
        scene_resolve_meshes(s);
        room_rebuild_one(st, best_room);
        st->selected_handle = h;
        scene_save(s, "scene.stml");
        printf("placed window on wall %d\n", best_wall);
    }
}

static sol_bool can_place_window(AppState *st) {
    return (sol_bool)(st->board_view == 0 && st->reader_state == READER_IDLE);
}

/* forward declaration — defined later in main.c (after the board-card helpers) */
static void delete_board_card(AppState *st, sol_u32 h);

/* ---------- window glass color presets (Task 7) -------------------------------- */
static const struct { const char *name; float r, g, b; } WINDOW_GLASS[] = {
    { "none",  0.00f, 0.00f, 0.00f },   /* open hole — no pane */
    { "clear", 0.85f, 0.92f, 1.00f },
    { "blue",  0.25f, 0.45f, 0.85f },
    { "green", 0.30f, 0.70f, 0.45f },
    { "amber", 0.95f, 0.70f, 0.25f },
    { "red",   0.85f, 0.25f, 0.30f }
};
#define WINDOW_GLASS_N ((int)(sizeof WINDOW_GLASS / sizeof WINDOW_GLASS[0]))

/* find a window's existing glass-pane child (mesh_ref "window_glass"), else 0. */
static sol_u32 window_glass_child(Scene *s, sol_u32 win) {
    int i;
    for (i = 0; i < (int)s->count; i++)
        if (s->objects[i].parent == win && s->objects[i].mesh_ref &&
            strcmp(s->objects[i].mesh_ref, "window_glass") == 0)
            return s->objects[i].handle;
    return 0;
}

/* find a window's existing oak inner-fill child (mesh_ref "window_fill"), else 0. */
static sol_u32 window_fill_child(Scene *s, sol_u32 win) {
    int i;
    for (i = 0; i < (int)s->count; i++)
        if (s->objects[i].parent == win && s->objects[i].mesh_ref &&
            strcmp(s->objects[i].mesh_ref, "window_fill") == 0)
            return s->objects[i].handle;
    return 0;
}

/* Keep a window's shaped oak FILL child in sync with its style. Shaped windows
   (arched/pointed/circular) carry a "window_fill" child — oak_veneer, distinct
   from the dark_wood casing; plain/french carry none. CREATE when a plain
   window becomes shaped, UPDATE the {w,h,t,fw,style} params when a shaped
   window's size/style changes (the registry-shared rebuild: re-key, release the
   old shape, drop the borrow), REMOVE when a shaped window becomes plain/french.
   Does NOT resolve meshes — every caller already does. Re-fetches any
   SceneObject* across scene_add (realloc). */
static void ensure_window_fill(AppState *st, sol_u32 win) {
    Scene       *s  = &st->scene;
    SceneObject *wo = scene_get(s, win);
    sol_u32      child;
    float        fp[5];
    int          shaped, si;
    if (!wo || !wo->mesh_ref || strcmp(wo->mesh_ref, "window") != 0) return;
    fp[0] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "w");
    fp[1] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "h");
    fp[2] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "t");
    fp[3] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "fw");
    fp[4] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "style");
    si     = (int)(fp[4] + 0.5f);
    shaped = (si == 1 || si == 2 || si == 3);
    child  = window_fill_child(s, win);
    if (!shaped) {                          /* plain/french: no fill */
        if (child) delete_board_card(st, child);
        return;
    }
    if (child == 0) {                       /* CREATE */
        vec3 one = vec3_make(1.0f, 1.0f, 1.0f);
        Mesh empty;
        memset(&empty, 0, sizeof empty);
        child = scene_add(s, win, empty, vec3_make(0.0f, 0.0f, 0.0f),
                          quat_identity(), one);
        scene_kind_set(s, child, KIND_PLAIN);
        scene_mesh_ref_set(s, child, "window_fill");
        scene_mesh_params_set(s, child, fp, 5);
    } else {                                /* UPDATE (registry-shared rebuild) */
        SceneObject *co    = scene_get(s, child);
        char         oldkey[160];
        sol_bool     keyed = co ? mesh_asset_key(co, oldkey) : SOL_FALSE;
        scene_mesh_params_set(s, child, fp, 5);
        if (keyed) asset_release(&g_mesh_assets, oldkey);
        co = scene_get(s, child);
        if (co) memset(&co->mesh, 0, sizeof co->mesh);
    }
}

/* Migrate every placed window to the standalone oak fill child. Gather the
   window handles first (ensure_window_fill scene_adds, which reallocs the
   objects array), then ensure each. A window saved before this feature had its
   fill baked into the casing mesh — make_window no longer emits it, so this is
   what gives those windows back their oak spandrel. Run on BOTH the rebuild
   path (world_rebuild) and the load path (load_palace), each BEFORE that
   function's scene_resolve_meshes so the new fill children get their meshes. */
static void windows_migrate_fills(AppState *st) {
    sol_u32 wins[256];
    int     nw = 0, i;
    for (i = 0; i < (int)st->scene.count && nw < 256; i++)
        if (st->scene.objects[i].mesh_ref &&
            strcmp(st->scene.objects[i].mesh_ref, "window") == 0)
            wins[nw++] = st->scene.objects[i].handle;
    for (i = 0; i < nw; i++) ensure_window_fill(st, wins[i]);
}

/* after a window's opening was resized, re-size its glass pane to match (the
   registry-shared rebuild: release the OLD shape by its old key, clear the
   borrow, re-resolve — never mesh_destroy a shared shape). The pane is
   center-origin at the window's local (0,0,0), so only its {w,h} change. */
static void window_glass_resize(AppState *st, sol_u32 win) {
    Scene       *s     = &st->scene;
    sol_u32      child = window_glass_child(s, win);
    SceneObject *wo    = scene_get(s, win);
    SceneObject *co    = child ? scene_get(s, child) : (SceneObject *)0;
    float        gp[3];
    char         oldkey[160];
    sol_bool     keyed;
    if (!wo || !co) return;
    gp[0] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "w");
    gp[1] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "h");
    gp[2] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "style");
    keyed = mesh_asset_key(co, oldkey);          /* key from the OLD params */
    scene_mesh_params_set(s, child, gp, 3);
    if (keyed) asset_release(&g_mesh_assets, oldkey);
    co = scene_get(s, child);
    if (co) memset(&co->mesh, 0, sizeof co->mesh);   /* drop the borrow */
    scene_resolve_meshes(s);
}

/* set a window's glass color preset by name: "none" removes the pane; any other
   adds/retints a window_glass child sized to the opening. */
static void window_set_glass(AppState *st, sol_u32 win, const char *name) {
    Scene       *s     = &st->scene;
    sol_u32      child = window_glass_child(s, win);
    SceneObject *wo    = scene_get(s, win);
    int          pi, i;
    if (!wo) return;
    scene_meta_set(s, win, "glass", name);
    if (strcmp(name, "none") == 0) {
        if (child) delete_board_card(st, child);
        return;
    }
    pi = 0;   /* unknown name falls back to preset 0; callers only pass valid names */
    for (i = 0; i < WINDOW_GLASS_N; i++)
        if (strcmp(WINDOW_GLASS[i].name, name) == 0) { pi = i; break; }
    if (child == 0) {
        float w    = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "w");
        float h    = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "h");
        float gp[3];
        vec3  one  = vec3_make(1.0f, 1.0f, 1.0f);
        Mesh  empty;
        gp[0] = w;
        gp[1] = h;
        gp[2] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "style");
        memset(&empty, 0, sizeof empty);
        child = scene_add(s, win, empty, vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
        scene_kind_set(s, child, KIND_PLAIN);
        scene_mesh_ref_set(s, child, "window_glass");
        scene_mesh_params_set(s, child, gp, 3);
        scene_resolve_meshes(s);
    }
    {
        SceneObject *co = scene_get(s, child);
        if (co) {
            co->material = material_default();
            co->material.base_color = vec3_make(
                WINDOW_GLASS[pi].r, WINDOW_GLASS[pi].g, WINDOW_GLASS[pi].b);
        }
    }
}

static const char *WINDOW_STYLE_NAME[5] = {
    "plain", "arched", "pointed", "circular", "french"
};
#define WINDOW_STYLE_N ((int)(sizeof WINDOW_STYLE_NAME / sizeof WINDOW_STYLE_NAME[0]))

/* Rewrite the style param on BOTH the window frame (5 params) and its glass
   child (3 params) via the registry-rebuild law.  The wall opening is unchanged
   so no wall rebuild is needed.  The glass child's params are {w, h, style}
   where w/h are copied from the FRAME (the pane always tracks the opening's
   dimensions), so the glass never reads its own w/h here. */
static void window_set_style(AppState *st, sol_u32 win, int style) {
    Scene       *s     = &st->scene;
    sol_u32      child = window_glass_child(s, win);
    SceneObject *wo    = scene_get(s, win);
    float        wp[5], gp[3];
    char         wkey[160], gkey[160];
    sol_bool     wkeyed, gkeyed;
    if (!wo) return;
    wp[0] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "w");
    wp[1] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "h");
    wp[2] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "t");
    wp[3] = mesh_ref_param("window", wo->mesh_params, wo->mesh_param_count, "fw");
    wp[4] = (float)style;
    wkeyed = mesh_asset_key(wo, wkey);
    scene_mesh_params_set(s, win, wp, 5);
    if (wkeyed) asset_release(&g_mesh_assets, wkey);
    wo = scene_get(s, win);
    if (wo) memset(&wo->mesh, 0, sizeof wo->mesh);
    if (child) {
        SceneObject *co = scene_get(s, child);
        gp[0] = wp[0]; gp[1] = wp[1]; gp[2] = (float)style;
        gkeyed = co ? mesh_asset_key(co, gkey) : SOL_FALSE;
        scene_mesh_params_set(s, child, gp, 3);
        if (gkeyed) asset_release(&g_mesh_assets, gkey);
        co = scene_get(s, child);
        if (co) memset(&co->mesh, 0, sizeof co->mesh);
    }
    ensure_window_fill(st, win);   /* add/update/remove the oak fill child to match the new style */
    scene_resolve_meshes(s);
}

/* Note: 'N' (note card) and 'Z' (abbey) stay inline, not in the registry:
   N's body needs the GLFW window (pick_ray for cursor placement); Z is a
   fixed-parameter scene compositor, not a generic mint. */
static Command g_commands[] = {
    { "Toggle bloom",                "K", GLFW_KEY_K, cmd_toggle_bloom,      NULL,                  SOL_FALSE },
    { "Toggle walk/fly",             "F", GLFW_KEY_F, cmd_toggle_fly,        can_toggle_fly,        SOL_FALSE },
    { "Toggle ghost (no-clip)",      "X", GLFW_KEY_X, cmd_toggle_ghost,      NULL,                  SOL_FALSE },
    { "Toggle shadow-map inspector", "M", GLFW_KEY_M, cmd_toggle_shadowmap,  NULL,                  SOL_FALSE },
    { "Toggle day/night",            "`", GLFW_KEY_GRAVE_ACCENT, cmd_toggle_daynight, NULL,         SOL_FALSE },
    { "Cycle color grade",           "9", GLFW_KEY_9, cmd_cycle_grade,       NULL,                  SOL_FALSE },
    { "Toggle irradiance view",      NULL, 0,        cmd_toggle_irradiance, can_toggle_irradiance, SOL_FALSE },
    { "Toggle mute",                 NULL, 0,        cmd_toggle_mute,       NULL,                  SOL_FALSE },
    { "Inventory",                   "I",  0,        cmd_inventory_open,    NULL,                  SOL_FALSE },
    { "Cycle prefilter inspector",   "P", GLFW_KEY_P, cmd_cycle_prefilter,   can_cycle_prefilter,   SOL_FALSE },
    { "Cycle text inspector",        "T", GLFW_KEY_T, cmd_cycle_textinspect, can_cycle_textinspect, SOL_FALSE },
    { "Toggle floor-plan overlay",   "J", GLFW_KEY_J, cmd_toggle_floorplan,  NULL,                  SOL_FALSE },
    { "Rescan mirrors",              "R", GLFW_KEY_R, cmd_rescan_mirrors,    NULL,                  SOL_FALSE },
    { "Reload scene",                "L", GLFW_KEY_L, cmd_reload_scene,      NULL,                  SOL_FALSE },
    { "Spawn whiteboard",            "B", GLFW_KEY_B, cmd_mint_whiteboard,   NULL,                  SOL_FALSE },
    { "Place window",                NULL, 0,          cmd_place_window,      can_place_window,      SOL_FALSE },
    { "Cut selected cards",          NULL, 0,          cmd_cut_selection,     can_cut_selection,     SOL_FALSE },
    { "Paste cards",                 NULL, 0,          cmd_paste_cards,       can_paste_cards,       SOL_FALSE },
    { "Mint codex (book)",           "V", GLFW_KEY_V, cmd_mint_codex,        NULL,                  SOL_FALSE },
    { "Mint synth book",             NULL, 0,          cmd_mint_synth,        NULL,                  SOL_FALSE },
    { "Mint island",                 "H", GLFW_KEY_H, cmd_mint_island,       NULL,                  SOL_FALSE },
    { "Mint church",                 "U", GLFW_KEY_U, cmd_mint_church,       can_mint_church,       SOL_FALSE },
    { "Mint lantern",                "O", GLFW_KEY_O, cmd_mint_lantern,      NULL,                  SOL_FALSE },
    { "Mint pond",                   "Q", GLFW_KEY_Q, cmd_mint_pond,         NULL,                  SOL_FALSE },
    { "Mint dust emitter",           NULL, 0,          cmd_mint_dust,         NULL,                  SOL_FALSE },
    { "New workspace",               NULL, 0,          cmd_new_workspace,     NULL,                  SOL_FALSE },
    { "Portal to",                   NULL, 0,          cmd_portal_to,         NULL,                  SOL_FALSE },
    { "Carry / place selected",      "E", GLFW_KEY_E, cmd_carry_toggle,      can_carry_toggle,      SOL_FALSE },
    { "Mint fox",                    "Y", GLFW_KEY_Y, cmd_mint_fox,          NULL,                  SOL_FALSE },
    { "New root...",                 NULL, 0,          cmd_new_root,          NULL,                  SOL_FALSE },
    { "Place furniture",             NULL, 0,          cmd_place_furniture,   NULL,                  SOL_FALSE },
    { "Top-down editor",             NULL, 0,          cmd_toggle_editor,     NULL,                  SOL_FALSE },
    { "Disconnect selected",         NULL, 0,          cmd_disconnect,        can_disconnect,        SOL_FALSE },
    { "Add room",                    NULL, 0,          cmd_add_room,          NULL,                  SOL_FALSE },
    { "Toggle stats card",           NULL, 0,          cmd_toggle_hud,        NULL,                  SOL_FALSE },
    { "Campus mode",                 NULL, 0,          cmd_toggle_campus,     NULL,                  SOL_FALSE }
};

#define G_COMMAND_COUNT ((int)(sizeof g_commands / sizeof g_commands[0]))

/* add `root` and every descendant (whose parent chain reaches root) to out[]. */
static void gather_subtree(Scene *s, sol_u32 root, sol_u32 *out, int *n, int cap) {
    sol_u32 i;
    if (*n < cap) out[(*n)++] = root;
    for (i = 0; i < s->count && *n < cap; i++) {
        sol_u32  h = s->objects[i].handle, p;
        int      guard = 0;
        sol_bool desc = SOL_FALSE;
        if (h == root) continue;
        p = s->objects[i].parent;
        while (p != 0 && guard++ < 256) {
            SceneObject *o;
            if (p == root) { desc = SOL_TRUE; break; }
            o = scene_get(s, p);
            if (!o) break;
            p = o->parent;
        }
        if (desc && *n < cap) out[(*n)++] = h;
    }
}

/* Editor: delete a footprint (a terrain island OR a free room) and everything
   tied to it — its subtree, every walkway connecting to it, and (for a terrain
   island) every plot-linked church + its subtree — then rebuild and save. A
   terrain delete also re-derives the world-baked FIELD data. */
static void editor_delete_footprint(AppState *st, sol_u32 footprint) {
    sol_u32      victims[512];
    int          nv = 0, vi;
    char         nidbuf[64];   /* a nid is 26 chars; 64 is plenty */
    sol_u32      i, j;
    SceneObject *io = scene_get(&st->scene, footprint);
    sol_bool     is_terrain = (sol_bool)(io && io->mesh_ref &&
                                         strcmp(io->mesh_ref, "terrain") == 0);
    nidbuf[0] = '\0';
    if (io && io->nid) { strncpy(nidbuf, io->nid, sizeof nidbuf - 1); nidbuf[sizeof nidbuf - 1] = '\0'; }
    gather_subtree(&st->scene, footprint, victims, &nv, 512);
    if (is_terrain && nidbuf[0]) {         /* an abbey: plot-linked churches + subtrees */
        for (i = 0; i < st->scene.count; i++) {
            sol_u32     h  = st->scene.objects[i].handle;
            const char *rt = scene_meta_get(&st->scene, h, "room_type");
            const char *pl = scene_meta_get(&st->scene, h, "plot");
            if (rt && strcmp(rt, "church") == 0 && pl && strcmp(pl, nidbuf) == 0)
                gather_subtree(&st->scene, h, victims, &nv, 512);
        }
    }
    for (i = 0; i < st->scene.count; i++) {     /* walkways connecting to it */
        SceneObject *o = &st->scene.objects[i];
        if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
        for (j = 0; j < o->rel_count; j++) {
            if (strcmp(o->relations[j].type, "connects") == 0 &&
                o->relations[j].target == footprint) {
                if (nv < 512) victims[nv++] = o->handle;
                break;
            }
        }
    }
    for (vi = 0; vi < nv; vi++) scene_remove(&st->scene, victims[vi]);
    st->selected_handle    = 0;
    st->editor.room        = 0;
    st->editor.selected_wk = 0;
    if (is_terrain) { meadow_rebuild(st); forest_rebuild(st); ornament_rebuild(st); }
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    bvh_refresh(st);
    scene_save(&st->scene, "scene.stml");
    printf("deleted %s\n", is_terrain ? "an island" : "a room");
}

/* Spawn a KIND_NOTE "card": pinned to the board under the cursor, else on the
   floor ahead. Tags the active workspace, selects it, saves. Returns the handle.
   Shared by the N key and the board-view double-click. */
static sol_u32 spawn_note(AppState *st, GLFWwindow *w) {
    Mesh    empty;
    vec3    one = vec3_make(1.0f, 1.0f, 1.0f);
    vec3    blocal;
    sol_u32 board, h;
    memset(&empty, 0, sizeof empty);
    blocal = vec3_make(0.0f, 0.0f, 0.0f);
    board  = board_under_ray(st, pick_ray(st, w), &blocal);
    if (board != 0) {
        h = scene_add(&st->scene, board, empty,
                      vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
    } else {
        vec3  f = camera_forward(&st->camera);
        vec3  pos;
        float yaw;
        f.y = 0.0f;
        if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
        f   = vec3_normalize(f);
        pos = vec3_add(st->camera.pos, vec3_scale(f, 1.8f));
        pos.y = mint_ground(st, pos);  /* bottom-origin card on YOUR ground */
        yaw = atan2f(-f.x, -f.z);       /* facing you */
        h = scene_add(&st->scene, 0, empty, pos,
                      quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw), one);
    }
    scene_kind_set(&st->scene, h, KIND_NOTE);
    scene_meta_set(&st->scene, h, "name", "note");
    scene_meta_set(&st->scene, h, "workspace",
                   st->scene.active_ws[0] ? st->scene.active_ws : "home");
    scene_meta_set(&st->scene, h, "text", "press Enter to edit me");
    scene_mesh_ref_set(&st->scene, h, "card");
    {   /* notes get a roomy landscape card + a matching min height */
        float np3[3];
        char  mhb[16];
        np3[0] = NOTE_CARD_W; np3[1] = NOTE_CARD_H; np3[2] = NOTE_CARD_T;
        scene_mesh_params_set(&st->scene, h, np3, 3);
        snprintf(mhb, sizeof mhb, "%.4f", (double)NOTE_CARD_H);
        scene_meta_set(&st->scene, h, "min_h", mhb);
    }
    if (board != 0) {
        SceneObject *no = scene_get(&st->scene, h);
        if (no) no->pos = board_pin_pos(&st->scene, board, h,
                                        blocal, 0.0f, -0.5f * NOTE_CARD_H);
        board_card_tag_page(st, h);     /* belongs to the page you're viewing */
    }
    scene_resolve_meshes(&st->scene);
    apply_kind_materials(&st->scene);
    st->selected_handle = h;
    scene_save(&st->scene, "scene.stml");
    printf("note #%u spawned%s\n", (unsigned)h, board ? " on the board" : "");
    return h;
}

/* defined later (after read_input); the board-view double-click calls it */
static void note_edit_begin(AppState *st, sol_u32 handle);
/* defined later (after note_edit_begin); the note-body renderer calls it */
static int caret_build(const Font *f, const char *src, float px2m, float wrap_w,
                       CaretField *out);
/* defined later (after caret_refresh_goal) */
static int caret_hit_offset(AppState *st, GLFWwindow *w, int *out);
static int click_seq_bump(AppState *st, double mx, double my);

/* Delete one selectable board card (note/picture/folder): release its keyed
   mesh, clear transient refs, remove it. Does NOT save or rebuild arrows. */
static void delete_board_card(AppState *st, sol_u32 h) {
    SceneObject *o = scene_get(&st->scene, h);
    char akey[160];
    if (!o) return;
    if (mesh_asset_key(o, akey)) asset_release(&g_mesh_assets, akey);
    if (st->resize_board       == h) st->resize_board       = 0;
    if (st->move_board         == h) st->move_board         = 0;
    if (st->carried            == h) st->carried            = 0;
    if (st->drag_handle        == h) st->drag_handle        = 0;
    if (st->drop_target_handle == h) st->drop_target_handle = 0;
    if (st->selected_handle    == h) st->selected_handle    = 0;
    msel_remove(st->cut, &st->cut_count, h);   /* a cut card was deleted: drop it */
    scene_remove(&st->scene, h);
}

/* Write `len` bytes to library/<nid>.png and fill out_path (cap bytes) with the
   relative path. Returns SOL_TRUE on success. */
static sol_bool library_write(const unsigned char *bytes, int len,
                              char *out_path, int cap) {
    char  nid[NID_LEN + 1];
    FILE *f;
    nid_generate(nid);
    fs_mkdir("library");
    snprintf(out_path, (size_t)cap, "library/%s.png", nid);
    f = fopen(out_path, "wb");
    if (!f) return SOL_FALSE;
    if (fwrite(bytes, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return SOL_FALSE;
    }
    fclose(f);
    return SOL_TRUE;
}

/* Cmd+V in board view: paste the clipboard image -> library/<nid>.png -> a
   picture on the board at the cursor, on the current page. */
static void cmd_paste_image(AppState *st, GLFWwindow *w) {
    unsigned char *bytes = (unsigned char *)0;
    int            len   = 0;
    Image          img;
    char           path[256];
    sol_u32        board, a;
    vec3           blocal;
    if (st->board_view == 0) return;
    if (!clipboard_read_image(&bytes, &len) || !bytes || len <= 0) {
        free(bytes);   /* safe (free(NULL) is defined); guards future clipboard impls */
        printf("paste: no image on the clipboard\n");
        return;
    }
    if (!image_load_from_memory(bytes, len, &img)) {   /* validate it decodes */
        printf("paste: clipboard image not decodable\n");
        free(bytes);
        return;
    }
    image_free(&img);
    if (!library_write(bytes, len, path, (int)sizeof path)) {
        printf("paste: could not write the library file\n");
        free(bytes);
        return;
    }
    free(bytes);
    blocal = vec3_make(0.0f, 0.0f, 0.0f);
    board  = board_under_ray(st, pick_ray(st, w), &blocal);
    if (board == 0) {                                  /* cursor off the board: center */
        board  = st->board_view;
        blocal = board_local_frac(st, board, 0.0f, 0.5f);
    }
    a = spawn_image_picture(st, board, vec3_make(0.0f, 0.0f, 0.0f),
                            quat_identity(), path);     /* tags the page internally */
    {
        SceneObject *ao = scene_get(&st->scene, a);
        if (ao) {
            float ph = mesh_ref_param("picture", ao->mesh_params,
                                      ao->mesh_param_count, "h");
            ao->pos = board_pin_pos(&st->scene, board, a, blocal, 0.0f, -0.5f * ph);
        }
    }
    st->selected_handle = a;
    scene_save(&st->scene, "scene.stml");
    printf("pasted image -> %s\n", path);
}

static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {
    float    look = (float)dt * LOOK_SPEED;
    sol_bool tab_now, dragging, fp, bv_active;
    double   mx, my;

    fp = (st->camera.mode != CAMERA_ORBIT);
    /* board view (and its outbound glide) freezes walking and look — the camera
       is pinned to the framed pose while you work the surface with the cursor. */
    bv_active = (sol_bool)(st->board_view != 0 || st->bv_t < 1.0f);
    st->hover_corner = -1;   /* recomputed below in the normal (non-modal) path */

    /* inventory: release the cursor for clicking on open, restore on close
       (edge-detect, mirroring the editor's cursor toggle). Runs every frame,
       before the modal gate below early-returns. */
    if (st->inv_open && !st->inv_was_open) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        st->mouse_skip = 2;
    } else if (!st->inv_open && st->inv_was_open && !st->editor.active) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        st->mouse_skip = 2;
    }
    st->inv_was_open = st->inv_open;

    /* board view frees the cursor for pointing at cards (mirrors inventory);
       first-person look re-locks on exit. */
    if (st->board_view && !st->board_view_was) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        st->mouse_skip = 2;
    } else if (!st->board_view && st->board_view_was &&
               !st->inv_open && !st->editor.active) {
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        st->mouse_skip = 2;
    }
    st->board_view_was = (sol_bool)(st->board_view != 0);

    /* the synth book frees the cursor for pointing at page widgets (mirrors the
       inventory toggle); first-person look re-locks on close. */
    {
        sol_bool app_now = reader_is_app(st);
        if (app_now && !st->reader_app_was) {
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            st->mouse_skip = 2;
        } else if (!app_now && st->reader_app_was &&
                   !st->inv_open && !st->editor.active) {
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            st->mouse_skip = 2;
        }
        st->reader_app_was = app_now;
    }

    /* FOCUS (item 8 piece 5): while a note is open for typing, the keyboard
       is TEXT — chars route to the note (on_char/on_key) and every button
       below goes quiet, so 'w' writes a letter instead of walking. A click
       blurs (saving) without also picking — the standard text-field deal:
       the first click leaves the field, the next one acts. The inventory
       screen joins this gate: it suppresses movement and click-takes items. */
    if (st->edit_handle != 0 || st->palette.open || reader_is_editing(st) ||
        st->inv_open || reader_is_app(st)) {
        in->forward = in->back = in->left = in->right = SOL_FALSE;
        in->up = in->down = SOL_FALSE;
        in->look_dx = 0.0f;
        in->look_dy = 0.0f;
        in->zoom    = 0.0f;
        st->scroll_accum = 0.0;
        glfwGetCursorPos(w, &mx, &my);
        if (st->edit_handle != 0) {     /* board-view click: place/select; drag extends; else blur */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) {            /* press */
                int seq = click_seq_bump(st, mx, my);
                int off;
                if (!caret_hit_offset(st, w, &off)) {
                    note_edit_end(st);                 /* off the note -> blur */
                } else if (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                           glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) {
                    st->edit_cursor = off;             /* shift-click: extend, keep anchor */
                    st->edit_dragging = SOL_TRUE;
                } else if (seq == 2) {                 /* double: word */
                    int s, e;
                    caret_word_at(st->edit_buf, off, &s, &e);
                    st->edit_sel_anchor = s; st->edit_cursor = e;
                    st->edit_dragging = SOL_FALSE;
                } else if (seq >= 3) {                 /* triple: all */
                    st->edit_sel_anchor = 0; st->edit_cursor = st->edit_len;
                    st->edit_dragging = SOL_FALSE;
                } else {                               /* single: caret + arm drag */
                    st->edit_cursor = st->edit_sel_anchor = off;
                    st->edit_dragging = SOL_TRUE;
                }
            } else if (lmb && st->lmb_was_down && st->edit_dragging) {   /* drag extends */
                int off;
                if (caret_hit_offset(st, w, &off)) st->edit_cursor = off;
            } else if (!lmb) {
                st->edit_dragging = SOL_FALSE;         /* release */
            }
            st->lmb_was_down = lmb;
        } else if (reader_is_editing(st)) {  /* click closes the book (saves) */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) reader_close(st);
            st->lmb_was_down = lmb;
        } else if (st->inv_open) {     /* click a tile to take it; arrows page */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) {
                int     ww, wh, n, slot;
                float   scale, px, py, rx, ry, rw, rh;
                sol_u32 items[INV_THUMB_CAP];
                n = inventory_collect(st, items, INV_THUMB_CAP);
                glfwGetWindowSize(w, &ww, &wh);
                scale = (ww > 0) ? (float)st->fb_width / (float)ww : 1.0f;
                px = (float)mx * scale;
                py = (float)my * scale;
                inv_prev_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
                if (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh) {
                    st->inv_page = inv_clamp_page(st->inv_page - 1, n, INV_PER_PAGE);
                } else {
                    inv_next_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
                    if (px >= rx && px <= rx + rw && py >= ry && py <= ry + rh) {
                        st->inv_page = inv_clamp_page(st->inv_page + 1, n, INV_PER_PAGE);
                    } else {
                        slot = inv_hit_slot(px, py, INV_COLS, INV_ROWS,
                                            st->fb_width, st->fb_height);
                        if (slot >= 0) {
                            int idx = st->inv_page * INV_PER_PAGE + slot;
                            if (idx < n) inventory_take(st, items[idx]);
                        }
                    }
                }
            }
            st->lmb_was_down = lmb;
        } else if (reader_is_app(st)) {     /* the synth book's widget UI */
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            synth_book_input(st, w, lmb);
            st->lmb_was_down = lmb;
        }
        st->mouse_last_x = mx;       /* keep tracking: no look-jump on blur */
        st->mouse_last_y = my;
        return;
    }

    /* movement (held; ignored by the camera in orbit) */
    in->forward = glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS;
    in->back    = glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS;
    in->left    = glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS;
    in->right   = glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS;
    in->up      = glfwGetKey(w, GLFW_KEY_SPACE)        == GLFW_PRESS;
    in->down    = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    if (bv_active) {
        in->forward = in->back = in->left = in->right = SOL_FALSE;
        in->up = in->down = SOL_FALSE;
    }

    /* keyboard look (held -> a rate -> dt-scaled). While READING, the
       arrows turn page-pairs instead (edge-triggered); the mouse still
       looks, so the book stays facing where you opened it. */
    in->look_dx = 0.0f;
    in->look_dy = 0.0f;
    if (st->reader_state == READER_OPEN && st->reader_text) {
        sol_bool lnow = glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS;
        sol_bool rnow = glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS;
        int L       = st->reader_lines_per_page;
        int pages   = (st->reader_line_count + L - 1) / L;
        int spreads = (pages + 1) / 2;
        if (st->reader_turning == 0) {          /* one leaf in flight at a time */
            if (rnow && !st->arrow_r_was && st->reader_spread + 1 < spreads) {
                st->reader_turn_old = st->reader_spread;
                st->reader_spread++;            /* commit; the leaf covers it */
                st->reader_turning  = 1;
                play_oneshot(g_snd_whoosh, g_snd_whoosh_frames, 0.30f, 0.0f);
                st->reader_turn_t   = 0.0f;
            }
            if (lnow && !st->arrow_l_was && st->reader_spread > 0) {
                st->reader_turn_old = st->reader_spread;
                st->reader_spread--;
                st->reader_turning  = -1;
                play_oneshot(g_snd_whoosh, g_snd_whoosh_frames, 0.30f, 0.0f);
                st->reader_turn_t   = 0.0f;
            }
        }
        st->arrow_l_was = lnow;
        st->arrow_r_was = rnow;
    } else if (!bv_active) {
        /* suppress UP/DOWN camera-look when a window is selected (Task 7 owns them) */
        sol_bool win_look_free = (sol_bool)(!window_on_wall(&st->scene, st->selected_handle));
        if (win_look_free && glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
        if (win_look_free && glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
        if (win_look_free && glfwGetKey(w, GLFW_KEY_UP)   == GLFW_PRESS) in->look_dy += look;
        if (win_look_free && glfwGetKey(w, GLFW_KEY_DOWN) == GLFW_PRESS) in->look_dy -= look;
    }

    /* Tab toggles first-person <-> orbit (edge); cursor mode follows */
    tab_now = glfwGetKey(w, GLFW_KEY_TAB) == GLFW_PRESS;
    if (tab_now && !st->tab_was_down && !st->board_view) {
        if (st->camera.mode == CAMERA_ORBIT) {
            camera_enter_fp(&st->camera);
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else {
            camera_enter_orbit(&st->camera, object_world_pos(&st->scene, st->selected_handle));  /* pivot on the selection (fallback to scene focus) */
            glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        st->mouse_skip = 2;                         /* cursor pos is discontinuous across the switch */
        fp = (st->camera.mode != CAMERA_ORBIT);
    }
    st->tab_was_down = tab_now;

    /* mouse look (displacement -> sensitivity-scaled, NOT dt-scaled). Always
       track the last position so a drag never starts with a jump; apply the
       delta in first-person (captured) or while left-dragging in orbit — but
       swallow a couple of frames after a cursor-mode change, since GLFW's
       virtual cursor position jumps (one frame later) when the mode flips. */
    glfwGetCursorPos(w, &mx, &my);
    dragging = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (st->mouse_skip > 0) {
        st->mouse_skip--;                           /* reseed only; don't apply the delta */
    } else if ((fp || (dragging && st->drag_handle == 0)) &&
               !st->editor.active && !bv_active) {
        /* in orbit, a drag that started ON an object carries it (item 4)
           instead of rotating the camera — drag_handle gates the look */
        float dx = (float)(mx - st->mouse_last_x);
        float dy = (float)(my - st->mouse_last_y);
        in->look_dx += dx * MOUSE_SENSITIVITY;
        in->look_dy -= dy * MOUSE_SENSITIVITY;      /* screen-y grows down -> negate */
    }
    st->mouse_last_x = mx;
    st->mouse_last_y = my;

    /* scroll: drain the accumulator into this frame (orbit dolly) */
    in->zoom = (float)st->scroll_accum;
    st->scroll_accum = 0.0;

    /* left button -> pick / drag (item 4). FP: a press picks + begins
       carrying through the crosshair (look around and the object follows —
       the page is excluded: clicking it means "read"). Orbit: a press ON an
       object becomes a carry once the cursor moves past the slop; a press on
       nothing rotates the camera as before; a tap still picks. A real move
       saves the scene on release — placement persists automatically (§1.2:
       no save button in a memory palace). */
    {
        sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        if (st->editor.active) {                       /* ---- editor mouse ---- */
            int    ww, wh;
            float  nx, ny, aspect;
            glfwGetWindowSize(w, &ww, &wh);
            aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
            nx = 2.0f * (float)mx / (float)ww - 1.0f;
            ny = 1.0f - 2.0f * (float)my / (float)wh;
            if (lmb && !st->lmb_was_down) {
                editor_press(&st->editor, &st->scene, &st->camera, nx, ny, aspect);
                if (st->editor.action == EDIT_IDLE) {
                    float   t;
                    sol_u32 hit = pick_at(st, w, nx, ny, &t);   /* rooms are pick-skipped: this finds a walkway */
                    st->editor.selected_wk = object_is_walkway(&st->scene, hit) ? hit : 0;
                    st->selected_handle    = st->editor.selected_wk;
                } else {
                    st->selected_handle = st->editor.room;       /* highlight the room */
                    /* a terrain RESIZE rebuilds the registry mesh on commit;
                       capture the OLD key now, before drag rewrites the params. */
                    st->editor_resize_keyed = SOL_FALSE;
                    if (st->editor.action == EDIT_RESIZE) {
                        SceneObject *ro = scene_get(&st->scene, st->editor.room);
                        if (ro && ro->mesh_ref && strcmp(ro->mesh_ref, "terrain") == 0)
                            st->editor_resize_keyed = mesh_asset_key(ro, st->editor_resize_key);
                    }
                }
            } else if (lmb && st->lmb_was_down) {
                editor_drag(&st->editor, &st->scene, &st->camera, nx, ny, aspect);
            } else if (!lmb && st->lmb_was_down) {
                editor_release(&st->editor, &st->scene, &st->camera, nx, ny, aspect);
            }
        } else {

        if (lmb && !st->lmb_was_down) {                 /* ---- press ---- */
            st->press_x = mx;
            st->press_y = my;
            if (st->reader_state != READER_IDLE) {
                reader_close(st);                       /* click-away, like blur:
                                                           this press only closes */
            } else if (fp) {
                float pnx = 0.0f, pny = 0.0f;           /* crosshair by default */
                sol_bool is_dbl = SOL_FALSE;
                if (st->board_view != 0) {              /* board view: pick at the cursor */
                    int bww, bwh;
                    glfwGetWindowSize(w, &bww, &bwh);
                    if (bww > 0 && bwh > 0) {
                        pnx = 2.0f * (float)mx / (float)bww - 1.0f;
                        pny = 1.0f - 2.0f * (float)my / (float)bwh;
                    }
                }
                do_pick(st, w, pnx, pny);               /* select on press */
                if (st->board_view != 0 && st->selected_handle == st->board_view)
                    st->selected_handle = 0;            /* clicked the board itself = deselect */
                if (st->board_view != 0)                /* shared multi-click counter */
                    is_dbl = (sol_bool)(click_seq_bump(st, mx, my) == 2);
                if (is_dbl) {                           /* navigate a folder, edit a note, or create on the board */
                    SceneObject *so = scene_get(&st->scene, st->selected_handle);
                    if (so && object_is_folder(&st->scene, st->selected_handle)) {
                        const char *link = scene_meta_get(&st->scene, so->handle, "link");
                        if (link && link[0]) {
                            scene_meta_set(&st->scene, st->board_view, "active_page", link);
                            st->selected_handle = 0;
                            scene_save(&st->scene, "scene.stml");
                        }
                    } else if (so && so->kind == KIND_NOTE) {
                        note_edit_begin(st, st->selected_handle);   /* focus only — no word-select */
                    } else if (st->selected_handle == 0) {
                        vec3 bl;
                        if (board_under_ray(st, pick_ray(st, w), &bl) != 0) {  /* over the board only */
                            sol_u32 nh = spawn_note(st, w);
                            scene_meta_set(&st->scene, nh, "text", "");        /* type into it empty */
                            note_edit_begin(st, nh);
                        }
                    }
                    /* else: double-click a non-note card -> nothing (the select stands) */
                } else if (try_connect(st, st->selected_handle)) {
                    /* the press completed a connection — no drag */
                } else if (st->board_view != 0 &&
                           (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                            glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS) &&
                           st->selected_handle != 0 &&
                           object_is_selectable(&st->scene, st->selected_handle)) {
                    sel_toggle_h(st, st->selected_handle);   /* shift-click: toggle, no drag */
                } else if (st->board_view != 0 && st->selected_handle == 0) {
                    /* empty board: begin a marquee (shift => add to the set) */
                    st->marquee_active   = SOL_TRUE;
                    st->marquee_dragging = SOL_FALSE;
                    st->marquee_add      = (sol_bool)(
                        glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                        glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
                    st->marquee_x0 = st->marquee_x1 = mx;
                    st->marquee_y0 = st->marquee_y1 = my;
                } else if (st->sel_count <= 1 && st->selected_handle != 0 &&
                           (board_is_mounted(&st->scene, st->selected_handle) ||
                            note_resizable(&st->scene, st->selected_handle) ||
                            picture_on_board(&st->scene, st->selected_handle) ||
                            window_on_wall(&st->scene, st->selected_handle)) &&
                           resize_corner_pick(st, w)) {
                    /* grabbed a corner handle — resize (single selection only) */
                } else if (st->sel_count <= 1 && st->selected_handle != 0 &&
                           picture_move_pick(st, w)) {
                    /* grabbed a picture's body — slide it (single selection only) */
                } else if (st->selected_handle != 0 && st->selected_handle != st->page_handle) {
                    /* a plain click on a card: keep an existing multi-selection it
                       belongs to (so a later drag moves the group — Task 7); otherwise
                       select just this card. */
                    if (st->board_view != 0 && st->sel_count > 1 &&
                        msel_contains(st->sel, st->sel_count, st->selected_handle)) {
                        group_drag_begin(st, w);            /* drag the whole set (M3) */
                    } else {
                        if (st->board_view != 0 &&
                            object_is_selectable(&st->scene, st->selected_handle))
                            sel_set_single(st, st->selected_handle);
                        else if (st->board_view != 0)
                            st->sel_count = 0;   /* clicked a non-card: drop any multi-
                                                    selection; keep this as a plain single
                                                    selection (selected_handle stays) */
                        drag_begin(st, w, st->selected_handle);
                    }
                }
            } else {
                float   t;
                int     ww, wh;
                sol_u32 hit;
                glfwGetWindowSize(w, &ww, &wh);
                hit = pick_at(st, w, 2.0f*(float)mx/(float)ww - 1.0f,
                                     1.0f - 2.0f*(float)my/(float)wh, &t);
                if (hit && !try_connect(st, hit))
                    drag_begin(st, w, hit);             /* candidate; selection waits for slop/tap */
            }
        }

        if (lmb && st->marquee_active) {                /* ---- marquee update ---- */
            vec3 c0, c1;
            int  mw, mh;
            glfwGetWindowSize(w, &mw, &mh);
            st->marquee_px_scale = (mw > 0) ? (float)st->fb_width / (float)mw : 1.0f;
            st->marquee_x1 = mx;
            st->marquee_y1 = my;
            if ((mx - st->marquee_x0) * (mx - st->marquee_x0) +
                (my - st->marquee_y0) * (my - st->marquee_y0) >= 25.0)
                st->marquee_dragging = SOL_TRUE;
            if (mw > 0 && mh > 0 &&
                board_ray_local(st, st->board_view, camera_ray(&st->camera,
                    2.0f * (float)st->marquee_x0 / (float)mw - 1.0f,
                    1.0f - 2.0f * (float)st->marquee_y0 / (float)mh,
                    (float)mw / (float)mh), &c0) &&
                board_ray_local(st, st->board_view, camera_ray(&st->camera,
                    2.0f * (float)st->marquee_x1 / (float)mw - 1.0f,
                    1.0f - 2.0f * (float)st->marquee_y1 / (float)mh,
                    (float)mw / (float)mh), &c1)) {
                st->marquee_lx0 = c0.x; st->marquee_ly0 = c0.y;
                st->marquee_lx1 = c1.x; st->marquee_ly1 = c1.y;
            }
        }
        if (lmb && st->move_board != 0) {               /* ---- sliding a picture ---- */
            SceneObject *o   = scene_get(&st->scene, st->move_board);
            SceneObject *par = o ? scene_get(&st->scene, st->resize_room) : (SceneObject *)0;
            if (!o || !par) {
                st->move_board = 0;
            } else {
                Ray   ray = pick_ray(st, w);
                float yaw = board_yaw(&st->scene, st->move_board);
                vec3  n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
                vec3  anchor = object_world_pos(&st->scene, st->move_board);
                const char *mr = o->mesh_ref ? o->mesh_ref : "picture";
                float pw  = mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "w");
                float ph  = mesh_ref_param(mr, o->mesh_params, o->mesh_param_count, "h");
                float tt;
                st->drop_target_handle = 0;     /* recompute the drop target each frame */
                ray.dir = vec3_normalize(ray.dir);
                if (ray_vs_plane(ray, anchor, n, &tt) && tt > 0.0f) {
                    vec3 hit = vec3_add(ray.origin, vec3_scale(ray.dir, tt));
                    vec3 P   = vec3_add(hit, st->move_grab);        /* new world origin */
                    if (o->mesh_ref && strcmp(o->mesh_ref, "window") == 0) {
                        /* center-origin window: slide along the wall + vertically,
                           the OUTER frame clamped to the wall, keeping the
                           perpendicular (in-wall) depth fixed. P is the new CENTRE
                           (move_grab kept the centre under the cursor). The wall
                           hole follows only on release (room_rebuild_one). */
                        float fw    = mesh_ref_param("window", o->mesh_params,
                                                     o->mesh_param_count, "fw");
                        float wo    = pw + 2.0f * fw;     /* outer frame width  */
                        float ho    = ph + 2.0f * fw;     /* outer frame height */
                        RoomRect r  = editor_room_rect(&st->scene, st->resize_room);
                        float ih    = room_interior_height(&st->scene, st->resize_room);
                        float perpH = (n.z * n.z > 0.25f) ? r.hd : r.hw;
                        WallMount m = wall_gable_geom(r, ih, (n.x * n.x > 0.25f));
                        vec3  wallc = vec3_sub(vec3_make(r.cx, P.y, r.cz),
                                               vec3_scale(n, perpH));
                        float noff  = vec3_dot(vec3_sub(anchor, wallc), n); /* keep in-wall depth */
                        float run   = vec3_dot(vec3_sub(P, wallc), st->resize_u);
                        float cy    = P.y;               /* origin IS the centre */
                        vec3  np;
                        wall_clamp_run_cy(m, wo * 0.5f, ho * 0.5f, &run, &cy);
                        np.x = wallc.x + st->resize_u.x * run + n.x * noff;
                        np.y = cy;
                        np.z = wallc.z + st->resize_u.z * run + n.z * noff;
                        o->pos = scene_world_to_local(&st->scene, o->parent, np);
                    } else if (par->mesh_ref && strcmp(par->mesh_ref, "board") == 0) {
                        /* on a whiteboard: slide in board-local, clamp to the face
                           (the plane is a constant board-local z, so z stays proud) */
                        vec3  lp = scene_world_to_local(&st->scene, st->resize_room, P);
                        float bw = mesh_ref_param("board", par->mesh_params, par->mesh_param_count, "w");
                        float bh = mesh_ref_param("board", par->mesh_params, par->mesh_param_count, "h");
                        float lx = bw * 0.5f - pw * 0.5f;
                        if (lx < 0.0f) lx = 0.0f;
                        if (lp.x >  lx) lp.x =  lx;
                        if (lp.x < -lx) lp.x = -lx;
                        if (lp.y < 0.0f)      lp.y = 0.0f;
                        if (lp.y > bh - ph)   lp.y = bh - ph;
                        o->pos = lp;
                        if (is_fileable_card(&st->scene, st->move_board))
                            st->drop_target_handle = folder_at_board_point(
                                st, st->resize_room,
                                vec3_make(lp.x, lp.y + ph * 0.5f, 0.0f));
                    } else {
                        /* on a room wall: slide along u + y, clamp to the wall +
                           gable, keep proud (t/2) */
                        RoomRect r  = editor_room_rect(&st->scene, st->resize_room);
                        float ih    = room_interior_height(&st->scene, st->resize_room);
                        float perpH = (n.z * n.z > 0.25f) ? r.hd : r.hw;
                        WallMount m = wall_gable_geom(r, ih, (n.x * n.x > 0.25f));
                        vec3  wallc = vec3_sub(vec3_make(r.cx, P.y, r.cz), vec3_scale(n, perpH));
                        float noff  = vec3_dot(vec3_sub(P, wallc), n);   /* the proud t/2 */
                        float run   = vec3_dot(vec3_sub(P, wallc), st->resize_u);
                        float cy    = P.y + ph * 0.5f;                   /* origin is bottom-centre */
                        vec3  np;
                        wall_clamp_run_cy(m, pw * 0.5f, ph * 0.5f, &run, &cy);
                        np.x = wallc.x + st->resize_u.x * run + n.x * noff;
                        np.y = cy - ph * 0.5f;
                        np.z = wallc.z + st->resize_u.z * run + n.z * noff;
                        o->pos = scene_world_to_local(&st->scene, o->parent, np);
                    }
                }
            }
        }
        if (lmb && st->resize_board != 0) {             /* ---- resizing ---- */
            SceneObject *o = scene_get(&st->scene, st->resize_board);
            if (!o) {
                st->resize_board = 0;                   /* object vanished */
            } else if (o->kind == KIND_NOTE) {
                /* free-standing note: drag the card's OWN front-face plane (no
                   wall). Horizontal drag sets the width (the wrap boundary);
                   vertical drag sets a MIN height; note_autosize then enforces
                   height >= wrapped content and keeps the top edge fixed. */
                Ray   ray = pick_ray(st, w);
                float yaw = board_yaw(&st->scene, st->resize_board);
                vec3  n   = vec3_make((float)sin((double)yaw), 0.0f,
                                      (float)cos((double)yaw));
                float tt;
                ray.dir = vec3_normalize(ray.dir);
                if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
                    vec3  hit = vec3_add(vec3_add(ray.origin, vec3_scale(ray.dir, tt)),
                                         st->resize_grab);   /* no jump on grab */
                    vec3  origin, cur_bottom, nb;
                    float nw, nh, p3[3];
                    float ct  = mesh_ref_param("card", o->mesh_params,
                                               o->mesh_param_count, "t");
                    char  oldkey[160], mhbuf[32];
                    sol_bool keyed;
                    board_resize_corner(st->resize_anchor, hit, st->resize_u,
                                        0.15f, 0.0f, &nw, &nh, &origin);
                    snprintf(mhbuf, sizeof mhbuf, "%.4f", (double)nh);
                    scene_meta_set(&st->scene, st->resize_board, "min_h", mhbuf);
                    /* width: release/rebuild the shared card mesh, keep height +
                       vertical; set horizontal centre from the drag's origin */
                    cur_bottom = object_world_pos(&st->scene, st->resize_board);
                    keyed = mesh_asset_key(o, oldkey);
                    p3[0] = nw;
                    p3[1] = mesh_ref_param("card", o->mesh_params,
                                           o->mesh_param_count, "h");
                    p3[2] = ct;
                    scene_mesh_params_set(&st->scene, st->resize_board, p3, 3);
                    if (keyed) asset_release(&g_mesh_assets, oldkey);
                    o = scene_get(&st->scene, st->resize_board);
                    if (o) {
                        memset(&o->mesh, 0, sizeof o->mesh);
                        nb = vec3_make(origin.x, cur_bottom.y, origin.z);
                        o->pos = scene_world_to_local(&st->scene, o->parent, nb);
                    }
                    scene_resolve_meshes(&st->scene);
                    note_autosize(st, st->resize_board);  /* height + top-anchor */
                }
            } else if (st->resize_room == 0 ||
                       scene_get(&st->scene, st->resize_room) == 0) {
                st->resize_board = 0;                   /* board/room vanished */
            } else if (object_is_board(&st->scene, st->resize_room)) {
                /* a picture on a whiteboard: resize on the board face,
                   aspect-locked, board-local, clamped to the board. */
                Ray   ray = pick_ray(st, w);
                float yaw = board_yaw(&st->scene, st->resize_board);
                vec3  n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
                float pt  = mesh_ref_param("picture", o->mesh_params, o->mesh_param_count, "t");
                float tt;
                if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
                    vec3         hit = vec3_add(vec3_add(ray.origin, vec3_scale(ray.dir, tt)),
                                                st->resize_grab);   /* no jump on grab */
                    SceneObject *par = scene_get(&st->scene, st->resize_room);
                    float cw  = mesh_ref_param("picture", o->mesh_params, o->mesh_param_count, "w");
                    float ch  = mesh_ref_param("picture", o->mesh_params, o->mesh_param_count, "h");
                    float bw  = par ? mesh_ref_param("board", par->mesh_params, par->mesh_param_count, "w") : 1.8f;
                    float bh  = par ? mesh_ref_param("board", par->mesh_params, par->mesh_param_count, "h") : 1.2f;
                    float aspect = (ch > 0.0f) ? cw / ch : 0.0f;
                    vec3  origin;
                    float nw, nh, p3[3];
                    char  oldkey[160];
                    sol_bool keyed;
                    board_resize_corner(st->resize_anchor, hit, st->resize_u,
                                        0.3f, aspect, &nw, &nh, &origin);
                    if (nw > bw) { nw = bw; if (aspect > 0.0f) nh = nw / aspect; }  /* fit the board */
                    if (nh > bh) { nh = bh; if (aspect > 0.0f) nw = nh * aspect; }
                    p3[0] = nw; p3[1] = nh; p3[2] = pt;
                    keyed = mesh_asset_key(o, oldkey);   /* registry-shared rebuild (P4 item 4) */
                    scene_mesh_params_set(&st->scene, st->resize_board, p3, 3);
                    if (keyed) asset_release(&g_mesh_assets, oldkey);
                    o = scene_get(&st->scene, st->resize_board);
                    if (o) {
                        vec3  lp;
                        float lx = bw * 0.5f - nw * 0.5f;
                        memset(&o->mesh, 0, sizeof o->mesh);     /* drop the borrow */
                        lp = scene_world_to_local(&st->scene, o->parent, origin);
                        if (lx < 0.0f) lx = 0.0f;                /* clamp to the board face */
                        if (lp.x >  lx) lp.x =  lx;
                        if (lp.x < -lx) lp.x = -lx;
                        if (lp.y < 0.0f)        lp.y = 0.0f;
                        if (lp.y > bh - nh)     lp.y = bh - nh;
                        o->pos = lp;
                    }
                    scene_resolve_meshes(&st->scene);
                }
            } else if (o->mesh_ref && strcmp(o->mesh_ref, "window") == 0) {
                /* center-origin window resize: drag the OUTER frame corner like a
                   wall board (free aspect), clamped to the wall; convert the outer
                   size back to the OPENING (w/h params = opening = outer - 2fw) and
                   re-centre. The wall hole re-cuts only on release. */
                Ray      ray = pick_ray(st, w);
                RoomRect r   = editor_room_rect(&st->scene, st->resize_room);
                float    yaw = board_yaw(&st->scene, st->resize_board);
                vec3     n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
                float    fw  = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "fw");
                float    wt  = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "t");
                float    tt;
                ray.dir = vec3_normalize(ray.dir);
                if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
                    vec3  hit   = vec3_add(vec3_add(ray.origin, vec3_scale(ray.dir, tt)),
                                           st->resize_grab);   /* no jump on grab */
                    float ih    = room_interior_height(&st->scene, st->resize_room);
                    float perpH = (n.z * n.z > 0.25f) ? r.hd : r.hw;
                    float runH  = (n.z * n.z > 0.25f) ? r.hw : r.hd;
                    WallMount m = wall_gable_geom(r, ih, (n.x * n.x > 0.25f));
                    float topcap= m.is_gable ? m.apex_y : r.floor_y + ih;
                    vec3  wallc = vec3_sub(vec3_make(r.cx, hit.y, r.cz), vec3_scale(n, perpH));
                    float du    = vec3_dot(vec3_sub(hit, wallc), st->resize_u);
                    float hy    = hit.y;
                    vec3  dragged, origin;
                    float nw, nh, ow, oh, p4[5], style;
                    char  oldkey[160];
                    sol_bool keyed;
                    if (du >  runH) du =  runH;          /* clamp along the wall */
                    if (du < -runH) du = -runH;
                    if (hy < r.floor_y) hy = r.floor_y;  /* clamp floor..apex (gable) */
                    if (hy > topcap)    hy = topcap;
                    dragged = vec3_add(vec3_make(wallc.x, hy, wallc.z),
                                       vec3_scale(st->resize_u, du));
                    /* OUTER min = opening min (0.3) + the two borders */
                    board_resize_corner(st->resize_anchor, dragged, st->resize_u,
                                        0.3f + 2.0f * fw, 0.0f, &nw, &nh, &origin);
                    ow = nw - 2.0f * fw; if (ow < 0.3f) ow = 0.3f;   /* outer -> opening */
                    oh = nh - 2.0f * fw; if (oh < 0.3f) oh = 0.3f;
                    style = mesh_ref_param("window", o->mesh_params, o->mesh_param_count, "style");
                    p4[0] = ow; p4[1] = oh; p4[2] = wt; p4[3] = fw; p4[4] = style;
                    keyed = mesh_asset_key(o, oldkey);   /* registry-shared rebuild (P4 i4) */
                    scene_mesh_params_set(&st->scene, st->resize_board, p4, 5);
                    if (keyed) asset_release(&g_mesh_assets, oldkey);
                    o = scene_get(&st->scene, st->resize_board);
                    if (o) {
                        vec3 center = vec3_make(origin.x, origin.y + nh * 0.5f, origin.z);
                        memset(&o->mesh, 0, sizeof o->mesh);   /* drop the borrow */
                        o->pos = scene_world_to_local(&st->scene, o->parent, center);
                    }
                    scene_resolve_meshes(&st->scene);
                }
            } else {
                Ray      ray = pick_ray(st, w);
                RoomRect r   = editor_room_rect(&st->scene, st->resize_room);
                float    yaw = board_yaw(&st->scene, st->resize_board);
                vec3     n   = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
                float    bt  = mesh_ref_param(o->mesh_ref ? o->mesh_ref : "board",
                                              o->mesh_params, o->mesh_param_count, "t");
                float    tt;
                if (ray_vs_plane(ray, st->resize_anchor, n, &tt) && tt > 0.0f) {
                    vec3  hit   = vec3_add(vec3_add(ray.origin, vec3_scale(ray.dir, tt)),
                                           st->resize_grab);   /* no jump on grab */
                    float ih    = room_interior_height(&st->scene, st->resize_room);
                    float perpH = (n.z * n.z > 0.25f) ? r.hd : r.hw;
                    float runH  = (n.z * n.z > 0.25f) ? r.hw : r.hd;
                    WallMount m = wall_gable_geom(r, ih, (n.x * n.x > 0.25f));
                    float topcap= m.is_gable ? m.apex_y : r.floor_y + ih;
                    vec3  wallc = vec3_sub(vec3_make(r.cx, hit.y, r.cz), vec3_scale(n, perpH));
                    float du    = vec3_dot(vec3_sub(hit, wallc), st->resize_u);
                    float hy    = hit.y;
                    vec3  dragged, origin;
                    float nw, nh, p3[3];
                    char  oldkey[160];
                    sol_bool keyed;
                    if (du >  runH) du =  runH;          /* clamp along the wall */
                    if (du < -runH) du = -runH;
                    if (hy < r.floor_y) hy = r.floor_y;  /* clamp floor..apex (gable) */
                    if (hy > topcap)    hy = topcap;
                    dragged   = vec3_add(vec3_make(wallc.x, hy, wallc.z),
                                         vec3_scale(st->resize_u, du));
                    {   /* pictures lock to their image aspect; whiteboards stay free */
                        float aspect = 0.0f;
                        if (o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0) {
                            float cw = mesh_ref_param("picture", o->mesh_params,
                                                      o->mesh_param_count, "w");
                            float ch = mesh_ref_param("picture", o->mesh_params,
                                                      o->mesh_param_count, "h");
                            if (ch > 0.0f) aspect = cw / ch;
                        }
                        board_resize_corner(st->resize_anchor, dragged, st->resize_u,
                                            0.3f, aspect, &nw, &nh, &origin);
                    }
                    p3[0] = nw; p3[1] = nh; p3[2] = bt;
                    /* the "board" mesh is registry-SHARED by params, and
                       scene_resolve_meshes only builds for objects with no mesh,
                       so a live resize must RELEASE the old shape (by its old key)
                       and clear the borrow before re-resolving rebuilds the new
                       size — never mesh_destroy a shared shape (P4 item 4). */
                    keyed = mesh_asset_key(o, oldkey);   /* key from the OLD params */
                    scene_mesh_params_set(&st->scene, st->resize_board, p3, 3);
                    if (keyed) asset_release(&g_mesh_assets, oldkey);
                    o = scene_get(&st->scene, st->resize_board);
                    if (o) {
                        memset(&o->mesh, 0, sizeof o->mesh);   /* drop the borrow */
                        o->pos = scene_world_to_local(&st->scene, o->parent, origin);
                    }
                    scene_resolve_meshes(&st->scene);
                }
            }
        }
        if (lmb && st->drag_handle != 0) {              /* ---- carrying ---- */
            if (!fp && !st->drag_moved) {               /* orbit: wait out the slop */
                double ddx = mx - st->press_x, ddy = my - st->press_y;
                if (ddx*ddx + ddy*ddy >= 25.0) {
                    st->drag_moved = SOL_TRUE;
                    st->selected_handle = st->drag_handle;
                }
            }
            if (fp || st->drag_moved) {
                SceneObject *o = scene_get(&st->scene, st->drag_handle);
                if (o) {
                    Ray     r     = pick_ray(st, w);
                    sol_u32 board = 0;
                    vec3    blocal;
                    blocal = vec3_make(0.0f, 0.0f, 0.0f);
                    /* only freely-owned cards seek boards: a FILE/FOLDER card
                       is the mirror's record and stays in its room (§1.3) —
                       its drop pins an ALIAS instead (see release below) */
                    if (o->kind == KIND_ALIAS || o->kind == KIND_NOTE ||
                        object_is_folder(&st->scene, st->drag_handle) ||
                        st->group_drag)   /* a group anchor is always a board card —
                                             incl. a picture, which alone slides via
                                             move_board, not this drag path */
                        board = board_under_ray(st, r, &blocal);
                    if (board != 0) {                  /* ---- board mode ---- */
                        if (st->drag_board != board) { /* entering / switching */
                            st->drag_board    = board;
                            st->drag_board_ox = 0.0f;  /* mid-drag entry: center
                                                          the card on the cursor */
                            st->drag_board_oy = -0.5f * mesh_ref_param(
                                o->mesh_ref ? o->mesh_ref : "card",
                                o->mesh_params, o->mesh_param_count, "h");
                            o->rot = quat_identity();  /* flat against the face */
                        }
                        o->parent = board;
                        o->pos = board_pin_pos(&st->scene, board, st->drag_handle,
                                               blocal, st->drag_board_ox,
                                               st->drag_board_oy);
                        st->drag_moved = SOL_TRUE;     /* a reparent is a real move */
                        arrows_rebuild(st);            /* its arrows follow live */
                        if (st->group_drag) {
                            int  gi, ai = -1;
                            vec3 gd;
                            for (gi = 0; gi < st->sel_count; gi++)
                                if (st->sel[gi] == st->drag_handle) { ai = gi; break; }
                            if (ai >= 0) {
                                gd = vec3_sub(o->pos, st->group_prepos[ai]);
                                for (gi = 0; gi < st->sel_count; gi++) {
                                    SceneObject *si;
                                    if (st->sel[gi] == st->drag_handle) continue;
                                    si = scene_get(&st->scene, st->sel[gi]);
                                    if (si) si->pos = vec3_add(st->group_prepos[gi], gd);
                                }
                            }
                            st->drop_target_handle =
                                folder_at_board_point(st, board, blocal);
                            if (msel_contains(st->sel, st->sel_count,
                                              st->drop_target_handle))
                                st->drop_target_handle = 0;   /* not a selected folder */
                        } else {
                            st->drop_target_handle =
                                is_fileable_card(&st->scene, st->drag_handle)
                                    ? folder_at_board_point(st, board, blocal) : 0;
                        }
                    } else {                           /* ---- ground mode ---- */
                        float t;
                        st->drop_target_handle = 0;
                        if (st->drag_board != 0) {     /* leaving a board: step off;
                                                          the floor write-back below
                                                          lands it under the cursor
                                                          (zero offset -> on the
                                                          floor, not floating at
                                                          board height) */
                            vec3 woff = object_world_pos(&st->scene, st->drag_handle);
                            st->drag_board  = 0;
                            o->parent       = st->drag_ground_parent;
                            o->rot          = st->drag_prev_rot;
                            o->pos          = scene_world_to_local(&st->scene,
                                                                   o->parent, woff);
                            st->drag_offset  = vec3_make(0.0f, 0.0f, 0.0f);
                            st->drag_plane_y = 0.0f;   /* off a board, a card
                                                          lands ON THE FLOOR */
                            arrows_rebuild(st);        /* its edges go dormant */
                        }
                        if (ray_vs_plane(r, vec3_make(0.0f, st->drag_plane_y, 0.0f),
                                         vec3_make(0.0f, 1.0f, 0.0f), &t) &&
                            t <= DRAG_MAX_DIST) {
                            vec3 new_world = vec3_add(vec3_add(r.origin, vec3_scale(r.dir, t)),
                                                      st->drag_offset);
                            /* chain-aware write-back: correct under ROTATED
                               parents too (the item-4 boundary, built) */
                            o->pos = scene_world_to_local(&st->scene, o->parent, new_world);
                            if (fp && !st->drag_moved) {
                                vec3 d = vec3_sub(new_world, st->drag_start_pos);
                                if (vec3_dot(d, d) > 1e-6f) st->drag_moved = SOL_TRUE;
                            }
                        }
                    }
                }
            }
        }

        if (!lmb && st->lmb_was_down) {                 /* ---- release ---- */
            if (st->marquee_active) {
                if (st->marquee_dragging) {             /* rubber-band: select covered cards */
                    sol_u32 i;
                    if (!st->marquee_add) sel_clear(st);
                    for (i = 0; i < st->scene.count; i++) {
                        sol_u32 h  = st->scene.objects[i].handle;
                        float   fx0, fy0, fx1, fy1;
                        if (st->scene.objects[i].parent != st->board_view) continue;
                        if (!object_is_selectable(&st->scene, h)) continue;
                        if (!scene_object_active(&st->scene, h)) continue;
                        card_footprint(&st->scene, h, &fx0, &fy0, &fx1, &fy1);
                        if (msel_rect_overlap(st->marquee_lx0, st->marquee_ly0,
                                              st->marquee_lx1, st->marquee_ly1,
                                              fx0, fy0, fx1, fy1))
                            msel_add(st->sel, &st->sel_count, MULTISEL_CAP, h);
                    }
                    st->selected_handle = st->sel_count
                                         ? st->sel[st->sel_count - 1] : 0;
                } else if (!st->marquee_add) {          /* plain click on empty board: clear */
                    sel_clear(st);
                }
                st->marquee_active   = SOL_FALSE;
                st->marquee_dragging = SOL_FALSE;
            } else if (st->resize_board != 0) {         /* finished a resize */
                /* a window's geometry changed: re-cut the wall hole ONCE, here on
                   release (never per-frame — the room mesh rebuild is heavy). */
                SceneObject *ro    = scene_get(&st->scene, st->resize_board);
                sol_u32      wroom = (ro && ro->mesh_ref &&
                                      strcmp(ro->mesh_ref, "window") == 0)
                                     ? ro->parent : 0;
                if (wroom != 0) {
                    window_glass_resize(st, st->resize_board);   /* pane tracks the new opening */
                    ensure_window_fill(st, st->resize_board);    /* oak fill tracks the new opening */
                    room_rebuild_one(st, wroom);
                    scene_resolve_meshes(&st->scene);            /* build the resized fill child */
                }
                scene_save(&st->scene, "scene.stml");
                st->resize_board = 0;
            } else if (st->move_board != 0) {           /* finished a picture/window slide */
                SceneObject *mo    = scene_get(&st->scene, st->move_board);
                sol_u32      wroom = (mo && mo->mesh_ref &&
                                      strcmp(mo->mesh_ref, "window") == 0)
                                     ? mo->parent : 0;
                if (st->drop_target_handle != 0 &&
                    is_fileable_card(&st->scene, st->move_board)) {
                    const char *link = scene_meta_get(&st->scene,
                                           st->drop_target_handle, "link");
                    if (link && link[0]) {              /* slid onto a folder: file it */
                        scene_meta_set(&st->scene, st->move_board, "page", link);
                        st->selected_handle = 0;        /* paged out: drop the selection */
                        printf("filed #%u onto %s\n", (unsigned)st->move_board, link);
                    }
                }
                if (wroom != 0) room_rebuild_one(st, wroom);  /* the hole follows the window */
                st->drop_target_handle = 0;
                scene_save(&st->scene, "scene.stml");
                st->move_board = 0;
            } else if (st->group_drag) {                /* finished a group move/file */
                sol_u32     tgt  = st->drop_target_handle;
                const char *link = (tgt != 0 &&
                                    !msel_contains(st->sel, st->sel_count, tgt))
                                   ? scene_meta_get(&st->scene, tgt, "link")
                                   : (const char *)0;
                int i;
                if (link && link[0]) {                  /* dropped on a folder: file all */
                    /* re-tag EVERY selected item, folders included, so the whole
                       selection moves; a folder's own link target is unaffected */
                    for (i = 0; i < st->sel_count; i++) {
                        SceneObject *si = scene_get(&st->scene, st->sel[i]);
                        if (si) si->pos = st->group_prepos[i];   /* keep the arrangement */
                        scene_meta_set(&st->scene, st->sel[i], "page", link);
                    }
                    printf("filed %d cards onto %s\n", st->sel_count, link);
                    sel_clear(st);
                }                                       /* else: a plain group move, positions kept */
                st->group_drag = SOL_FALSE;
                arrows_rebuild(st);
                scene_save(&st->scene, "scene.stml");
            } else if (st->drag_handle != 0 && st->drag_moved) {
                SceneObject *o = scene_get(&st->scene, st->drag_handle);
                sol_bool filed = SOL_FALSE;
                if (st->drop_target_handle != 0) {
                    SceneObject *fold = scene_get(&st->scene, st->drop_target_handle);
                    const char  *link = fold ? scene_meta_get(&st->scene,
                                                   st->drop_target_handle, "link")
                                             : (const char *)0;
                    if (o && is_fileable_card(&st->scene, st->drag_handle) &&
                        link && link[0]) {
                        scene_meta_set(&st->scene, st->drag_handle, "page", link);
                        scene_save(&st->scene, "scene.stml");
                        printf("filed #%u onto %s\n",
                               (unsigned)st->drag_handle, link);
                        filed = SOL_TRUE;            /* skip the ordinary placement save */
                        st->selected_handle = 0;     /* card is paged out; drop the stale selection */
                    }
                    st->drop_target_handle = 0;
                }
                if (!filed) {
                if (o && o->kind != KIND_PLAIN) {       /* a dragged card is PLACED:
                                                           the tray's claim ends here */
                    const char *u = scene_meta_get(&st->scene, st->drag_handle, "unplaced");
                    if (u && strcmp(u, "1") == 0) {
                        scene_meta_set(&st->scene, st->drag_handle, "unplaced", "0");
                        apply_kind_materials(&st->scene);   /* its tray glow goes out */
                    }
                }
                if (o && (o->kind == KIND_FILE || o->kind == KIND_FOLDER) && o->content) {
                    /* a mirror's record never leaves its room (§1.3) — dropping it
                       on a board snaps the record home. An IMAGE drops a resizable
                       PICTURE; any other file pins a filename ALIAS card. */
                    vec3    blocal;
                    sol_u32 board = board_under_ray(st, pick_ray(st, w), &blocal);
                    if (board != 0) {
                        const char *cpath = o->content;     /* heap str survives scene_add */
                        o->parent = st->drag_prev_parent;   /* snap the record home */
                        o->pos    = st->drag_prev_pos;
                        o->rot    = st->drag_prev_rot;
                        if (reader_is_image_path(cpath)) {
                            sol_u32      a = spawn_image_picture(st, board,
                                              vec3_make(0.0f, 0.0f, 0.0f),
                                              quat_identity(), cpath);
                            SceneObject *ao = scene_get(&st->scene, a);
                            if (ao) {
                                float ph = mesh_ref_param("picture", ao->mesh_params,
                                                          ao->mesh_param_count, "h");
                                ao->pos = board_pin_pos(&st->scene, board, a,
                                                        blocal, 0.0f, -0.5f * ph);
                            }
                            st->selected_handle = a;
                            printf("dropped an image picture on the board — the record stays home\n");
                        } else {
                            char        lbuf[16];
                            const char *nm = object_label(&st->scene, st->drag_handle, lbuf);
                            Mesh        empty;
                            vec3        one = vec3_make(1.0f, 1.0f, 1.0f);
                            float       ch  = mesh_ref_param("card", (const float *)0, 0, "h");
                            sol_u32     a;
                            memset(&empty, 0, sizeof empty);
                            a = scene_add(&st->scene, board, empty,
                                          vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), one);
                            scene_kind_set(&st->scene, a, KIND_ALIAS);
                            scene_content_set(&st->scene, a, cpath);
                            scene_meta_set(&st->scene, a, "name", nm);
                            scene_mesh_ref_set(&st->scene, a, "card");
                            {
                                SceneObject *ao = scene_get(&st->scene, a);
                                if (ao) ao->pos = board_pin_pos(&st->scene, board, a,
                                                                blocal, 0.0f, -0.5f * ch);
                            }
                            scene_resolve_meshes(&st->scene);
                            apply_kind_materials(&st->scene);
                            board_card_tag_page(st, a);     /* lands on the current page */
                            st->selected_handle = a;
                            printf("pinned alias '%s' to the board — the record stays home\n", nm);
                        }
                        o = scene_get(&st->scene, st->drag_handle);  /* re-fetch after scene_add */
                    }
                }
                board_card_tag_page(st, st->drag_handle);   /* a card dropped on a
                                                board joins the page you're viewing */
                if (scene_save(&st->scene, "scene.stml"))
                    printf("placed #%u at (%.2f, %.2f, %.2f) — saved\n",
                           (unsigned)st->drag_handle,
                           o ? (double)o->pos.x : 0.0,
                           o ? (double)o->pos.y : 0.0,
                           o ? (double)o->pos.z : 0.0);
                collide_rebuild(&st->colliders, &st->scene);  /* walls and
                                       paths are draggable props — the
                                       architecture may just have moved */
                }                                                 /* end if (!filed) */
            } else if (!fp) {
                double ddx = mx - st->press_x;
                double ddy = my - st->press_y;
                if (ddx*ddx + ddy*ddy < 25.0) {         /* moved < 5px -> a tap */
                    int ww, wh;
                    glfwGetWindowSize(w, &ww, &wh);
                    do_pick(st, w, 2.0f*(float)mx/(float)ww - 1.0f,
                                   1.0f - 2.0f*(float)my/(float)wh);
                }
            }
            st->drag_handle = 0;
            st->drag_moved  = SOL_FALSE;
            st->drop_target_handle = 0;
        }
        }                                               /* ---- end editor else ---- */
        st->lmb_was_down = lmb;
    }

    /* hover highlight: the resize corner the pointer is over goes blue so the
       grab reads. While dragging, the grabbed corner stays lit. When NOT
       dragging, it lights only if the pointer is actually over the selected
       card (a corner's grab radius spills past the card edge, but a click out
       there re-picks/deselects instead of resizing — so it must not read hot). */
    if (st->resize_board != 0) {
        st->hover_corner = st->resize_corner;
    } else if (st->selected_handle != 0 &&
               (board_is_mounted(&st->scene, st->selected_handle) ||
                note_resizable(&st->scene, st->selected_handle) ||
                picture_on_board(&st->scene, st->selected_handle) ||
                window_on_wall(&st->scene, st->selected_handle))) {
        int c = resize_corner_at(st, w, (vec3 *)0, (vec3 *)0);
        if (c >= 0) {
            float   nx = 0.0f, ny = 0.0f, pt;
            if (st->board_view != 0) {                 /* cursor pick (else crosshair) */
                int ww2, wh2;
                glfwGetWindowSize(w, &ww2, &wh2);
                if (ww2 > 0 && wh2 > 0) {
                    nx = 2.0f * (float)mx / (float)ww2 - 1.0f;
                    ny = 1.0f - 2.0f * (float)my / (float)wh2;
                }
            }
            if (pick_at(st, w, nx, ny, &pt) == st->selected_handle)
                st->hover_corner = c;
        }
    }

    /* Registered discrete commands: poll each hotkey, edge-trigger, honour the
       precondition. The palette dispatches these same run()s. */
    {
        int ci;
        for (ci = 0; ci < G_COMMAND_COUNT; ci++) {
            Command *cmd = &g_commands[ci];
            sol_bool now;
            if (cmd->key == 0) continue;
            if (st->editor.active || st->board_view != 0)
                { cmd->was_down = SOL_FALSE; continue; }       /* editor / board view: hotkeys off */
            now = glfwGetKey(w, cmd->key) == GLFW_PRESS;
            if (now && !cmd->was_down && (cmd->can_run == NULL || cmd->can_run(st)))
                cmd->run(st);
            cmd->was_down = now;
        }
    }

    /* editor enter/exit: framed RTS camera in, saved first-person camera out */
    if (st->editor.active && !st->editor.was_active) {
        vec3  center;
        float radius;
        st->editor.saved_cam = st->camera;
        editor_frame_rooms(st, &center, &radius);
        camera_enter_rts(&st->camera, center, radius);
        st->editor.action      = EDIT_IDLE;
        st->editor.selected_wk = 0;
        st->selected_handle    = 0;
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        st->mouse_skip = 2;
    } else if (!st->editor.active && st->editor.was_active) {
        st->camera = st->editor.saved_cam;
        st->selected_handle = 0;
        glfwSetInputMode(w, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        st->mouse_skip = 2;
    }
    st->editor.was_active = st->editor.active;

    /* Delete / Backspace: drop the selected walkway, else delete the selected
       island/abbey (terrain) — its dressing, plot churches, and walkways go too. */
    {
        sol_bool del_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_DELETE) == GLFW_PRESS ||
                                      glfwGetKey(w, GLFW_KEY_BACKSPACE) == GLFW_PRESS);
        if (st->editor.active && del_now && !st->editor_del_was) {
            if (st->editor.selected_wk != 0) {
                editor_delete_selected(&st->editor, &st->scene);
            } else {
                SceneObject *so = scene_get(&st->scene, st->selected_handle);
                if (so && so->mesh_ref && strcmp(so->mesh_ref, "terrain") == 0) {
                    editor_delete_footprint(st, st->selected_handle);   /* island/abbey */
                } else if (so && !so->mesh_ref) {                       /* a FREE room only */
                    const char *rt = scene_meta_get(&st->scene, st->selected_handle, "room_type");
                    const char *sp = scene_meta_get(&st->scene, st->selected_handle, "source_path");
                    if (rt && strcmp(rt, "mirror") == 0 && (!sp || sp[0] == '\0'))
                        editor_delete_footprint(st, st->selected_handle);
                }
            }
        }
        st->editor_del_was = del_now;
    }

    /* commit (an edit's release) = full authoritative rebuild + colliders +
       save. A mid-drag `dirty` does the cheap INCIDENT re-thread (only the
       dragged room + its walkways) so paths follow live — affordable now that
       the solver is fast and the rebuild is scoped to a few meshes, not all ~19.
       Colliders aren't used by the RTS camera, so they wait for release. */
    if (st->editor.commit) {
        if (st->editor_resize_keyed) {           /* a terrain island was resized: re-tessellate */
            SceneObject *ro = scene_get(&st->scene, st->editor.room);
            asset_release(&g_mesh_assets, st->editor_resize_key);
            if (ro) memset(&ro->mesh, 0, sizeof ro->mesh);   /* drop the borrow; resolve rebuilds */
            scene_resolve_meshes(&st->scene);
            st->editor_resize_keyed = SOL_FALSE;
        }
        {   /* an island move/resize relocates its WORLD-baked FIELD data
               (grass, forest, gothic ornament) — re-derive it at the new pose */
            SceneObject *er = scene_get(&st->scene, st->editor.room);
            if (er && er->mesh_ref && strcmp(er->mesh_ref, "terrain") == 0) {
                meadow_rebuild(st);
                forest_rebuild(st);
                ornament_rebuild(st);
            }
        }
        connections_rebuild(st);
        collide_rebuild(&st->colliders, &st->scene);
        scene_save(&st->scene, "scene.stml");
        st->editor.commit = SOL_FALSE;
        st->editor.dirty  = SOL_FALSE;
    } else if (st->editor.dirty) {
        connections_rebuild_focus(st, st->editor.room);
        st->editor.dirty = SOL_FALSE;
    }

    /* G gathers the selected FILE/FOLDER card into the first workspace as an
       ALIAS (item 6d) — a reference, never a copy: the same file may stand
       in many rooms at once, the arrangement the filesystem forbids and
       this project exists to allow. */
    {
        sol_bool g_now = glfwGetKey(w, GLFW_KEY_G) == GLFW_PRESS;
        if (g_now && !st->g_was_down && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && (o->kind == KIND_FILE || o->kind == KIND_FOLDER) && o->content) {
                sol_u32 ws = 0, i;
                for (i = 0; i < st->scene.count; i++) {
                    const char *rt = scene_meta_get(&st->scene,
                                                    st->scene.objects[i].handle, "room_type");
                    if (rt && strcmp(rt, "workspace") == 0) {
                        ws = st->scene.objects[i].handle;
                        break;
                    }
                }
                if (ws != 0) {
                    char        lbuf[16], wbuf[16];
                    const char *nm = object_label(&st->scene, st->selected_handle, lbuf);
                    sol_u32     a  = workspace_add_alias(&st->scene, ws, o->content, nm);
                    if (a != 0) {
                        scene_resolve_meshes(&st->scene);
                        apply_kind_materials(&st->scene);
                        scene_save(&st->scene, "scene.stml");
                        printf("gathered '%s' into %s\n", nm,
                               object_label(&st->scene, ws, wbuf));
                    }
                }
            }
        }
        st->g_was_down = g_now;
    }

    /* C arms a connection from the selected board card (item 8): the next
       press on a second card of the same board births an ARROW object —
       two `connects` rels made visible. C again disarms. */
    {
        sol_bool c_now = glfwGetKey(w, GLFW_KEY_C) == GLFW_PRESS;
        if (c_now && !st->c_was_down) {
            if (st->connect_from != 0) {
                st->connect_from = 0;
                printf("connect: disarmed\n");
            } else if (st->selected_handle != 0) {
                SceneObject *o = scene_get(&st->scene, st->selected_handle);
                if (o && (o->kind == KIND_ALIAS || o->kind == KIND_NOTE) &&
                    object_is_board(&st->scene, o->parent)) {
                    char lbuf[16];
                    st->connect_from = st->selected_handle;
                    printf("connect: from '%s' — click another card on the same board\n",
                           object_label(&st->scene, st->selected_handle, lbuf));
                }
            }
        }
        st->c_was_down = c_now;
    }

    /* N spawns a NOTE card (item 8): pinned to the board under the cursor,
       else standing on the floor ahead. Its text lives in the inline `text`
       meta — the multiline raw-block escaping ladder's first real customer;
       Enter (with the note selected) opens it for typing. */
    {
        sol_bool n_now = glfwGetKey(w, GLFW_KEY_N) == GLFW_PRESS;
        if (n_now && !st->n_was_down)
            (void)spawn_note(st, w);
        st->n_was_down = n_now;
    }

    /* Z composes THE ABBEY (P6 item 10, the phase's portfolio): one large
       island minted ahead, its hall church at ruin 0.55, the churchyard
       cross, a broken colonnade descending the west approach, balustrades
       at the porch, candle sconces in the choir — and the night sky.
       Everything here is vocabulary items 1-9 built; this key only
       COMPOSES. The follies ride as CHILDREN of the church anchor in
       PLAN coordinates: they inherit the datum lift and the swap yaw,
       and their feet find the hill through the same plan_to_local +
       terrain_height read the datum sampling uses. */
    {
        sol_bool z_now = glfwGetKey(w, GLFW_KEY_Z) == GLFW_PRESS;
        if (z_now && !st->z_was_down) {
            float      iw = 44.0f, id = 44.0f, iseed = 9001.0f;
            float      tpar[5];
            float      cpar[5];
            ChurchPlan cp;
            Mesh       empty;
            vec3       one = vec3_make(1.0f, 1.0f, 1.0f);
            vec3       f, ipos;
            float      datum = 0.0f, x, z;
            int        i2, j2;
            sol_u32    island, anchor;
            quat       rot;

            f = camera_forward(&st->camera);
            f.y = 0.0f;
            if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
            f    = vec3_normalize(f);
            ipos = vec3_add(st->camera.pos, vec3_scale(f, 0.55f * iw + 8.0f));
            ipos.y = st->camera.pos.y - CAMERA_EYE_HEIGHT;

            memset(&empty, 0, sizeof empty);
            tpar[0] = iw; tpar[1] = id; tpar[2] = 56.0f;
            tpar[3] = 2.4f; tpar[4] = iseed;
            island = scene_add(&st->scene, 0, empty, ipos, quat_identity(), one);
            scene_mesh_ref_set(&st->scene, island, "terrain");
            scene_mesh_params_set(&st->scene, island, tpar, 5);
            scene_meta_set(&st->scene, island, "room_type", "land");
            scene_meta_set(&st->scene, island, "name", "the abbey hill");
            mint_tag_ws(st, island);             /* the abbey lands in the active world */

            /* the hall church, already half-fallen (style 1, ruin 0.55) */
            cpar[0] = 15.0f; cpar[1] = 24.0f; cpar[2] = iseed;
            cpar[3] = 1.0f;  cpar[4] = 0.55f;
            church_plan(&cp, cpar, 5);
            {
                SceneObject *isl = scene_get(&st->scene, island);
                for (i2 = 0; i2 <= cp.nbays; i2++)        /* the datum */
                    for (j2 = 0; j2 <= PIER_ROW_N_WALL; j2++) {
                        float lx, lz, hgt;
                        if (!plan_pier(&cp, i2, j2, &x, &z)) continue;
                        plan_to_local(&cp, x, z, &lx, &lz);
                        hgt = terrain_height(isl->mesh_params,
                                             isl->mesh_param_count, lx, lz);
                        if (hgt > datum) datum = hgt;
                    }
                for (i2 = 0; i2 < 6; i2++) {
                    float lx, lz, hgt;
                    if (!plan_apse_pier(&cp, i2, &x, &z)) continue;
                    plan_to_local(&cp, x, z, &lx, &lz);
                    hgt = terrain_height(isl->mesh_params,
                                         isl->mesh_param_count, lx, lz);
                    if (hgt > datum) datum = hgt;
                }
            }

            {   /* P8 item 3: the abbey brings a low RAKING SUN, so the new
                   god-ray pass throws real shafts through the half-fallen
                   arcade (its ruin gaps are perfect occluders). With the
                   directional sun + cascades (P8 item 6), pos/target set only
                   the rake DIRECTION; the cascades cover the whole island, so
                   no distance compensation is needed. */
                vec3 ctr = vec3_add(ipos, vec3_make(0.0f, datum + 5.0f, 0.0f));
                st->light_pos       = vec3_add(ctr, vec3_make(-10.0f, 10.0f, 5.0f));
                st->light_target    = ctr;       /* sun dir = normalize(target - pos) */
                st->light_color     = vec3_make(1.0f, 0.93f, 0.78f);
                st->light_intensity = 3.5f;      /* directional: a plain multiplier */
                st->light_inner_deg = 30.0f;     /* (cone fields unused by the sun) */
                st->light_outer_deg = 42.0f;
            }
            rot = cp.swapped
                ? quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f),
                                       -0.5f * (float)SOL_PI)
                : quat_identity();
            anchor = scene_add(&st->scene, 0, empty,
                               vec3_add(ipos, vec3_make(0.0f, datum, 0.0f)),
                               rot, one);
            scene_meta_set(&st->scene, anchor, "room_type", "church");
            scene_meta_set(&st->scene, anchor, "name", "the abbey");
            mint_tag_ws(st, anchor);             /* the abbey church in the active world */
            {
                SceneObject *iso = scene_get(&st->scene, island);
                if (iso && iso->nid)
                    scene_meta_set(&st->scene, anchor, "plot", iso->nid);
            }
            {
                static const char *refs[5] = {
                    "church_stone", "church_glass",
                    "church_roof",  "church_floor",
                    "church_decals"
                };
                int r2;
                for (r2 = 0; r2 < 5; r2++) {
                    sol_u32 ch = scene_add(&st->scene, anchor, empty,
                                           vec3_make(0, 0, 0),
                                           quat_identity(), one);
                    scene_mesh_ref_set(&st->scene, ch, refs[r2]);
                    scene_mesh_params_set(&st->scene, ch, cpar, 5);
                    {
                        SceneObject *co = scene_get(&st->scene, ch);
                        if (co) {
                            Material mm = material_default();
                            if (r2 == 0 || r2 == 3) {
                                mm.base_color = vec3_make(1.0f, 1.0f, 1.0f);
                                mm.roughness  = 1.0f;
                                mm.metallic   = 1.0f;
                            } else if (r2 == 1) {
                                mm.base_color = vec3_make(0.02f, 0.025f, 0.045f);
                                mm.roughness  = 0.08f;
                                mm.emissive   = vec3_make(1.8f, 1.12f, 0.5f);
                            } else {
                                mm.base_color = vec3_make(0.30f, 0.32f, 0.36f);
                                mm.roughness  = 0.55f;
                            }
                            co->material = mm;
                        }
                        if (r2 == 0) {
                            scene_tex_ref_set(&st->scene, ch, "stone");
                        } else if (r2 == 3) {
                            float fp2[4];
                            fp2[0] = iseed + 7.0f;
                            fp2[1] = 2.4f; fp2[2] = 0.8f; fp2[3] = 0.8f;
                            scene_tex_ref_set(&st->scene, ch, "stone");
                            scene_tex_params_set(&st->scene, ch, fp2, 4);
                        }
                    }
                }
            }

            /* a folly child: plan coords in, its feet on the hill (child
               y is datum-relative, so the terrain read subtracts it) */
            {
                SceneObject *isl = scene_get(&st->scene, island);
#define ABBEY_CHILD(refname, px, pz, yawq, prm, prmn, nm, texed)           \
                do {                                                       \
                    float lx_, lz_, hgt_;                                  \
                    sol_u32 ch_;                                           \
                    plan_to_local(&cp, (px), (pz), &lx_, &lz_);            \
                    hgt_ = terrain_height(isl->mesh_params,                \
                                          isl->mesh_param_count, lx_, lz_);\
                    ch_ = scene_add(&st->scene, anchor, empty,             \
                                    vec3_make((px), hgt_ - datum, (pz)),   \
                                    (yawq), one);                          \
                    scene_mesh_ref_set(&st->scene, ch_, (refname));        \
                    if ((prmn) > 0)                                        \
                        scene_mesh_params_set(&st->scene, ch_, (prm), (prmn)); \
                    scene_meta_set(&st->scene, ch_, "name", (nm));         \
                    if (texed) {                                           \
                        SceneObject *fo_ = scene_get(&st->scene, ch_);     \
                        if (fo_) {                                         \
                            Material fm_ = material_default();             \
                            fm_.base_color = vec3_make(1.0f, 1.0f, 1.0f);  \
                            fm_.roughness = 1.0f; fm_.metallic = 1.0f;     \
                            fo_->material = fm_;                           \
                        }                                                  \
                        scene_tex_ref_set(&st->scene, ch_, "stone");       \
                    }                                                      \
                } while (0)

                /* the churchyard cross, south of the west approach */
                {
                    float xp[1];
                    xp[0] = 3.4f;
                    ABBEY_CHILD("cross", cp.west_x - 3.5f, 4.5f,
                                quat_identity(), xp, 1,
                                "the churchyard cross", 1);
                }
                /* the broken colonnade descending the approach: three
                   pairs, more fallen the farther from the door */
                {
                    int   pk;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
                    for (pk = 0; pk < 3; pk++) {
                        float cx2 = cp.west_x - 4.0f - 2.8f * (float)pk;
                        float cprm[4];
                        char  cname[32];
                        cprm[0] = 4.2f; cprm[1] = 0.0f;
                        cprm[2] = (pk == 0) ? 0.0f
                                : 0.18f + 0.26f * (float)pk;
                        cprm[3] = iseed + (float)pk * 3.0f;
                        sprintf(cname, "colonnade %d s", pk);
                        ABBEY_CHILD("column", cx2, -2.3f, quat_identity(),
                                    cprm, 4, cname, 1);
                        cprm[2] = (pk == 2) ? 0.8f
                                : 0.10f + 0.22f * (float)pk;
                        cprm[3] = iseed + (float)pk * 3.0f + 1.0f;
                        sprintf(cname, "colonnade %d n", pk);
                        ABBEY_CHILD("column", cx2, 2.3f, quat_identity(),
                                    cprm, 4, cname, 1);
                    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
                }
                /* balustrades flanking the porch, one more ruined */
                {
                    float bprm[4];
                    bprm[0] = 3.0f; bprm[1] = 1.0f;
                    bprm[2] = iseed; bprm[3] = 0.3f;
                    ABBEY_CHILD("balustrade", cp.west_x - 2.6f, -2.3f,
                                quat_identity(), bprm, 4,
                                "porch balustrade s", 1);
                    bprm[2] = iseed + 1.0f; bprm[3] = 0.6f;
                    ABBEY_CHILD("balustrade", cp.west_x - 2.6f, 2.3f,
                                quat_identity(), bprm, 4,
                                "porch balustrade n", 1);
                }
#undef ABBEY_CHILD
            }

            /* candle sconces in the choir (P4 item 5's lights + flicker):
               children at PAVEMENT height — they ride the church frame */
            {
                static const float SCZ[3] = { -2.2f, 2.2f, 0.0f };
                int sc;
                for (sc = 0; sc < 3; sc++) {
                    float sx = cp.east_x - 2.0f - (sc == 2 ? 2.2f : 0.0f);
                    sol_u32 h2 = scene_add(&st->scene, anchor, empty,
                                vec3_make(sx, cp.plinth_h + 1.9f, SCZ[sc]),
                                quat_identity(),
                                vec3_make(0.10f, 0.10f, 0.10f));
                    scene_mesh_ref_set(&st->scene, h2, "box");
                    scene_meta_set(&st->scene, h2, "name", "sconce");
                    scene_meta_set(&st->scene, h2, "light", "point");
                    scene_meta_set(&st->scene, h2, "light_color",
                                   "1.0 0.62 0.30");
                    scene_meta_set(&st->scene, h2, "light_intensity", "9");
                    scene_meta_set(&st->scene, h2, "light_radius", "7");
                    if (sc == 0) {     /* one designated shadow-caster (P8 item 7):
                                          aimed down the nave + to the floor so it
                                          rakes the piers and throws their shadows */
                        scene_meta_set(&st->scene, h2, "cast", "1");
                        scene_meta_set(&st->scene, h2, "light_dir", "-0.7 -0.55 0.25");
                        scene_meta_set(&st->scene, h2, "light_cone", "48");
                    }
                    scene_component_add(&st->scene, h2, "flicker",
                                        (const float *)0, 0);
                    {
                        SceneObject *so = scene_get(&st->scene, h2);
                        if (so) {
                            Material m = material_default();
                            m.base_color = vec3_make(0.95f, 0.78f, 0.50f);
                            m.emissive   = vec3_make(1.5f, 0.85f, 0.38f);
                            m.roughness  = 0.4f;
                            so->material = m;
                        }
                    }
                }
            }

            /* the HERO planting (P7 item 10): the hand-placed pieces the
               scatter can't give — the churchyard yew, an orchard pair,
               the pond in the western hollow, a few erratics. The forest,
               flowers, scree and wind grow themselves (items 5-9). */
            {
                SceneObject *isl = scene_get(&st->scene, island);
                static const float FALL_LEAVES[24] = {
                    3.0f, 5.0f,  0.0f, -0.30f, 0.0f,  1.6f, 0.3f, 1.6f,
                    0.0f, 4.5f, 0.0f,  0.09f, 0.07f,
                    0.55f, 0.35f, 0.10f, 1.0f,  0.35f, 0.18f, 0.06f, 0.0f,
                    0.0f, -0.15f, 0.0f
                };
#define ABBEY_PLANT(ref, px, pz, prm, np, texkind, leaf, nm)              \
                do {                                                       \
                    float lx_, lz_, hgt_;                                  \
                    sol_u32 t_;                                            \
                    plan_to_local(&cp, (px), (pz), &lx_, &lz_);            \
                    hgt_ = terrain_height(isl->mesh_params,                \
                                          isl->mesh_param_count, lx_, lz_);\
                    t_ = scene_add(&st->scene, anchor, empty,              \
                                   vec3_make((px), hgt_ - datum, (pz)),    \
                                   quat_identity(), one);                  \
                    scene_mesh_ref_set(&st->scene, t_, (ref));             \
                    if ((np) > 0)                                          \
                        scene_mesh_params_set(&st->scene, t_, (prm), (np));\
                    scene_meta_set(&st->scene, t_, "name", (nm));          \
                    {   SceneObject *po_ = scene_get(&st->scene, t_);      \
                        if (po_) { Material pm_ = material_default();      \
                            pm_.base_color = vec3_make(1.0f, 1.0f, 1.0f);  \
                            pm_.roughness = 1.0f; pm_.metallic = 1.0f;     \
                            po_->material = pm_; } }                       \
                    scene_tex_ref_set(&st->scene, t_, (texkind));          \
                    if (leaf) scene_component_add(&st->scene, t_, "emit",  \
                                                  FALL_LEAVES, 24);        \
                } while (0)

                {   /* the churchyard yew (an evergreen cypress) by the cross */
                    float yp[3];
                    yp[0] = iseed + 200.0f; yp[1] = 1.25f; yp[2] = 9.0f;  /* seed, age, height(m) — was a slip: the seed had landed in the HEIGHT slot (9201 m yew) */
                    ABBEY_PLANT("cypress", cp.west_x - 5.5f, 6.5f, yp, 3,
                                "bark", 0, "the churchyard yew");
                }
                {   /* an orchard pair, south of the nave — deciduous, shedding */
                    float op[3];
                    float ox = cp.west_x + cp.tower_d + 1.5f * cp.bay_l;
                    float oz = -(0.5f * cp.nave_w + cp.aisle_w) - 4.0f;
                    op[0] = iseed + 211.0f; op[1] = 0.9f; op[2] = 6.5f;
                    ABBEY_PLANT("oak", ox, oz, op, 3, "bark", 1, "orchard oak");
                    op[0] = iseed + 223.0f;
                    ABBEY_PLANT("oak", ox + 1.7f * cp.bay_l, oz, op, 3,
                                "bark", 1, "orchard oak");
                }
                {   /* a few erratics on the south slope, course-free stone */
                    int bk;
                    for (bk = 0; bk < 3; bk++) {
                        float bp[3];
                        bp[0] = 0.7f + 0.5f * (float)bk;     /* size  */
                        bp[1] = iseed + 30.0f + (float)bk * 7.0f;
                        bp[2] = (bk == 1) ? 0.6f : 0.0f;     /* one flat-top */
                        ABBEY_PLANT("boulder",
                                    cp.west_x + (float)bk * 2.4f,
                                    0.5f * cp.nave_w + cp.aisle_w + 3.0f + (float)bk,
                                    bp, 3, "stone", 0, "erratic");
                    }
                }
                {   /* the pond in the western hollow: sample the dip, set
                       the surface just above the lowest ground there */
                    float lx_, lz_, lowest, pp[3];
                    int   a;
                    sol_u32 pondh;
                    plan_to_local(&cp, cp.west_x - 11.0f, 0.0f, &lx_, &lz_);
                    lowest = terrain_height(isl->mesh_params,
                                            isl->mesh_param_count, lx_, lz_);
                    for (a = 0; a < 8; a++) {
                        float ang = (float)a / 8.0f * 6.2831853f;
                        float sx2, sz2, g2;
                        plan_to_local(&cp, cp.west_x - 11.0f + cosf(ang) * 3.0f,
                                      sinf(ang) * 3.0f, &sx2, &sz2);
                        g2 = terrain_height(isl->mesh_params,
                                            isl->mesh_param_count, sx2, sz2);
                        if (g2 < lowest) lowest = g2;
                    }
                    pondh = scene_add(&st->scene, anchor, empty,
                                vec3_make(cp.west_x - 11.0f,
                                          lowest - datum + 0.2f, 0.0f),
                                quat_identity(), one);
                    scene_mesh_ref_set(&st->scene, pondh, "pond");
                    pp[0] = 4.5f; pp[1] = 2.0f; pp[2] = 7.0f;
                    scene_mesh_params_set(&st->scene, pondh, pp, 3);
                    scene_meta_set(&st->scene, pondh, "name", "the abbey pond");
                }
#undef ABBEY_PLANT
            }

            scene_resolve_meshes(&st->scene);
            collide_rebuild(&st->colliders, &st->scene);
            forest_rebuild(st);                /* the hill grows its forest too */
            meadow_rebuild(st);                /* the hill grows its grass —
                                                  and through the roofless bays */
            scene_save(&st->scene, "scene.stml");
            hdr_reload(st, "rogland_clear_night_4k.hdr");  /* dusk falls */
            st->night = SOL_TRUE;                          /* so ` flips to day next (P8 item 9) */
            printf("the abbey rises on its hill (datum %.2f) — night falls\n",
                   datum);
        }
        st->z_was_down = z_now;
    }

    /* Backspace dismisses a selected TOMBSTONE — manual, deliberate (the 6c
       decision): the system never throws away the marker for you. Item 8
       extends it to ARROWS: deleting the edge object deletes the relation. */
    {
        sol_bool bs_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_BACKSPACE) == GLFW_PRESS ||
                                     glfwGetKey(w, GLFW_KEY_DELETE)    == GLFW_PRESS);
        if (bs_now && !st->bs_was_down && st->board_view != 0 && st->sel_count > 1) {
            sol_u32 doomed[MULTISEL_CAP];
            int     i, n = st->sel_count;
            for (i = 0; i < n; i++) doomed[i] = st->sel[i];   /* snapshot: handles are stable */
            sel_clear(st);
            for (i = 0; i < n; i++) delete_board_card(st, doomed[i]);
            arrows_rebuild(st);
            scene_save(&st->scene, "scene.stml");
            printf("deleted %d cards\n", n);
        } else if (bs_now && !st->bs_was_down && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_TOMBSTONE) {
                char        lbuf[16];
                char        akey[160];
                const char *label = object_label(&st->scene, st->selected_handle, lbuf);
                printf("dismissed tombstone: %s\n", label);
                if (mesh_asset_key(o, akey))           /* its shape goes back (P4 i4) */
                    asset_release(&g_mesh_assets, akey);
                scene_remove(&st->scene, st->selected_handle);
                st->selected_handle = 0;
                arrows_rebuild(st);            /* edges to the dead card go dormant */
                scene_save(&st->scene, "scene.stml");
            } else if (o && object_is_arrow(&st->scene, st->selected_handle)) {
                mesh_destroy(&o->mesh);        /* derived GPU buffers, freed now */
                scene_remove(&st->scene, st->selected_handle);
                st->selected_handle = 0;
                scene_save(&st->scene, "scene.stml");
                printf("removed arrow — the connection is gone\n");
            } else if (o && o->mesh_ref != NULL &&
                       strcmp(o->mesh_ref, "picture") == 0) {
                /* a placed image (on a wall or pinned to a whiteboard): remove
                   the display copy. The file card already returned to its shelf
                   when the picture was planted, and the texture is a shared,
                   session-lived asset — only the per-object mesh buffer goes. */
                char    akey[160];
                sol_u32 doomed = st->selected_handle;
                if (mesh_asset_key(o, akey))
                    asset_release(&g_mesh_assets, akey);
                st->selected_handle = 0;
                if (st->resize_board == doomed) st->resize_board = 0;
                if (st->move_board   == doomed) st->move_board   = 0;
                scene_remove(&st->scene, doomed);
                scene_save(&st->scene, "scene.stml");
                printf("deleted picture #%u\n", (unsigned)doomed);
            } else if (o && o->mesh_ref != NULL &&
                       strcmp(o->mesh_ref, "window") == 0) {
                /* a placed window: delete any child panes (window_glass) first,
                   then the window, then re-cut the wall so the hole closes.
                   delete_board_card releases each shape's keyed mesh + clears any
                   resize/move/carry/selection references. */
                sol_u32 doomed = st->selected_handle;
                sol_u32 room   = o->parent;
                sol_u32 kids[64];
                int     nk = 0, i;
                for (i = 0; i < (int)st->scene.count && nk < 64; i++)
                    if (st->scene.objects[i].parent == doomed)
                        kids[nk++] = st->scene.objects[i].handle;   /* handles are stable */
                for (i = 0; i < nk; i++) delete_board_card(st, kids[i]);
                delete_board_card(st, doomed);
                room_rebuild_one(st, room);
                scene_save(&st->scene, "scene.stml");
                printf("deleted window #%u\n", (unsigned)doomed);
            } else if (o && o->mesh_ref != NULL &&
                       strcmp(o->mesh_ref, "folderbook") == 0) {
                /* delete ONLY this folder link. The target page and its
                   contents survive (still reachable by arrow-cycle); the
                   backlink on the other page is left intact. */
                char    akey[160];
                sol_u32 doomed = st->selected_handle;
                if (mesh_asset_key(o, akey))
                    asset_release(&g_mesh_assets, akey);
                st->selected_handle = 0;
                if (st->resize_board       == doomed) st->resize_board       = 0;
                if (st->move_board         == doomed) st->move_board         = 0;
                if (st->drag_handle        == doomed) st->drag_handle        = 0;
                if (st->drop_target_handle == doomed) st->drop_target_handle = 0;
                scene_remove(&st->scene, doomed);
                scene_save(&st->scene, "scene.stml");
                printf("deleted folder #%u - its page survives\n", (unsigned)doomed);
            } else if (o && o->kind == KIND_NOTE) {
                /* a selected note (not in edit mode — read_input returns above
                   while typing): delete it. Release its keyed card mesh, drop any
                   carry/resize references, and rebuild arrows so connections to it
                   go dormant. */
                char    akey[160];
                sol_u32 doomed = st->selected_handle;
                if (mesh_asset_key(o, akey))
                    asset_release(&g_mesh_assets, akey);
                st->selected_handle = 0;
                if (st->resize_board == doomed) st->resize_board = 0;
                if (st->move_board   == doomed) st->move_board   = 0;
                if (st->carried      == doomed) st->carried      = 0;
                if (st->drag_handle  == doomed) st->drag_handle  = 0;
                scene_remove(&st->scene, doomed);
                arrows_rebuild(st);
                scene_save(&st->scene, "scene.stml");
                printf("deleted note #%u\n", (unsigned)doomed);
            }
        }
        st->bs_was_down = bs_now;
    }

    /* 'd' in board view: open a prompt, then create a folder book linking the
       current page to the named target; only fires in board view (board_view!=0)
       and only on the leading edge of the key press. The palette/edit early-return
       above already suppresses this while typing. */
    {
        sol_bool d_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS);
        if (d_now && !st->d_was_down && st->board_view != 0) {
            vec3 bl;
            if (board_under_ray(st, pick_ray(st, w), &bl) != 0) {
                st->folder_place_local = bl;
                st->folder_place_has   = SOL_TRUE;
            } else {
                st->folder_place_has   = SOL_FALSE;
            }
            palette_prompt(&st->palette, "/folder name", create_folder_from_name);
        }
        st->d_was_down = d_now;
    }

    /* Cmd+V in board view: paste cut cards if a cut is pending, else the
       clipboard image (Finder-style: paste whatever's on the clipboard). */
    {
        sol_bool paste_now = (sol_bool)(
            (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
             glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) &&
            glfwGetKey(w, GLFW_KEY_V) == GLFW_PRESS);
        if (paste_now && !st->paste_was_down && st->board_view != 0) {
            if (st->cut_count > 0) cmd_paste_cut(st);
            else                   cmd_paste_image(st, w);
        }
        st->paste_was_down = paste_now;
    }

    /* Cmd+X in board view: cut the selection (cards stay, render dimmed). With
       nothing selected it cancels a pending cut. */
    {
        sol_bool cut_now = (sol_bool)(
            (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
             glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) &&
            glfwGetKey(w, GLFW_KEY_X) == GLFW_PRESS);
        if (cut_now && !st->cut_was_down && st->board_view != 0)
            cmd_cut_selection(st);
        st->cut_was_down = cut_now;
    }

    /* Arrow LEFT/RIGHT: cycle the focused board's active page (edge-triggered,
       no selection, no book open). The camera-look arrow handler above is already
       gated with !bv_active so it is inert in board view — no extra guard needed. */
    {
        sol_bool left_now  = (sol_bool)(glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS);
        sol_bool right_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS);
        sol_bool shift     = (sol_bool)(glfwGetKey(w, GLFW_KEY_LEFT_SHIFT)  == GLFW_PRESS ||
                                        glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        if (st->board_view != 0 && st->reader_state == READER_IDLE) {
            if (shift) {                          /* Shift+Right: new page (+ move selection) */
                if (right_now && !st->page_next_was) board_new_page(st);
            } else if (st->selected_handle == 0) {/* plain arrows cycle (nothing selected) */
                if (right_now && !st->page_next_was) cycle_page(st, +1);
                if (left_now  && !st->page_prev_was) cycle_page(st, -1);
            }
        }
        st->page_prev_was = left_now;
        st->page_next_was = right_now;
    }

    /* Up/Down: cycle the selected window's glass color preset (Task 7).
       Suppressed in board view and when carrying/palette (covered by the early-
       return gate above).  The camera-look block above is already guarded so
       UP/DOWN do NOT also turn the camera while a window is selected. */
    if (st->selected_handle != 0 && st->board_view == 0) {
        if (window_on_wall(&st->scene, st->selected_handle)) {
            sol_bool  up   = (sol_bool)(glfwGetKey(w, GLFW_KEY_UP)   == GLFW_PRESS);
            sol_bool  down = (sol_bool)(glfwGetKey(w, GLFW_KEY_DOWN) == GLFW_PRESS);
            sol_bool  now  = (sol_bool)(up || down);
            if (now && !st->win_color_was) {
                const char *cur = scene_meta_get(&st->scene, st->selected_handle, "glass");
                int idx = 0, i;
                for (i = 0; i < WINDOW_GLASS_N; i++)
                    if (cur && strcmp(WINDOW_GLASS[i].name, cur) == 0) { idx = i; break; }
                idx = (idx + (up ? 1 : WINDOW_GLASS_N - 1)) % WINDOW_GLASS_N;
                window_set_glass(st, st->selected_handle, WINDOW_GLASS[idx].name);
                scene_save(&st->scene, "scene.stml");
                printf("window glass: %s\n", WINDOW_GLASS[idx].name);
            }
            st->win_color_was = now;
        } else {
            st->win_color_was = SOL_FALSE;
        }
    } else {
        st->win_color_was = SOL_FALSE;
    }

    /* Left/Right: cycle the selected window's shape style.
       The camera-look block above is already guarded by win_look_free so LEFT/RIGHT
       do NOT also pan the camera while a window is selected. */
    if (st->selected_handle != 0 && st->board_view == 0) {
        if (window_on_wall(&st->scene, st->selected_handle)) {
            sol_bool left  = (sol_bool)(glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS);
            sol_bool right = (sol_bool)(glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS);
            sol_bool now   = (sol_bool)(left || right);
            if (now && !st->win_style_was) {
                SceneObject *so = scene_get(&st->scene, st->selected_handle);
                int idx = so ? (int)(mesh_ref_param("window", so->mesh_params,
                                    so->mesh_param_count, "style") + 0.5f) : 0;
                idx = (idx + (right ? 1 : WINDOW_STYLE_N - 1)) % WINDOW_STYLE_N;
                window_set_style(st, st->selected_handle, idx);
                scene_save(&st->scene, "scene.stml");
                printf("window style: %s\n", WINDOW_STYLE_NAME[idx]);
            }
            st->win_style_was = now;
        } else {
            st->win_style_was = SOL_FALSE;
        }
    } else {
        st->win_style_was = SOL_FALSE;
    }

    /* +/- resize the SELECTED note's body text. read_input has already returned
       above if a note is being edited or the palette is open, so these keys are
       free here. =/+ grows, -/_ shrinks; numpad +/- too. A press gives one clear
       step; HOLDING keeps scaling (dt-rate, like the exposure scrub); the size is
       saved once on release rather than every frame. */
    {
        sol_bool plus_now  = (sol_bool)(glfwGetKey(w, GLFW_KEY_EQUAL) == GLFW_PRESS ||
                                        glfwGetKey(w, GLFW_KEY_KP_ADD) == GLFW_PRESS);
        sol_bool minus_now = (sol_bool)(glfwGetKey(w, GLFW_KEY_MINUS) == GLFW_PRESS ||
                                        glfwGetKey(w, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS);
        sol_bool ts_now    = (sol_bool)(plus_now || minus_now);
        if (ts_now && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_NOTE) {
                float ts    = note_text_size(&st->scene, st->selected_handle);
                float delta = (float)dt * 0.10f;             /* held: continuous m/s */
                char  tb[32];
                if (!st->textsize_was_down) delta += 0.004f; /* initial press: a click */
                ts += plus_now ? delta : -delta;
                if (ts < 0.015f) ts = 0.015f;
                if (ts > 0.180f) ts = 0.180f;
                snprintf(tb, sizeof tb, "%.4f", (double)ts);
                scene_meta_set(&st->scene, st->selected_handle, "text_size", tb);
                note_autosize(st, st->selected_handle);      /* live; rebuilds only on change */
            }
        }
        if (!ts_now && st->textsize_was_down) {              /* released: persist once */
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_NOTE) scene_save(&st->scene, "scene.stml");
        }
        st->textsize_was_down = ts_now;
    }

    /* exposure scrub: '[' down, ']' up (held; dt-scaled). The readout lives
       in the debug panel now (3c) — the window-title hack is retired. */
    {
        float erate = (float)dt * 1.5f;
        if (!st->place_active) {
            if (glfwGetKey(w, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) st->exposure += erate;
            if (glfwGetKey(w, GLFW_KEY_LEFT_BRACKET)  == GLFW_PRESS) st->exposure -= erate;
        }
        if (st->exposure < 0.1f) st->exposure = 0.1f;
        if (st->exposure > 8.0f) st->exposure = 8.0f;
    }
}

/* Decode a page image with stb (via image.c) and upload it as an sRGB texture.
   Returns a zero handle if the file is missing/undecodable. */
static RhiTexture load_texture(const char *path) {
    Image      img;
    RhiTexture tex;
    char       key[320];
    tex.id = 0;
    tex_asset_key(path, SOL_TRUE, key);            /* color images: sRGB */
    if (asset_acquire(&g_tex_assets, key, &tex, sizeof tex))
        return tex;                                /* already resident: share it */
    if (image_load(path, &img)) {
        tex = rhi_create_texture(img.pixels, img.w, img.h, RHI_TEX_SRGB8);
        image_free(&img);
        if (tex.id) {
            asset_store_add(&g_tex_assets, key, &tex, sizeof tex);
            watch_add(path, WATCH_TEX_SRGB, tex);   /* edits land in place */
        }
    } else {
        fprintf(stderr, "image load failed: %s\n", path);
    }
    return tex;
}

/* like load_texture but LINEAR (RGBA8) — for normal/ORM maps that must not be
   sRGB-decoded. distinct cache key (sRGB flag false); no watcher. */
static RhiTexture load_texture_linear(const char *path) {
    Image      img;
    RhiTexture tex;
    char       key[320];
    tex.id = 0;
    tex_asset_key(path, SOL_FALSE, key);           /* linear: data maps, not colour */
    if (asset_acquire(&g_tex_assets, key, &tex, sizeof tex))
        return tex;
    if (image_load(path, &img)) {
        tex = rhi_create_texture(img.pixels, img.w, img.h, RHI_TEX_RGBA8);
        image_free(&img);
        if (tex.id) asset_store_add(&g_tex_assets, key, &tex, sizeof tex);
    } else {
        fprintf(stderr, "image load failed: %s\n", path);
    }
    return tex;
}

/* ---- sandstone floor overlay (sourced-texture experiment, flagged) ----
   A deliberate, reversible departure from synthesized-never-sourced: a PolyHaven
   (CC0) PBR set tiled over room floors, render-time. No scene change, no shader. */
#define FLOOR_TILE_M    2.25f    /* meters per texture-repeat (the tile-size knob) */
#define FLOOR_EPS       0.012f   /* lift above the room's own floor (anti z-fight) */
#define FLOOR_CACHE_MAX 32

static Material g_floor_mat;      /* sandstone; albedo_tex.id == 0 => overlay disabled */
static Material g_book_leather;   /* folder-book cover: red_leather grain, blue-tinted; normal_tex.id 0 => flat */
static struct { float w, d; Mesh mesh; } g_floor_cache[FLOOR_CACHE_MAX];
static int      g_floor_cache_n = 0;

/* a 3-map PBR material (sourced-texture experiments): albedo sRGB, normal +
   ORM linear (ARM packs AO/Rough/Metal). albedo id 0 = load failed = disabled. */
static Material load_pbr_material(const char *diff, const char *nor, const char *arm) {
    Material m = material_default();
    m.albedo_tex = load_texture(diff);             /* sRGB */
    if (m.albedo_tex.id == 0) return m;            /* missing: stay disabled */
    m.normal_tex   = load_texture_linear(nor);
    m.mr_tex       = load_texture_linear(arm);
    m.ao_tex       = m.mr_tex;                     /* ARM: R=AO, G=rough, B=metal */
    m.base_color   = vec3_make(1.0f, 1.0f, 1.0f);
    m.metallic     = 1.0f;
    m.roughness    = 1.0f;
    m.normal_scale = 1.0f;
    m.ao_strength  = 1.0f;
    return m;
}

static void floor_mat_init(void) {
    g_floor_mat = load_pbr_material(
        "red_sandstone_tiles/red_sandstone_tiles_diff_1k.png",
        "red_sandstone_tiles/red_sandstone_tiles_nor_gl_1k.png",
        "red_sandstone_tiles/red_sandstone_tiles_arm_1k.png");
}

static void wall_mat_init(void) {
    g_wall_mat = load_pbr_material(
        "weathered_brown_planks/weathered_brown_planks_diff_1k.png",
        "weathered_brown_planks/weathered_brown_planks_nor_gl_1k.png",
        "weathered_brown_planks/weathered_brown_planks_arm_1k.png");
}

static void dark_wood_mat_init(void) {
    g_dark_wood = load_pbr_material(
        "dark_wood/dark_wood_diff_1k.png",
        "dark_wood/dark_wood_nor_gl_1k.png",
        "dark_wood/dark_wood_arm_1k.png");
}

static void roof_mat_init(void) {
    g_roof_mat = load_pbr_material(
        "distressed_painted_planks/distressed_painted_planks_diff_1k.png",
        "distressed_painted_planks/distressed_painted_planks_nor_gl_1k.png",
        "distressed_painted_planks/distressed_painted_planks_arm_1k.png");
}

static void path_mat_init(void) {
    g_path_mat = load_pbr_material(
        "red_sandstone_tiles/red_sandstone_tiles_diff_1k.png",
        "red_sandstone_tiles/red_sandstone_tiles_nor_gl_1k.png",
        "red_sandstone_tiles/red_sandstone_tiles_arm_1k.png");
}

static void campus_mat_init(void) {
    g_campus_mat = load_pbr_material(
        "rocky_terrain/rocky_terrain_02_diff_1k.png",
        "rocky_terrain/rocky_terrain_02_nor_gl_1k.png",
        "rocky_terrain/rocky_terrain_02_arm_1k.png");
}

static void stone_mat_init(void) {
    g_stone_mat = load_pbr_material(
        "stone_wall/japanese_stone_wall_diff_1k.png",
        "stone_wall/japanese_stone_wall_nor_gl_1k.png",
        "stone_wall/japanese_stone_wall_arm_1k.png");
}

static void plaster_mat_init(void) {
    g_plaster_mat = load_pbr_material(
        "painted_plaster_wall/painted_plaster_wall_diff_1k.png",
        "painted_plaster_wall/painted_plaster_wall_nor_gl_1k.png",
        "painted_plaster_wall/painted_plaster_wall_arm_1k.png");
}

static void oak_mat_init(void) {
    g_oak_mat = load_pbr_material(
        "oak_veneer/oak_veneer_01_diff_1k.png",
        "oak_veneer/oak_veneer_01_nor_gl_1k.png",
        "oak_veneer/oak_veneer_01_arm_1k.png");
}

/* The folder-book cover material: red_leather (PolyHaven CC0, normal/ARM only —
   NO albedo map), so the COLOR rides base_color (folderbook_materialize sets the
   per-folder blue). If the files are absent it stays flat (no textures) and the
   folders fall back to the plain-blue look. */
static void book_leather_mat_init(void) {
    Material m = material_default();
    m.normal_tex = load_texture_linear("red_leather/leather_red_02_nor_gl_1k.png");
    if (m.normal_tex.id == 0) { g_book_leather = m; return; }   /* missing -> disabled (flat) */
    m.mr_tex = load_texture_linear("red_leather/leather_red_02_arm_1k.png");  /* ARM: R=AO,G=rough,B=metal */
    if (m.mr_tex.id != 0) {
        m.ao_tex      = m.mr_tex;
        m.metallic    = 1.0f;
        m.roughness   = 1.0f;
        m.ao_strength = 1.0f;
    }
    m.normal_scale = 1.0f;
    /* base_color stays white here — the blue is applied per folder */
    g_book_leather = m;
}

/* a w x d floor quad (XZ, +Y up) with meter-based tiling UVs, cached by size so a
   tile is the same physical size in every room. empty mesh on cache overflow. */
static Mesh floor_quad_for(float w, float d) {
    MeshBuilder mb;
    Mesh        m;
    int         i, slot;
    static int  ev = 0;        /* round-robin eviction cursor */
    float       uw, ud;
    sol_u32     a, b2, c, e;
    for (i = 0; i < g_floor_cache_n; i++)
        if (fabs((double)(g_floor_cache[i].w - w)) < 1e-3 &&
            fabs((double)(g_floor_cache[i].d - d)) < 1e-3)
            return g_floor_cache[i].mesh;
    uw = w / FLOOR_TILE_M;
    ud = d / FLOOR_TILE_M;
    mb_init(&mb);
    a  = mb_push_vertex(&mb, -w * 0.5f, 0.0f, -d * 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    b2 = mb_push_vertex(&mb,  w * 0.5f, 0.0f, -d * 0.5f, 0.0f, 1.0f, 0.0f, uw,   0.0f);
    c  = mb_push_vertex(&mb,  w * 0.5f, 0.0f,  d * 0.5f, 0.0f, 1.0f, 0.0f, uw,   ud);
    e  = mb_push_vertex(&mb, -w * 0.5f, 0.0f,  d * 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, ud);
    mb_push_triangle(&mb, a, c, b2);               /* CCW from +Y (matches make_grid) */
    mb_push_triangle(&mb, a, e, c);
    m = mesh_from_builder(&mb);                     /* tangents auto-computed */
    mb_free(&mb);
    /* insert; when full, evict round-robin so a resize drag's size-sweep never
       starves the cache (the floor would otherwise vanish at FLOOR_CACHE_MAX
       distinct sizes). always returns a freshly-built valid mesh. */
    if (g_floor_cache_n < FLOOR_CACHE_MAX) {
        slot = g_floor_cache_n++;
    } else {
        slot = ev;
        ev   = (ev + 1) % FLOOR_CACHE_MAX;
        mesh_destroy(&g_floor_cache[slot].mesh);
    }
    g_floor_cache[slot].w    = w;
    g_floor_cache[slot].d    = d;
    g_floor_cache[slot].mesh = m;
    return m;
}

/* pre-build floor quads for the active rooms — called at the top of render(),
   BEFORE any rhi pass, so no GPU mesh is created mid-encoder. */
static void floor_quads_ensure(AppState *st) {
    sol_u32 i;
    if (g_floor_mat.albedo_tex.id == 0) return;
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        float        w, d;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;
        w = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
        d = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
        (void)floor_quad_for(w, d);
    }
}

/* The six cubemap face bases (B1). The (forward, up) pairs are the standard
   cubemap convention; right = cross(fwd, up) makes dir = fwd + ndc.x*right +
   ndc.y*up the exact inverse of how texture(cube, dir) reads each face. Shared
   by every render-to-cubemap pass (env conversion, irradiance, later prefilter). */
static const vec3 g_face_fwd[6] = {
    { 1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
    { 0.0f, 1.0f, 0.0f}, { 0.0f,-1.0f, 0.0f},
    { 0.0f, 0.0f, 1.0f}, { 0.0f, 0.0f,-1.0f}
};
static const vec3 g_face_up[6] = {
    { 0.0f,-1.0f, 0.0f}, { 0.0f,-1.0f, 0.0f},
    { 0.0f, 0.0f, 1.0f}, { 0.0f, 0.0f,-1.0f},
    { 0.0f,-1.0f, 0.0f}, { 0.0f,-1.0f, 0.0f}
};

/* Bake the equirectangular HDR into a cubemap (B1): render each of the 6 faces
   with the equirect skybox shader, fed that face's fixed camera basis. 90deg
   FOV -> tan(45)=1, square aspect. */
static void build_env_cubemap(AppState *state) {
    int f;
    state->env_cubemap = rhi_create_cubemap(ENV_CUBE_SIZE, SOL_TRUE);

    /* the equirect skybox shader is the conversion tool: bind it + the equirect
       once, then vary only the per-face basis. */
    rhi_set_pipeline(state->skybox_pipeline);
    rhi_bind_texture(state->skybox_tex, 0);
    rhi_set_uniform_int  ("uEquirect", 0);
    rhi_set_uniform_float("uTanHalfFovY", 1.0f);
    rhi_set_uniform_float("uAspect",      1.0f);
    for (f = 0; f < 6; f++) {
        vec3 r = vec3_cross(g_face_fwd[f], g_face_up[f]);
        rhi_begin_cubemap_face(state->env_cubemap, f, 0, ENV_CUBE_SIZE);
        rhi_set_uniform_vec3("uCamForward", g_face_fwd[f].x, g_face_fwd[f].y, g_face_fwd[f].z);
        rhi_set_uniform_vec3("uCamRight",   r.x, r.y, r.z);
        rhi_set_uniform_vec3("uCamUp",      g_face_up[f].x, g_face_up[f].y, g_face_up[f].z);
        rhi_draw(0, 3);
        rhi_end_pass();
    }
    rhi_cubemap_generate_mips(state->env_cubemap);
}

/* Convolve the env cubemap into a diffuse irradiance cubemap (B2): same per-face
   loop, but the shader integrates the cosine-weighted hemisphere around each
   output direction. Tiny target, no mips. */
static void build_irradiance_map(AppState *state) {
    int f;
    state->irradiance_cubemap = rhi_create_cubemap(IRRADIANCE_SIZE, SOL_FALSE);

    rhi_set_pipeline(state->irradiance_pipeline);
    rhi_bind_texture(state->env_cubemap, 0);          /* the B1 cubemap is the input */
    rhi_set_uniform_int  ("uEnvCube", 0);
    rhi_set_uniform_float("uTanHalfFovY", 1.0f);
    rhi_set_uniform_float("uAspect",      1.0f);
    for (f = 0; f < 6; f++) {
        vec3 r = vec3_cross(g_face_fwd[f], g_face_up[f]);
        rhi_begin_cubemap_face(state->irradiance_cubemap, f, 0, IRRADIANCE_SIZE);
        rhi_set_uniform_vec3("uCamForward", g_face_fwd[f].x, g_face_fwd[f].y, g_face_fwd[f].z);
        rhi_set_uniform_vec3("uCamRight",   r.x, r.y, r.z);
        rhi_set_uniform_vec3("uCamUp",      g_face_up[f].x, g_face_up[f].y, g_face_up[f].z);
        rhi_draw(0, 3);
        rhi_end_pass();
    }
}

/* Build the specular prefilter cubemap (C1): each mip is the env convolved with
   the GGX lobe for an increasing roughness. Same per-face loop, but now also per
   mip (a roughness level), rendering into that mip via rhi_begin_cubemap_face. */
static void build_prefilter_map(AppState *state) {
    int mip, f;
    state->prefilter_cubemap = rhi_create_cubemap(PREFILTER_SIZE, SOL_TRUE);

    rhi_set_pipeline(state->prefilter_pipeline);
    rhi_bind_texture(state->env_cubemap, 0);          /* sample the (mipmapped, finite) env cube */
    rhi_set_uniform_int  ("uEnvCube", 0);
    rhi_set_uniform_float("uTanHalfFovY", 1.0f);
    rhi_set_uniform_float("uAspect",      1.0f);
    for (mip = 0; mip < PREFILTER_MIPS; mip++) {
        int   sz    = PREFILTER_SIZE >> mip;
        float rough = (float)mip / (float)(PREFILTER_MIPS - 1);
        rhi_set_uniform_float("uRoughness", rough);
        for (f = 0; f < 6; f++) {
            vec3 r = vec3_cross(g_face_fwd[f], g_face_up[f]);
            rhi_begin_cubemap_face(state->prefilter_cubemap, f, mip, sz);
            rhi_set_uniform_vec3("uCamForward", g_face_fwd[f].x, g_face_fwd[f].y, g_face_fwd[f].z);
            rhi_set_uniform_vec3("uCamRight",   r.x, r.y, r.z);
            rhi_set_uniform_vec3("uCamUp",      g_face_up[f].x, g_face_up[f].y, g_face_up[f].z);
            rhi_draw(0, 3);
            rhi_end_pass();
        }
    }
}

/* Bake the BRDF integration LUT (C2): a single 2D fullscreen pass into a render
   target; its color texture is the LUT (R=F0 scale, G=bias). Environment-
   independent, so this runs once and never changes. */
static void build_brdf_lut(AppState *state) {
    state->brdf_lut_rt = rhi_create_render_target(BRDF_LUT_SIZE, BRDF_LUT_SIZE, RHI_TEX_RGBA16F);
    rhi_begin_pass(state->brdf_lut_rt, RHI_CLEAR_ALL, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_pipeline(state->brdf_lut_pipeline);
    rhi_draw(0, 3);
    rhi_end_pass();
}

/* A changed .glb (the watcher): tear down everything derived from it and
   re-import. Every part's borrow ticket releases (refcounts hit zero, old
   meshes die), the part objects are removed, the embedded textures purge
   (registered at ref 1, never acquired — one release each kills them), the
   memo forgets the path, and the import runs again: the ANCHORS survive
   (they are the durable identity), the bodies rebuild from the fresh file.
   A torn read shows up as "anchor kept, body missing" and heals on the
   editor's next save. */
static void glb_reload(AppState *st, const char *path) {
    sol_u32 doomed[256];
    int     nd = 0, n;
    sol_u32 i;
    char    prefix[320];
    size_t  plen;
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    sprintf(prefix, "g|%s|", path);
    plen = strlen(prefix);
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o  = &st->scene.objects[i];
        const char  *ak = scene_meta_get(&st->scene, o->handle, "akey");
        if (!ak || strncmp(ak, prefix, plen) != 0) continue;
        if (nd < 256) doomed[nd++] = o->handle;
    }
    for (n = 0; n < nd; n++) {
        SceneObject *o  = scene_get(&st->scene, doomed[n]);
        const char  *ak = o ? scene_meta_get(&st->scene, doomed[n], "akey")
                            : (const char *)0;
        if (ak) asset_release(&g_glbpart_assets, ak);
        if (o) memset(&o->mesh, 0, sizeof o->mesh);
        scene_remove(&st->scene, doomed[n]);
    }
    for (n = 0; ; n++) {
        char tkey[320];
        sprintf(tkey, "t|%s#%d|g", path, n);
        if (!asset_release(&g_tex_assets, tkey)) break;
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    glb_memo_remove(path);
    scene_reimport_glbs(st);
    printf("glb hot-reloaded: %s\n", path);
}

/* A changed .hdr: reload the sky and re-run the bakes (between frames,
   nothing is sampling the old ones). The BRDF LUT is hdr-independent and
   stays. The whole lighting mood of the palace changes in one save. */
static void hdr_reload(AppState *st, const char *path) {
    HdrImage sky;
    if (!image_load_hdr(path, &sky)) return;          /* fail-open */
    if (st->skybox_tex.id)         rhi_destroy_texture(st->skybox_tex);
    if (st->env_cubemap.id)        rhi_destroy_texture(st->env_cubemap);
    if (st->irradiance_cubemap.id) rhi_destroy_texture(st->irradiance_cubemap);
    if (st->prefilter_cubemap.id)  rhi_destroy_texture(st->prefilter_cubemap);
    st->skybox_tex = rhi_create_texture_hdr(sky.pixels, sky.w, sky.h);
    image_hdr_free(&sky);
    build_env_cubemap(st);
    build_irradiance_map(st);
    build_prefilter_map(st);
    rhi_flush();    /* the re-bake ran OUTSIDE the frame loop: commit + drain it
                       so it doesn't corrupt the next frame's pool (Metal abort) */
    printf("environment hot-reloaded: %s\n", path);
}

/* the day/night lever (P8 item 9, reserved decision #7 = swap-only): the
   backtick toggles st->night, and this swaps BOTH the sky/IBL (hdr_reload's
   full re-bake) and the directional sun — a warm bright day vs a dim cool
   MOONLIGHT, so night actually reads as night. The cascades cast either way;
   night shadows just go softer and cooler. No day/night CYCLE system (that
   brushes GI, deferred) — just the two curated ends. */
static void apply_time_of_day(AppState *st) {
    if (st->night) {
        hdr_reload(st, "rogland_clear_night_4k.hdr");
        st->light_color     = vec3_make(0.55f, 0.62f, 0.85f);   /* cool moonlight */
        st->light_intensity = 0.8f;
    } else {
        hdr_reload(st, "horn-koppe_spring_4k.hdr");
        st->light_color     = vec3_make(1.0f, 0.95f, 0.85f);    /* warm day */
        st->light_intensity = 3.5f;
    }
}

static void watch_poll(AppState *st) {
    int i;
    for (i = 0; i < g_watch_count; i++) {
        WatchEntry *w = &g_watch[i];
        long        m = fs_mtime(w->path);
        if (m == 0 || m == w->mtime) continue;
        if (w->kind == WATCH_TEX_SRGB) {
            Image img;
            if (image_load(w->path, &img)) {
                rhi_update_texture(w->tex, img.pixels, img.w, img.h, RHI_TEX_SRGB8);
                image_free(&img);
                w->mtime = m;
                printf("texture hot-reloaded: %s\n", w->path);
            }                          /* decode failed: keep old, retry next poll */
        } else if (w->kind == WATCH_GLB) {
            glb_reload(st, w->path);
            w->mtime = m;
        } else if (w->kind == WATCH_SND) {
            load_sound_overrides();           /* edit a knob, hear it: the
                                                 paper-picture moment for ears */
            sound_bank_mint();
            audio_loops_restart();
            w->mtime = m;
            printf("sounds re-minted: %s\n", w->path);
        } else if (w->kind == WATCH_MAT) {
            load_material_overrides();        /* edit a knob, see the stone:
                                                 the same moment for walls */
            asset_store_visit(&g_texgen_assets, texgen_revoice_visit, NULL);
            w->mtime = m;
            printf("materials re-minted: %s\n", w->path);
        } else {
            hdr_reload(st, w->path);
            w->mtime = m;
        }
    }
}

/* The mesh-ref resolver, GPU half (P3 item 1): realize geometry for every
   object whose ref names a generator and whose mesh is still empty. Runs
   AFTER rhi_init (it uploads) and after scene_load or scene-building — the
   data phase stays pure (headless iotest keeps working), realization happens
   here. An unknown ref leaves the object as a transform-only empty, warned:
   placed data must outlive a missing generator (it re-saves intact).
   SINCE P4 ITEM 4 this is also the ACQUIRE path: the registry is consulted
   before any upload, so two objects naming the same shape (same ref, same
   effective params — fifty default cards, twin rooms) share ONE GPU mesh. */
static void scene_resolve_meshes(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        MeshBuilder  mb;
        char         key[160];
        sol_bool     keyed;
        if (!o->mesh_ref || o->mesh.index_count != 0) continue;
        if (strcmp(o->mesh_ref, "arrow") == 0) continue;   /* scene-derived:
                                                              arrows_rebuild owns it */
        keyed = mesh_asset_key(o, key);
        if (keyed && asset_acquire(&g_mesh_assets, key, &o->mesh, sizeof o->mesh))
            continue;                          /* the shape already lives: borrow it */
        mb_init(&mb);
        if (mesh_ref_build(o->mesh_ref, o->mesh_params, o->mesh_param_count, &mb)) {
            o->mesh = mesh_from_builder(&mb);
            if (keyed)
                asset_store_add(&g_mesh_assets, key, &o->mesh, sizeof o->mesh);
        } else {
            fprintf(stderr, "scene: unknown mesh ref \"%s\" on %s — left empty\n",
                    o->mesh_ref, o->nid ? o->nid : "(no nid)");
        }
        mb_free(&mb);
    }

    /* and the texture half (texture side-quest): realize maps for every
       object whose tex ref names a kind and whose material is still bare.
       Same acquire-first handshake — every church on every island borrows
       ONE stone set. An unknown kind leaves the scalars (placed data must
       outlive a missing generator), warned. */
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        char         xkey[200];
        TexgenSet    ts;
        float        knobs[TEXGEN_PARAMS];
        int          k;
        if (!o->tex_ref || o->material.albedo_tex.id) continue;
        if (!texgen_asset_key(o, xkey)) {
            fprintf(stderr, "scene: unknown material kind \"%s\" on %s — scalars only\n",
                    o->tex_ref, o->nid ? o->nid : "(no nid)");
            continue;
        }
        if (!asset_acquire(&g_texgen_assets, xkey, &ts, sizeof ts)) {
            memset(&ts, 0, sizeof ts);
            ts.kind  = texgen_kind(o->tex_ref);
            ts.count = o->tex_param_count;
            for (k = 0; k < o->tex_param_count && k < TEXGEN_PARAMS; k++)
                ts.prefix[k] = o->tex_params[k];
            texgen_compose(ts.kind, ts.prefix, ts.count, knobs);
            if (!texgen_mint(ts.kind, knobs, &ts.albedo, &ts.normal, &ts.orm))
                continue;                          /* fail-open: scalars only */
            asset_store_add(&g_texgen_assets, xkey, &ts, sizeof ts);
        }
        o->material.albedo_tex = ts.albedo;
        o->material.normal_tex = ts.normal;
        o->material.mr_tex     = ts.orm;           /* ORM packed: G=rough B=metal */
        o->material.ao_tex     = ts.orm;           /* R=occlusion, the shared-map idiom */
    }

    /* picture albedo (images on walls): a "picture" object shows its image
       file through the lit-albedo path; decode it once via the texture registry
       (sRGB, hot-reload, shared). KIND_PLAIN, so apply_kind_materials never
       clobbers this. */
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->mesh_ref && strcmp(o->mesh_ref, "picture") == 0 &&
            o->content && o->content[0] && !o->material.albedo_tex.id)
            o->material.albedo_tex = load_texture(o->content);
    }
}

/* Cosmetic until 6e serializes materials: a card's color DERIVES from its
   kind on every load (kind round-trips; materials don't yet), so mirrors
   look right after a reload without material io. */
static void apply_kind_materials(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        Material     m;
        const char  *u;
        if (o->kind == KIND_PLAIN) continue;
        m = material_default();
        switch (o->kind) {
            case KIND_FILE:      m.base_color = vec3_make(0.90f, 0.88f, 0.80f); m.roughness = 0.85f; break;
            case KIND_FOLDER:    m.base_color = vec3_make(0.78f, 0.60f, 0.32f); m.roughness = 0.75f; break;
            case KIND_ALIAS:     m.base_color = vec3_make(0.55f, 0.68f, 0.92f); m.roughness = 0.80f; break;
            case KIND_NOTE:      m.base_color = vec3_make(0.95f, 0.90f, 0.55f); m.roughness = 0.90f; break;
            case KIND_TOMBSTONE: m.base_color = vec3_make(0.32f, 0.32f, 0.36f); m.roughness = 0.95f; break;
            case KIND_PORTAL:    m.base_color = vec3_make(0.32f, 0.22f, 0.13f); m.roughness = 0.60f; break;
            default: break;
        }
        /* a STALE alias (target gone from disk) flags itself dull red —
           flagged, never removed: the system does not garbage-collect intent */
        if (o->kind == KIND_ALIAS) {
            const char *flag = scene_meta_get(s, o->handle, "stale");
            if (flag && strcmp(flag, "1") == 0)
                m.base_color = vec3_make(0.60f, 0.28f, 0.26f);
        }
        /* UNPLACED cards burn brighter — the tray isn't a place, it's a
           STATE, and it should be visible across the room: these are the
           cards you haven't dealt with yet */
        u = scene_meta_get(s, o->handle, "unplaced");
        if (u && strcmp(u, "1") == 0 && o->kind != KIND_TOMBSTONE) {
            m.base_color.x = m.base_color.x * 0.4f + 0.60f;
            m.base_color.y = m.base_color.y * 0.4f + 0.55f;
            m.base_color.z = m.base_color.z * 0.4f + 0.35f;
        }
        o->material = m;
    }
}

/* Re-apply g_book_leather to every folder book's cover, keeping each folder's
   own blue base_color. Needed on load/rebuild because texture handles aren't
   serialized (scene_io keeps only the scalar PBR factors). apply_kind_materials
   skips KIND_PLAIN, so it never touches folderbooks — this is their derive. */
static void folderbook_materialize(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        vec3         keep;
        if (!o->mesh_ref || strcmp(o->mesh_ref, "folderbook") != 0) continue;
        keep = o->material.base_color;               /* the per-folder blue shade */
        o->material.albedo_tex   = g_book_leather.albedo_tex;   /* id 0 (no albedo map) */
        o->material.normal_tex   = g_book_leather.normal_tex;
        o->material.mr_tex       = g_book_leather.mr_tex;
        o->material.ao_tex       = g_book_leather.ao_tex;
        o->material.metallic     = g_book_leather.metallic;
        o->material.roughness    = g_book_leather.roughness;
        o->material.normal_scale = g_book_leather.normal_scale;
        o->material.ao_strength  = g_book_leather.ao_strength;
        o->material.base_color   = keep;             /* keep the blue */
    }
}

/* Find the PROP (KIND_PLAIN) with this name. Names are not unique across
   kinds — the archive mirrors this very repo, so a FILE card named
   "sword.glb" stands two rooms from the sword itself. The first binder
   grabbed the card, whose parent is the archive anchor — and the whole
   mirror room spun while the real sword froze (Fran's find). Kind
   disambiguates: animated props are PLAIN; cards never are. */
static sol_u32 find_prop_by_name(Scene *s, const char *name) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        const char *n;
        if (s->objects[i].kind != KIND_PLAIN) continue;
        n = scene_meta_get(s, s->objects[i].handle, "name");
        if (n && strcmp(n, name) == 0) return s->objects[i].handle;
    }
    return 0;
}

/* After a load, re-find the animated/special objects (6e): runtime handles
   are SESSION identity; names are the durable key we control. A missing
   name yields 0, which every consumer already guards. */
static void bind_runtime_handles(AppState *st) {
    SceneObject *o;
    sol_u32      i;
    st->box_handle  = find_prop_by_name(&st->scene, "the box");
    o = scene_get(&st->scene, st->box_handle);
    st->anchor_handle = o ? o->parent : 0;
    st->page_handle = find_prop_by_name(&st->scene, "the page");
    st->sword_handle = find_prop_by_name(&st->scene, "sword.glb");
    o = scene_get(&st->scene, st->sword_handle);
    st->sword_precess_handle = o ? o->parent : 0;
    st->floor_handle = 0;
    for (i = 0; i < st->scene.count; i++) {
        const SceneObject *f = &st->scene.objects[i];
        if (f->parent == 0 && f->mesh_ref && strcmp(f->mesh_ref, "grid") == 0) {
            st->floor_handle = f->handle;
            break;
        }
    }
    st->selected_handle = 0;
    st->drag_handle     = 0;
    st->current_room    = 0;
    st->portal_debounce = 0;
    st->file_aim     = SOL_FALSE;
    st->file_target  = 0;
    st->place_active = SOL_FALSE;
    st->place_index  = 0;
    st->place_yaw    = 0.0f;
    st->place_label_target = 0;
    memset(&st->place_ghost, 0, sizeof st->place_ghost);
    st->folder_place_has   = SOL_FALSE;
    st->d_was_down         = SOL_FALSE;
    st->paste_was_down     = SOL_FALSE;
    st->cut_count          = 0;
    st->cut_was_down       = SOL_FALSE;
    st->win_color_was      = SOL_FALSE;
    st->win_style_was      = SOL_FALSE;
    /* page_*_was: Task 5 arrow-cycle; drop_target_handle: Task 8 */
    st->page_prev_was      = SOL_FALSE;
    st->page_next_was      = SOL_FALSE;
    st->drop_target_handle = 0;
    /* board multi-select (Board Multi-Select Task 2) */
    st->sel_count        = 0;
    st->marquee_active   = SOL_FALSE;
    st->marquee_dragging = SOL_FALSE;
    st->marquee_add      = SOL_FALSE;
    st->marquee_px_scale = 1.0f;
    st->group_drag       = SOL_FALSE;
}

/* Reconcile every mirror against disk + validate aliases (6c/6d): resolves
   and recolors when anything changed, and points the selection at the first
   arrival (gold + name pin beat scanning fifty identical cards). Returns the
   change count; the CALLER decides whether to save. */
static int rescan_mirrors(AppState *st) {
    sol_u32 mirrors[16];
    sol_u32 i, mc = 0, before;
    int     total = 0;
    for (i = 0; i < st->scene.count && mc < 16; i++) {   /* collect first:
               scanning ADDS objects, which can realloc the array */
        const SceneObject *o  = &st->scene.objects[i];
        const char        *rt = scene_meta_get(&st->scene, o->handle, "room_type");
        const char        *sp = scene_meta_get(&st->scene, o->handle, "source_path");
        if (rt && strcmp(rt, "mirror") == 0 && sp && sp[0] != '\0')
            mirrors[mc++] = o->handle;
    }
    before = st->scene.count;                   /* arrivals append past here */
    for (i = 0; i < mc; i++) {
        const char *sp = scene_meta_get(&st->scene, mirrors[i], "source_path");
        int n = sp ? room_mirror_scan(&st->scene, mirrors[i], sp) : -1;
        if (n > 0) total += n;
    }
    total += workspace_validate_aliases(&st->scene);
    if (total > 0) {
        scene_resolve_meshes(&st->scene);
        apply_kind_materials(&st->scene);
        for (i = before; i < st->scene.count; i++) {
            if (st->scene.objects[i].kind == KIND_FILE ||
                st->scene.objects[i].kind == KIND_FOLDER) {
                char lbuf[16];
                st->selected_handle = st->scene.objects[i].handle;
                printf("arrived: %s\n",
                       object_label(&st->scene, st->selected_handle, lbuf));
                break;
            }
        }
    }
    return total;
}

/* Re-derive everything that hangs off the scene spine after the ACTIVE set
   changes (a workspace switch) — no file load, no glb reimport. NOTE: load_palace
   does NOT call this; it inlines its own (similar but distinct) tail, so any
   derive that must also run on load (e.g. windows_migrate_fills) has to be added
   to BOTH places. */
static void world_rebuild(AppState *st) {
    windows_migrate_fills(st);   /* shaped windows -> oak fill child, before the resolve */
    boards_migrate_pages(st);    /* seed ordered "pages" meta for un-migrated boards */
    scene_resolve_meshes(&st->scene);
    connections_rebuild(st);
    collide_rebuild(&st->colliders, &st->scene);
    meadow_rebuild(st);
    forest_rebuild(st);
    apply_kind_materials(&st->scene);
    folderbook_materialize(&st->scene);   /* re-apply the leather covers */
    st->routes_last_t = 0.0;   /* the world changed (workspace switch / re-derive):
                                  drop the doorway-label route cache so it re-solves
                                  next frame, not flashing the old world's routes */
}

#define PORTAL_TRIGGER_R 1.1f    /* walk within this of a gate's mouth to travel */

/* Switch to the workspace gate `g` leads to and set the camera at its partner. */
static void portal_travel(AppState *st, sol_u32 g) {
    const char *target = scene_meta_get(&st->scene, g, "target_ws");
    const char *retid  = scene_meta_get(&st->scene, g, "target_portal_id");
    sol_u32     ret;
    vec3        pos; float yaw;
    if (!target || !retid) return;
    scene_save(&st->scene, "scene.stml");           /* persist the world you leave */
    strncpy(st->scene.active_ws, target, SOL_WS_NAME_CAP - 1);  /* fixed buffer: */
    st->scene.active_ws[SOL_WS_NAME_CAP - 1] = '\0';            /* a hand-edited overlong name can't overflow */
    world_rebuild(st);
    ret = workspace_find_gate_by_id(&st->scene, retid);
    if (ret != 0) {
        workspace_spawn_at_gate(&st->scene, ret, 1.5f, CAMERA_EYE_HEIGHT, &pos, &yaw);
        st->camera.pos = pos;
        st->camera.yaw = yaw;
        st->portal_debounce = ret;                  /* don't bounce back through it */
    } else {
        st->portal_debounce = 0;                    /* fallback: stay put */
    }
    st->current_room = room_containing(&st->scene, st->camera.pos);
    printf("entered workspace '%s'\n", target);
}

/* Per-frame: if not carrying, fire the first active gate whose mouth you're in
   (skipping the one you just arrived at until you step away from it). */
static void portal_update(AppState *st) {
    sol_u32 i;
    if (st->carried != 0) return;                   /* hands full: no travel */
    /* clear the debounce once you've stepped clear of the arrival gate */
    if (st->portal_debounce != 0) {
        vec3 gp = object_world_pos(&st->scene, st->portal_debounce);
        float dx = st->camera.pos.x - gp.x, dz = st->camera.pos.z - gp.z;
        if ((float)sqrt((double)(dx*dx + dz*dz)) > PORTAL_TRIGGER_R * 1.6f)
            st->portal_debounce = 0;
    }
    for (i = 0; i < st->scene.count; i++) {
        SceneObject *o = &st->scene.objects[i];
        vec3  gp; float dx, dz, dy;
        if (o->kind != KIND_PORTAL) continue;
        if (!scene_object_active(&st->scene, o->handle)) continue;
        if (o->handle == st->portal_debounce) continue;
        gp = object_world_pos(&st->scene, o->handle);
        dx = st->camera.pos.x - gp.x; dz = st->camera.pos.z - gp.z;
        dy = st->camera.pos.y - (gp.y + CAMERA_EYE_HEIGHT);   /* gate base vs eye */
        if ((float)sqrt((double)(dx*dx + dz*dz)) < PORTAL_TRIGGER_R &&
            dy > -1.0f && dy < 2.5f) {               /* roughly at the mouth's height */
            portal_travel(st, o->handle);
            return;                                  /* one switch per frame */
        }
    }
}

/* Load the palace from scene.stml and bring it fully to life (6e): glb
   bodies re-imported from their anchors' refs, procedural meshes resolved,
   kind colors applied, runtime handles re-found by name, the page's image
   (a runtime texture handle) rebound, and the mirrors reconciled against
   today's disk — the folder may have changed while the palace slept.
   SOL_FALSE if the file is absent or won't parse. */
/* timber halls (stage 1): one-time idempotent room-height migration. A room
   still at the old 3.0m default becomes 4.5m; a deliberately-resized height is
   left alone, and re-running on a 4.5m room is a no-op (4.5 != 3.0). Height is
   mesh_params[2] in the room schema {w,d,h,...}. */
static void migrate_room_heights(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        if (o->mesh_ref && strcmp(o->mesh_ref, "room") == 0 &&
            o->mesh_param_count >= 3 &&
            fabs((double)(o->mesh_params[2] - 3.0f)) < 0.01) {
            o->mesh_params[2] = 4.5f;
        }
    }
}

static sol_bool load_palace(AppState *st) {
    Scene fresh, old;
    int   changes;
    char  keep_ws[SOL_WS_NAME_CAP];
    if (!scene_load(&fresh, "scene.stml")) return SOL_FALSE;
    /* THE SWAP, acquire-first (P4 item 4): the fresh scene realizes its
       meshes BEFORE the old scene releases, so a shape present on both
       sides never sees refcount zero — survivors keep their buffers.
       Then the old scene's registry refs go back and its uniquely-owned
       meshes (arrows, glb parts) die outright. */
    strcpy(keep_ws, st->scene.active_ws[0] ? st->scene.active_ws : "home");
    old       = st->scene;
    st->scene = fresh;
    /* RESTORE THE FILTER BEFORE ANY DERIVE: the freshly-loaded scene's active_ws
       is "" (never serialized), so the swap above leaves us unfiltered. The
       derives below READ active_ws — connections_rebuild routes/doors only the
       active rooms, collide_rebuild walls only them. Left unfiltered, both fire
       across EVERY workspace at once: overlapping home rooms from two workspaces
       stack their colliders (a solid wall lands across another's doorway) and the
       route solver treats a hidden workspace's room as a bystander, deflecting a
       door onto the wrong wall. Set the filter here, not at the tail. */
    strcpy(st->scene.active_ws, keep_ws);
    migrate_room_heights(&st->scene);   /* timber halls: 3.0m rooms -> 4.5m (idempotent) */
    scene_reimport_glbs(st);
    windows_migrate_fills(st);          /* shaped windows -> oak fill child, before the ACQUIRE */
    boards_migrate_pages(st);    /* seed ordered "pages" meta for un-migrated boards */
    scene_resolve_meshes(&st->scene);             /* ACQUIRE (the new) */
    scene_release_meshes(&old);                   /* RELEASE (the old) */
    scene_free(&old);
    arrows_rebuild(st);              /* edges re-derive from the loaded cards */
    connections_rebuild(st);         /* rooms + walkway connectors re-derive */
    collide_rebuild(&st->colliders, &st->scene);  /* and so do the walls */
    meadow_rebuild(st);                           /* and the grass */
    forest_rebuild(st);                           /* and the woods */
    apply_kind_materials(&st->scene);
    folderbook_materialize(&st->scene);           /* re-apply the leather covers */
    st->routes_last_t = 0.0;   /* fresh world: re-solve doorway-label routes next frame */
    bind_runtime_handles(st);
    adopt_legacy_motion(st);         /* pre-component saves get their dance back */
    {
        SceneObject *p = scene_get(&st->scene, st->page_handle);
        if (p) p->material.albedo_tex = st->albedo_tex;
    }
    changes = rescan_mirrors(st);
    if (changes > 0) {
        scene_save(&st->scene, "scene.stml");
        printf("reconciled on load: %d change(s)\n", changes);
    }
    return SOL_TRUE;   /* active_ws already restored above, before the derives */
}

/* Build a FRESH scene: one floating "home" room you spawn into — the hub the
   filesystem-tree roots will hang off (later phases). Replaces the old P3 demo
   palace, which is preserved in git history. */
static void populate_home_scene(AppState *state) {
    Mesh     empty = {0};
    sol_u32  home, shell;
    float    home_p[8];
    Material stone = material_default();

    scene_init(&state->scene);

    /* the home scene has no demo box/page/anchor — null the handles so the
       guarded references elsewhere all no-op */
    state->box_handle           = 0;
    state->page_handle          = 0;
    state->anchor_handle        = 0;
    state->floor_handle         = 0;
    state->sword_handle         = 0;
    state->sword_precess_handle = 0;

    /* an open-topped 8x8 parametric room (floor + 4 walls, no ceiling) anchored
       high up, so it reads as a platform floating in the sky */
    home_p[0] = 8.0f;  home_p[1] = 8.0f;  home_p[2] = 4.5f;   /* timber halls: tall */
    home_p[3] = 1.0f;  home_p[4] = 1.0f;  home_p[5] = 1.0f;  home_p[6] = 1.0f;
    home_p[7] = 0.0f;

    home = scene_add(&state->scene, 0, empty,
              vec3_make(0.0f, HOME_FLOOR_Y, 0.0f), quat_identity(),
              vec3_make(1.0f, 1.0f, 1.0f));
    scene_meta_set(&state->scene, home, "room_type", "home");
    scene_meta_set(&state->scene, home, "name", "home");

    shell = scene_add(&state->scene, home, empty,
              vec3_make(0.0f, 0.0f, 0.0f), quat_identity(),
              vec3_make(1.0f, 1.0f, 1.0f));
    scene_mesh_ref_set(&state->scene, shell, "room");
    scene_mesh_params_set(&state->scene, shell, home_p, 8);

    stone.base_color = vec3_make(0.58f, 0.55f, 0.50f);
    stone.roughness  = 0.92f;
    scene_material_set(&state->scene, shell, stone);

    scene_resolve_meshes(&state->scene);

    /* Persist the fresh home scene — unless a scene.stml EXISTS but failed to
       load (corrupt?): never overwrite what might be the user's palace. The
       persistence self-check below loads this back, so it must exist on disk. */
    {
        FILE *probe = fopen("scene.stml", "rb");
        if (probe) {
            fclose(probe);
            printf("scene.stml exists but did not load — NOT overwriting it\n");
        } else if (scene_save(&state->scene, "scene.stml")) {
            printf("saved home scene -> scene.stml\n");
        }
    }
}

/* One-time setup: build the pipeline + meshes, populate the scene. */
static int init_scene(AppState *state) {
    RhiShader shader;
    RhiPipelineDesc desc = {0};  /* {0} all descs: a future field defaults off, not garbage */

    shader = rhi_create_shader(VERTEX_SRC, FRAGMENT_SRC);
    /* item 10: a failed shader DEGRADES instead of failing the launch — the
       seam's id-0 contract (enforced in BOTH backends: a zero shader yields
       a zero pipeline, a zero pipeline swallows its draws) lets the staged
       Metal port boot with partial twins. The compile failure still reports
       loudly on stderr in either backend; nothing is silent. The pattern
       holds for every shader below — only RESOURCE failures stay fatal. */

    /* one pipeline, shared by all objects (same 8-float layout) */
    desc.shader = shader;
    desc.attrs[0].location = 0; desc.attrs[0].format = RHI_FORMAT_FLOAT3; desc.attrs[0].offset = 0;
    desc.attrs[1].location = 1; desc.attrs[1].format = RHI_FORMAT_FLOAT3;
    desc.attrs[1].offset = 3 * sizeof(float);
    desc.attrs[2].location = 2; desc.attrs[2].format = RHI_FORMAT_FLOAT2;
    desc.attrs[2].offset = 6 * sizeof(float);
    desc.attrs[3].location = 3; desc.attrs[3].format = RHI_FORMAT_FLOAT4;
    desc.attrs[3].offset = 8 * sizeof(float);   /* tangent (item 8d); unused by the shader until 8d-2 */
    desc.attr_count = 4;
    desc.stride     = 12 * sizeof(float);
    desc.depth_test = SOL_TRUE;
    desc.blend      = SOL_FALSE;
    state->pipeline = rhi_create_pipeline(&desc);
    {   /* P9 item 2: the glass twin — same PBR shader + vertex layout, but
           alpha-blended and depth-write-off (the water precedent). */
        RhiPipelineDesc gd = desc;
        gd.blend           = RHI_BLEND_ALPHA;
        gd.depth_write_off = SOL_TRUE;
        state->glass_pipeline = rhi_create_pipeline(&gd);
    }
    {   /* P9 item 3: the decal pipeline — same vertex layout, the unlit decal
           shader, alpha-blend + depth-write-off (weathering quads). */
        RhiPipelineDesc dd = desc;
        dd.shader          = rhi_create_shader(DECAL_VERTEX_SRC, DECAL_FRAGMENT_SRC);
        dd.blend           = RHI_BLEND_ALPHA;
        dd.depth_write_off = SOL_TRUE;
        if (dd.shader.id) state->decal_pipeline = rhi_create_pipeline(&dd);
    }
    {   /* Portal Material: the energy-pane pipeline — same vertex layout, the
           procedural energy shader, opaque (depth-tested, writes depth). */
        RhiPipelineDesc pp = desc;
        pp.shader = rhi_create_shader(PORTAL_VERTEX_SRC, PORTAL_FRAGMENT_SRC);
        if (pp.shader.id) state->portal_pipeline = rhi_create_pipeline(&pp);
    }

    /* the skinned twins (item 9): the canonical 12 floats + joints4 +
       weights4 = 20-float stride; same fragment shader (a skinned surface
       is lit like any surface), and a palette-blending shadow twin so a
       running fox never casts a T-posed shadow */
    {
        RhiShader       skin_sh, skin_sh_shadow;
        RhiPipelineDesc skd  = {0};
        RhiPipelineDesc skds = {0};
        skin_sh = rhi_create_shader(SKINNED_VERTEX_SRC, FRAGMENT_SRC);
        if (skin_sh.id) {
            skd.shader = skin_sh;
            skd.attrs[0].location = 0; skd.attrs[0].format = RHI_FORMAT_FLOAT3;
            skd.attrs[0].offset   = 0;
            skd.attrs[1].location = 1; skd.attrs[1].format = RHI_FORMAT_FLOAT3;
            skd.attrs[1].offset   = 3 * sizeof(float);
            skd.attrs[2].location = 2; skd.attrs[2].format = RHI_FORMAT_FLOAT2;
            skd.attrs[2].offset   = 6 * sizeof(float);
            skd.attrs[3].location = 3; skd.attrs[3].format = RHI_FORMAT_FLOAT4;
            skd.attrs[3].offset   = 8 * sizeof(float);
            skd.attrs[4].location = 4; skd.attrs[4].format = RHI_FORMAT_FLOAT4;
            skd.attrs[4].offset   = 12 * sizeof(float);
            skd.attrs[5].location = 5; skd.attrs[5].format = RHI_FORMAT_FLOAT4;
            skd.attrs[5].offset   = 16 * sizeof(float);
            skd.attr_count = 6;
            skd.stride     = 20 * sizeof(float);
            skd.depth_test = SOL_TRUE;
            skd.blend      = SOL_FALSE;
            state->skinned_pipeline = rhi_create_pipeline(&skd);
        }
        skin_sh_shadow = rhi_create_shader(SKINNED_SHADOW_VERTEX_SRC,
                                           SHADOW_FRAGMENT_SRC);
        if (skin_sh_shadow.id) {
            skds.shader = skin_sh_shadow;
            skds.attrs[0].location = 0; skds.attrs[0].format = RHI_FORMAT_FLOAT3;
            skds.attrs[0].offset   = 0;
            skds.attrs[1].location = 4; skds.attrs[1].format = RHI_FORMAT_FLOAT4;
            skds.attrs[1].offset   = 12 * sizeof(float);
            skds.attrs[2].location = 5; skds.attrs[2].format = RHI_FORMAT_FLOAT4;
            skds.attrs[2].offset   = 16 * sizeof(float);
            skds.attr_count = 3;
            skds.stride     = 20 * sizeof(float);
            skds.depth_test = SOL_TRUE;
            skds.blend      = SOL_FALSE;
            state->skinned_shadow_pipeline = rhi_create_pipeline(&skds);
        }
    }

    /* the fullscreen post pass: no vertex attributes (gl_VertexID builds the
       triangle), no depth test (it's a screen-space overlay) */
    {
        RhiShader       post_shader;
        RhiPipelineDesc post_desc = {0};
        post_shader = rhi_create_shader(POST_VERTEX_SRC, POST_FRAGMENT_SRC);
        post_desc.shader     = post_shader;
        post_desc.attr_count = 0;
        post_desc.stride     = 0;
        post_desc.depth_test = SOL_FALSE;
        post_desc.blend      = SOL_FALSE;
        state->post_pipeline = rhi_create_pipeline(&post_desc);
        state->grade_luts[0] = build_grade_lut(0);   /* P9 item 1: identity strip */
        state->grade_luts[1] = build_grade_lut(1);   /* P9 item 1: filmic split-tone */
        state->decal_atlas   = build_decal_atlas();  /* P9 item 3: stain + moss atlas */
        {   /* inventory thumbnail scratch + neutral post inputs (created once) */
            unsigned char wpx[4] = { 255, 255, 255, 255 };
            unsigned char bpx[4] = { 0, 0, 0, 255 };
            state->inv_thumb_hdr = rhi_create_render_target(INV_THUMB_SIZE, INV_THUMB_SIZE, RHI_TEX_RGBA16F);
            state->inv_white_tex = rhi_create_texture(wpx, 1, 1, RHI_TEX_RGBA8);
            state->inv_black_tex = rhi_create_texture(bpx, 1, 1, RHI_TEX_RGBA8);
        }
    }

    {   /* god-rays (P8 item 3): a fullscreen raymarch, same desc shape as post */
        RhiPipelineDesc gr_desc = {0};
        gr_desc.shader     = rhi_create_shader(POST_VERTEX_SRC, GODRAY_FRAGMENT_SRC);
        gr_desc.attr_count = 0;
        gr_desc.stride     = 0;
        gr_desc.depth_test = SOL_FALSE;
        gr_desc.blend      = SOL_FALSE;
        state->godray_pipeline = rhi_create_pipeline(&gr_desc);
    }

    {   /* soft particles (P8 item 4): the depth-copy pass */
        RhiPipelineDesc cp_desc = {0};
        cp_desc.shader     = rhi_create_shader(POST_VERTEX_SRC, DEPTHCOPY_FRAGMENT_SRC);
        cp_desc.attr_count = 0;
        cp_desc.stride     = 0;
        cp_desc.depth_test = SOL_FALSE;
        cp_desc.blend      = SOL_FALSE;
        state->copy_pipeline = rhi_create_pipeline(&cp_desc);
    }

    {   /* SSAO (P8 item 5): the occlusion pass + its bilateral blur */
        RhiPipelineDesc sd = {0}, sbd = {0};
        sd.shader     = rhi_create_shader(POST_VERTEX_SRC, SSAO_FRAGMENT_SRC);
        sd.attr_count = 0; sd.stride = 0;
        sd.depth_test = SOL_FALSE; sd.blend = SOL_FALSE;
        state->ssao_pipeline = rhi_create_pipeline(&sd);
        sbd.shader     = rhi_create_shader(POST_VERTEX_SRC, SSAO_BLUR_FRAGMENT_SRC);
        sbd.attr_count = 0; sbd.stride = 0;
        sbd.depth_test = SOL_FALSE; sbd.blend = SOL_FALSE;
        state->ssao_blur_pipeline = rhi_create_pipeline(&sbd);
    }

    /* the shadow map + its two pipelines (item 9b): a depth-only pass that reads
       just position, and a fullscreen inspector that shows the depth map. */
    {
        RhiShader       sh_shader, dbg_shader;
        RhiPipelineDesc sh_desc = {0}, dbg_desc = {0};

        {
            int sc;
            for (sc = 0; sc < SHADOW_CASCADES; sc++) {
                state->shadow_rt[sc] = rhi_create_depth_target(SHADOW_MAP_SIZE,
                                                               SHADOW_MAP_SIZE);
                if (!state->shadow_rt[sc].id) return 0;
            }
        }
        /* the spot sconce's map (P8 item 7): created unconditionally so unit 9
           is always bindable; rendered into only when a caster exists */
        state->spot_shadow_rt = rhi_create_depth_target(SPOT_SHADOW_SIZE,
                                                        SPOT_SHADOW_SIZE);
        if (!state->spot_shadow_rt.id) return 0;

        sh_shader = rhi_create_shader(SHADOW_VERTEX_SRC, SHADOW_FRAGMENT_SRC);
        sh_desc.shader     = sh_shader;
        sh_desc.attrs[0].location = 0; sh_desc.attrs[0].format = RHI_FORMAT_FLOAT3; sh_desc.attrs[0].offset = 0;
        sh_desc.attr_count = 1;                 /* position only */
        sh_desc.stride     = 12 * sizeof(float);  /* same VBO; skip the other 9 floats */
        sh_desc.depth_test = SOL_TRUE;
        sh_desc.blend      = SOL_FALSE;
        state->shadow_pipeline = rhi_create_pipeline(&sh_desc);

        dbg_shader = rhi_create_shader(POST_VERTEX_SRC, SHADOW_DEBUG_FRAGMENT_SRC);
        dbg_desc.shader     = dbg_shader;
        dbg_desc.attr_count = 0;
        dbg_desc.stride     = 0;
        dbg_desc.depth_test = SOL_FALSE;
        dbg_desc.blend      = SOL_FALSE;
        state->shadow_debug_pipeline = rhi_create_pipeline(&dbg_desc);
    }

    /* the skybox pipeline (Phase A2): fullscreen triangle, no attrs, depth off */
    {
        RhiShader       sky_shader;
        RhiPipelineDesc sky_desc = {0};
        sky_shader = rhi_create_shader(SKYBOX_VERTEX_SRC, SKYBOX_FRAGMENT_SRC);
        sky_desc.shader     = sky_shader;
        sky_desc.attr_count = 0;
        sky_desc.stride     = 0;
        sky_desc.depth_test = SOL_FALSE;
        sky_desc.blend      = SOL_FALSE;
        state->skybox_pipeline = rhi_create_pipeline(&sky_desc);
    }

    /* the cubemap skybox pipeline (B1): same fullscreen triangle, samplerCube */
    {
        RhiShader       cube_shader;
        RhiPipelineDesc cube_desc = {0};
        cube_shader = rhi_create_shader(SKYBOX_VERTEX_SRC, SKYBOX_CUBE_FRAGMENT_SRC);
        cube_desc.shader     = cube_shader;
        cube_desc.attr_count = 0;
        cube_desc.stride     = 0;
        cube_desc.depth_test = SOL_FALSE;
        cube_desc.blend      = SOL_FALSE;
        state->skybox_cube_pipeline = rhi_create_pipeline(&cube_desc);
    }

    /* the irradiance convolution pipeline (B2): fullscreen triangle, depth off */
    {
        RhiShader       irr_shader;
        RhiPipelineDesc irr_desc = {0};
        irr_shader = rhi_create_shader(SKYBOX_VERTEX_SRC, IRRADIANCE_FRAGMENT_SRC);
        irr_desc.shader     = irr_shader;
        irr_desc.attr_count = 0;
        irr_desc.stride     = 0;
        irr_desc.depth_test = SOL_FALSE;
        irr_desc.blend      = SOL_FALSE;
        state->irradiance_pipeline = rhi_create_pipeline(&irr_desc);
    }

    /* the specular prefilter pipeline (C1): fullscreen triangle, depth off */
    {
        RhiShader       pre_shader;
        RhiPipelineDesc pre_desc = {0};
        pre_shader = rhi_create_shader(SKYBOX_VERTEX_SRC, PREFILTER_FRAGMENT_SRC);
        pre_desc.shader     = pre_shader;
        pre_desc.attr_count = 0;
        pre_desc.stride     = 0;
        pre_desc.depth_test = SOL_FALSE;
        pre_desc.blend      = SOL_FALSE;
        state->prefilter_pipeline = rhi_create_pipeline(&pre_desc);
    }

    /* the BRDF LUT pipeline (C2): fullscreen triangle, depth off */
    {
        RhiShader       brdf_shader;
        RhiPipelineDesc brdf_desc = {0};
        brdf_shader = rhi_create_shader(POST_VERTEX_SRC, BRDF_LUT_FRAGMENT_SRC);
        brdf_desc.shader     = brdf_shader;
        brdf_desc.attr_count = 0;
        brdf_desc.stride     = 0;
        brdf_desc.depth_test = SOL_FALSE;
        brdf_desc.blend      = SOL_FALSE;
        state->brdf_lut_pipeline = rhi_create_pipeline(&brdf_desc);
    }

    /* the bloom pipelines (P4 item 5 piece 3): three fullscreen-triangle
       passes sharing POST_VERTEX_SRC; the upsample blends ADDITIVELY into
       targets that already hold their own level's content */
    {
        RhiShader       es, ds, us2;
        RhiPipelineDesc bd = {0};
        es = rhi_create_shader(POST_VERTEX_SRC, BLOOM_EXTRACT_FRAGMENT_SRC);
        ds = rhi_create_shader(POST_VERTEX_SRC, BLOOM_DOWN_FRAGMENT_SRC);
        us2 = rhi_create_shader(POST_VERTEX_SRC, BLOOM_UP_FRAGMENT_SRC);
        if (es.id && ds.id && us2.id) {
            bd.attr_count = 0;
            bd.stride     = 0;
            bd.depth_test = SOL_FALSE;
            bd.blend      = RHI_BLEND_NONE;
            bd.shader     = es;
            state->bloom_extract_pipeline = rhi_create_pipeline(&bd);
            bd.shader     = ds;
            state->bloom_down_pipeline    = rhi_create_pipeline(&bd);
            bd.shader     = us2;
            bd.blend      = RHI_BLEND_ADD;          /* the accumulating walk up */
            state->bloom_up_pipeline      = rhi_create_pipeline(&bd);
        }
        state->bloom_on = SOL_TRUE;
    }
    state->show_hud = SOL_TRUE;        /* the stats card shows by default */

    /* the meadow's machinery (P4 item 3 piece 2): ONE crossed-tapered-quad
       tuft (stream 0) shared by every island; the per-island instance
       buffers come from meadow_rebuild after the palace loads */
    {
        static const float TUFT[24] = {
            -0.35f, 0.0f, 0.0f,    0.35f, 0.0f, 0.0f,    /* quad in XY, tapered */
             0.14f, 1.0f, 0.0f,   -0.14f, 1.0f, 0.0f,
             0.0f, 0.0f, -0.35f,   0.0f, 0.0f, 0.35f,    /* crossed quad in ZY */
             0.0f, 1.0f,  0.14f,   0.0f, 1.0f, -0.14f
        };
        static const sol_u32 TIDX[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
        RhiShader       msh;
        RhiPipelineDesc mdesc = {0};
        msh = rhi_create_shader(MEADOW_VERTEX_SRC, MEADOW_FRAGMENT_SRC);
        if (msh.id) {
            mdesc.shader = msh;
            mdesc.attr_count = 3;
            mdesc.attrs[0].location = 0;  mdesc.attrs[0].format = RHI_FORMAT_FLOAT3;
            mdesc.attrs[0].offset   = 0;  mdesc.attrs[0].per_instance = 0;
            mdesc.attrs[1].location = 1;  mdesc.attrs[1].format = RHI_FORMAT_FLOAT4;
            mdesc.attrs[1].offset   = 0;  mdesc.attrs[1].per_instance = 1;
            mdesc.attrs[2].location = 2;  mdesc.attrs[2].format = RHI_FORMAT_FLOAT4;
            mdesc.attrs[2].offset   = 16; mdesc.attrs[2].per_instance = 1;
            mdesc.stride          = 3 * sizeof(float);
            mdesc.instance_stride = 8 * sizeof(float);
            mdesc.depth_test      = SOL_TRUE;
            mdesc.blend           = SOL_FALSE;
            state->meadow_pipeline = rhi_create_pipeline(&mdesc);
            state->meadow_vbuf = rhi_create_buffer(RHI_BUFFER_VERTEX, TUFT, sizeof TUFT);
            state->meadow_ibuf = rhi_create_buffer(RHI_BUFFER_INDEX,  TIDX, sizeof TIDX);
            {   /* the bloom (item 7): a short tuft splayed wider at the
                   top — petals. Same layout/shader as grass; the bright
                   tint comes from the per-instance color. */
                static const float BLOOM[24] = {
                    -0.12f, 0.0f, 0.0f,    0.12f, 0.0f, 0.0f,
                     0.32f, 0.7f, 0.0f,   -0.32f, 0.7f, 0.0f,
                     0.0f, 0.0f, -0.12f,   0.0f, 0.0f, 0.12f,
                     0.0f, 0.7f,  0.32f,   0.0f, 0.7f, -0.32f
                };
                static const sol_u32 BIDX[12] = { 0,1,2, 0,2,3, 4,5,6, 4,6,7 };
                state->flower_vbuf = rhi_create_buffer(RHI_BUFFER_VERTEX, BLOOM, sizeof BLOOM);
                state->flower_ibuf = rhi_create_buffer(RHI_BUFFER_INDEX,  BIDX, sizeof BIDX);
            }
        }
    }

    /* the particle machinery (P4 item 7 piece 2): one shared unit quad
       (stream 0), one pool-sized instance buffer re-uploaded per frame
       (stream 1 — the meadow's exact 8-float layout, but ALIVE), additive,
       depth-tested but write-off: hidden by walls, occluding nothing. */
    {
        static const float QUAD[8] = {
            -0.5f, -0.5f,   0.5f, -0.5f,   0.5f, 0.5f,   -0.5f, 0.5f
        };
        static const sol_u32 QIDX[6] = { 0, 1, 2, 0, 2, 3 };
        RhiShader       psh;
        RhiPipelineDesc pdesc = {0};
        particles_init(&state->particles);
        component_set_particle_pool(&state->particles);   /* the emit outlet */
        component_set_ground_fn(wander_ground, state);    /* the wander outlet */
        psh = rhi_create_shader(PARTICLE_VERTEX_SRC, PARTICLE_FRAGMENT_SRC);
        if (psh.id) {
            pdesc.shader = psh;
            pdesc.attr_count = 3;
            pdesc.attrs[0].location = 0;  pdesc.attrs[0].format = RHI_FORMAT_FLOAT2;
            pdesc.attrs[0].offset   = 0;  pdesc.attrs[0].per_instance = 0;
            pdesc.attrs[1].location = 1;  pdesc.attrs[1].format = RHI_FORMAT_FLOAT4;
            pdesc.attrs[1].offset   = 0;  pdesc.attrs[1].per_instance = 1;
            pdesc.attrs[2].location = 2;  pdesc.attrs[2].format = RHI_FORMAT_FLOAT4;
            pdesc.attrs[2].offset   = 16; pdesc.attrs[2].per_instance = 1;
            pdesc.stride          = 2 * sizeof(float);
            pdesc.instance_stride = PARTICLE_INST_FLOATS * sizeof(float);
            pdesc.depth_test      = SOL_TRUE;
            pdesc.depth_write_off = SOL_TRUE;
            pdesc.blend           = RHI_BLEND_ADD;
            state->part_pipeline = rhi_create_pipeline(&pdesc);
            state->part_vbuf = rhi_create_buffer(RHI_BUFFER_VERTEX, QUAD, sizeof QUAD);
            state->part_ibuf = rhi_create_buffer(RHI_BUFFER_INDEX,  QIDX, sizeof QIDX);
            state->part_inst = rhi_create_buffer(RHI_BUFFER_VERTEX, (const void *)0,
                                  PARTICLE_CAP * PARTICLE_INST_FLOATS * sizeof(float));
        }
    }

    /* the instanced-ornament machinery (P6 item 10: the item-7 debt paid):
       ONE canonical baluster in the full 12-float layout (stream 0),
       per-balustrade local-slot buffers (stream 1), the whole PBR
       fragment over every copy — and the shadow twin, because masonry
       casts. The buffers come from ornament_sync once the palace loads. */
    {
        RhiShader       osh, oshs;
        RhiPipelineDesc od  = {0};
        RhiPipelineDesc osd = {0};
        MeshBuilder     mb;
        mb_init(&mb); gothic_baluster_unit(&mb);
        state->orn_mesh[ORN_BALUSTER] = mesh_from_builder(&mb);
        mb_free(&mb);
        mb_init(&mb); flora_leafcard_unit(&mb, FLORA_LEAF_BROAD);
        state->orn_mesh[ORN_LEAF_BROAD] = mesh_from_builder(&mb);
        mb_free(&mb);
        mb_init(&mb); flora_leafcard_unit(&mb, FLORA_LEAF_CONIFER);
        state->orn_mesh[ORN_LEAF_CONIFER] = mesh_from_builder(&mb);
        mb_free(&mb);
        osh = rhi_create_shader(ORNAMENT_VERTEX_SRC, FRAGMENT_SRC);
        if (osh.id) {
            od.shader = osh;
            od.attrs[0].location = 0; od.attrs[0].format = RHI_FORMAT_FLOAT3;
            od.attrs[0].offset   = 0;
            od.attrs[1].location = 1; od.attrs[1].format = RHI_FORMAT_FLOAT3;
            od.attrs[1].offset   = 3 * sizeof(float);
            od.attrs[2].location = 2; od.attrs[2].format = RHI_FORMAT_FLOAT2;
            od.attrs[2].offset   = 6 * sizeof(float);
            od.attrs[3].location = 3; od.attrs[3].format = RHI_FORMAT_FLOAT4;
            od.attrs[3].offset   = 8 * sizeof(float);
            od.attrs[4].location = 4; od.attrs[4].format = RHI_FORMAT_FLOAT4;
            od.attrs[4].offset   = 0;  od.attrs[4].per_instance = 1;
            od.attrs[5].location = 5; od.attrs[5].format = RHI_FORMAT_FLOAT4;
            od.attrs[5].offset   = 16; od.attrs[5].per_instance = 1;
            od.attr_count      = 6;
            od.stride          = 12 * sizeof(float);
            od.instance_stride = 8 * sizeof(float);
            od.depth_test      = SOL_TRUE;
            od.blend           = SOL_FALSE;
            state->ornament_pipeline = rhi_create_pipeline(&od);
        }
        oshs = rhi_create_shader(ORNAMENT_SHADOW_VERTEX_SRC,
                                 SHADOW_FRAGMENT_SRC);
        if (oshs.id) {
            osd.shader = oshs;
            osd.attrs[0].location = 0; osd.attrs[0].format = RHI_FORMAT_FLOAT3;
            osd.attrs[0].offset   = 0;
            osd.attrs[1].location = 4; osd.attrs[1].format = RHI_FORMAT_FLOAT4;
            osd.attrs[1].offset   = 0;  osd.attrs[1].per_instance = 1;
            osd.attrs[2].location = 5; osd.attrs[2].format = RHI_FORMAT_FLOAT4;
            osd.attrs[2].offset   = 16; osd.attrs[2].per_instance = 1;
            osd.attr_count      = 3;
            osd.stride          = 12 * sizeof(float);
            osd.instance_stride = 8 * sizeof(float);
            osd.depth_test      = SOL_TRUE;
            osd.blend           = SOL_FALSE;
            state->ornament_shadow_pipeline = rhi_create_pipeline(&osd);
        }
    }

    /* the forest's variant meshes + shared bark, built once (P7 item 5):
       must exist before any forest_rebuild scatters them */
    forest_build_variants(state);

    /* the water pipeline + ripple normal (P7 item 8): the disc rides the
       standard 12-float vertex layout; alpha-blended, depth-tested but
       write-off (drawn last, occluding nothing) */
    {
        RhiShader       wsh;
        RhiPipelineDesc wd = {0};
        wsh = rhi_create_shader(WATER_VERTEX_SRC, WATER_FRAGMENT_SRC);
        if (wsh.id) {
            wd.shader = wsh;
            wd.attrs[0].location = 0; wd.attrs[0].format = RHI_FORMAT_FLOAT3; wd.attrs[0].offset = 0;
            wd.attrs[1].location = 1; wd.attrs[1].format = RHI_FORMAT_FLOAT3; wd.attrs[1].offset = 3 * sizeof(float);
            wd.attrs[2].location = 2; wd.attrs[2].format = RHI_FORMAT_FLOAT2; wd.attrs[2].offset = 6 * sizeof(float);
            wd.attrs[3].location = 3; wd.attrs[3].format = RHI_FORMAT_FLOAT4; wd.attrs[3].offset = 8 * sizeof(float);
            wd.attr_count      = 4;
            wd.stride          = 12 * sizeof(float);
            wd.depth_test      = SOL_TRUE;
            wd.depth_write_off = SOL_TRUE;
            wd.blend           = RHI_BLEND_ALPHA;
            state->water_pipeline = rhi_create_pipeline(&wd);
        }
        {   /* the ripple normal map — synthesized once, scrolled by the
               shader (texgen animates by phase, not re-render) */
            float wk[TEXGEN_PARAMS];
            const float *wdf;
            unsigned char *nb = (unsigned char *)malloc(
                (size_t)TEXGEN_SIZE * TEXGEN_SIZE * 4);
            unsigned char *ab = (unsigned char *)malloc(
                (size_t)TEXGEN_SIZE * TEXGEN_SIZE * 4);
            unsigned char *ob = (unsigned char *)malloc(
                (size_t)TEXGEN_SIZE * TEXGEN_SIZE * 4);
            int k;
            texgen_schema(TEXGEN_WATER, (const char *const **)0, &wdf);
            for (k = 0; k < TEXGEN_PARAMS; k++) wk[k] = wdf[k];
            if (ab && nb && ob &&
                texgen_render(TEXGEN_WATER, wk, TEXGEN_PARAMS, ab, nb, ob))
                state->water_ripple = rhi_create_texture(nb, TEXGEN_SIZE,
                                          TEXGEN_SIZE, RHI_TEX_RGBA8);
            free(ab); free(nb); free(ob);
        }
    }

    state->albedo_tex = load_texture("paper-picture.png");   /* item 5b: decode via stb */
    floor_mat_init();   /* sandstone floor overlay (sourced experiment, flagged) */
    wall_mat_init();    /* plank walls overlay (sourced experiment, flagged) */
    dark_wood_mat_init();   /* timber halls: corner columns + trusses */
    roof_mat_init();    /* timber halls: pitched roof */
    path_mat_init();    /* sandstone walkway deck + steps between rooms */
    campus_mat_init();  /* rocky campus ground (experiment) */
    stone_mat_init();   /* stone-wall room shell (veneers cover the rest) */
    plaster_mat_init(); /* painted-plaster whiteboard */
    oak_mat_init();     /* oak-veneer file/folder tablets */
    book_leather_mat_init();   /* folder-book covers: red_leather, blue-tinted */

    /* THE PALACE REMEMBERS ITSELF (6e): an existing scene.stml IS the world —
       loaded and brought fully to life. The home scene builds only on first
       run (or after deleting/renaming scene.stml); L remains the manual
       mid-session revert. */
    if (load_palace(state)) {
        printf("palace loaded from scene.stml (%u objects)\n",
               (unsigned)state->scene.count);
    } else {
        populate_home_scene(state);
        collide_rebuild(&state->colliders, &state->scene);
        meadow_rebuild(state);
        forest_rebuild(state);
        adopt_legacy_motion(state);   /* no-op for the home scene; movers exist only in loaded scenes */
    }

    /* start filtered to "home" (every untagged object IS home, so this shows
       everything in today's scene) + register the enumerable home anchor */
    strcpy(state->scene.active_ws, "home");
    workspace_anchor_add(&state->scene, "home");   /* enumerable home (idempotent) */

    /* spawn standing at the south edge of the scene, facing -Z at eye height
       with a slight downward tilt (2.5 was above the 2.2 doorway lintels) */
    {
        vec3 spawn = vec3_make(0.0f, CAMERA_EYE_HEIGHT, 5.0f);   /* fallback */
        int  i;
        for (i = 0; i < (int)state->scene.count; i++) {
            const char *rt = scene_meta_get(&state->scene,
                                 state->scene.objects[i].handle, "room_type");
            if (rt && strcmp(rt, "home") == 0) {
                vec3 c = object_world_pos(&state->scene,
                             state->scene.objects[i].handle);
                spawn = vec3_make(c.x, c.y + CAMERA_EYE_HEIGHT, c.z + 2.0f);
                break;
            }
        }
        camera_init(&state->camera, spawn, sol_radians(-90.0f), sol_radians(-10.0f));
    }
    state->exposure   = 1.0f;
    state->ambient_scale = 1.0f;   /* zero-init would fade up from black */

    /* the sun (P8 item 6): a DIRECTIONAL light. pos -> target sets only the
       direction — warm light from the upper corner. No position/cone/falloff;
       the interior is carried by IBL ambient + the point lights/lanterns. */
    state->light_pos       = vec3_make(2.2f, 3.1f, 2.2f);
    state->light_target    = vec3_make(0.0f, 0.5f, 0.0f);   /* sun dir = normalize(target - pos) */
    state->light_color     = vec3_make(1.0f, 0.95f, 0.85f);
    state->light_intensity = 3.5f;                          /* directional: a plain multiplier */
    state->light_inner_deg = 22.0f;                         /* (cone fields unused by the sun) */
    state->light_outer_deg = 38.0f;

    /* environment map (Phase A1): load the equirectangular HDR -> a linear float
       texture. Held now, drawn as a skybox in A2. */
    {
        HdrImage sky;
        if (image_load_hdr("horn-koppe_spring_4k.hdr", &sky)) {
            /* max radiance across all channels: the definitive HDR check — a real
               .hdr has bright spots well above 1.0; clamped LDR would cap at 1.0 */
            long  i, n = (long)sky.w * sky.h * 4;
            float maxv = 0.0f;
            for (i = 0; i < n; i++) if (sky.pixels[i] > maxv) maxv = sky.pixels[i];
            printf("skybox HDR: %dx%d, max radiance=%.2f (>1.0 confirms HDR)\n",
                   sky.w, sky.h, (double)maxv);
            state->skybox_tex = rhi_create_texture_hdr(sky.pixels, sky.w, sky.h);
            image_hdr_free(&sky);
            build_env_cubemap(state);     /* bake equirect -> cubemap (B1) */
            build_irradiance_map(state);  /* convolve -> irradiance cubemap (B2) */
            build_prefilter_map(state);   /* GGX prefilter -> roughness mips (C1) */
            build_brdf_lut(state);        /* BRDF integration LUT (C2) */
            {
                RhiTexture none;
                none.id = 0;
                watch_add("horn-koppe_spring_4k.hdr", WATCH_HDR, none);
            }
        } else {
            printf("skybox HDR: load failed (skybox disabled)\n");
        }
    }

    printf("scene: %u objects (1 empty anchor)\n", (unsigned)state->scene.count);
    if (state->box_handle) {
        printf("box meta: title=\"%s\", author=\"%s\"; %u relations\n",
               scene_meta_get(&state->scene, state->box_handle, "title"),
               scene_meta_get(&state->scene, state->box_handle, "author"),
               (unsigned)scene_get(&state->scene, state->box_handle)->rel_count);
    }
    return 1;
}

/* update(): per-frame DERIVED state only, since P4 item 6 — every hardcoded
   animation block this function carried for four phases now lives as
   component DATA attached in the scene file (see adopt_legacy_motion). */
static void update(AppState *state, double dt) {
    /* item 7: containment is DERIVED state, recomputed each frame — walking
       through a doorway "transitions" only in the sense that this query's
       answer changes. The ambient eases toward the room's level (an
       exponential glide; no pop at the threshold). */
    float target;
    state->current_room = room_containing(&state->scene, state->camera.pos);
    target = state->current_room
           ? room_ambient(&state->scene, state->current_room) : 1.0f;
    state->ambient_scale += (target - state->ambient_scale)
                          * (1.0f - (float)exp(-dt * 5.0));
}

/* MIGRATION (P4 item 6 piece 2): scenes saved before components carried
   their motion in update()'s hardcoded blocks — and their saved rotations
   are a BAKED FRAME of that dance, the very hazard §1.6 names. One-time
   adoption: if a legacy mover exists and carries no components, RESET its
   base to the placed pose and attach the motion as data. The box's tumble
   is literally two spins composed; the sword spins about its own blade
   (local Z) over a static stand-up-and-lean base. Idempotent: components
   present = nothing to do. */
static void adopt_legacy_motion(AppState *st) {
    SceneObject *o;
    float        p[4];
    int          adopted = 0;

    o = scene_get(&st->scene, st->anchor_handle);      /* the box's orbit */
    if (o && o->comp_count == 0) {
        o->rot = quat_identity();
        p[0] = 0.0f; p[1] = 1.0f; p[2] = 0.0f; p[3] = 0.8f;
        scene_component_add(&st->scene, st->anchor_handle, "spin", p, 4);
        adopted++;
    }
    o = scene_get(&st->scene, st->box_handle);         /* the tumble: two spins */
    if (o && o->comp_count == 0) {
        o->rot = quat_identity();
        p[0] = 0.0f; p[1] = 1.0f; p[2] = 0.0f; p[3] = 1.2f;
        scene_component_add(&st->scene, st->box_handle, "spin", p, 4);
        p[0] = 1.0f; p[1] = 0.0f; p[2] = 0.0f; p[3] = 0.6f;
        scene_component_add(&st->scene, st->box_handle, "spin", p, 4);
        adopted++;
    }
    o = scene_get(&st->scene, st->sword_precess_handle); /* the precession */
    if (o && o->comp_count == 0) {
        o->rot = quat_identity();
        p[0] = 0.0f; p[1] = 1.0f; p[2] = 0.0f; p[3] = 0.4f;
        scene_component_add(&st->scene, st->sword_precess_handle, "spin", p, 4);
        adopted++;
    }
    o = scene_get(&st->scene, st->sword_handle);       /* the sword: spin the blade */
    if (o && o->comp_count == 0) {
        quat standup = quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), sol_radians(-90.0f));
        quat tilt    = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), sol_radians(-15.0f));
        o->rot = quat_normalize(quat_mul(tilt, standup));   /* the POSE persists;
                                                               the spin is overlay */
        p[0] = 0.0f; p[1] = 0.0f; p[2] = 1.0f; p[3] = 0.64f;
        scene_component_add(&st->scene, st->sword_handle, "spin", p, 4);
        adopted++;
    }
    if (adopted > 0) {
        scene_save(&st->scene, "scene.stml");
        printf("adopted legacy motion: %d object(s) now carry their dance as data\n",
               adopted);
    }
}

/* The light's view-projection (item 9b). Both the shadow pass and 9c's lighting
   pass call this — they MUST use the identical matrix or the cast shadow won't
   line up with the lit cone. The spot's perspective frustum IS the light matrix:
   fovy = the cone's full angle (+ slack), square map, near/far bracket the scene. */
/* ---- the sun's cascaded shadow projection (P8 item 6) ---- */
typedef struct { mat4 vp[SHADOW_CASCADES]; } CascadeSet;

/* mat4 * (p,1) keeping w (for the perspective divide of inv_cam_vp). */
static vec4 m4_point(mat4 m, vec3 p) {
    vec4 r;
    r.x = m.m[0]*p.x + m.m[4]*p.y + m.m[8] *p.z + m.m[12];
    r.y = m.m[1]*p.x + m.m[5]*p.y + m.m[9] *p.z + m.m[13];
    r.z = m.m[2]*p.x + m.m[6]*p.y + m.m[10]*p.z + m.m[14];
    r.w = m.m[3]*p.x + m.m[7]*p.y + m.m[11]*p.z + m.m[15];
    return r;
}
static float vec3_len(vec3 v)            { return sqrtf(vec3_dot(v, v)); }
static vec3  vec3_lerp(vec3 a, vec3 b, float t) {
    return vec3_add(a, vec3_scale(vec3_sub(b, a), t));
}

/* Fit ONE orthographic cascade to the camera-frustum slice between view
   fractions t_near..t_far (0 = camera near, 1 = camera far), looking down the
   sun. A bounding-SPHERE fit makes the box size invariant to camera rotation;
   pinning a fixed world reference (the origin) to the texel grid quantizes the
   box's translation to whole texels — so shadow edges JUMP rather than crawl as
   the slow camera drifts. That is the §1.3 anti-shimmer, done geometrically (no
   TAA). inv_cam_vp = inverse(camProj * camView). */
static mat4 cascade_fit(mat4 inv_cam_vp, vec3 sundir, float t_near, float t_far) {
    const float xy[4][2] = { {-1.0f,-1.0f}, {1.0f,-1.0f}, {1.0f,1.0f}, {-1.0f,1.0f} };
    vec3  corner[8];
    vec3  center = vec3_make(0.0f, 0.0f, 0.0f);
    vec3  up, eye;
    float radius = 0.0f, tex;
    mat4  lview, lproj, sm;
    vec4  o;
    float h, dx, dy;
    int   i;
    for (i = 0; i < 4; i++) {                      /* 4 frustum edges, near->far */
        vec4 nh = m4_point(inv_cam_vp, vec3_make(xy[i][0], xy[i][1], -1.0f));
        vec4 fh = m4_point(inv_cam_vp, vec3_make(xy[i][0], xy[i][1],  1.0f));
        vec3 nw = vec3_scale(vec3_make(nh.x, nh.y, nh.z), 1.0f / nh.w);
        vec3 fw = vec3_scale(vec3_make(fh.x, fh.y, fh.z), 1.0f / fh.w);
        corner[i]     = vec3_lerp(nw, fw, t_near);  /* slice's near plane */
        corner[i + 4] = vec3_lerp(nw, fw, t_far);   /* slice's far plane  */
    }
    for (i = 0; i < 8; i++) center = vec3_add(center, corner[i]);
    center = vec3_scale(center, 1.0f / 8.0f);
    for (i = 0; i < 8; i++) {
        float d = vec3_len(vec3_sub(corner[i], center));
        if (d > radius) radius = d;
    }
    tex = (2.0f * radius) / (float)SHADOW_MAP_SIZE;
    up  = (fabsf(sundir.y) > 0.99f) ? vec3_make(0.0f, 0.0f, 1.0f)
                                    : vec3_make(0.0f, 1.0f, 0.0f);
    eye = vec3_sub(center, vec3_scale(sundir, radius + SHADOW_CASTER_PAD));
    lview = mat4_look_at(eye, center, up);
    lproj = mat4_ortho(-radius, radius, -radius, radius,
                       0.1f, SHADOW_CASTER_PAD + 2.0f * radius + 1.0f);
    /* texel-snap: nudge the projection so the world origin lands on a texel
       center; every fragment then shifts by the same texel-aligned amount. */
    sm = mat4_mul(lproj, lview);
    o  = m4_point(sm, vec3_make(0.0f, 0.0f, 0.0f));   /* ortho => o.w == 1 */
    h  = (float)SHADOW_MAP_SIZE * 0.5f;
    dx = (floorf(o.x * h + 0.5f) - o.x * h) / h;
    dy = (floorf(o.y * h + 0.5f) - o.y * h) / h;
    lproj.m[12] += dx;
    lproj.m[13] += dy;
    (void)tex;                                    /* tex documents the texel size */
    return mat4_mul(lproj, lview);
}

/* Build all cascades for this frame from the camera's view + projection. The
   sun's DIRECTION is encoded by light_pos -> light_target (magnitude ignored). */
static CascadeSet sun_cascades(const AppState *state, mat4 view, mat4 proj) {
    CascadeSet cs;
    vec3  sundir = vec3_normalize(vec3_sub(state->light_target, state->light_pos));
    mat4  inv    = mat4_inverse(mat4_mul(proj, view));
    float cam_near = 0.1f, cam_far = 100.0f;      /* must match camera_proj() */
    float split[SHADOW_CASCADES + 1];
    int   c;
    split[0]               = cam_near;
    split[SHADOW_CASCADES] = SHADOW_DIST;
    for (c = 1; c < SHADOW_CASCADES; c++) {        /* practical split (log/uniform blend) */
        float p     = (float)c / (float)SHADOW_CASCADES;
        float log_d = cam_near * powf(SHADOW_DIST / cam_near, p);
        float uni_d = cam_near + (SHADOW_DIST - cam_near) * p;
        split[c]    = 0.8f * log_d + 0.2f * uni_d; /* lambda 0.8: tighter near cascade */
    }
    for (c = 0; c < SHADOW_CASCADES; c++) {
        float tN = (split[c]     - cam_near) / (cam_far - cam_near);
        float tF = (split[c + 1] - cam_near) / (cam_far - cam_near);
        cs.vp[c] = cascade_fit(inv, sundir, tN, tF);
    }
    return cs;
}

/* Resolve the ONE shadow-casting spot sconce (P8 item 7): the nearest scene
   object tagged light=point AND cast=1. Reads its pos/color/intensity/radius,
   an optional aim (light_dir meta; default straight down) and cone (light_cone
   half-angle deg). The position is SWAYED by two incommensurate sines (handle-
   seeded desync) so the flame dances and the shadows swing; intensity breathes
   through overlay_glow like the fills. Builds the perspective shadow matrix.
   Sets spot_enabled = 0 when no caster exists -> the FS term vanishes. */
static void resolve_spot_caster(AppState *state, float t) {
    sol_u32 k;
    state->spot_enabled = SOL_FALSE;
    for (k = 0; k < state->scene.count; k++) {
        SceneObject *o    = &state->scene.objects[k];
        const char  *lt   = scene_meta_get(&state->scene, o->handle, "light");
        const char  *cast = scene_meta_get(&state->scene, o->handle, "cast");
        const char  *s;
        float r = 1.0f, g = 1.0f, b = 1.0f, inten = 10.0f, radius = 8.0f;
        float outer = 45.0f, inner, ph;
        vec3  dir = vec3_make(0.0f, -1.0f, 0.0f), pos, up;
        mat4  wm, lview, lproj;
        if (!lt || strcmp(lt, "point") != 0)  continue;
        if (!cast || atoi(cast) == 0)         continue;
        wm  = scene_world_matrix(&state->scene, o);
        pos = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
        s = scene_meta_get(&state->scene, o->handle, "light_color");
        if (s) sscanf(s, "%f %f %f", &r, &g, &b);
        s = scene_meta_get(&state->scene, o->handle, "light_intensity");
        if (s) inten = (float)atof(s);
        s = scene_meta_get(&state->scene, o->handle, "light_radius");
        if (s) radius = (float)atof(s);
        s = scene_meta_get(&state->scene, o->handle, "light_dir");
        if (s) {
            float dx, dy, dz;
            if (sscanf(s, "%f %f %f", &dx, &dy, &dz) == 3)
                dir = vec3_normalize(vec3_make(dx, dy, dz));
        }
        s = scene_meta_get(&state->scene, o->handle, "light_cone");
        if (s) outer = (float)atof(s);
        inner = outer * 0.7f;                      /* full-bright core inside the falloff */
        inten *= o->overlay_glow;                  /* the flame breathes (same channel as fills) */
        ph    = (float)o->handle * 0.7f;           /* desync each flame */
        pos.x += sinf(t * 5.3f + ph)        * SPOT_SWAY_AMP;   /* the flame dances -> shadows sway */
        pos.z += sinf(t * 4.1f + ph * 1.7f) * SPOT_SWAY_AMP;
        state->spot_pos       = pos;
        state->spot_dir       = dir;
        state->spot_color     = vec3_make(r * inten, g * inten, b * inten);  /* premultiplied */
        state->spot_radius    = radius;
        state->spot_cos_inner = cosf(sol_radians(inner));
        state->spot_cos_outer = cosf(sol_radians(outer));
        up    = (fabsf(dir.y) > 0.99f) ? vec3_make(0.0f, 0.0f, 1.0f)
                                       : vec3_make(0.0f, 1.0f, 0.0f);
        lview = mat4_look_at(pos, vec3_add(pos, dir), up);
        lproj = mat4_perspective(sol_radians(2.0f * outer + 8.0f), 1.0f,
                                 SPOT_SHADOW_NEAR, SPOT_SHADOW_FAR);
        state->spot_vp        = mat4_mul(lproj, lview);
        state->spot_enabled   = SOL_TRUE;
        break;                                     /* one caster in v1 */
    }
}

/* everything a lit surface needs, bound on the given pipeline — shared
   verbatim by the standard draw and (item 9) the skinned one: a skinned
   surface is lit like any surface, so the uniform vocabulary is ONE. */
static void bind_scene_uniforms(const AppState *state, RhiPipeline pipeline,
                                mat4 model, mat4 view, mat4 proj, vec3 eye,
                                float highlight, Material mat) {
    mat3 nrm = mat3_normal_matrix(model);

    rhi_set_pipeline(pipeline);
    rhi_set_uniform_mat4("uModel",        model.m);
    rhi_set_uniform_mat4("uView",         view.m);
    rhi_set_uniform_mat4("uProj",         proj.m);
    rhi_set_uniform_mat3("uNormalMatrix", nrm.m);
    rhi_set_uniform_vec3("uViewPos",      eye.x, eye.y, eye.z);
    rhi_set_uniform_vec3("uEmissive",     mat.emissive.x, mat.emissive.y, mat.emissive.z);
    rhi_set_uniform_float("uHighlight",   highlight);
    rhi_set_uniform_float("uTerrainBlend", state->terrain_blend ? 1.0f : 0.0f);
    rhi_set_uniform_float("uTerrainY0",    state->terrain_y0);
    rhi_set_uniform_float("uTerrainAmp",   state->terrain_amp);

    /* spot light (item 9a): constant per-frame, set per-mesh like uViewPos.
       dir + cone cosines computed CPU-side from the AppState light fields. */
    {
        vec3  ldir = vec3_normalize(vec3_sub(state->light_target, state->light_pos));
        float ci   = cosf(sol_radians(state->light_inner_deg));
        float co   = cosf(sol_radians(state->light_outer_deg));
        rhi_set_uniform_vec3 ("uLightPos",   state->light_pos.x, state->light_pos.y, state->light_pos.z);
        rhi_set_uniform_vec3 ("uLightDir",   ldir.x, ldir.y, ldir.z);
        rhi_set_uniform_vec3 ("uLightColor", state->light_color.x, state->light_color.y, state->light_color.z);
        rhi_set_uniform_float("uLightIntensity", state->light_intensity);
        rhi_set_uniform_float("uCosInner", ci);
        rhi_set_uniform_float("uCosOuter", co);

        /* cascaded shadow maps (P8 item 6): the SAME matrices the depth pass
           used, one depth texture per cascade. Near cascade on unit 4 (0-3 are
           material), far on unit 8 (5-7 are IBL). Two cascades is wired
           explicitly here; a third would add uShadowMap2 on unit 9. */
        rhi_set_uniform_mat4("uLightVP",   state->cascade_vp[0].m);
        rhi_bind_texture(rhi_render_target_depth_texture(state->shadow_rt[0]), 4);
        rhi_set_uniform_int("uShadowMap",  4);
        rhi_set_uniform_mat4("uLightVP1",  state->cascade_vp[1].m);
        rhi_bind_texture(rhi_render_target_depth_texture(state->shadow_rt[1]), 8);
        rhi_set_uniform_int("uShadowMap1", 8);

        /* spot sconce (P8 item 7): unit 9 is ALWAYS bound (Metal needs a texture
           there); the FS gates the term on uSpotEnabled, so a stale matrix when
           disabled is never sampled. */
        rhi_set_uniform_float("uSpotEnabled", state->spot_enabled ? 1.0f : 0.0f);
        rhi_set_uniform_vec3 ("uSpotPos",   state->spot_pos.x, state->spot_pos.y, state->spot_pos.z);
        rhi_set_uniform_vec3 ("uSpotDir",   state->spot_dir.x, state->spot_dir.y, state->spot_dir.z);
        rhi_set_uniform_vec3 ("uSpotColor", state->spot_color.x, state->spot_color.y, state->spot_color.z);
        rhi_set_uniform_float("uSpotRadius",   state->spot_radius);
        rhi_set_uniform_float("uSpotCosInner", state->spot_cos_inner);
        rhi_set_uniform_float("uSpotCosOuter", state->spot_cos_outer);
        rhi_set_uniform_mat4 ("uSpotVP", state->spot_vp.m);
        rhi_bind_texture(rhi_render_target_depth_texture(state->spot_shadow_rt), 9);
        rhi_set_uniform_int  ("uSpotShadowMap", 9);

        /* IBL: irradiance (diffuse, B3) unit 5; prefilter + BRDF LUT (specular, C3)
           units 6/7. (0-3 material textures, 4 shadow.) */
        rhi_set_uniform_float("uUseIBL", state->irradiance_cubemap.id ? 1.0f : 0.0f);
        rhi_set_uniform_float("uAmbientScale", state->ambient_scale);   /* item 7: room feel */
        if (state->irradiance_cubemap.id) {
            rhi_bind_texture(state->irradiance_cubemap, 5);
            rhi_set_uniform_int("uIrradianceMap", 5);
            rhi_bind_texture(state->prefilter_cubemap, 6);
            rhi_set_uniform_int("uPrefilterMap", 6);
            rhi_bind_texture(rhi_render_target_texture(state->brdf_lut_rt), 7);
            rhi_set_uniform_int("uBrdfLUT", 7);
        }
    }

    rhi_set_uniform_vec3("uBaseColor",    mat.base_color.x, mat.base_color.y, mat.base_color.z);
    rhi_set_uniform_float("uMetallic",    mat.metallic);
    rhi_set_uniform_float("uRoughness",   mat.roughness);
    rhi_set_uniform_float("uUseAlbedoTex", mat.albedo_tex.id ? 1.0f : 0.0f);
    if (mat.albedo_tex.id) {
        rhi_bind_texture(mat.albedo_tex, 0);
        rhi_set_uniform_int("uAlbedoTex", 0);   /* sampler -> texture unit 0 */
    }
    rhi_set_uniform_float("uUseMRTex", mat.mr_tex.id ? 1.0f : 0.0f);
    if (mat.mr_tex.id) {
        rhi_bind_texture(mat.mr_tex, 1);
        rhi_set_uniform_int("uMRTex", 1);       /* sampler -> texture unit 1 */
    }
    rhi_set_uniform_float("uUseAOTex",   mat.ao_tex.id ? 1.0f : 0.0f);
    rhi_set_uniform_float("uAOStrength", mat.ao_strength);
    if (mat.ao_tex.id) {
        rhi_bind_texture(mat.ao_tex, 2);        /* may be the same GL texture as unit 1 (ORM) — fine */
        rhi_set_uniform_int("uAOTex", 2);       /* sampler -> texture unit 2 */
    }
    rhi_set_uniform_float("uUseNormalTex", mat.normal_tex.id ? 1.0f : 0.0f);
    rhi_set_uniform_float("uNormalScale",  mat.normal_scale);
    if (mat.normal_tex.id) {
        rhi_bind_texture(mat.normal_tex, 3);
        rhi_set_uniform_int("uNormalTex", 3);   /* sampler -> texture unit 3 */
    }
    rhi_set_uniform_float("uOpacity", 1.0f);    /* P9 item 2: opaque default; draw_glass overrides */
}

static void draw_mesh(const AppState *state, Mesh mesh, mat4 model,
                      mat4 view, mat4 proj, vec3 eye, float highlight,
                      Material mat) {
    bind_scene_uniforms(state, state->pipeline, model, view, proj, eye,
                        highlight, mat);
    rhi_bind_vertex_buffer(mesh.vbuffer);
    rhi_bind_index_buffer(mesh.ibuffer);
    rhi_draw_indexed(0, mesh.index_count);
}

#define GLASS_OPACITY   0.6f  /* P9 item 2: stained-glass surface opacity (tune by eye) */
#define GLASS_DRAW_MAX  64    /* sorted transparent pass capacity (church_glass + window_glass) */
/* P9 item 2: the stained-glass draw — the SAME PBR shader (glass stays lit + IBL
   + emissive), on the alpha-blend / depth-write-off glass pipeline, with the
   surface opacity overriding bind_scene_uniforms' opaque default. */
static void draw_glass(const AppState *state, Mesh mesh, mat4 model, mat4 view,
                       mat4 proj, vec3 eye, Material mat) {
    bind_scene_uniforms(state, state->glass_pipeline, model, view, proj, eye,
                        0.0f, mat);
    rhi_set_uniform_float("uOpacity", GLASS_OPACITY);
    rhi_bind_vertex_buffer(mesh.vbuffer);
    rhi_bind_index_buffer(mesh.ibuffer);
    rhi_draw_indexed(0, mesh.index_count);
}

/* item 9: the skinned draw — the same uniform vocabulary on the skinned
   pipeline, plus the palette in one array upload */
static void draw_skinned(const AppState *state, const SkinnedModel *sm,
                         int clip, float t, mat4 model, mat4 view, mat4 proj,
                         vec3 eye, float highlight) {
    mat4 pal[SKEL_MAX_JOINTS];
    skinned_palette_at(sm, clip, t, pal);
    bind_scene_uniforms(state, state->skinned_pipeline, model, view, proj,
                        eye, highlight, sm->material);
    rhi_set_uniform_mat4_array("uPalette", (const float *)pal,
                               sm->rig.skel.joint_count);
    rhi_bind_vertex_buffer(sm->mesh.vbuffer);
    rhi_bind_index_buffer(sm->mesh.ibuffer);
    rhi_draw_indexed(0, sm->mesh.index_count);
}

/* Ensure the HDR target matches the window's framebuffer size, recreating it on
   resize (the one path that both first-creates and resizes it). A minimized
   window reports 0 — clamp so we never make a zero-size framebuffer. */
static void ensure_render_target(AppState *state, int w, int h) {
    int lv, bw, bh;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (state->hdr_rt.id != 0 && state->rt_width == w && state->rt_height == h)
        return;                                   /* already the right size */
    if (state->hdr_rt.id != 0)
        rhi_destroy_render_target(state->hdr_rt);
    state->hdr_rt    = rhi_create_render_target(w, h, RHI_TEX_RGBA16F);
    state->rt_width  = w;
    state->rt_height = h;

    /* the bloom chain follows the frame (P4 item 5): half, quarter, ... */
    bw = w; bh = h;
    for (lv = 0; lv < BLOOM_LEVELS; lv++) {
        bw = bw / 2; if (bw < 8) bw = 8;
        bh = bh / 2; if (bh < 8) bh = 8;
        if (state->bloom_rt[lv].id != 0)
            rhi_destroy_render_target(state->bloom_rt[lv]);
        state->bloom_rt[lv] = rhi_create_render_target(bw, bh, RHI_TEX_RGBA16F);
        state->bloom_w[lv]  = bw;
        state->bloom_h[lv]  = bh;
    }

    /* god-rays (P8 item 3) ride one half-res buffer, like a bloom level */
    state->godray_w = w / 2; if (state->godray_w < 8) state->godray_w = 8;
    state->godray_h = h / 2; if (state->godray_h < 8) state->godray_h = 8;
    if (state->godray_rt.id != 0)
        rhi_destroy_render_target(state->godray_rt);
    state->godray_rt = rhi_create_render_target(state->godray_w, state->godray_h,
                                                RHI_TEX_RGBA16F);

    /* soft particles (P8 item 4): a full-res copy of the scene depth — the
       particle pass can't sample the depth it draws into, so it reads this */
    if (state->depthcopy_rt.id != 0)
        rhi_destroy_render_target(state->depthcopy_rt);
    state->depthcopy_rt = rhi_create_render_target(w, h, RHI_TEX_RGBA16F);

    /* SSAO (P8 item 5): half-res occlusion + its blur, like the god-ray buffer */
    state->ssao_w = w / 2; if (state->ssao_w < 8) state->ssao_w = 8;
    state->ssao_h = h / 2; if (state->ssao_h < 8) state->ssao_h = 8;
    if (state->ssao_rt.id != 0)      rhi_destroy_render_target(state->ssao_rt);
    if (state->ssao_blur_rt.id != 0) rhi_destroy_render_target(state->ssao_blur_rt);
    state->ssao_rt      = rhi_create_render_target(state->ssao_w, state->ssao_h, RHI_TEX_RGBA16F);
    state->ssao_blur_rt = rhi_create_render_target(state->ssao_w, state->ssao_h, RHI_TEX_RGBA16F);
}

/* fold a per-frame sample (seconds) into a readable ms readout. Fixed blend:
   a readout wants steadiness to be legible, not framerate-exact response. */
static void yardstick_ms(float *acc, double seconds) {
    *acc += ((float)(seconds * 1000.0) - *acc) * 0.08f;
}

/* Collect up to 8 point lights (P4 item 5): a light is META on an ordinary
   object ("light"="point" + light_color/intensity/radius), so it parents,
   persists through the existing meta round-trip, and DRAGS — the carried
   lantern is the proof. Color premultiplies intensity; past the cap the
   nearest-to-camera win. Read fresh each frame: the world matrix is the
   light's position, so motion is free. */
#define MAX_POINT_LIGHTS 8
static int collect_point_lights(AppState *st, vec3 *pos, vec3 *col, float *rad) {
    vec3    cpos[32], ccol[32];
    float   crad[32], cd2[32];
    int     n = 0, i, j, count;
    sol_u32 k;
    for (k = 0; k < st->scene.count && n < 32; k++) {
        SceneObject *o    = &st->scene.objects[k];
        const char  *lt   = scene_meta_get(&st->scene, o->handle, "light");
        const char  *cast = scene_meta_get(&st->scene, o->handle, "cast");
        const char  *s;
        float        r = 1.0f, g = 1.0f, b = 1.0f, inten = 10.0f, radius = 8.0f;
        mat4         wm;
        vec3         p, dd;
        if (!lt || strcmp(lt, "point") != 0) continue;
        if (cast && atoi(cast) != 0) continue;    /* the shadow-caster is its own
                                                     dedicated term (P8 item 7) */
        wm = scene_world_matrix(&st->scene, o);
        p  = vec3_make(wm.m[12], wm.m[13], wm.m[14]);
        s = scene_meta_get(&st->scene, o->handle, "light_color");
        if (s) sscanf(s, "%f %f %f", &r, &g, &b);
        s = scene_meta_get(&st->scene, o->handle, "light_intensity");
        if (s) inten = (float)atof(s);
        s = scene_meta_get(&st->scene, o->handle, "light_radius");
        if (s) radius = (float)atof(s);
        inten *= o->overlay_glow;              /* flicker lives here (P4 i6 p3):
                                                  the glow channel multiplies in
                                                  at the consumer, the meta never
                                                  changes */
        cpos[n] = p;
        ccol[n] = vec3_make(r * inten, g * inten, b * inten);
        crad[n] = radius;
        dd      = vec3_sub(p, st->camera.pos);
        cd2[n]  = vec3_dot(dd, dd);
        n++;
    }
    count = (n < MAX_POINT_LIGHTS) ? n : MAX_POINT_LIGHTS;
    for (i = 0; i < count; i++) {              /* nearest first (n is tiny) */
        int best = i;
        vec3 tv; float tf;
        for (j = i + 1; j < n; j++)
            if (cd2[j] < cd2[best]) best = j;
        tv = cpos[i]; cpos[i] = cpos[best]; cpos[best] = tv;
        tv = ccol[i]; ccol[i] = ccol[best]; ccol[best] = tv;
        tf = crad[i]; crad[i] = crad[best]; crad[best] = tf;
        tf = cd2[i];  cd2[i]  = cd2[best];  cd2[best]  = tf;
        pos[i] = cpos[i];
        col[i] = ccol[i];
        rad[i] = crad[i];
    }
    return count;
}

/* mark one candidate visible — the BVH walk already tested its box exactly */
static void cull_mark(sol_u32 id, void *ctx) {
    ((unsigned char *)ctx)[id] = 1;
}

/* Visibility for one view volume (P4 item 2 piece 4): a handle-indexed byte
   map filled by the frustum walk (handles are monotonic and small — the map
   is a few KB, memset per pass). Refilled per pass: the shadow pass culls
   by the LIGHT's frustum (what can't be seen by the light casts nothing
   into its map), the HDR pass by the camera's — same machinery, two
   volumes. Returns NULL on OOM, and the loops then draw everything:
   culling must only ever be an optimization. */
static unsigned char *vis_fill(AppState *st, mat4 vp) {
    sol_u32 need = st->scene.next_handle;
    if (need > st->vis_cap) {
        unsigned char *nv = (unsigned char *)realloc(st->vis, (size_t)need);
        if (nv == NULL) return NULL;
        st->vis     = nv;
        st->vis_cap = need;
    }
    memset(st->vis, 0, (size_t)need);
    {
        Frustum f = frustum_from_vp(vp);
        bvh_frustum_query(&st->bvh, &f, cull_mark, st->vis);
    }
    return st->vis;
}

/* Emit every shadow caster into the currently-bound depth target, projected by
   `lvp` and culled by `lvis` (one cascade's view volume). Factored out of pass 0
   so the cascade loop can call it once per cascade (P8 item 6); the body is the
   former single-map shadow pass verbatim — static meshes, the skinned fox, the
   instanced ornament, and the FIELD forest. The depth shaders are unchanged:
   they just multiply by whatever uLightVP is bound. */
static void emit_shadow_casters(AppState *state, mat4 lvp, unsigned char *lvis) {
    sol_u32 i;
    rhi_set_pipeline(state->shadow_pipeline);
    rhi_set_uniform_mat4("uLightVP", lvp.m);
    for (i = 0; i < state->scene.count; i++) {
        const SceneObject *o = &state->scene.objects[i];
        mat4 model;
        if (o->mesh.index_count == 0) continue;   /* empties cast nothing */
        if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
        if (o->mesh_ref && strcmp(o->mesh_ref, "pond") == 0)
            continue;                             /* water casts nothing */
        if (lvis && !lvis[o->handle]) continue;   /* outside this cascade's box */
        model = scene_world_matrix(&state->scene, o);
        rhi_set_uniform_mat4("uModel", model.m);
        rhi_bind_vertex_buffer(o->mesh.vbuffer);
        rhi_bind_index_buffer(o->mesh.ibuffer);
        rhi_draw_indexed(0, o->mesh.index_count);
    }
    /* skinned casters (item 9): same palette as the visible draw —
       the shadow runs WITH the fox */
    if (state->skinned_shadow_pipeline.id) {
        sol_u32 sk;
        for (sk = 0; sk < state->scene.count; sk++) {
            const SceneObject *o  = &state->scene.objects[sk];
            const char        *sg = scene_meta_get(&state->scene,
                                        o->handle, "skin_glb");
            SkinnedModel      *sm;
            mat4               model, pal[SKEL_MAX_JOINTS];
            int                clip;
            float              speed;
            if (!sg) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
            sm = skinned_get(sg);
            if (!sm) continue;
            skin_anim_of(o, &clip, &speed);
            skinned_palette_at(sm, clip, (float)glfwGetTime() * speed, pal);
            rhi_set_pipeline(state->skinned_shadow_pipeline);
            rhi_set_uniform_mat4("uLightVP", lvp.m);
            model = scene_world_matrix(&state->scene, o);
            rhi_set_uniform_mat4("uModel", model.m);
            rhi_set_uniform_mat4_array("uPalette", (const float *)pal,
                                       sm->rig.skel.joint_count);
            rhi_bind_vertex_buffer(sm->mesh.vbuffer);
            rhi_bind_index_buffer(sm->mesh.ibuffer);
            rhi_draw_indexed(0, sm->mesh.index_count);
        }
    }
    /* instanced ornament casts too (P6 item 10; P7 item 4: leaves
       cast their canopy) — floating shadowless ornament reads wrong.
       The shadow uses the UN-swayed pose (no uTime here): the rustle
       is gentle and the canopy shadow is a blob — a flagged v1. */
    if (state->ornament_shadow_pipeline.id && state->ornament_count > 0) {
        int op;
        rhi_set_pipeline(state->ornament_shadow_pipeline);
        rhi_set_uniform_mat4("uLightVP", lvp.m);
        for (op = 0; op < state->ornament_count; op++) {
            OrnamentPatch *pt = &state->ornament[op];
            Mesh          *um = &state->orn_mesh[pt->kind];
            SceneObject   *o  = scene_get(&state->scene, pt->source);
            mat4 model;
            if (!o || um->index_count == 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
            if (lvis && !lvis[o->handle]) continue;
            model = scene_world_matrix(&state->scene, o);
            rhi_set_uniform_mat4("uModel", model.m);
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_bind_instance_buffer(pt->data);
            rhi_draw_indexed_instanced(0, um->index_count, pt->count);
        }
    }
    /* the FIELD forest casts too (P7 item 5): wood + canopy, gated by this
       cascade's view volume — the near cascade is tight, so the cost is
       bounded to trees actually near the slice */
    if (state->ornament_shadow_pipeline.id && state->forest_count > 0) {
        int fp, v, lk;
        rhi_set_pipeline(state->ornament_shadow_pipeline);
        rhi_set_uniform_mat4("uLightVP", lvp.m);
        for (fp = 0; fp < state->forest_count; fp++) {
            ForestPatch *f   = &state->forest[fp];
            SceneObject *isl = scene_get(&state->scene, f->island);
            mat4 model;
            if (!isl) continue;
            if (lvis && !lvis[f->island]) continue;
            model = scene_world_matrix(&state->scene, isl);
            rhi_set_uniform_mat4("uModel", model.m);
            for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
                Mesh *um = &state->forest_wood[v];
                if (f->wood_count[v] == 0 || um->index_count == 0) continue;
                rhi_bind_vertex_buffer(um->vbuffer);
                rhi_bind_index_buffer(um->ibuffer);
                rhi_bind_instance_buffer(f->wood[v]);
                rhi_draw_indexed_instanced(0, um->index_count,
                                           f->wood_count[v]);
            }
            for (lk = 0; lk < 2; lk++) {
                Mesh *um = &state->orn_mesh[lk == 0 ? ORN_LEAF_BROAD
                                                    : ORN_LEAF_CONIFER];
                if (f->canopy_count[lk] == 0 || um->index_count == 0) continue;
                rhi_bind_vertex_buffer(um->vbuffer);
                rhi_bind_index_buffer(um->ibuffer);
                rhi_bind_instance_buffer(f->canopy[lk]);
                rhi_draw_indexed_instanced(0, um->index_count,
                                           f->canopy_count[lk]);
            }
        }
    }
    /* the campus trees cast too (derived; no scene handle -> model is the
       campus-centre translate, like their main-pass draw) */
    if (state->ornament_shadow_pipeline.id && g_campus.enabled) {
        mat4 cmodel = mat4_translate(g_campus.center);
        int  v, lk;
        rhi_set_pipeline(state->ornament_shadow_pipeline);
        rhi_set_uniform_mat4("uLightVP", lvp.m);
        rhi_set_uniform_mat4("uModel", cmodel.m);
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
            Mesh *um = &state->forest_wood[v];
            if (g_campus_flora.wood_n[v] == 0 || um->index_count == 0) continue;
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_bind_instance_buffer(g_campus_flora.wood[v]);
            rhi_draw_indexed_instanced(0, um->index_count, g_campus_flora.wood_n[v]);
        }
        for (lk = 0; lk < 2; lk++) {
            Mesh *um = &state->orn_mesh[lk == 0 ? ORN_LEAF_BROAD : ORN_LEAF_CONIFER];
            if (g_campus_flora.canopy_n[lk] == 0 || um->index_count == 0) continue;
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_bind_instance_buffer(g_campus_flora.canopy[lk]);
            rhi_draw_indexed_instanced(0, um->index_count, g_campus_flora.canopy_n[lk]);
        }
    }
}

/* Project a world point to framebuffer pixels (UI space, y-down). FALSE if
   behind the camera. */
static sol_bool editor_world_to_screen(AppState *st, float aspect, vec3 wp,
                                        float *sx, float *sy) {
    mat4 vp = mat4_mul(camera_proj(&st->camera, aspect), camera_view(&st->camera));
    vec3 ndc;
    if (!mat4_project_point(vp, wp, &ndc)) return SOL_FALSE;
    *sx = (ndc.x * 0.5f + 0.5f) * (float)st->fb_width;
    *sy = (0.5f - ndc.y * 0.5f) * (float)st->fb_height;
    return SOL_TRUE;
}

/* a stable-ish tile color from a kind, so the placeholder grid is legible. */
/* The representative mesh + material for an item's thumbnail: a codex shows its
   cover; everything else shows its own mesh. SOL_FALSE if it has no drawable
   mesh. */
static sol_bool inventory_thumb_mesh(AppState *st, sol_u32 item, Mesh *mesh, Material *mat) {
    sol_u32      cov = codex_cover_child(&st->scene, item);
    SceneObject *o   = scene_get(&st->scene, cov != 0 ? cov : item);
    if (!o || o->mesh.index_count == 0) return SOL_FALSE;
    *mesh = o->mesh;
    *mat  = o->material;
    return SOL_TRUE;
}

/* Tonemap+encode the HDR scratch into `dst` using the EXISTING post pipeline,
   driven with neutral inputs (no bloom/godray/fog/grade/vignette) so the
   thumbnail is a clean sRGB-encoded RGBA8 that ui_textured_quad can show. */
static void inventory_thumb_tonemap(AppState *st, RhiRenderTarget dst) {
    mat4 ident = mat4_identity();
    rhi_begin_pass(dst, RHI_CLEAR_ALL, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_pipeline(st->post_pipeline);
    rhi_bind_texture(rhi_render_target_texture(st->inv_thumb_hdr), 0);
    rhi_bind_texture(st->inv_black_tex, 1);   /* uBloom (gated to 0 anyway)   */
    rhi_bind_texture(st->inv_black_tex, 2);   /* uDepth (fog gated off)       */
    rhi_bind_texture(st->inv_black_tex, 3);   /* uGodray = black (added raw)  */
    rhi_bind_texture(st->inv_white_tex, 4);   /* uAO = white (multiplied)     */
    rhi_bind_texture(st->inv_black_tex, 5);   /* uLut (gated to 0)            */
    rhi_set_uniform_int("uHdr", 0);    rhi_set_uniform_int("uBloom", 1);
    rhi_set_uniform_int("uDepth", 2);  rhi_set_uniform_int("uGodray", 3);
    rhi_set_uniform_int("uAO", 4);     rhi_set_uniform_int("uLut", 5);
    rhi_set_uniform_float("uBloomStrength", 0.0f);
    rhi_set_uniform_float("uExposure", st->exposure);
    rhi_set_uniform_float("uFogDensity", 0.0f);
    rhi_set_uniform_float("uFogFalloff", 1.0f);
    rhi_set_uniform_float("uFogHeight", 0.0f);
    rhi_set_uniform_float("uFogStrength", 0.0f);
    rhi_set_uniform_vec3 ("uAerialColor", 0.0f, 0.0f, 0.0f);
    rhi_set_uniform_vec3 ("uFogColor", 0.0f, 0.0f, 0.0f);
    rhi_set_uniform_vec3 ("uCamPos", 0.0f, 0.0f, 0.0f);
    rhi_set_uniform_mat4 ("uInvViewProj", ident.m);
    rhi_set_uniform_vec3 ("uGradeTint", 1.0f, 1.0f, 1.0f);
    rhi_set_uniform_float("uGradeContrast", 1.0f);
    rhi_set_uniform_float("uGradeSaturation", 1.0f);
    rhi_set_uniform_float("uLutMix", 0.0f);
    rhi_set_uniform_float("uVignetteStrength", 0.0f);
    rhi_set_uniform_float("uVignetteRadius", 1.0f);
    rhi_draw(0, 3);
    rhi_end_pass();
}

/* Render an item's thumbnail into `dst`: the representative mesh, framed by a
   fixed 3/4 view, into the HDR scratch (existing forward PBR draw), then
   tonemapped into dst. */
static void inventory_thumb_render(AppState *st, sol_u32 item, RhiRenderTarget dst) {
    Mesh     mesh;
    Material mat;
    mat4     model, view, proj;
    vec3     eye, ctr, ext;
    float    rad, dist;
    if (!inventory_thumb_mesh(st, item, &mesh, &mat)) return;
    ctr  = vec3_scale(vec3_add(mesh.bounds.min, mesh.bounds.max), 0.5f);
    ext  = vec3_sub(mesh.bounds.max, mesh.bounds.min);
    rad  = 0.5f * (float)sqrt((double)vec3_dot(ext, ext));   /* no vec3_length in sol_math */
    if (rad < 1e-3f) rad = 0.5f;
    dist = rad * 2.6f;
    eye  = vec3_add(ctr, vec3_make(dist * 0.7f, dist * 0.6f, dist * 0.7f));
    model = mat4_identity();
    view  = mat4_look_at(eye, ctr, vec3_make(0.0f, 1.0f, 0.0f));
    proj  = mat4_perspective(sol_radians(35.0f), 1.0f, 0.05f, dist * 4.0f + 10.0f);
    rhi_begin_pass(st->inv_thumb_hdr, RHI_CLEAR_ALL, 0.10f, 0.12f, 0.15f, 1.0f);
    draw_mesh(st, mesh, model, view, proj, eye, 0.0f, mat);
    rhi_end_pass();
    inventory_thumb_tonemap(st, dst);
}

/* Look up (or build) the cached thumbnail target for an item. {0} if the item
   has no mesh. Simple eviction: when full, recycle slot 0. */
static RhiRenderTarget inventory_thumb_get(AppState *st, sol_u32 item) {
    int i;
    for (i = 0; i < st->inv_thumb_count; i++)
        if (st->inv_thumbs[i].handle == item) return st->inv_thumbs[i].rt;
    if (st->inv_thumb_count >= INV_THUMB_POOL) {    /* pool full: evict the oldest */
        rhi_destroy_render_target(st->inv_thumbs[0].rt);
        for (i = 1; i < st->inv_thumb_count; i++) st->inv_thumbs[i - 1] = st->inv_thumbs[i];
        st->inv_thumb_count--;
    }
    i = st->inv_thumb_count++;
    st->inv_thumbs[i].handle = item;
    st->inv_thumbs[i].rt     = rhi_create_render_target(INV_THUMB_SIZE, INV_THUMB_SIZE, RHI_TEX_RGBA8);
    inventory_thumb_render(st, item, st->inv_thumbs[i].rt);
    return st->inv_thumbs[i].rt;
}

/* Cache-only lookup ({0} if not built). The overlay uses this so it NEVER
   triggers a render mid-UI-batch — inventory_ensure_thumbs (frame top) builds. */
static RhiRenderTarget inventory_thumb_lookup(AppState *st, sol_u32 item) {
    RhiRenderTarget none;
    int i;
    none.id = 0;
    for (i = 0; i < st->inv_thumb_count; i++)
        if (st->inv_thumbs[i].handle == item) return st->inv_thumbs[i].rt;
    return none;
}

/* Build the thumbnails for the CURRENT PAGE (only — to bound GPU render-target
   slots, since each thumbnail is a render target), and drop cached targets whose
   item left the bag. Call at the top of the frame while the screen is open
   (inside the frame's command stream — no rhi_flush). */
static void inventory_ensure_thumbs(AppState *st) {
    sol_u32 items[INV_THUMB_CAP];
    int     n = inventory_collect(st, items, INV_THUMB_CAP), i, j, start, end;
    st->inv_page = inv_clamp_page(st->inv_page, n, INV_PER_PAGE);
    start = st->inv_page * INV_PER_PAGE;
    end   = start + INV_PER_PAGE;
    if (end > n) end = n;
    /* Evict every cached thumbnail NOT on the current page (covers departed
       items too). The pool then holds at most one page, so building the page's
       misses below can never evict a thumbnail this page still needs. */
    for (i = 0; i < st->inv_thumb_count; ) {
        sol_bool keep = SOL_FALSE;
        for (j = start; j < end; j++)
            if (items[j] == st->inv_thumbs[i].handle) { keep = SOL_TRUE; break; }
        if (!keep) {
            rhi_destroy_render_target(st->inv_thumbs[i].rt);
            for (j = i + 1; j < st->inv_thumb_count; j++) st->inv_thumbs[j - 1] = st->inv_thumbs[j];
            st->inv_thumb_count--;
        } else i++;
    }
    for (i = start; i < end; i++) (void)inventory_thumb_get(st, items[i]);   /* build the page */
}

/* The inventory screen: a dim backdrop, a grid of item tiles (live 3D
   thumbnails, built at frame top), name labels, and pagination. Drawn inside
   the open UI batch, like the palette/editor overlays. */
static void inventory_draw_overlay(AppState *st) {
    sol_u32 items[INV_THUMB_CAP];
    int     n, pages, slot;
    float   sw = (float)st->fb_width, sh = (float)st->fb_height;
    float   us = sh / 1080.0f;
    char    pagebuf[32];
    if (!st->inv_open) return;
    n     = inventory_collect(st, items, INV_THUMB_CAP);
    st->inv_page = inv_clamp_page(st->inv_page, n, INV_PER_PAGE);
    pages = inv_page_count(n, INV_PER_PAGE);

    ui_quad(0.0f, 0.0f, sw, sh, 0.04f, 0.04f, 0.06f, 0.82f);   /* dim backdrop */

    for (slot = 0; slot < INV_PER_PAGE; slot++) {
        int   idx = st->inv_page * INV_PER_PAGE + slot;
        float x, y, w, h;
        SceneObject    *o;
        RhiRenderTarget rt;
        if (idx >= n) break;
        o = scene_get(&st->scene, items[idx]);
        inv_cell_rect(slot, INV_COLS, INV_ROWS, st->fb_width, st->fb_height, &x, &y, &w, &h);
        ui_quad(x, y, w, h, 0.08f, 0.09f, 0.11f, 1.0f);        /* matte behind */
        rt = inventory_thumb_lookup(st, items[idx]);           /* built at frame top */
        if (rt.id != 0) ui_textured_quad(rhi_render_target_texture(rt), x, y, w, h);
        ui_quad_outline(x, y, w, h, 2.0f, 0.85f, 0.88f, 0.95f, 1.0f);
        {   /* the item's name, centred under the tile */
            const char *nm = o ? scene_meta_get(&st->scene, items[idx], "name") : (const char *)0;
            const char *content = o ? o->content : (const char *)0;
            const char *label = nm ? nm : (content ? content : "item");
            float ts = 0.40f * us;
            float lw, lh;
            text_measure(st->ui_font, label, ts, &lw, &lh);
            ui_text(st->ui_font, label, x + (w - lw) * 0.5f,
                    y + h + font_ascent(st->ui_font) * ts + 4.0f * us, ts,
                    0.92f, 0.94f, 0.98f, 1.0f);
        }
    }

    if (pages > 1) {                                           /* page arrows + label */
        float rx, ry, rw, rh, ts = 0.45f * us, lw, lh;
        inv_prev_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
        ui_quad(rx, ry, rw, rh, 0.20f, 0.22f, 0.28f, 1.0f);
        ui_text(st->ui_font, "<", rx + rw * 0.32f, ry + rh * 0.5f + font_ascent(st->ui_font) * ts * 0.5f, ts, 1.0f, 1.0f, 1.0f, 1.0f);
        inv_next_rect(st->fb_width, st->fb_height, &rx, &ry, &rw, &rh);
        ui_quad(rx, ry, rw, rh, 0.20f, 0.22f, 0.28f, 1.0f);
        ui_text(st->ui_font, ">", rx + rw * 0.32f, ry + rh * 0.5f + font_ascent(st->ui_font) * ts * 0.5f, ts, 1.0f, 1.0f, 1.0f, 1.0f);
        snprintf(pagebuf, sizeof pagebuf, "page %d / %d", st->inv_page + 1, pages);
        ts = 0.36f * us;
        text_measure(st->ui_font, pagebuf, ts, &lw, &lh);
        ui_text(st->ui_font, pagebuf, sw * 0.5f - lw * 0.5f, sh * 0.965f, ts, 0.85f, 0.88f, 0.95f, 1.0f);
    }
}

/* The editor's 2D affordances, drawn inside the open UI batch. */
static void editor_draw_overlay(AppState *st) {
    float   aspect = (st->fb_height > 0)
                   ? (float)st->fb_width / (float)st->fb_height : 1.0f;
    float   hs = 6.0f;     /* half handle size, pixels */
    sol_u32 i;
    if (!st->editor.active) return;
    for (i = 0; i < st->scene.count; i++) {
        sol_u32  h = st->scene.objects[i].handle;
        RoomRect r;
        vec3     cw[4], port;
        float    px[4], py[4], psx, psy;
        int      k, ok = 1;
        sol_bool active_room;
        if (st->scene.objects[i].mesh_ref &&
            strcmp(st->scene.objects[i].mesh_ref, "terrain") != 0) continue;
        {
            const char *rt = scene_meta_get(&st->scene, h, "room_type");
            if (!rt || strcmp(rt, "church") == 0) continue;  /* a church rides its hill */
        }
        if (!scene_object_active(&st->scene, h)) continue;   /* only the active world */
        r = editor_room_rect(&st->scene, h);
        cw[0] = vec3_make(r.cx - r.hw, r.floor_y, r.cz - r.hd);
        cw[1] = vec3_make(r.cx + r.hw, r.floor_y, r.cz - r.hd);
        cw[2] = vec3_make(r.cx + r.hw, r.floor_y, r.cz + r.hd);
        cw[3] = vec3_make(r.cx - r.hw, r.floor_y, r.cz + r.hd);
        for (k = 0; k < 4; k++)
            if (!editor_world_to_screen(st, aspect, cw[k], &px[k], &py[k])) ok = 0;
        if (!ok) continue;
        active_room = (sol_bool)(st->editor.action != EDIT_IDLE && st->editor.room == h);
        /* footprint outline (brighter on the active room) */
        for (k = 0; k < 4; k++) {
            int n = (k + 1) & 3;
            if (active_room)
                ui_line(px[k], py[k], px[n], py[n], 2.0f, 1.0f, 0.85f, 0.30f, 0.95f);
            else
                ui_line(px[k], py[k], px[n], py[n], 1.5f, 0.65f, 0.72f, 0.80f, 0.85f);
        }
        /* corner resize handles — only for resizable footprints (rooms,
           church-less islands); an abbey hill shows move + connect, no resize */
        if (editor_resizable(&st->scene, h))
            for (k = 0; k < 4; k++)
                ui_quad(px[k] - hs, py[k] - hs, 2.0f * hs, 2.0f * hs,
                        0.92f, 0.92f, 0.96f, 0.95f);
        /* connection node (port) above the center */
        port = vec3_make(r.cx, r.floor_y + EDITOR_PORT_LIFT, r.cz);
        if (editor_world_to_screen(st, aspect, port, &psx, &psy))
            ui_quad(psx - 5.0f, psy - 5.0f, 10.0f, 10.0f, 0.45f, 0.85f, 1.0f, 0.95f);
    }
    /* rubber-band: from the source room's port to the live cursor ground point */
    if (st->editor.action == EDIT_CONNECT && st->editor.connect_from != 0) {
        RoomRect r = editor_room_rect(&st->scene, st->editor.connect_from);
        vec3     port = vec3_make(r.cx, r.floor_y + EDITOR_PORT_LIFT, r.cz);
        float    ax, ay, bx, by;
        if (editor_world_to_screen(st, aspect, port, &ax, &ay) &&
            editor_world_to_screen(st, aspect, st->editor.cursor_world, &bx, &by))
            ui_line(ax, ay, bx, by, 2.0f, 0.45f, 0.85f, 1.0f, 0.9f);
    }
}

static void render(AppState *state) {
    float   aspect;
    float   us;        /* UI scale: sizes track the framebuffer (see pass 3) */
    vec3    eye;
    mat4    view, proj;
    sol_u32 i;
    sol_u32 sel_root;
    double  rt0, rt1, rt2;
    double  t_text0;                /* P9 perf #2 measure: world-text section start */
    unsigned char *vis;

    ensure_render_target(state, state->fb_width, state->fb_height);
    if (state->inv_open) inventory_ensure_thumbs(state);   /* build missing bag thumbnails
                                                              inside the frame stream */
    rt0 = glfwGetTime();
    state->draws_total = 0;
    state->draws_done  = 0;
    floor_quads_ensure(state);   /* build floor quads before any rhi pass */

    sel_root = selection_root(&state->scene, state->selected_handle);  /* 0 if nothing selected */

    /* ---- pass 0: depth from the SUN into each cascade's shadow map (P8 item 6).
       The camera view/proj are built HERE because the cascades fit the camera
       frustum; pass 1 reuses them. ---- */
    aspect = (state->fb_height > 0)
           ? (float)state->fb_width / (float)state->fb_height
           : 1.0f;
    eye  = state->camera.pos;
    view = camera_view(&state->camera);
    proj = camera_proj(&state->camera, aspect);
    resolve_spot_caster(state, (float)glfwGetTime());   /* the casting sconce (P8 item 7) */
    {
        CascadeSet cs = sun_cascades(state, view, proj);
        int c;
        for (c = 0; c < SHADOW_CASCADES; c++) state->cascade_vp[c] = cs.vp[c];
        for (c = 0; c < SHADOW_CASCADES; c++) {
            /* vis_fill shares one buffer — used fully by emit before the next
               cascade refills it, so the sequential calls don't alias */
            unsigned char *lvis = vis_fill(state, state->cascade_vp[c]);
            rhi_begin_pass(state->shadow_rt[c], RHI_CLEAR_ALL,
                           0.0f, 0.0f, 0.0f, 1.0f);   /* clears depth to 1.0 */
            emit_shadow_casters(state, state->cascade_vp[c], lvis);
            rhi_end_pass();
        }
    }
    rt1 = glfwGetTime();
    yardstick_ms(&state->t_shadow, rt1 - rt0);

    /* ---- pass 0b: the spot sconce's shadow map (P8 item 7), one extra depth
       render via the same emit helper — timed separately so the per-caster
       cost shows on the HUD. ---- */
    if (state->spot_enabled) {
        unsigned char *svis = vis_fill(state, state->spot_vp);
        rhi_begin_pass(state->spot_shadow_rt, RHI_CLEAR_ALL, 0.0f, 0.0f, 0.0f, 1.0f);
        emit_shadow_casters(state, state->spot_vp, svis);
        rhi_end_pass();
    }
    {
        double rts = glfwGetTime();
        yardstick_ms(&state->t_spot_shadow, rts - rt1);
        rt1 = rts;                                  /* pass 1 timing starts after the sconce */
    }

    /* ---- pass 1: render the scene into the offscreen HDR target ---- */
    rhi_begin_pass(state->hdr_rt, RHI_CLEAR_ALL, 0.10f, 0.12f, 0.15f, 1.0f);

    /* aspect/eye/view/proj were built before pass 0 (the cascades fit them) */
    vis = vis_fill(state, mat4_mul(proj, view));    /* the CAMERA's view volume */

    /* point lights (P4 item 5): collect once per frame, upload once — the
       arrays are program state, so they survive draw_mesh's re-binds of the
       same pipeline through the whole object loop */
    {
        static const char *PN[8] = {
            "uPointPos[0]", "uPointPos[1]", "uPointPos[2]", "uPointPos[3]",
            "uPointPos[4]", "uPointPos[5]", "uPointPos[6]", "uPointPos[7]" };
        static const char *CN[8] = {
            "uPointColor[0]", "uPointColor[1]", "uPointColor[2]", "uPointColor[3]",
            "uPointColor[4]", "uPointColor[5]", "uPointColor[6]", "uPointColor[7]" };
        static const char *RN[8] = {
            "uPointRadius[0]", "uPointRadius[1]", "uPointRadius[2]", "uPointRadius[3]",
            "uPointRadius[4]", "uPointRadius[5]", "uPointRadius[6]", "uPointRadius[7]" };
        vec3  lp[MAX_POINT_LIGHTS], lc[MAX_POINT_LIGHTS];
        float lr[MAX_POINT_LIGHTS];
        int   ln = collect_point_lights(state, lp, lc, lr), li;
        rhi_set_pipeline(state->pipeline);
        rhi_set_uniform_int("uPointCount", ln);
        for (li = 0; li < ln; li++) {
            rhi_set_uniform_vec3 (PN[li], lp[li].x, lp[li].y, lp[li].z);
            rhi_set_uniform_vec3 (CN[li], lc[li].x, lc[li].y, lc[li].z);
            rhi_set_uniform_float(RN[li], lr[li]);
        }
    }

    /* skybox first (Phase A2): fills the background by sampling the equirect HDR
       per-pixel via the world view ray. Depth off -> writes color, not depth, so
       the object loop below draws over it normally. */
    if (state->env_cubemap.id) {
        vec3  fwd   = camera_forward(&state->camera);
        vec3  right = vec3_normalize(vec3_cross(fwd, vec3_make(0.0f, 1.0f, 0.0f)));
        vec3  up    = vec3_cross(right, fwd);
        float thf   = tanf(state->camera.fov * 0.5f);
        RhiTexture skytex = state->env_cubemap;       /* default: the sharp environment */
        float      skylod = 0.0f;
        if (state->show_prefilter) {                  /* 'P' inspects a prefilter roughness level */
            skytex = state->prefilter_cubemap;
            skylod = (float)state->prefilter_mip;
        } else if (state->show_irradiance) {          /* 'I' inspects the irradiance map */
            skytex = state->irradiance_cubemap;
        }
        rhi_set_pipeline(state->skybox_cube_pipeline);
        rhi_bind_texture(skytex, 0);
        rhi_set_uniform_int  ("uEnvCube", 0);
        rhi_set_uniform_float("uLod", skylod);
        rhi_set_uniform_vec3 ("uCamForward", fwd.x, fwd.y, fwd.z);
        rhi_set_uniform_vec3 ("uCamRight",   right.x, right.y, right.z);
        rhi_set_uniform_vec3 ("uCamUp",      up.x, up.y, up.z);
        rhi_set_uniform_float("uTanHalfFovY", thf);
        rhi_set_uniform_float("uAspect",      aspect);
        rhi_draw(0, 3);
    }

    /* iterate the scene — each object's WORLD matrix (parent * local) */
    for (i = 0; i < state->scene.count; i++) {
        const SceneObject *o = &state->scene.objects[i];
        mat4  model;
        float hl;
        if (o->mesh.index_count == 0) continue;   /* empty: transform-only, don't draw */
        if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
        if (o->mesh_ref && strcmp(o->mesh_ref, "pond") == 0)
            continue;                             /* ponds: the WATER PASS draws them */
        if (o->mesh_ref && strcmp(o->mesh_ref, "church_glass") == 0)
            continue;                             /* P9 item 2: the GLASS sub-pass draws them */
        if (o->mesh_ref && strcmp(o->mesh_ref, "window_glass") == 0)
            continue;                             /* Windows: the GLASS sub-pass draws them */
        if (o->mesh_ref && strcmp(o->mesh_ref, "church_decals") == 0)
            continue;                             /* P9 item 3: the DECAL sub-pass draws them */
        state->draws_total++;                     /* the yardstick: would draw */
        if (vis && !vis[o->handle]) continue;     /* outside the camera frustum
                                                     (piece 4): the HUD's left
                                                     number drops right here */
        if (state->reader_source != 0 &&
            group_root(&state->scene, o->handle) == state->reader_source)
            continue;                             /* its book is aloft (item 9) */
        model = scene_world_matrix(&state->scene, o);
        hl    = (sel_root != 0 && selection_root(&state->scene, o->handle) == sel_root)
              ? 1.0f : 0.0f;                       /* light the whole selected group */
        if (state->drop_target_handle != 0 &&
            o->handle == state->drop_target_handle)
            hl = 1.0f;                            /* folder under a dragged card */
        if (state->sel_count > 1 &&
            msel_contains(state->sel, state->sel_count, o->handle))
            hl = 1.0f;                            /* a member of the multi-selection */
        if (state->marquee_active && state->marquee_dragging &&
            o->parent == state->board_view &&
            object_is_selectable(&state->scene, o->handle)) {
            float fw = mesh_ref_param(o->mesh_ref ? o->mesh_ref : "card",
                                      o->mesh_params, o->mesh_param_count, "w");
            float fh = mesh_ref_param(o->mesh_ref ? o->mesh_ref : "card",
                                      o->mesh_params, o->mesh_param_count, "h");
            if (msel_rect_overlap(state->marquee_lx0, state->marquee_ly0,
                                  state->marquee_lx1, state->marquee_ly1,
                                  o->pos.x - fw * 0.5f, o->pos.y,
                                  o->pos.x + fw * 0.5f, o->pos.y + fh))
                hl = 1.0f;                        /* live marquee preview */
        }
        state->terrain_blend = (o->mesh_ref &&
                                strcmp(o->mesh_ref, "terrain") == 0)
                                   ? SOL_TRUE : SOL_FALSE;
        if (state->terrain_blend) {
            state->terrain_y0  = model.m[13];      /* the plot's world base */
            state->terrain_amp = mesh_ref_param("terrain", o->mesh_params,
                                                o->mesh_param_count, "amp");
        }
        {
            Material dm = o->material;
            if (g_path_mat.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "walkway") == 0)
                dm = g_path_mat;                  /* sandstone deck + steps (sourced experiment) */
            if (g_stone_mat.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "room") == 0)
                dm = g_stone_mat;                 /* stone-wall shell (the veneers cover the rest) */
            if (g_plaster_mat.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "board") == 0)
                dm = g_plaster_mat;               /* painted-plaster whiteboard (writing draws on top) */
            if (g_dark_wood.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "bookshelf") == 0)
                dm = g_dark_wood;                 /* dark-wood bookshelves */
            if (g_dark_wood.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "gate") == 0)
                dm = g_dark_wood;                 /* portal frame */
            if (g_dark_wood.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "window") == 0)
                dm = g_dark_wood;                 /* window frame (the glass child is "window_glass", untouched) */
            if (g_oak_mat.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "window_fill") == 0)
                dm = g_oak_mat;                   /* shaped inner fill: oak veneer, distinct from the casing */
            if (g_oak_mat.albedo_tex.id != 0 && o->mesh_ref &&
                strcmp(o->mesh_ref, "card") == 0 &&
                (o->kind == KIND_FILE || o->kind == KIND_FOLDER)) {
                dm = g_oak_mat;                   /* oak-veneer tablets, split LIGHT/DARK by kind so
                                                     files and folders read apart even over the oak,
                                                     and the per-kind label ink stays high-contrast */
                if (o->kind == KIND_FOLDER)
                    dm.base_color = vec3_make(0.42f, 0.30f, 0.18f);   /* dark walnut-toned oak */
                else
                    dm.base_color = vec3_make(1.00f, 0.95f, 0.85f);   /* light honey oak */
            }
            dm.emissive.x *= o->overlay_glow;     /* the heart and the pool of
                                                     light breathe TOGETHER —
                                                     same channel, two consumers */
            dm.emissive.y *= o->overlay_glow;
            dm.emissive.z *= o->overlay_glow;
            if (handle_is_cut(state, o->handle))
                draw_glass(state, o->mesh, model, view, proj, eye, dm);  /* cut: dimmed (Cmd+X) */
            else
                draw_mesh(state, o->mesh, model, view, proj, eye, hl, dm);
        }
        /* a folder book's white page block: a unit cube scaled to the leaf
           cavity, drawn right after its cover so it inherits the folder's
           visibility + selection highlight. Geometry mirrors the leaf box
           make_folderbook used to build (board=d*0.18, inset=h*0.05, lip=w*0.03). */
        if (o->mesh_ref && strcmp(o->mesh_ref, "folderbook") == 0) {
            float fw = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "w");
            float fh = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "h");
            float fd = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "d");
            float fboard = fd * 0.18f, finset = fh * 0.05f, flip = fw * 0.03f;
            float lw, lh2, ld;
            if (fboard < 1e-4f) fboard = 1e-4f;       /* match make_folderbook */
            lw  = fw - flip - fboard;
            lh2 = fh - 2.0f * finset;
            ld  = fd - 2.0f * fboard;
            if (lw > 0.0f && lh2 > 0.0f && ld > 0.0f) {
                Material pm = material_default();      /* plain white pages */
                vec3     c  = vec3_make((fboard - flip) * 0.5f, fh * 0.5f, fd * 0.5f);
                mat4     lm = mat4_mul(model, mat4_from_trs(c, quat_identity(),
                                       vec3_make(lw, lh2, ld)));
                if (state->folderbook_leaves_mesh.index_count == 0) {
                    MeshBuilder mb;
                    mb_init(&mb);
                    make_box(&mb, 1.0f, 1.0f, 1.0f);  /* centered unit cube */
                    state->folderbook_leaves_mesh = mesh_from_builder(&mb);
                    mb_free(&mb);
                }
                draw_mesh(state, state->folderbook_leaves_mesh, lm, view, proj, eye, hl, pm);
            }
        }
        state->draws_done++;                      /* == total until culling (P4 i2 p4) */
    }
    state->terrain_blend = SOL_FALSE;              /* the reader rig is not land */

    /* sandstone floor overlay (sourced experiment): a tiled sandstone quad over
       each active room's stone floor, just above it. render-only, no scene change. */
    if (g_floor_mat.albedo_tex.id != 0) {
        sol_u32 fi;
        for (fi = 0; fi < state->scene.count; fi++) {
            SceneObject *o = &state->scene.objects[fi];
            float fw, fd;
            Mesh  fq;
            mat4  fm;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;
            if (vis && !vis[o->handle]) continue;
            fw = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "w");
            fd = mesh_ref_param("room", o->mesh_params, o->mesh_param_count, "d");
            fq = floor_quad_for(fw, fd);
            if (fq.index_count == 0) continue;
            fm = mat4_mul(scene_world_matrix(&state->scene, o),
                          mat4_translate(vec3_make(0.0f, FLOOR_EPS, 0.0f)));
            draw_mesh(state, fq, fm, view, proj, eye, 0.0f, g_floor_mat);
        }
    }

    /* room frame overlay: each active room's cached wall-panel mesh, drawn
       over its inner wall faces. render-only. */
    {
        sol_u32 wi;
        for (wi = 0; wi < state->scene.count; wi++) {
            SceneObject *o = &state->scene.objects[wi];
            RoomFrame   *rf;
            mat4         rm;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "room") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;
            if (vis && !vis[o->handle]) continue;
            rf = room_frame_get(o->handle);
            if (!rf) continue;
            rm = scene_world_matrix(&state->scene, o);
            if (g_wall_mat.albedo_tex.id != 0 && rf->wall.index_count > 0)
                draw_mesh(state, rf->wall, rm, view, proj, eye, 0.0f, g_wall_mat);
            if (g_dark_wood.albedo_tex.id != 0 && rf->wood.index_count > 0)
                draw_mesh(state, rf->wood, rm, view, proj, eye, 0.0f, g_dark_wood);
            if (g_roof_mat.albedo_tex.id != 0 && rf->roof.index_count > 0)
                draw_mesh(state, rf->roof, rm, view, proj, eye, 0.0f, g_roof_mat);
            if (g_wall_mat.albedo_tex.id != 0 && rf->gable.index_count > 0)
                draw_mesh(state, rf->gable, rm, view, proj, eye, 0.0f, g_wall_mat);
            if (g_path_mat.albedo_tex.id != 0 && rf->door_floor.index_count > 0)
                draw_mesh(state, rf->door_floor, rm, view, proj, eye, 0.0f, g_path_mat);
            if (g_dark_wood.albedo_tex.id != 0 && rf->door_trim.index_count > 0)
                draw_mesh(state, rf->door_trim, rm, view, proj, eye, 0.0f, g_dark_wood);
        }
    }

    /* the campus ground (derived; drawn with the terrain slope/height palette) */
    if (g_campus.enabled && g_campus.mesh.index_count > 0) {
        mat4 cm = mat4_translate(g_campus.center);        /* heights are world-absolute */
        if (g_campus_mat.albedo_tex.id != 0) {            /* sourced rock ground (experiment) */
            draw_mesh(state, g_campus.mesh, cm, view, proj, eye, 0.0f, g_campus_mat);
        } else {                                          /* fallback: the slope/height palette */
            state->terrain_blend = SOL_TRUE;
            state->terrain_y0    = g_campus.y0;
            state->terrain_amp   = g_campus.amp_range;
            draw_mesh(state, g_campus.mesh, cm, view, proj, eye, 0.0f, material_default());
            state->terrain_blend = SOL_FALSE;
        }
    }

    /* dark-wood curb trim along the marble walkways (its own cached mesh, since
       it wears a different material than the deck). gated on the deck mesh so a
       stale entry for a now-invalid walkway isn't drawn. */
    if (g_dark_wood.albedo_tex.id != 0) {
        sol_u32 ti;
        for (ti = 0; ti < state->scene.count; ti++) {
            SceneObject *o = &state->scene.objects[ti];
            WalkwayTrim *wt;
            mat4         tmw;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "walkway") != 0) continue;
            if (o->mesh.index_count == 0) continue;            /* invalid/empty walkway */
            if (!scene_object_active(&state->scene, o->handle)) continue;
            if (vis && !vis[o->handle]) continue;
            wt = walk_trim_get(o->handle);
            if (!wt || wt->trim.index_count == 0) continue;
            tmw = scene_world_matrix(&state->scene, o);
            draw_mesh(state, wt->trim, tmw, view, proj, eye, 0.0f, g_dark_wood);
        }
    }

    /* skinned models (item 9): objects wearing skin_glb meta, drawn at the
       ANIMATED pose — the animate component picks clip and speed, absolute
       t x speed keeps it deterministic, absent component = the rest pose
       (the file IS the behavior). */
    if (state->skinned_pipeline.id) {
        sol_u32 sk;
        for (sk = 0; sk < state->scene.count; sk++) {
            SceneObject  *o  = &state->scene.objects[sk];
            const char   *sg = scene_meta_get(&state->scene, o->handle,
                                              "skin_glb");
            SkinnedModel *sm;
            mat4          model;
            int           clip;
            float         speed;
            if (!sg) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
            sm = skinned_get(sg);
            if (!sm) continue;
            skin_anim_of(o, &clip, &speed);
            model = scene_world_matrix(&state->scene, o);
            draw_skinned(state, sm, clip, (float)glfwGetTime() * speed,
                         model, view, proj, eye,
                         o->handle == sel_root ? 1.0f : 0.0f);
            state->draws_done++;
        }
    }

    /* the meadow (P4 item 3 piece 2): thousands of tufts, ONE draw per
       island — and an island culled by the frustum takes its grass with it
       (the patch rides the island's own visibility bit) */
    if (state->meadow_pipeline.id && state->meadow_count > 0) {
        int mp;
        rhi_set_pipeline(state->meadow_pipeline);
        rhi_set_uniform_mat4("uView", view.m);
        rhi_set_uniform_mat4("uProj", proj.m);
        rhi_set_uniform_float("uTime", (float)glfwGetTime());
        rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
        rhi_bind_vertex_buffer(state->meadow_vbuf);
        rhi_bind_index_buffer(state->meadow_ibuf);
        for (mp = 0; mp < state->meadow_count; mp++) {
            if (vis && !vis[state->meadow[mp].island]) continue;
            rhi_bind_instance_buffer(state->meadow[mp].data);
            rhi_draw_indexed_instanced(0, 12, state->meadow[mp].count);
        }
        /* the flowers (item 7): the bloom mesh, same pipeline, one more
           draw per island where blooms gathered */
        if (state->flower_vbuf.id) {
            rhi_bind_vertex_buffer(state->flower_vbuf);
            rhi_bind_index_buffer(state->flower_ibuf);
            for (mp = 0; mp < state->meadow_count; mp++) {
                if (state->meadow[mp].flower_count == 0) continue;
                if (vis && !vis[state->meadow[mp].island]) continue;
                rhi_bind_instance_buffer(state->meadow[mp].flowers);
                rhi_draw_indexed_instanced(0, 12, state->meadow[mp].flower_count);
            }
        }
    }

    /* the FIELD forest (P7 item 5): per island, each variant's wood in
       one instanced draw + the canopy merged per leaf-kind — through the
       ornament PBR pipeline, riding the island's visibility bit. uModel
       = the island world, so a dragged island carries its wood. */
    if (state->ornament_pipeline.id && state->forest_count > 0) {
        int fp, v;
        Material leafmat[2];
        leafmat[0] = material_default();
        leafmat[0].base_color = flora_leaf_color(FLORA_OAK);
        leafmat[0].roughness  = 0.85f;
        leafmat[1] = material_default();
        leafmat[1].base_color = flora_leaf_color(FLORA_PINE);
        leafmat[1].roughness  = 0.85f;
        for (fp = 0; fp < state->forest_count; fp++) {
            ForestPatch *f  = &state->forest[fp];
            SceneObject *isl = scene_get(&state->scene, f->island);
            mat4         model;
            if (!isl) continue;
            if (vis && !vis[f->island]) continue;
            model = scene_world_matrix(&state->scene, isl);
            for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
                Mesh *um = &state->forest_wood[v];
                if (f->wood_count[v] == 0 || um->index_count == 0) continue;
                bind_scene_uniforms(state, state->ornament_pipeline, model,
                                    view, proj, eye, 0.0f, f->wood_mat[v]);
                rhi_set_uniform_float("uTime", (float)glfwGetTime());
                rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
                rhi_bind_vertex_buffer(um->vbuffer);
                rhi_bind_instance_buffer(f->wood[v]);
                rhi_bind_index_buffer(um->ibuffer);
                rhi_draw_indexed_instanced(0, um->index_count, f->wood_count[v]);
                state->draws_done++;
            }
            {   /* the canopy: broadleaf (mesh 1) then conifer (mesh 2) */
                int lk;
                for (lk = 0; lk < 2; lk++) {
                    Mesh *um = &state->orn_mesh[lk == 0 ? ORN_LEAF_BROAD
                                                        : ORN_LEAF_CONIFER];
                    if (f->canopy_count[lk] == 0 || um->index_count == 0) continue;
                    bind_scene_uniforms(state, state->ornament_pipeline, model,
                                        view, proj, eye, 0.0f, leafmat[lk]);
                    rhi_set_uniform_float("uTime", (float)glfwGetTime());
                    rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
                    rhi_bind_vertex_buffer(um->vbuffer);
                    rhi_bind_instance_buffer(f->canopy[lk]);
                    rhi_bind_index_buffer(um->ibuffer);
                    rhi_draw_indexed_instanced(0, um->index_count,
                                               f->canopy_count[lk]);
                    state->draws_done++;
                }
            }
            if (f->scree_count > 0 && state->scree_mesh.index_count > 0) {
                Mesh *um = &state->scree_mesh;
                bind_scene_uniforms(state, state->ornament_pipeline, model,
                                    view, proj, eye, 0.0f,
                                    forest_scree_material());
                rhi_set_uniform_float("uTime", (float)glfwGetTime());
                rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
                rhi_bind_vertex_buffer(um->vbuffer);
                rhi_bind_instance_buffer(f->scree);
                rhi_bind_index_buffer(um->ibuffer);
                rhi_draw_indexed_instanced(0, um->index_count, f->scree_count);
                state->draws_done++;
            }
        }
    }

    /* the campus flora: grass (meadow pipeline, world-space) + trees (ornament
       pipeline, translated by the campus centre). Drawn when the campus is on;
       no per-island vis cull (the campus IS the active world). */
    if (g_campus.enabled && g_campus_flora.grass_n > 0 && state->meadow_pipeline.id) {
        rhi_set_pipeline(state->meadow_pipeline);
        rhi_set_uniform_mat4("uView", view.m);
        rhi_set_uniform_mat4("uProj", proj.m);
        rhi_set_uniform_float("uTime", (float)glfwGetTime());
        rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
        rhi_bind_vertex_buffer(state->meadow_vbuf);
        rhi_bind_index_buffer(state->meadow_ibuf);
        rhi_bind_instance_buffer(g_campus_flora.grass);
        rhi_draw_indexed_instanced(0, 12, g_campus_flora.grass_n);
    }
    if (g_campus.enabled && g_campus_flora.flower_n > 0 && state->meadow_pipeline.id && state->flower_vbuf.id) {
        rhi_set_pipeline(state->meadow_pipeline);
        rhi_set_uniform_mat4("uView", view.m);
        rhi_set_uniform_mat4("uProj", proj.m);
        rhi_set_uniform_float("uTime", (float)glfwGetTime());
        rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
        rhi_bind_vertex_buffer(state->flower_vbuf);
        rhi_bind_index_buffer(state->flower_ibuf);
        rhi_bind_instance_buffer(g_campus_flora.flowers);
        rhi_draw_indexed_instanced(0, 12, g_campus_flora.flower_n);
    }
    if (g_campus.enabled && state->ornament_pipeline.id) {
        mat4     cmodel = mat4_translate(g_campus.center);
        Material leafmat[2];
        int      v, lk;
        leafmat[0] = material_default(); leafmat[0].base_color = flora_leaf_color(FLORA_OAK);  leafmat[0].roughness = 0.85f;
        leafmat[1] = material_default(); leafmat[1].base_color = flora_leaf_color(FLORA_PINE); leafmat[1].roughness = 0.85f;
        for (v = 0; v < FOREST_VARIANT_COUNT; v++) {
            Mesh *um = &state->forest_wood[v];
            if (g_campus_flora.wood_n[v] == 0 || um->index_count == 0) continue;
            bind_scene_uniforms(state, state->ornament_pipeline, cmodel, view, proj, eye, 0.0f, forest_bark_material());
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_instance_buffer(g_campus_flora.wood[v]);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_draw_indexed_instanced(0, um->index_count, g_campus_flora.wood_n[v]);
        }
        for (lk = 0; lk < 2; lk++) {
            Mesh *um = &state->orn_mesh[lk == 0 ? ORN_LEAF_BROAD : ORN_LEAF_CONIFER];
            if (g_campus_flora.canopy_n[lk] == 0 || um->index_count == 0) continue;
            bind_scene_uniforms(state, state->ornament_pipeline, cmodel, view, proj, eye, 0.0f, leafmat[lk]);
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_instance_buffer(g_campus_flora.canopy[lk]);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_draw_indexed_instanced(0, um->index_count, g_campus_flora.canopy_n[lk]);
        }
        if (g_campus_flora.scree_n > 0 && state->scree_mesh.index_count > 0) {
            Mesh *um = &state->scree_mesh;
            bind_scene_uniforms(state, state->ornament_pipeline, cmodel, view, proj, eye, 0.0f, forest_scree_material());
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_instance_buffer(g_campus_flora.scree);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_draw_indexed_instanced(0, um->index_count, g_campus_flora.scree_n);
        }
    }

    /* the instanced ornament (P6 item 10; P7 item 4 grew it to KINDS):
       balusters wear their carcass's OWN material (stone), leaf clusters
       wear the canopy green carried on the patch — one draw each, riding
       the source's visibility bit. uTime drives the leaf sway (balusters
       carry sway amp 0, so they hold still). */
    if (state->ornament_pipeline.id && state->ornament_count > 0) {
        int op;
        for (op = 0; op < state->ornament_count; op++) {
            OrnamentPatch *pt = &state->ornament[op];
            Mesh          *um = &state->orn_mesh[pt->kind];
            SceneObject   *o  = scene_get(&state->scene, pt->source);
            Material       mat;
            mat4 model;
            if (!o || um->index_count == 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
            if (vis && !vis[o->handle]) continue;
            mat   = (pt->kind == ORN_BALUSTER) ? o->material : pt->material;
            model = scene_world_matrix(&state->scene, o);
            bind_scene_uniforms(state, state->ornament_pipeline, model,
                                view, proj, eye,
                                o->handle == sel_root ? 1.0f : 0.0f, mat);
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3("uWind", state->wind_dx, state->wind_dz, state->wind_gust);
            rhi_bind_vertex_buffer(um->vbuffer);
            rhi_bind_instance_buffer(pt->data);
            rhi_bind_index_buffer(um->ibuffer);
            rhi_draw_indexed_instanced(0, um->index_count, pt->count);
            state->draws_done++;
        }
    }

    /* the reader's open book (item 9): view-state geometry, drawn at the
       animated pose — never part of the scene graph */
    if (state->reader_state != READER_IDLE) {
        mat4 bm = mat4_from_trs(state->reader_pos, state->reader_rot,
                                vec3_make(1.0f, 1.0f, 1.0f));
        if (state->reader_cover.index_count > 0)
            draw_mesh(state, state->reader_cover, bm, view, proj, eye, 0.0f,
                      state->reader_cover_mat);
        if (state->reader_block.index_count > 0)
            draw_mesh(state, state->reader_block, bm, view, proj, eye, 0.0f,
                      state->reader_block_mat);
        if (state->reader_turning != 0 && state->reader_leaf.index_count > 0)
            draw_mesh(state, state->reader_leaf, bm, view, proj, eye, 0.0f,
                      state->reader_block_mat);
    }

    /* the floor-plan overlay (P6 item 3): the island's destined church
       as glowing chalk — view-state geometry like the reader, drawn in
       the island's frame so the plan rides its plot */
    if (state->plan_on && state->plan_mesh.index_count > 0) {
        SceneObject *isl = scene_get(&state->scene, state->plan_plot);
        if (isl) {
            Material pm   = material_default();
            pm.base_color = vec3_make(0.85f, 0.75f, 0.40f);
            pm.emissive   = vec3_make(1.6f, 1.15f, 0.35f);
            pm.roughness  = 1.0f;
            draw_mesh(state, state->plan_mesh,
                      scene_world_matrix(&state->scene, isl),
                      view, proj, eye, 0.0f, pm);
        }
    }

    /* world text (item 8): card labels + note bodies, drawn as depth-tested
       INK after the opaque geometry, still inside the HDR pass — it rides
       through ACES like everything else. Same atlas as the HUD; the SDF
       fwidth threshold keeps it crisp at any distance. */
    text_shape_stats_reset();        /* P9 perf #2 measure: scope to this section */
    wtext_frame_begin();             /* advance the glyph-cache LRU clock */
    wtext_stats_reset();
    t_text0 = glfwGetTime();
    if (state->ui_font) {
        const Font *uf  = state->ui_font;
        const float lh  = font_line_height(uf);            /* px at base size */
        mat4        vp  = mat4_mul(proj, view);

        /* the reader's pages (item 9): the page plane is book-local xz at
           the text-field height; the text frame is R_x(-90), so text-up =
           toward the book's head. Pages are line ranges (piece 4). */
        if (state->reader_state != READER_IDLE &&
            state->reader_cover.index_count > 0) {
            const float *bp = state->reader_params;
            float wb, zh, xf, mg;
            mat4  bm, page;
            page = reader_page_matrix(state, &wb, &zh, &xf, &mg);
            bm   = mat4_from_trs(state->reader_pos, state->reader_rot,
                                 vec3_make(1.0f, 1.0f, 1.0f));
            if (state->reader_app) {
                /* walk the synth UI draw-list (Task 6 filled widget_ctx) onto
                   the page: RECT -> a flat-colour quad via draw_mesh (existing
                   lit pipeline, no new shader), TEXT -> wtext. A per-command z
                   step (higher ci = prouder) preserves emission/paint order so
                   a slider handle sits above its track, without z-fighting the
                   page surface. */
                int ci;
                for (ci = 0; ci < state->widget_ctx.cmd_count; ci++) {
                    const WidgetCmd *cmd = &state->widget_ctx.cmds[ci];
                    float z = 0.0006f + (float)ci * 0.00004f;
                    if (cmd->type == WIDGET_CMD_RECT) {
                        vec3     ctr = vec3_make(cmd->x + cmd->w * 0.5f,
                                                 cmd->y - cmd->h * 0.5f, z);
                        mat4     m   = mat4_mul(page,
                                       mat4_from_trs(ctr, quat_identity(),
                                           vec3_make(cmd->w, cmd->h, 1.0f)));
                        Material wm  = material_default();
                        wm.base_color = vec3_make(cmd->r, cmd->g, cmd->b);
                        wm.roughness  = 0.85f;
                        if (state->widget_quad.index_count > 0)
                            draw_mesh(state, state->widget_quad, m,
                                      view, proj, eye, 0.0f, wm);
                    } else {
                        mat4  tm   = mat4_mul(page,
                                     mat4_translate(vec3_make(0.0f, 0.0f, z)));
                        float px2m = (lh > 0.0f) ? cmd->h / lh : cmd->h;
                        wtext_block(uf, vp, tm, cmd->text, cmd->x, cmd->y,
                                    px2m, 0.0f, cmd->r, cmd->g, cmd->b);
                    }
                }
            } else if (state->reader_is_image && state->reader_image_tex.id) {
                /* the image on the RIGHT page (fitted), filename on the LEFT */
                float field_w_left = wb - xf - 2.0f * mg;
                if (state->reader_image_quad.index_count > 0) {
                    float    cx = (xf + wb) * 0.5f;        /* right-page center x */
                    mat4     im = mat4_mul(page,
                                  mat4_translate(vec3_make(cx, 0.0f, 0.0008f)));
                    Material pm = material_default();
                    pm.base_color = vec3_make(1.0f, 1.0f, 1.0f);  /* show as-is */
                    pm.roughness  = 0.95f;
                    pm.albedo_tex = state->reader_image_tex;
                    draw_mesh(state, state->reader_image_quad, im,
                              view, proj, eye, 0.0f, pm);
                }
                {   /* the filename, shrunk to the left page width */
                    SceneObject *src  = scene_get(&state->scene,
                                                  state->reader_source);
                    const char  *path = (src && src->content) ? src->content
                                                              : (const char *)0;
                    const char  *nm;
                    char         lbuf[16];
                    float        cpx, nw;
                    if (path) { nm = strrchr(path, '/'); nm = nm ? nm + 1 : path; }
                    else        nm = object_label(&state->scene,
                                                  state->reader_source, lbuf);
                    cpx = (bp[1] * 0.020f) / lh;
                    text_measure(uf, nm, 1.0f, &nw, (float *)0);
                    if (nw * cpx > field_w_left && nw > 0.0f)
                        cpx = field_w_left / nw;
                    wtext_block(uf, vp, page, nm, -wb + mg, zh - mg, cpx, 0.0f,
                                0.13f, 0.10f, 0.08f);
                }
            } else if (state->reader_text) {
                int L      = state->reader_lines_per_page;
                int pages  = (state->reader_line_count + L - 1) / L;
                int pl       = state->reader_spread * 2;
                int left_pg  = pl, right_pg = pl + 1;
                sol_bool hug_r, hug_l;
                /* while a leaf is in flight it CARRIES the moving text, so
                   the static page it will land on keeps the OLD text for
                   the whole turn — at landing the leaf's ink and the
                   static page's ink coincide, and the handoff is seamless */
                if (state->reader_turning > 0)
                    left_pg = state->reader_turn_old * 2;
                if (state->reader_turning < 0)
                    right_pg = state->reader_turn_old * 2 + 1;
                /* while the leaf still HUGS a page (the blend weights say
                   so), that page's static text stays off entirely: the
                   leaf's own ink is the page's text, mm-coincident static
                   ink would z-flicker, and the next page's text should
                   only appear under a leaf that is visibly airborne */
                hug_r = state->reader_turning != 0 &&
                        state->reader_leaf_shape.wR > 0.35f;
                hug_l = state->reader_turning != 0 &&
                        state->reader_leaf_shape.wL > 0.35f;
                if (!hug_l)
                    reader_draw_page(state, vp, page, left_pg,  -wb + mg, zh - mg);
                if (!hug_r)
                    reader_draw_page(state, vp, page, right_pg,  xf + mg, zh - mg);
                if (state->reader_turning != 0) {
                    /* the leaf's ink: recto = the right-page side, verso =
                       the left-page side (negative layout x mirrors through
                       the spine); the paper itself depth-occludes whichever
                       side faces away — no facing logic anywhere */
                    mat4 hinge = mat4_mul(bm, mat4_mul(
                        mat4_translate(vec3_make(0.0f,
                            state->reader_leaf_shape.pinch, 0.0f)),
                        quat_to_mat4(quat_from_axis_angle(
                            vec3_make(1.0f, 0.0f, 0.0f), sol_radians(-90.0f)))));
                    int recto = (state->reader_turning > 0)
                              ? state->reader_turn_old * 2 + 1
                              : state->reader_spread * 2 + 1;
                    int verso = (state->reader_turning > 0)
                              ? state->reader_spread * 2
                              : state->reader_turn_old * 2;
                    reader_draw_page_bent(state, vp, hinge, recto,
                                          xf + mg, zh - mg, 0.0012f);
                    reader_draw_page_bent(state, vp, hinge, verso,
                                          -wb + mg, zh - mg, 0.0009f);
                }
                {   /* the folio line, foot of the right page */
                    char  fbuf[48];
                    float flh = font_line_height(state->reader_font)
                              * state->reader_px2m * 0.8f;
                    sprintf(fbuf, "%d / %d",
                            state->reader_spread + 1, (pages + 1) / 2);
                    wtext_block(state->reader_font, vp, page, fbuf,
                                xf + mg, -zh + mg * 0.5f + flh,
                                state->reader_px2m * 0.8f, 0.0f,
                                0.45f, 0.40f, 0.34f);
                }
            } else {
                char lbuf[16];
                wtext_block(uf, vp, page,
                            object_label(&state->scene, state->reader_source, lbuf),
                            -wb + mg, zh - mg, (bp[1] * 0.035f) / lh,
                            wb - xf - 2.0f * mg, 0.13f, 0.10f, 0.08f);
            }
        }

        for (i = 0; i < state->scene.count; i++) {
            SceneObject *o = &state->scene.objects[i];
            float cw, ch, ct, margin, usable, px2m, name_w;
            float ink_r, ink_g, ink_b;
            mat4  face;
            char  lbuf[16];
            const char *nm;
            if (o->kind == KIND_PLAIN) continue;            /* cards only */
            if (!o->mesh_ref || strcmp(o->mesh_ref, "card") != 0) continue;
            if (vis && !vis[o->handle]) continue;           /* culled card: its ink
                                                               would be offscreen too,
                                                               and SHAPING text per
                                                               frame is the real cost */
            cw = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
            ch = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "h");
            ct = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "t");
            face = mat4_mul(scene_world_matrix(&state->scene, o),
                            mat4_translate(vec3_make(0.0f, 0.0f, ct * 0.5f + 0.0008f)));
            if (o->kind == KIND_TOMBSTONE) {                /* light ink on slate */
                ink_r = 0.82f; ink_g = 0.82f; ink_b = 0.86f;
            } else if ((o->kind == KIND_FOLDER || o->kind == KIND_FILE) &&
                       g_oak_mat.albedo_tex.id != 0) {
                ink_r = 0.93f; ink_g = 0.90f; ink_b = 0.82f;  /* light ink on the oak tablets */
            } else {                                        /* dark ink on paper */
                ink_r = 0.10f; ink_g = 0.09f; ink_b = 0.08f;
            }
            margin = 0.025f;
            usable = cw - 3.0f * margin;       /* left pad 2*margin (doubled), right margin */
            /* a tablet filed onto a bookshelf reads like a book SPINE: the name
               runs DOWN the visible edge (the card's -X face, turned toward you
               by the spine's +90 yaw), letters rotated to read top-to-bottom.
               The flat front/back label is skipped for these (a spine card). */
            {
                SceneObject *par = (o->parent != 0)
                                 ? scene_get(&state->scene, o->parent) : (SceneObject *)0;
                if (par && par->mesh_ref && strcmp(par->mesh_ref, "bookshelf") == 0) {
                    quat  q = quat_mul(
                                quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), sol_radians(90.0f)),
                                quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), sol_radians(-90.0f)));
                    float spx = 0.020f / lh;                 /* small spine letters */
                    mat4  spine;
                    nm = object_label(&state->scene, o->handle, lbuf);
                    text_measure(uf, nm, 1.0f, &name_w, (float *)0);
                    if (name_w * spx > ch - 0.05f && name_w > 0.0f)
                        spx = (ch - 0.05f) / name_w;         /* fit the spine height */
                    /* The text's "up" (toward top_y) maps to the card's +z, i.e.
                       across the spine's thickness; the line hangs DOWN from
                       top_y, so its band sits a full line-height BELOW the
                       origin. Seat the origin at +lineHeight/2 so that band is
                       centered on the spine's thickness (z=0) instead of running
                       alongside it with only the top edge on the spine. */
                    spine = mat4_mul(
                                mat4_mul(scene_world_matrix(&state->scene, o),
                                         mat4_translate(vec3_make(-cw * 0.5f - 0.001f,
                                                                  ch - 0.02f,
                                                                  spx * lh * 0.5f))),
                                quat_to_mat4(q));
                    wtext_block(uf, vp, spine, nm, 0.0f, 0.0f, spx, 0.0f,
                                ink_r, ink_g, ink_b);
                    continue;                                /* spines: no flat label */
                }
            }
            /* the label: the card's name across the top, shrunk to fit. Notes
               carry their content in the body, not a name — skip their label. */
            if (o->kind != KIND_NOTE) {
                nm = object_label(&state->scene, o->handle, lbuf);
                px2m = 0.038f / lh;                             /* ~3.8cm line */
                text_measure(uf, nm, 1.0f, &name_w, (float *)0);
                if (name_w * px2m > usable && name_w > 0.0f)
                    px2m = usable / name_w;                     /* shrink, don't clip */
                wtext_block(uf, vp, face, nm,
                            -cw * 0.5f + 2.0f * margin, ch - 2.0f * margin, px2m, 0.0f,
                            ink_r, ink_g, ink_b);
                {   /* the same label on the BACK face, so the tablet names itself
                       from both sides (rotate 180 about Y so it reads, not mirrors;
                       the engine never culls back faces, so the back is visible) */
                    mat4 back = mat4_mul(
                        mat4_mul(scene_world_matrix(&state->scene, o),
                                 quat_to_mat4(quat_from_axis_angle(
                                     vec3_make(0.0f, 1.0f, 0.0f), sol_radians(180.0f)))),
                        mat4_translate(vec3_make(0.0f, 0.0f, ct * 0.5f + 0.0008f)));
                    wtext_block(uf, vp, back, nm,
                                -cw * 0.5f + 2.0f * margin, ch - 2.0f * margin, px2m, 0.0f,
                                ink_r, ink_g, ink_b);
                }
            }
            /* the note body: the inline text meta, wrapped to the card.
               While the note has focus, a moveable caret quad marks the
               insertion point (edit_cursor); the meta mirrors every
               keystroke, so this renders the typing live. */
            if (o->kind == KIND_NOTE) {
                const char *txt = scene_meta_get(&state->scene, o->handle, "text");
                if (state->edit_handle == o->handle) {     /* selection highlight (behind ink) */
                    float       bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    float       x0 = -cw * 0.5f + 2.0f * margin;
                    float       y0 = ch - 2.0f * margin;
                    const char *src = txt ? txt : "";
                    CaretField  cf;
                    caret_build(uf, src, bpx2m, usable, &cf);
                    if (state->caret_mesh.index_count == 0) {
                        MeshBuilder mb;
                        mb_init(&mb);
                        make_page(&mb, 1.0f, 1.0f);
                        state->caret_mesh = mesh_from_builder(&mb);
                        mb_free(&mb);
                    }
                    if (state->edit_cursor != state->edit_sel_anchor) {
                        CaretSpan sp[CARET_MAX_LINES];
                        int lo = state->edit_cursor < state->edit_sel_anchor
                                     ? state->edit_cursor : state->edit_sel_anchor;
                        int hi = state->edit_cursor > state->edit_sel_anchor
                                     ? state->edit_cursor : state->edit_sel_anchor;
                        int ns = caret_sel_spans(&cf, lo, hi, sp, CARET_MAX_LINES), si;
                        Material hm = material_default();
                        hm.base_color = vec3_make(0.30f, 0.45f, 0.85f);   /* soft blue */
                        for (si = 0; si < ns; si++) {
                            float w  = sp[si].x1 - sp[si].x0;
                            vec3  hp;
                            mat4  hmodel;
                            if (w <= 0.0f) continue;
                            hp = vec3_make(x0 + sp[si].x0 + w * 0.5f,
                                           y0 - (float)sp[si].line * cf.line_h - cf.line_h * 0.5f,
                                           0.0004f);
                            hmodel = mat4_mul(face, mat4_from_trs(hp, quat_identity(),
                                              vec3_make(w, cf.line_h, 1.0f)));
                            draw_glass(state, state->caret_mesh, hmodel, view, proj, eye, hm);
                        }
                    }
                }
                if (txt && txt[0]) {
                    float bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    wtext_block(uf, vp, face, txt,
                                -cw * 0.5f + 2.0f * margin, ch - 2.0f * margin,
                                bpx2m, usable, ink_r, ink_g, ink_b);
                }
                if (state->edit_handle == o->handle) {     /* the caret quad, on top */
                    float       bpx2m = note_text_size(&state->scene, o->handle) / lh;
                    float       x0 = -cw * 0.5f + 2.0f * margin;
                    float       y0 = ch - 2.0f * margin;
                    const char *src = txt ? txt : "";
                    CaretField  cf;
                    int         slot, linei;
                    Material    cm = material_default();
                    caret_build(uf, src, bpx2m, usable, &cf);
                    slot  = caret_slot_for_offset(&cf, state->edit_cursor);
                    linei = (slot >= 0) ? caret_line_of_slot(&cf, slot) : 0;
                    if (slot >= 0) {
                        float cw_caret = 0.006f;
                        float ctop = y0 - (float)linei * cf.line_h;
                        vec3  cpos = vec3_make(x0 + cf.slots[slot].x + cw_caret * 0.5f,
                                               ctop - cf.line_h * 0.5f, 0.0006f);
                        mat4  cmodel = mat4_mul(face,
                                   mat4_from_trs(cpos, quat_identity(),
                                                 vec3_make(cw_caret, cf.line_h, 1.0f)));
                        cm.base_color = vec3_make(0.10f, 0.09f, 0.08f);
                        cm.emissive   = vec3_make(0.10f, 0.09f, 0.08f);
                        draw_mesh(state, state->caret_mesh, cmodel, view, proj, eye, 0.0f, cm);
                    }
                }
            }
        }

        /* doorway labels (fs-tree): each room's name — the path it represents —
           in large text above its doorway, facing outward so you read it as you
           approach. Derived from the routes, like the doorways themselves; the
           home hub is skipped (you start inside it). */
        {
            double rnow = glfwGetTime();   /* render already samples the clock (see 14861) */
            int    di, dside;
            if (state->routes_last_t == 0.0 ||
                rnow - state->routes_last_t > ROUTE_LABEL_REFRESH_S) {
                state->route_count   = route_all(&state->scene, state->routes, ROUTE_MAX);
                state->routes_last_t = rnow;
            }
            for (di = 0; di < state->route_count; di++) {
                Route *r = &state->routes[di];
                if (!r->valid) continue;
                for (dside = 0; dside < 2; dside++) {
                    sol_u32     room = dside ? r->room_hi : r->room_lo;
                    int         wall = dside ? r->wall_hi : r->wall_lo;
                    vec3        door = dside ? r->door_hi : r->door_lo;
                    const char *rt   = scene_meta_get(&state->scene, room, "room_type");
                    const char *nm   = scene_meta_get(&state->scene, room, "name");
                    float       yaw  = 0.0f, nx = 0.0f, nz = 1.0f, lpx, nw, x0;
                    mat4        dm;
                    if (rt && strcmp(rt, "home") == 0) continue;
                    if (!nm || !nm[0]) continue;
                    if      (wall == ROOM_WALL_N) { yaw = sol_radians(180.0f); nx = 0.0f;  nz = -1.0f; }
                    else if (wall == ROOM_WALL_E) { yaw = sol_radians(90.0f);  nx = 1.0f;  nz = 0.0f;  }
                    else if (wall == ROOM_WALL_S) { yaw = 0.0f;                nx = 0.0f;  nz = 1.0f;  }
                    else                          { yaw = sol_radians(-90.0f); nx = -1.0f; nz = 0.0f;  }
                    lpx = 0.35f / lh;                       /* ~35 cm tall letters */
                    text_measure(uf, nm, 1.0f, &nw, (float *)0);
                    x0  = -nw * lpx * 0.5f;                 /* centered over the opening */
                    dm  = mat4_mul(
                              mat4_translate(vec3_make(door.x + nx * 0.30f, door.y,
                                                       door.z + nz * 0.30f)),
                              quat_to_mat4(quat_from_axis_angle(
                                  vec3_make(0.0f, 1.0f, 0.0f), yaw)));
                    wtext_block(uf, vp, dm, nm, x0, ROUTE_DOOR_H + 0.2f, lpx, 0.0f,
                                0.93f, 0.91f, 0.85f);       /* warm off-white */
                }
            }
        }

        /* bookshelf labels: meta["label"] floating ABOVE the shelf, facing the
           shelf's front (+Z local), workspace-filtered like everything else.
           Seated ~2 line-heights above the top rail so books/tablets on the top
           shelf don't occlude it. Blue ink for contrast against the warm stone. */
        {
            sol_u32 bi;
            for (bi = 0; bi < state->scene.count; bi++) {
                const SceneObject *o = &state->scene.objects[bi];
                const char *lbl, *mr = o->mesh_ref;
                float lpx, nw, x0, h;
                mat4  m;
                if (!mr || strcmp(mr, "bookshelf") != 0) continue;
                if (!scene_object_active(&state->scene, o->handle)) continue;
                lbl = scene_meta_get(&state->scene, o->handle, "label");
                if (!lbl || !lbl[0]) continue;
                h   = mesh_ref_param("bookshelf", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.18f / lh;                          /* ~18 cm letters */
                text_measure(uf, lbl, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;                    /* centered on the shelf */
                m   = mat4_mul(scene_world_matrix(&state->scene, o),
                               mat4_translate(vec3_make(0.0f,
                                   h + 0.02f + 2.0f * lpx * lh, /* float 2 lines clear */
                                   0.16f)));
                wtext_block(uf, vp, m, lbl, x0, 0.0f, lpx, 0.0f, 0.225f, 0.325f, 0.5f);
            }
        }

        /* folder labels: meta["link"] (the target path) floating above each
           folderbook, workspace+page filtered like everything else. */
        {
            sol_u32 fi;
            for (fi = 0; fi < state->scene.count; fi++) {
                const SceneObject *o = &state->scene.objects[fi];
                const char *lnk, *mr = o->mesh_ref;
                float lpx, nw, x0, fh;
                mat4  m;
                if (!mr || strcmp(mr, "folderbook") != 0) continue;
                if (!scene_object_active(&state->scene, o->handle)) continue;
                lnk = scene_meta_get(&state->scene, o->handle, "link");
                if (!lnk || !lnk[0]) continue;
                fh  = mesh_ref_param("folderbook", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.135f / lh;                       /* ~13.5 cm letters (3x) */
                text_measure(uf, lnk, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
                m   = mat4_mul(scene_world_matrix(&state->scene, o),
                               mat4_translate(vec3_make(0.0f,
                                   fh + 2.0f * lpx * lh, 0.06f)));
                wtext_block(uf, vp, m, lnk, x0, 0.0f, lpx, 0.0f,
                            0.05f, 0.05f, 0.06f);        /* near-black */
            }
        }

        /* board page title: the active_page path above a board, shown in
           board view, and in the world whenever the board is off its root. */
        {
            sol_u32 ti;
            for (ti = 0; ti < state->scene.count; ti++) {
                const SceneObject *o = &state->scene.objects[ti];
                const char *ap, *mr = o->mesh_ref;
                float lpx, nw, x0, bh;
                mat4  m;
                if (!mr || strcmp(mr, "board") != 0) continue;
                if (!scene_object_active(&state->scene, o->handle)) continue;
                ap = scene_meta_get(&state->scene, o->handle, "active_page");
                if (!ap) ap = "/";
                if (state->board_view != o->handle && strcmp(ap, "/") == 0)
                    continue;                            /* plain root board: stay clean */
                bh  = mesh_ref_param("board", o->mesh_params, o->mesh_param_count, "h");
                lpx = 0.12f / lh;                        /* ~12 cm letters */
                text_measure(uf, ap, 1.0f, &nw, (float *)0);
                x0  = -nw * lpx * 0.5f;
                m   = mat4_mul(scene_world_matrix(&state->scene, o),
                               mat4_translate(vec3_make(0.0f,
                                   bh + 0.04f + 2.0f * lpx * lh, 0.04f)));
                wtext_block(uf, vp, m, ap, x0, 0.0f, lpx, 0.0f,
                            0.20f, 0.20f, 0.24f);         /* near-black ink */
            }
        }

        /* resize handles (whiteboard resize): a selected wall-mounted board
           shows a small glowing quad at each of its 4 corners. */
        if (state->selected_handle != 0 &&
            (board_is_mounted(&state->scene, state->selected_handle) ||
             note_resizable(&state->scene, state->selected_handle) ||
             picture_on_board(&state->scene, state->selected_handle) ||
             window_on_wall(&state->scene, state->selected_handle))) {
            vec3     cor[4], u, n;
            float    yaw = board_yaw(&state->scene, state->selected_handle);
            Material hm  = material_default();
            int      hk;
            if (state->resize_handle_mesh.index_count == 0) {
                MeshBuilder mb;
                mb_init(&mb);
                make_page(&mb, 0.12f, 0.12f);
                state->resize_handle_mesh = mesh_from_builder(&mb);
                mb_free(&mb);
            }
            board_world_corners(&state->scene, state->selected_handle, cor, &u);
            n  = vec3_make((float)sin((double)yaw), 0.0f, (float)cos((double)yaw));
            hm.base_color = vec3_make(1.0f, 0.85f, 0.2f);
            hm.emissive   = vec3_make(1.2f, 0.9f, 0.2f);   /* glow: reads on any wall */
            for (hk = 0; hk < 4; hk++) {
                Material chm = hm;
                mat4 m = mat4_mul(
                    mat4_translate(vec3_add(cor[hk], vec3_scale(n, 0.01f))),
                    quat_to_mat4(quat_from_axis_angle(vec3_make(0.0f,1.0f,0.0f), yaw)));
                if (hk == state->hover_corner) {           /* pointer over it: blue */
                    chm.base_color = vec3_make(0.2f, 0.5f, 1.0f);
                    chm.emissive   = vec3_make(0.3f, 0.6f, 1.5f);
                }
                draw_mesh(state, state->resize_handle_mesh, m, view, proj, eye, 0.0f, chm);
            }
        }
    }
    {   /* P9 perf #2 measure: snapshot the world-text section's cost */
        int  tb, tu, tm;
        long tc, tg;
        text_shape_stats_get(&tc, &tg);
        wtext_stats_get(&tb, &tu, &tm);
        state->t_text_shape_calls  = tc;
        state->t_text_shape_glyphs = tg;
        state->t_text_blocks       = tb;
        state->t_text_uploads      = tu;
        state->t_text_misses       = tm;
        state->t_text_ms = (float)((glfwGetTime() - t_text0) * 1000.0);
    }

    /* P9 item 2: stained glass — the church_glass + window_glass objects,
       translucent, drawn AFTER all opaque (tests their depth), back-to-front
       by distance, into the HDR buffer (so it blooms + tonemaps + grades). One
       mesh per church; the object-level sort orders multiple churches. */
    {
        sol_u32 gidx[GLASS_DRAW_MAX];
        float   gdist[GLASS_DRAW_MAX];
        int     gn = 0, ga, gb;
        for (i = 0; i < state->scene.count && gn < GLASS_DRAW_MAX; i++) {
            const SceneObject *o = &state->scene.objects[i];
            mat4  gm;
            float dx, dy, dz;
            if (o->mesh.index_count == 0) continue;
            if (!o->mesh_ref ||
                (strcmp(o->mesh_ref, "church_glass") != 0 &&
                 strcmp(o->mesh_ref, "window_glass") != 0)) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
            if (vis && !vis[o->handle]) continue;
            gm = scene_world_matrix(&state->scene, o);
            dx = gm.m[12] - eye.x; dy = gm.m[13] - eye.y; dz = gm.m[14] - eye.z;
            gidx[gn]  = (sol_u32)i;
            gdist[gn] = dx * dx + dy * dy + dz * dz;
            gn++;
        }
        for (ga = 1; ga < gn; ga++) {             /* insertion sort: far -> near */
            sol_u32 ki = gidx[ga];
            float   kd = gdist[ga];
            gb = ga - 1;
            while (gb >= 0 && gdist[gb] < kd) {
                gidx[gb + 1] = gidx[gb]; gdist[gb + 1] = gdist[gb]; gb--;
            }
            gidx[gb + 1] = ki; gdist[gb + 1] = kd;
        }
        for (ga = 0; ga < gn; ga++) {
            const SceneObject *o     = &state->scene.objects[gidx[ga]];
            mat4               model = scene_world_matrix(&state->scene, o);
            Material           dm    = o->material;
            dm.emissive.x *= o->overlay_glow;     /* the glass breathes like the sconces */
            dm.emissive.y *= o->overlay_glow;
            dm.emissive.z *= o->overlay_glow;
            draw_glass(state, o->mesh, model, view, proj, eye, dm);
        }
    }

    /* the place-mode ghost: the catalog item previewed translucent at the
       ground-aim point, spun by place_yaw. Reuses the glass (alpha) path. */
    if (state->place_active && state->place_ghost.index_count > 0) {
        vec3     gp = carry_place_point(state);          /* camera-aim ground point */
        mat4     model = mat4_mul(mat4_translate(gp),
                            quat_to_mat4(quat_from_axis_angle(
                                vec3_make(0.0f, 1.0f, 0.0f), state->place_yaw)));
        Material gm = material_default();
        gm.base_color = vec3_make(0.55f, 0.62f, 0.78f);  /* cool preview tint */
        draw_glass(state, state->place_ghost, model, view, proj, eye, gm);
    }

    /* P9 item 3: weathering decals — the church_decals objects, unlit textured
       alpha quads modulating the lit walls (stains darken, moss tints), into
       the HDR buffer (so they bloom + grade). After opaque, depth-tested. */
    if (state->decal_pipeline.id && state->decal_atlas.id) {
        sol_u32 di;
        rhi_set_pipeline(state->decal_pipeline);
        rhi_bind_texture(state->decal_atlas, 0);
        rhi_set_uniform_int ("uDecalAtlas", 0);
        rhi_set_uniform_mat4("uView", view.m);
        rhi_set_uniform_mat4("uProj", proj.m);
        for (di = 0; di < state->scene.count; di++) {
            const SceneObject *o = &state->scene.objects[di];
            mat4 model;
            if (o->mesh.index_count == 0) continue;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "church_decals") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
            if (vis && !vis[o->handle]) continue;
            model = scene_world_matrix(&state->scene, o);
            rhi_set_uniform_mat4("uModel", model.m);
            rhi_bind_vertex_buffer(o->mesh.vbuffer);
            rhi_bind_index_buffer(o->mesh.ibuffer);
            rhi_draw_indexed(0, o->mesh.index_count);
        }
    }

    /* Portal Material: the energy pane — one shared unit quad, drawn per
       active portal scaled to its opening, with the procedural swirl shader
       (uTime-animated). Opaque, in the HDR pass so it blooms. */
    if (state->portal_pipeline.id) {
        sol_u32 pi;
        if (state->gate_pane.index_count == 0) {          /* lazy-build the quad */
            MeshBuilder mb;
            mb_init(&mb);
            make_page(&mb, 1.0f, 1.0f);                   /* unit XY quad, 0..1 UV */
            state->gate_pane = mesh_from_builder(&mb);
            mb_free(&mb);
        }
        rhi_set_pipeline(state->portal_pipeline);
        rhi_set_uniform_mat4 ("uView", view.m);
        rhi_set_uniform_mat4 ("uProj", proj.m);
        rhi_set_uniform_float("uTime", (float)glfwGetTime());
        rhi_set_uniform_vec3 ("uPortalColor", 0.20f, 0.45f, 0.95f);
        for (pi = 0; pi < state->scene.count; pi++) {
            const SceneObject *o = &state->scene.objects[pi];
            float w, h, pw, oh;
            mat4  model;
            if (o->kind != KIND_PORTAL) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;  /* workspace filter */
            if (vis && !vis[o->handle]) continue;                          /* frustum cull */
            w  = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "w");
            h  = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "h");
            pw = mesh_ref_param("gate", o->mesh_params, o->mesh_param_count, "post");
            oh = h - pw;                                   /* opening height */
            model = mat4_mul(scene_world_matrix(&state->scene, o),
                      mat4_mul(mat4_translate(vec3_make(0.0f, oh * 0.5f, 0.0f)),
                               mat4_scale(vec3_make(w, oh, 1.0f))));
            rhi_set_uniform_mat4("uModel", model.m);
            rhi_bind_vertex_buffer(state->gate_pane.vbuffer);
            rhi_bind_index_buffer (state->gate_pane.ibuffer);
            rhi_draw_indexed(0, state->gate_pane.index_count);
        }
    }

    /* particles (P4 item 7): LAST in the HDR pass — additive over whatever
       the frame built, depth-tested against it, writing no depth of their
       own. The whole pool is one instanced draw; the fill is arithmetic
       (particles.c), the upload is an orphaning stream write. rgb > 1 on a
       spark feeds the bloom extract exactly like an emissive surface. */
    /* the pond surfaces (P7 item 8): alpha-blended, drawn after the
       opaque scene — reflection from the prefilter IBL, ripples scrolled,
       the rim dissolving into the shore. The scene pass skipped these. */
    if (state->water_pipeline.id && state->water_ripple.id) {
        sol_u32 wi;
        for (wi = 0; wi < state->scene.count; wi++) {
            SceneObject *o = &state->scene.objects[wi];
            mat4  model;
            float r, depth, alpha;
            if (!o->mesh_ref || strcmp(o->mesh_ref, "pond") != 0) continue;
            if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
            if (o->mesh.index_count == 0) continue;
            if (vis && !vis[o->handle]) continue;
            r     = mesh_ref_param("pond", o->mesh_params, o->mesh_param_count, "r");
            depth = mesh_ref_param("pond", o->mesh_params, o->mesh_param_count, "depth");
            alpha = 0.55f + 0.12f * (depth > 3.0f ? 3.0f : depth);  /* deeper = more opaque */
            model = scene_world_matrix(&state->scene, o);
            rhi_set_pipeline(state->water_pipeline);
            rhi_set_uniform_mat4("uModel", model.m);
            rhi_set_uniform_mat4("uView", view.m);
            rhi_set_uniform_mat4("uProj", proj.m);
            rhi_set_uniform_vec3("uViewPos", eye.x, eye.y, eye.z);
            rhi_set_uniform_float("uTime", (float)glfwGetTime());
            rhi_set_uniform_vec3("uWaterTint", 0.015f, 0.055f, 0.075f);
            rhi_set_uniform_float("uWaterAlpha", alpha);
            rhi_set_uniform_float("uPondR", r);
            rhi_set_uniform_float("uRippleStr", 0.5f);
            {   /* a fixed soft glint direction — a stylized sun/moon
                   glitter on the ripples (v1; sconce glints flagged) */
                vec3 ld = vec3_normalize(vec3_make(-0.4f, -0.7f, -0.5f));
                rhi_set_uniform_vec3("uLightDir", ld.x, ld.y, ld.z);
                rhi_set_uniform_vec3("uLightColor", 0.55f, 0.55f, 0.6f);
            }
            rhi_bind_texture(state->water_ripple, 0);
            rhi_set_uniform_int("uRippleTex", 0);
            if (state->prefilter_cubemap.id) {
                rhi_bind_texture(state->prefilter_cubemap, 6);
                rhi_set_uniform_int("uPrefilterMap", 6);
            }
            rhi_bind_vertex_buffer(o->mesh.vbuffer);
            rhi_bind_index_buffer(o->mesh.ibuffer);
            rhi_draw_indexed(0, o->mesh.index_count);
            state->draws_done++;
        }
    }

    /* soft particles (P8 item 4): close the HDR pass so its depth is readable,
       copy it, then re-open (CLEAR_NONE loads color+depth) for the particles —
       they still hardware-occlude and still composite before bloom; they just
       also sample the copy to fade as they near geometry. */
    rhi_end_pass();
    rhi_begin_pass(state->depthcopy_rt, RHI_CLEAR_COLOR, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_set_pipeline(state->copy_pipeline);
    rhi_bind_texture(rhi_render_target_depth_texture(state->hdr_rt), 0);
    rhi_set_uniform_int("uDepth", 0);
    rhi_draw(0, 3);
    rhi_end_pass();
    rhi_begin_pass(state->hdr_rt, RHI_CLEAR_NONE, 0.0f, 0.0f, 0.0f, 1.0f);

    {
        static float part_data[PARTICLE_CAP * PARTICLE_INST_FLOATS];
        int n = particles_fill(&state->particles, part_data, PARTICLE_CAP);
        state->part_count = n;
        if (n > 0 && state->part_pipeline.id) {
            rhi_set_pipeline(state->part_pipeline);
            rhi_set_uniform_mat4("uView", view.m);
            rhi_set_uniform_mat4("uProj", proj.m);
            rhi_bind_texture(rhi_render_target_texture(state->depthcopy_rt), 0);
            rhi_set_uniform_int("uSceneDepth", 0);
            rhi_set_uniform_float("uNear", 0.1f);    /* must match camera_proj's near/far */
            rhi_set_uniform_float("uFar", 100.0f);
            rhi_update_buffer(state->part_inst, part_data,
                              (size_t)n * PARTICLE_INST_FLOATS * sizeof(float));
            rhi_bind_vertex_buffer(state->part_vbuf);
            rhi_bind_index_buffer(state->part_ibuf);
            rhi_bind_instance_buffer(state->part_inst);
            rhi_draw_indexed_instanced(0, 6, n);
        }
    }

    rhi_end_pass();

    /* ---- pass 2: fullscreen pass to the window — sample the HDR buffer, encode
       linear->sRGB (7c adds the tonemap). One triangle, no vertex buffer. ---- */
    {
        RhiRenderTarget screen = {0};   /* {0} = default framebuffer (declaration init, not a compound literal) */
        rt2 = glfwGetTime();
        yardstick_ms(&state->t_hdr, rt2 - rt1);

        /* ---- god-rays (P8 item 3): half-res raymarch of the SUN's cascaded
           shadow volume into godray_rt; reads the scene depth (stop at geometry)
           and both cascade maps (lit/shadowed). Added in post like a bloom
           level. Now directional (P8 item 6): the rays rake the whole island
           instead of a 20m cone — the §1.4 payoff (improve the author, every
           reader benefits). ---- */
        {
            mat4 grivp = mat4_inverse(mat4_mul(proj, view));
            vec3 grld  = vec3_normalize(vec3_sub(state->light_target, state->light_pos));
            rhi_begin_pass(state->godray_rt, RHI_CLEAR_COLOR, 0.0f, 0.0f, 0.0f, 1.0f);
            rhi_set_pipeline(state->godray_pipeline);
            rhi_bind_texture(rhi_render_target_depth_texture(state->hdr_rt), 0);
            rhi_set_uniform_int("uDepth", 0);
            rhi_bind_texture(rhi_render_target_depth_texture(state->shadow_rt[0]), 1);
            rhi_set_uniform_int("uShadowMap", 1);
            rhi_bind_texture(rhi_render_target_depth_texture(state->shadow_rt[1]), 8);
            rhi_set_uniform_int("uShadowMap1", 8);
            rhi_set_uniform_mat4("uInvViewProj", grivp.m);
            rhi_set_uniform_mat4("uLightVP",  state->cascade_vp[0].m);
            rhi_set_uniform_mat4("uLightVP1", state->cascade_vp[1].m);
            rhi_set_uniform_vec3("uCamPos", state->camera.pos.x,
                                 state->camera.pos.y, state->camera.pos.z);
            rhi_set_uniform_vec3("uLightColor", state->light_color.x,
                                 state->light_color.y, state->light_color.z);
            rhi_set_uniform_vec3("uLightDir", grld.x, grld.y, grld.z);
            rhi_set_uniform_float("uFogDensity",  FOG_DENSITY);
            rhi_set_uniform_float("uFogHeight",   FOG_HEIGHT);
            rhi_set_uniform_float("uFogFalloff",  FOG_FALLOFF);
            rhi_set_uniform_float("uGodrayIntensity", GODRAY_INTENSITY);
            rhi_draw(0, 3);
            rhi_end_pass();
        }

        /* ---- SSAO (P8 item 5): a half-res occlusion factor + a bilateral
           blur, both reading the scene depth like the god-ray pass ---- */
        {
            mat4 ssivp = mat4_inverse(proj);
            rhi_begin_pass(state->ssao_rt, RHI_CLEAR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
            rhi_set_pipeline(state->ssao_pipeline);
            rhi_bind_texture(rhi_render_target_depth_texture(state->hdr_rt), 0);
            rhi_set_uniform_int("uDepth", 0);
            rhi_set_uniform_mat4("uProj", proj.m);
            rhi_set_uniform_mat4("uInvProj", ssivp.m);
            rhi_set_uniform_float("uRadius", 0.5f);
            rhi_set_uniform_float("uBias", 0.03f);
            rhi_set_uniform_float("uStrength", 1.4f);
            rhi_draw(0, 3);
            rhi_end_pass();

            rhi_begin_pass(state->ssao_blur_rt, RHI_CLEAR_COLOR, 1.0f, 1.0f, 1.0f, 1.0f);
            rhi_set_pipeline(state->ssao_blur_pipeline);
            rhi_bind_texture(rhi_render_target_texture(state->ssao_rt), 0);
            rhi_set_uniform_int("uAO", 0);
            rhi_bind_texture(rhi_render_target_depth_texture(state->hdr_rt), 1);
            rhi_set_uniform_int("uDepth", 1);
            rhi_set_uniform_float("uNear", 0.1f);   /* match camera_proj */
            rhi_set_uniform_float("uFar", 100.0f);
            rhi_draw(0, 3);
            rhi_end_pass();
        }

        /* ---- the bloom chain (P4 item 5): extract, walk down, walk up ---- */
        if (state->bloom_on && state->bloom_up_pipeline.id) {
            int lv;
            rhi_begin_pass(state->bloom_rt[0], RHI_CLEAR_COLOR, 0.0f, 0.0f, 0.0f, 1.0f);
            rhi_set_pipeline(state->bloom_extract_pipeline);
            rhi_bind_texture(rhi_render_target_texture(state->hdr_rt), 0);
            rhi_set_uniform_int("uSrc", 0);
            rhi_draw(0, 3);
            rhi_end_pass();
            for (lv = 1; lv < BLOOM_LEVELS; lv++) {
                rhi_begin_pass(state->bloom_rt[lv], RHI_CLEAR_COLOR, 0.0f, 0.0f, 0.0f, 1.0f);
                rhi_set_pipeline(state->bloom_down_pipeline);
                rhi_bind_texture(rhi_render_target_texture(state->bloom_rt[lv - 1]), 0);
                rhi_set_uniform_int("uSrc", 0);
                rhi_set_uniform_vec3("uTexel", 1.0f / (float)state->bloom_w[lv - 1],
                                               1.0f / (float)state->bloom_h[lv - 1], 0.0f);
                rhi_draw(0, 3);
                rhi_end_pass();
            }
            for (lv = BLOOM_LEVELS - 2; lv >= 0; lv--) {
                rhi_begin_pass(state->bloom_rt[lv], RHI_CLEAR_NONE, 0.0f, 0.0f, 0.0f, 1.0f);
                rhi_set_pipeline(state->bloom_up_pipeline);    /* blend = ADD */
                rhi_bind_texture(rhi_render_target_texture(state->bloom_rt[lv + 1]), 0);
                rhi_set_uniform_int("uSrc", 0);
                rhi_set_uniform_vec3("uTexel", 1.0f / (float)state->bloom_w[lv + 1],
                                               1.0f / (float)state->bloom_h[lv + 1], 0.0f);
                rhi_draw(0, 3);
                rhi_end_pass();
            }
        }

        rhi_begin_pass(screen, RHI_CLEAR_ALL, 0.0f, 0.0f, 0.0f, 1.0f);
        if (state->show_shadow_map) {
            /* inspector: cascade 0 (the near map) as grayscale. Ortho depth is
               already linear; the debug shader's perspective linearize just
               makes a monotonic debug view (P8 item 6). */
            rhi_set_pipeline(state->shadow_debug_pipeline);
            rhi_bind_texture(rhi_render_target_depth_texture(state->shadow_rt[0]), 0);
            rhi_set_uniform_int("uDepth", 0);
            rhi_set_uniform_float("uNear", 1.0f);
            rhi_set_uniform_float("uFar",  SHADOW_DIST);
        } else {
            rhi_set_pipeline(state->post_pipeline);
            rhi_bind_texture(rhi_render_target_texture(state->hdr_rt), 0);
            rhi_bind_texture(rhi_render_target_texture(state->bloom_rt[0]), 1);
            rhi_bind_texture(rhi_render_target_texture(state->godray_rt), 3);
            rhi_bind_texture(rhi_render_target_texture(state->ssao_blur_rt), 4);
            rhi_set_uniform_int("uHdr", 0);             /* sampler -> texture unit 0 */
            rhi_set_uniform_int("uBloom", 1);
            rhi_set_uniform_int("uGodray", 3);          /* P8 item 3: the shafts buffer */
            rhi_set_uniform_int("uAO", 4);              /* P8 item 5: ambient occlusion */
            rhi_set_uniform_float("uBloomStrength",
                                  state->bloom_on ? 0.06f : 0.0f);
            rhi_set_uniform_float("uExposure", state->exposure);
            {   /* atmospheric fog (P8 item 2): the post pass reads the now-
                   samplable scene depth, reconstructs world position, and fogs
                   by distance and height toward a horizon color. Linear, pre-
                   tonemap (aerial perspective). uFogDensity 0 = today's image. */
                mat4 ivp = mat4_inverse(mat4_mul(proj, view));
                rhi_bind_texture(rhi_render_target_depth_texture(state->hdr_rt), 2);
                rhi_set_uniform_int("uDepth", 2);
                rhi_set_uniform_mat4("uInvViewProj", ivp.m);
                rhi_set_uniform_vec3("uCamPos", state->camera.pos.x,
                                     state->camera.pos.y, state->camera.pos.z);
                rhi_set_uniform_vec3("uAerialColor", 0.46f, 0.56f, 0.66f);
                rhi_set_uniform_float("uFogDensity", FOG_DENSITY);
                rhi_set_uniform_float("uFogHeight",  FOG_HEIGHT);
                rhi_set_uniform_float("uFogFalloff", FOG_FALLOFF);
            }
            {   /* under-water tint (item 8): the camera below a pond's
                   surface, within its disc — a cool fog sells the wade */
                float fr = 0.0f, fg = 0.0f, fb = 0.0f, fa = 0.0f;
                sol_u32 wi;
                for (wi = 0; wi < state->scene.count; wi++) {
                    SceneObject *o = &state->scene.objects[wi];
                    mat4  m;
                    float r, dx, dz;
                    if (!o->mesh_ref || strcmp(o->mesh_ref, "pond") != 0) continue;
                    if (!scene_object_active(&state->scene, o->handle)) continue;   /* hidden workspace */
                    m = scene_world_matrix(&state->scene, o);
                    if (state->camera.pos.y >= m.m[13]) continue;   /* above the surface */
                    r  = mesh_ref_param("pond", o->mesh_params, o->mesh_param_count, "r");
                    dx = state->camera.pos.x - m.m[12];
                    dz = state->camera.pos.z - m.m[14];
                    if (dx * dx + dz * dz <= r * r) {
                        fr = 0.04f; fg = 0.10f; fb = 0.14f; fa = 0.72f;
                        break;
                    }
                }
                rhi_set_uniform_vec3("uFogColor", fr, fg, fb);
                rhi_set_uniform_float("uFogStrength", fa);
            }
            {   /* P9 item 1: color grade — a display-referred preset (mode 0 =
                   neutral = today's image, exact). '9' cycles. */
                const GradePreset *gp = &GRADE_PRESETS[state->grade_mode % GRADE_PRESET_COUNT];
                rhi_set_uniform_vec3 ("uGradeTint", gp->tint_r, gp->tint_g, gp->tint_b);
                rhi_set_uniform_float("uGradeContrast",    gp->contrast);
                rhi_set_uniform_float("uGradeSaturation",  gp->saturation);
                rhi_set_uniform_float("uVignetteStrength", gp->vig_strength);
                rhi_set_uniform_float("uVignetteRadius",   gp->vig_radius);
                rhi_bind_texture(state->grade_luts[gp->lut], 5);   /* P9 item 1: the LUT */
                rhi_set_uniform_int  ("uLut", 5);
                rhi_set_uniform_float("uLutMix", gp->lut_mix);
            }
        }
        rhi_draw(0, 3);
    }

    /* ---- pass 3: the 2D overlay (item 2) — display-referred sRGB colors
       composited OVER the tonemapped image in a no-clear (load) pass. After
       the tonemap on purpose: ACES would roll UI white down to ~0.8 gray.
       SIZES are multiplied by `us` (fb_height / the 1080 the 960x540 retina
       window opens at) so the HUD keeps its proportions when the window
       grows — fixed pixels stay the same physical size while everything
       else scales (the DPI-relative unit family); POSITIONS keep their
       px / vw / vh anchors. ---- */
    ui_begin(state->fb_width, state->fb_height);
    us = (float)state->fb_height / 1080.0f;

    /* the debug readout panel — live state, monospace-aligned (3c; this
       retired the window-title sprintf hack). ui_text positions BASELINES.
       ':' "Toggle stats card" hides it. */
    if (state->show_hud) {
    ui_quad(8.0f * us, 8.0f * us, 340.0f * us, 212.0f * us, 0.05f, 0.07f, 0.10f, 0.55f);
    ui_quad_outline(8.0f * us, 8.0f * us, 340.0f * us, 212.0f * us,
                    1.0f * us, 0.95f, 0.80f, 0.45f, 0.9f);
    if (state->ui_font) {
        float ts   = 0.45f * us;
        float base = 14.0f * us + font_ascent(state->ui_font) * ts;
        ui_text(state->ui_font, "solarium", 20.0f * us, base, ts,
                0.95f, 0.80f, 0.45f, 1.0f);
        if (state->mono_font) {
            char        line[96];
            const char *mode = state->camera.mode == CAMERA_ORBIT ? "orbit"
                             : state->camera.mode == CAMERA_FLY   ? "fly  " : "walk ";
            float       ms = 0.36f * us;
            float       mb = base + font_line_height(state->ui_font) * ts;
            SceneObject *sel = scene_get(&state->scene, state->selected_handle);

            /* C89 has no snprintf; the bounded buffer is checked by eye and
               the macOS deprecation nag is silenced locally (the shelved fix) */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            sprintf(line, "cam %s%s  exposure %.2f", mode,
                    state->ghost ? " GHOST" : "", (double)state->exposure);
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 1.0f, 1.0f, 1.0f, 0.85f);
            mb += font_line_height(state->mono_font) * ms;
            {
                const char *rn = state->current_room
                    ? scene_meta_get(&state->scene, state->current_room, "name") : (const char *)0;
                if (!rn && !state->current_room && state->current_terrain)
                    rn = scene_meta_get(&state->scene, state->current_terrain,
                                        "name");           /* "on the heath" */
                sprintf(line, "%s %s",
                        (!state->current_room && state->current_terrain) ? "on" : "room",
                        rn ? rn
                           : ((state->current_room || state->current_terrain)
                                  ? "(unnamed)" : "outside"));
                ui_text(state->mono_font, line, 20.0f * us, mb, ms, 0.80f, 0.85f, 1.0f, 0.85f);
                mb += font_line_height(state->mono_font) * ms;
            }
            if (sel) {
                sprintf(line, "sel #%-3u %-8s %.1f %.1f %.1f",
                        (unsigned)state->selected_handle,
                        sel->mesh_ref ? sel->mesh_ref : "(import)",
                        (double)sel->pos.x, (double)sel->pos.y, (double)sel->pos.z);
            } else {
                sprintf(line, "sel none");
            }
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 1.0f, 1.0f, 1.0f, 0.85f);
            mb += font_line_height(state->mono_font) * ms;

            /* the yardstick (P4 item 2): frame = true period (vsync-pinned,
               the documented swap cadence); cpu = everything we control;
               the sub-times are per-pass ENCODE cost, where culling and
               instancing wins will actually show. */
            if (state->t_frame_gpu > 0.0f)
                sprintf(line, "frame %4.1f cpu %4.2f gpu %4.1f swap %4.1f",
                        (double)state->t_frame, (double)state->t_cpu,
                        (double)state->t_frame_gpu, (double)state->t_swap);
            else
                sprintf(line, "frame %4.1f cpu %4.2f swap %4.1f",
                        (double)state->t_frame, (double)state->t_cpu,
                        (double)state->t_swap);
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 0.70f, 0.90f, 0.70f, 0.85f);
            mb += font_line_height(state->mono_font) * ms;
            sprintf(line, "up %4.2f shadow %4.2f spot %4.2f hdr %4.2f",
                    (double)state->t_update, (double)state->t_shadow,
                    (double)state->t_spot_shadow, (double)state->t_hdr);
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 0.70f, 0.90f, 0.70f, 0.85f);
            mb += font_line_height(state->mono_font) * ms;
            sprintf(line, "post %4.2f draws %d/%d parts %d",
                    (double)state->t_post,
                    state->draws_done, state->draws_total,
                    state->part_count);    /* §1.7: the pool's live count —
                                              thousands at one draw, says the
                                              acceptance; this is the proof */
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 0.70f, 0.90f, 0.70f, 0.85f);
            mb += font_line_height(state->mono_font) * ms;

            /* the registry's instruments (P4 item 4): entries alive and refs
               held — the L-reload acceptance is these NOT moving */
            sprintf(line, "assets %dm %dt %dx (%d refs) snd %d",
                    asset_live_count(&g_mesh_assets)
                        + asset_live_count(&g_glbpart_assets),
                    asset_live_count(&g_tex_assets),
                    asset_live_count(&g_texgen_assets),
                    asset_ref_total(&g_mesh_assets)
                        + asset_ref_total(&g_glbpart_assets)
                        + asset_ref_total(&g_tex_assets)
                        + asset_ref_total(&g_texgen_assets),
                    g_loop_voices);    /* loops alive: wind + crackling flames */
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 0.70f, 0.90f, 0.70f, 0.85f);
            mb += font_line_height(state->mono_font) * ms;
            sprintf(line, "text %dblk %ldgly %dup %dmiss %4.2fms",
                    state->t_text_blocks, state->t_text_shape_glyphs,
                    state->t_text_uploads, state->t_text_misses,
                    (double)state->t_text_ms);
#ifdef __clang__
#pragma clang diagnostic pop
#endif
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 0.90f, 0.82f, 0.70f, 0.85f);
        }
    }
    }   /* show_hud */

    if (state->marquee_active && state->marquee_dragging) {
        float s  = state->marquee_px_scale > 0.0f ? state->marquee_px_scale : 1.0f;
        float rx = s * (float)(state->marquee_x0 < state->marquee_x1 ?
                           state->marquee_x0 : state->marquee_x1);
        float ry = s * (float)(state->marquee_y0 < state->marquee_y1 ?
                           state->marquee_y0 : state->marquee_y1);
        float rw = s * (float)(state->marquee_x0 < state->marquee_x1 ?
                           state->marquee_x1 - state->marquee_x0 :
                           state->marquee_x0 - state->marquee_x1);
        float rh = s * (float)(state->marquee_y0 < state->marquee_y1 ?
                           state->marquee_y1 - state->marquee_y0 :
                           state->marquee_y0 - state->marquee_y1);
        ui_quad(rx, ry, rw, rh, 0.45f, 0.62f, 0.95f, 0.18f);            /* fill */
        ui_quad_outline(rx, ry, rw, rh, 1.5f, 0.55f, 0.72f, 1.0f, 0.9f);/* border */
    }

    /* first-person crosshair at the pick point (screen centre, via the
       viewport-relative units) — orbit mode picks at the cursor instead, and
       board view hides it (you point with the free cursor, not the crosshair) */
    if (state->camera.mode != CAMERA_ORBIT && state->board_view == 0) {
        float cx = ui_vw(50.0f), cy = ui_vh(50.0f);
        ui_line(cx - 9.0f * us, cy, cx + 9.0f * us, cy, 1.5f * us, 1.0f, 1.0f, 1.0f, 0.7f);
        ui_line(cx, cy - 9.0f * us, cx, cy + 9.0f * us, 1.5f * us, 1.0f, 1.0f, 1.0f, 0.7f);
    }

    /* 'T' mode 1: the font-atlas inspector (3a) — the raw SDF atlas as a
       textured quad. Glyphs read as soft white shapes (you are looking at
       DISTANCE, not coverage; the text shader's threshold sharpens them). */
    if (state->text_inspect == 1 && state->ui_font) {
        float side = ui_vh(85.0f);
        float ax   = ui_vw(50.0f) - side * 0.5f;
        float ay   = ui_vh(50.0f) - side * 0.5f;
        ui_quad(ax - 8.0f, ay - 8.0f, side + 16.0f, side + 16.0f,
                0.0f, 0.0f, 0.0f, 0.85f);                  /* backdrop */
        ui_textured_quad(font_atlas(state->ui_font), ax, ay, side, side);
        ui_quad_outline(ax - 8.0f, ay - 8.0f, side + 16.0f, side + 16.0f,
                        1.0f, 0.95f, 0.80f, 0.45f, 0.9f);
    }

    /* 'T' mode 2: the type specimen (3b acceptance) — the SAME fuzzy atlas
       drawn through the SDF threshold, at scales from huge to small. Every
       size is the one 48px bake; sharpness at 4x is the proof. */
    if (state->text_inspect == 2 && state->ui_font) {
        static const float scales[5] = { 4.0f, 2.0f, 1.0f, 0.5f, 0.33f };
        static const char *latin = "Sphinx of black quartz, judge my vow.";
        static const char *greek =
            "\xce\x9e\xce\xb5\xcf\x83\xce\xba\xce\xb5\xcf\x80\xce\xac\xce\xb6"
            "\xcf\x89 \xcf\x84\xce\xb7\xce\xbd \xcf\x88\xcf\x85\xcf\x87\xce\xbf"
            "\xcf\x86\xce\xb8\xcf\x8c\xcf\x81\xce\xb1 \xce\xb2\xce\xb4\xce\xb5"
            "\xce\xbb\xcf\x85\xce\xb3\xce\xbc\xce\xaf\xce\xb1";   /* Greek pangram */
        float yy = ui_vh(8.0f);
        int   k;
        ui_quad(0.0f, 0.0f, ui_vw(100.0f), ui_vh(100.0f), 0.0f, 0.0f, 0.0f, 0.88f);
        for (k = 0; k < 5; k++) {
            float s = scales[k] * us;
            yy += font_ascent(state->ui_font) * s;
            ui_text(state->ui_font, k < 2 ? "Ag \xce\xa9\xce\xbe" : latin,
                    ui_vw(6.0f), yy, s, 1.0f, 1.0f, 1.0f, 1.0f);
            if (k >= 2) {
                yy += font_line_height(state->ui_font) * s;
                ui_text(state->ui_font, greek, ui_vw(6.0f), yy, s,
                        0.95f, 0.80f, 0.45f, 1.0f);
            }
            yy += (font_line_height(state->ui_font) - font_ascent(state->ui_font)) * s + 14.0f * us;
        }

        /* monospace: code wants every advance identical (alignment is the
           typeface's job, not the layout's) */
        if (state->mono_font) {
            float cs = 0.55f * us;
            yy += font_ascent(state->mono_font) * cs;
            ui_text(state->mono_font,
                    "if (x < 10 && y > 5) { return SOL_TRUE; }   /* mono */",
                    ui_vw(6.0f), yy, cs, 0.65f, 0.95f, 0.65f, 1.0f);
            yy += (font_line_height(state->mono_font) - font_ascent(state->mono_font)) * cs
                + 14.0f * us;
        }

        /* wrap-to-width (3c acceptance): greedy word wrap against the white
           guide line's width — resize the window and the breaks move */
        {
            float ws = 0.5f * us;
            float wrap_w = ui_vw(55.0f);
            yy += 6.0f * us;
            ui_line(ui_vw(6.0f), yy, ui_vw(6.0f) + wrap_w, yy,
                    1.0f * us, 1.0f, 1.0f, 1.0f, 0.35f);
            ui_text_wrapped(state->ui_font,
                    "The palace must reload exactly, or object permanence is "
                    "a lie. Geometry is reconstructed by reference, text is "
                    "thresholded distance, and every line you are reading "
                    "broke against the guide above at a word boundary.",
                    ui_vw(6.0f), yy + 8.0f * us + font_ascent(state->ui_font) * ws,
                    ws, wrap_w, 1.0f, 1.0f, 1.0f, 0.95f);
        }
    }

    /* a billboarded mark pinned to the selected object: its WORLD position is
       projected through the same view-proj the scene used, then drawn in
       screen space — constant pixel size, always crisp, faces the camera by
       construction. Culled when behind the camera (w<=0: see
       mat4_project_point). The tag names the object (item 6: a FILE card's
       tag is its filename — the seat item 2 reserved, filled at last). */
    if (state->selected_handle != 0 && state->board_view == 0 &&
        scene_get(&state->scene, state->selected_handle)) {
        vec3 world = object_world_pos(&state->scene, state->selected_handle);
        vec3 ndc;
        if (mat4_project_point(mat4_mul(proj, view), world, &ndc)) {
            char        lbuf[16];
            const char *label = object_label(&state->scene, state->selected_handle, lbuf);
            float ts2 = 0.32f * us;
            float text_w = 0.0f;
            float sx = (ndc.x * 0.5f + 0.5f) * (float)state->fb_width;
            float sy = (0.5f - ndc.y * 0.5f) * (float)state->fb_height;   /* NDC y-up -> UI y-down */
            float tw, th = 20.0f * us, tx, ty;
            if (state->ui_font)
                text_measure(state->ui_font, label, ts2, &text_w, (float *)0);
            tw = text_w + 16.0f * us;                    /* the tag fits its name */
            if (tw < 48.0f * us) tw = 48.0f * us;
            tx = sx - tw * 0.5f;
            ty = sy - 46.0f * us;
            ui_line(sx, sy, sx, ty + th, 1.5f * us, 0.95f, 0.80f, 0.45f, 0.9f);   /* leader */
            ui_quad(tx, ty, tw, th, 0.05f, 0.07f, 0.10f, 0.75f);
            ui_quad_outline(tx, ty, tw, th, 1.0f * us, 0.95f, 0.80f, 0.45f, 0.9f);
            if (state->ui_font)
                ui_text(state->ui_font, label, sx - text_w * 0.5f,
                        ty + 14.5f * us, ts2, 1.0f, 1.0f, 1.0f, 0.95f);
        }
    }
    inventory_draw_overlay(state);
    palette_draw(&state->palette, state, state->mono_font,
                 g_commands, G_COMMAND_COUNT, state->fb_width, state->fb_height);
    editor_draw_overlay(state);
    ui_end();
    yardstick_ms(&state->t_post, glfwGetTime() - rt2);
}

/* ---- note editing (item 8 piece 5) ---- */

static int edit_sel_lo(const AppState *st) {
    return st->edit_cursor < st->edit_sel_anchor ? st->edit_cursor : st->edit_sel_anchor;
}
static int edit_sel_hi(const AppState *st) {
    return st->edit_cursor > st->edit_sel_anchor ? st->edit_cursor : st->edit_sel_anchor;
}
static int edit_has_sel(const AppState *st) {
    return st->edit_cursor != st->edit_sel_anchor;
}

/* Build the note's caret field: wrap exactly as the renderer does, recover
   source offsets, gather per-char advances from the font, assemble (pure). */
static int caret_build(const Font *f, const char *src, float px2m, float wrap_w,
                       CaretField *out) {
    char  wrapped[CARET_MAX_SLOTS];
    int   map[CARET_MAX_SLOTS];
    float adv[CARET_MAX_SLOTS];
    int   wlen, wi, prevg, sgi;
    float space_adv;
    const FontGlyph *sg;
    text_wrap(f, src, px2m, wrap_w, wrapped, WT_WRAP_CAP);
    wlen  = caret_reconcile(src, wrapped, map, CARET_MAX_SLOTS);
    prevg = 0;
    wi    = 0;
    while (wi < wlen) {
        unsigned long    cp;
        int              n = caret_cplen((unsigned char)wrapped[wi]);
        int              gi, k;
        const FontGlyph *g;
        for (k = 0; k < n && wi + k < wlen; k++) adv[wi + k] = 0.0f;
        if (wrapped[wi] == '\n') { prevg = 0; wi += n; continue; }
        cp = (unsigned long)(unsigned char)wrapped[wi];
        if (n > 1) {
            cp = (unsigned long)((unsigned char)wrapped[wi] & (0x7Fu >> n));
            for (k = 1; k < n && wi + k < wlen; k++)
                cp = (cp << 6) | ((unsigned long)(unsigned char)wrapped[wi + k] & 0x3Fu);
        }
        gi = font_glyph_index(f, cp);
        g  = gi ? font_glyph(f, gi) : (const FontGlyph *)0;
        if (g) {
            float a = g->advance;
            if (prevg) a += font_kern(f, prevg, gi);
            adv[wi] = a * px2m;
            prevg = gi;
        } else {
            prevg = 0;
        }
        wi += n;
    }
    sgi = font_glyph_index(f, (unsigned long)' ');   /* space width for trailing spaces */
    sg  = sgi ? font_glyph(f, sgi) : (const FontGlyph *)0;
    space_adv = sg ? sg->advance * px2m : 0.0f;
    return caret_field_build(src, wrapped, map, adv,
                             wlen, font_line_height(f) * px2m, space_adv, out);
}

/* recompute edit_goal_x from the caret's current slot (after a horizontal move/edit). */
static void caret_refresh_goal(AppState *st) {
    SceneObject *o = scene_get(&st->scene, st->edit_handle);
    CaretField   cf;
    float        bpx2m, usable, cw, lh;
    int          slot;
    if (!o || !st->ui_font) { st->edit_goal_x = 0.0f; return; }
    lh     = font_line_height(st->ui_font);
    cw     = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
    usable = cw - 3.0f * 0.025f;                 /* matches the render's margin math */
    bpx2m  = note_text_size(&st->scene, st->edit_handle) / lh;
    caret_build(st->ui_font, st->edit_buf, bpx2m, usable, &cf);
    slot = caret_slot_for_offset(&cf, st->edit_cursor);
    st->edit_goal_x = (slot >= 0) ? cf.slots[slot].x : 0.0f;
}

/* remove the current selection [lo,hi) and collapse; mirror to meta + autosize.
   No-op when there's no selection. */
static void selection_delete(AppState *st) {
    int lo, hi;
    if (!edit_has_sel(st)) return;
    lo = edit_sel_lo(st);
    hi = edit_sel_hi(st);
    memmove(st->edit_buf + lo, st->edit_buf + hi, (size_t)(st->edit_len - hi));
    st->edit_len -= (hi - lo);
    st->edit_cursor = st->edit_sel_anchor = lo;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    note_autosize(st, st->edit_handle);
    caret_refresh_goal(st);
}

/* advance the shared multi-click counter on a left-press; returns the count within
   the BOARD_DBL time/px window (1,2,3,...). Updates last_press_*. */
static int click_seq_bump(AppState *st, double mx, double my) {
    double now = glfwGetTime();
    if (now - st->last_press_t < BOARD_DBL_S &&
        fabs(mx - st->last_press_x) < BOARD_DBL_PX &&
        fabs(my - st->last_press_y) < BOARD_DBL_PX)
        st->click_seq += 1;
    else
        st->click_seq = 1;
    st->last_press_t = now;
    st->last_press_x = mx;
    st->last_press_y = my;
    return st->click_seq;
}

/* the source byte offset under the cursor on the note being edited; returns 1 and
   writes *out on a hit, 0 if off the card / not board view. Sets edit_goal_x. */
static int caret_hit_offset(AppState *st, GLFWwindow *w, int *out) {
    SceneObject *o;
    Ray   ray;
    mat4  face;
    vec3  origin, rx, ry, nrm, hit, d;
    float cw, ch, ct, t, rr2, ru2, lx, ly, lh, bpx2m, usable, x0, y0;
    CaretField cf;
    int   line, slot;
    if (st->board_view == 0) return 0;
    o = scene_get(&st->scene, st->edit_handle);
    if (!o || !st->ui_font) return 0;
    cw = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
    ch = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "h");
    ct = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "t");
    face = mat4_mul(scene_world_matrix(&st->scene, o),
                    mat4_translate(vec3_make(0.0f, 0.0f, ct * 0.5f + 0.0008f)));
    origin = mat4_mul_point(face, vec3_make(0.0f, 0.0f, 0.0f));
    rx     = vec3_sub(mat4_mul_point(face, vec3_make(1.0f, 0.0f, 0.0f)), origin);
    ry     = vec3_sub(mat4_mul_point(face, vec3_make(0.0f, 1.0f, 0.0f)), origin);
    nrm    = vec3_normalize(vec3_cross(rx, ry));
    ray    = pick_ray(st, w);
    if (!ray_vs_plane(ray, origin, nrm, &t) || t <= 0.0f) return 0;
    hit = vec3_add(ray.origin, vec3_scale(ray.dir, t));
    d   = vec3_sub(hit, origin);
    rr2 = vec3_dot(rx, rx); ru2 = vec3_dot(ry, ry);
    if (rr2 < 1e-9f || ru2 < 1e-9f) return 0;
    lx = vec3_dot(d, rx) / rr2;
    ly = vec3_dot(d, ry) / ru2;
    if (lx < -cw * 0.5f || lx > cw * 0.5f || ly < 0.0f || ly > ch) return 0;
    lh     = font_line_height(st->ui_font);
    usable = cw - 3.0f * 0.025f;
    bpx2m  = note_text_size(&st->scene, st->edit_handle) / lh;
    x0     = -cw * 0.5f + 2.0f * 0.025f;
    y0     = ch - 2.0f * 0.025f;
    caret_build(st->ui_font, st->edit_buf, bpx2m, usable, &cf);
    line = (int)((y0 - ly) / cf.line_h);
    if (line < 0) line = 0;
    if (line >= cf.line_count) line = cf.line_count - 1;
    slot = caret_slot_nearest_x(&cf, line, lx - x0);
    if (slot < 0) return 0;
    *out = cf.slots[slot].src;
    st->edit_goal_x = lx - x0;
    return 1;
}


/* Open a note for typing: seed the buffer from its text meta. */
static void note_edit_begin(AppState *st, sol_u32 handle) {
    const char *t = scene_meta_get(&st->scene, handle, "text");
    size_t      n = t ? strlen(t) : 0;
    if (n >= EDIT_BUF_CAP) n = EDIT_BUF_CAP - 1;
    memcpy(st->edit_buf, t ? t : "", n);
    st->edit_buf[n] = '\0';
    st->edit_len    = (int)n;
    st->edit_handle = handle;
    st->edit_cursor = st->edit_len;    /* caret at the end, matching today's feel */
    st->edit_sel_anchor = st->edit_cursor;   /* no selection on open */
    st->edit_goal_x = 0.0f;
    printf("editing note — type away; Enter = newline, Esc or click = done\n");
}

/* Blur: the buffer already mirrors into the meta per keystroke (the card
   renders live); the blur is what makes it durable — save on blur, the
   same reflex as save-on-release. */
static void note_edit_end(AppState *st) {
    if (st->edit_handle == 0) return;
    st->click_seq = 0;          /* blur is a mode change: start the click sequence fresh */
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    st->edit_handle = 0;
    scene_save(&st->scene, "scene.stml");
    printf("note saved\n");
}

/* codepoint -> UTF-8 bytes (the encoder mirroring text.c's decoder).
   Returns the byte count, 0 for an invalid codepoint. */
static int utf8_encode(unsigned int cp, char *out) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) return 0;   /* surrogates: not chars */
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

/* CHARS are text: the OS keymap's output, delivered only here. Appends to
   the focused note and mirrors into the meta so the card renders live. */
static void on_char(GLFWwindow *w, unsigned int cp) {
    AppState *st = (AppState *)glfwGetWindowUserPointer(w);
    char      enc[4];
    int       n;
    if (!st) return;
    if (st->palette.open) { palette_input_char(&st->palette, cp); return; }
    if (reader_is_editing(st)) {
        n = utf8_encode(cp, enc);
        if (n > 0) reader_page_append(st, enc, n);
        return;
    }
    if (st->edit_handle == 0) return;
    if (glfwGetKey(w, GLFW_KEY_LEFT_SUPER)  == GLFW_PRESS ||
        glfwGetKey(w, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS) return;  /* Cmd+key is a command, not text */
    n = utf8_encode(cp, enc);
    if (n <= 0) return;
    selection_delete(st);                                   /* type-over a selection */
    if (st->edit_len + n >= EDIT_BUF_CAP) return;
    memmove(st->edit_buf + st->edit_cursor + n,
            st->edit_buf + st->edit_cursor,
            (size_t)(st->edit_len - st->edit_cursor));
    memcpy(st->edit_buf + st->edit_cursor, enc, (size_t)n);
    st->edit_len    += n;
    st->edit_cursor += n;
    st->edit_sel_anchor = st->edit_cursor;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    note_autosize(st, st->edit_handle);
    caret_refresh_goal(st);
}

/* Insert a UTF-8 string at the caret, replacing any selection. Truncates to the
   buffer's remaining room on a codepoint boundary. Mirrors + autosizes like on_char. */
static void note_insert_text(AppState *st, const char *s, int len) {
    int room;
    selection_delete(st);                                   /* replace a selection */
    room = EDIT_BUF_CAP - 1 - st->edit_len;
    if (len > room) len = room;
    while (len > 0 && ((unsigned char)s[len] & 0xC0u) == 0x80u) len--;  /* don't split a codepoint */
    if (len <= 0) return;
    memmove(st->edit_buf + st->edit_cursor + len,
            st->edit_buf + st->edit_cursor,
            (size_t)(st->edit_len - st->edit_cursor));
    memcpy(st->edit_buf + st->edit_cursor, s, (size_t)len);
    st->edit_len    += len;
    st->edit_cursor += len;
    st->edit_sel_anchor = st->edit_cursor;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
    note_autosize(st, st->edit_handle);
    caret_refresh_goal(st);
}

/* Copy the selection to the system clipboard. Returns 1 if there was a selection. */
static int note_clip_copy(AppState *st, GLFWwindow *w) {
    int  lo, hi, k;
    char cb[EDIT_BUF_CAP];
    if (!edit_has_sel(st)) return 0;
    lo = edit_sel_lo(st);
    hi = edit_sel_hi(st);
    k  = hi - lo;
    if (k > EDIT_BUF_CAP - 1) k = EDIT_BUF_CAP - 1;
    memcpy(cb, st->edit_buf + lo, (size_t)k);
    cb[k] = '\0';
    glfwSetClipboardString(w, cb);
    return 1;
}

/* Paste the system clipboard's text at the caret (replacing a selection), stripping '\r'. */
static void note_clip_paste(AppState *st, GLFWwindow *w) {
    const char *cb = glfwGetClipboardString(w);
    const char *p;
    char        s[EDIT_BUF_CAP];
    int         n = 0, lp, need;
    unsigned    lead;
    if (!cb || !cb[0]) return;
    for (p = cb; *p != '\0' && n < EDIT_BUF_CAP - 1; p++)
        if (*p != '\r') s[n++] = *p;     /* drop carriage returns: \r\n -> \n */
    if (n == EDIT_BUF_CAP - 1) {          /* hit capacity: the last codepoint may be partial */
        lp = n - 1;
        while (lp > 0 && ((unsigned char)s[lp] & 0xC0u) == 0x80u) lp--;
        lead = (unsigned char)s[lp];
        need = (lead >= 0xF0u) ? 4 : (lead >= 0xE0u) ? 3 : (lead >= 0xC0u) ? 2 : 1;
        if (lp + need > n) n = lp;        /* drop the incomplete trailing codepoint */
    }
    s[n] = '\0';
    if (n > 0) note_insert_text(st, s, n);
}

/* KEYS are buttons. While a note has focus the only buttons that mean
   anything are the text-control ones — and they get GLFW_REPEAT for free
   here (held Backspace erases a run), which polling cannot give. */
static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods) {
    AppState *st = (AppState *)glfwGetWindowUserPointer(window);
    (void)scancode;

    if (!st) return;

    /* Command palette owns the keyboard while open. */
    if (st->palette.open) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            PaletteKey pk = PALETTE_KEY_NONE;
            if      (key == GLFW_KEY_ESCAPE)    pk = PALETTE_KEY_CANCEL;
            else if (key == GLFW_KEY_UP)        pk = PALETTE_KEY_UP;
            else if (key == GLFW_KEY_DOWN)      pk = PALETTE_KEY_DOWN;
            else if (key == GLFW_KEY_ENTER ||
                     key == GLFW_KEY_KP_ENTER)  pk = PALETTE_KEY_ENTER;
            else if (key == GLFW_KEY_BACKSPACE) pk = PALETTE_KEY_BACKSPACE;
            if (pk != PALETTE_KEY_NONE)
                palette_input_key(&st->palette, pk, st, g_commands, G_COMMAND_COUNT);
        }
        return;
    }

    /* The inventory screen owns the keyboard while open. */
    if (st->inv_open) {
        if (action == GLFW_PRESS) {
            sol_u32 items[INV_THUMB_CAP];
            int     n = inventory_collect(st, items, INV_THUMB_CAP);
            if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_I) st->inv_open = SOL_FALSE;
            else if (key == GLFW_KEY_LEFT)
                st->inv_page = inv_clamp_page(st->inv_page - 1, n, INV_PER_PAGE);
            else if (key == GLFW_KEY_RIGHT)
                st->inv_page = inv_clamp_page(st->inv_page + 1, n, INV_PER_PAGE);
        }
        return;
    }

    /* Place mode owns the keyboard while previewing furniture. */
    if (st->place_active) {
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            if      (key == GLFW_KEY_ESCAPE) { st->place_active = SOL_FALSE; mesh_destroy(&st->place_ghost); }
            else if (key == GLFW_KEY_LEFT_BRACKET)  { st->place_index = furniture_catalog_cycle(st->place_index, -1); place_realize_ghost(st); }
            else if (key == GLFW_KEY_RIGHT_BRACKET) { st->place_index = furniture_catalog_cycle(st->place_index,  1); place_realize_ghost(st); }
            else if (key == GLFW_KEY_COMMA)  { st->place_yaw -= sol_radians(15.0f); }
            else if (key == GLFW_KEY_PERIOD) { st->place_yaw += sol_radians(15.0f); }
            else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) { place_confirm(st); }
        }
        return;
    }

    /* ',' / '.' rotate a carried tablet (the table-filing yaw control). */
    if (st->place_active == SOL_FALSE && st->carried != 0 &&
        (action == GLFW_PRESS || action == GLFW_REPEAT)) {
        if      (key == GLFW_KEY_COMMA)  st->place_yaw -= sol_radians(15.0f);
        else if (key == GLFW_KEY_PERIOD) st->place_yaw += sol_radians(15.0f);
    }

    /* Enter while carrying = stow the held item into the inventory bag. */
    if (action == GLFW_PRESS && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
        && st->carried != 0 && !st->place_active && !st->editor.active) {
        inventory_stow(st);
        return;
    }

    /* ':' (Shift+;) opens the palette when nothing else owns the keyboard. */
    if (action == GLFW_PRESS && key == GLFW_KEY_SEMICOLON && (mods & GLFW_MOD_SHIFT)
        && st->edit_handle == 0 && st->reader_state == READER_IDLE && st->board_view == 0) {
        palette_open_now(&st->palette);
        return;
    }

    /* 'i' opens the inventory when nothing else owns the keyboard. */
    if (action == GLFW_PRESS && key == GLFW_KEY_I && st->carried == 0 &&
        st->edit_handle == 0 && st->reader_state == READER_IDLE &&
        !st->place_active && !st->palette.open && !st->editor.active && st->board_view == 0) {
        cmd_inventory_open(st);
        return;
    }

    if (reader_is_editing(st)) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        if (key == GLFW_KEY_ESCAPE) { if (action == GLFW_PRESS) reader_close(st); }
        else if (key == GLFW_KEY_BACKSPACE) reader_page_backspace(st);
        else if (key == GLFW_KEY_ENTER)     reader_page_newline(st);
        else if (key == GLFW_KEY_RIGHT)     reader_edit_flip(st, +1);
        else if (key == GLFW_KEY_LEFT)      reader_edit_flip(st, -1);
        return;
    }
    if (st->edit_handle != 0) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            note_edit_end(st);
        } else if (key == GLFW_KEY_BACKSPACE && (edit_has_sel(st) || st->edit_cursor > 0)) {
            if (edit_has_sel(st)) { selection_delete(st); }
            else {
                int e = st->edit_cursor, s = e - 1;
                while (s > 0 && ((unsigned char)st->edit_buf[s] & 0xC0u) == 0x80u) s--;
                memmove(st->edit_buf + s, st->edit_buf + e, (size_t)(st->edit_len - e));
                st->edit_len   -= (e - s);
                st->edit_cursor = st->edit_sel_anchor = s;
                st->edit_buf[st->edit_len] = '\0';
                scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
                note_autosize(st, st->edit_handle);
                caret_refresh_goal(st);
            }
        } else if (key == GLFW_KEY_DELETE && (edit_has_sel(st) || st->edit_cursor < st->edit_len)) {
            if (edit_has_sel(st)) { selection_delete(st); }
            else {
                int s = st->edit_cursor, e = s + 1;
                while (e < st->edit_len && ((unsigned char)st->edit_buf[e] & 0xC0u) == 0x80u) e++;
                memmove(st->edit_buf + s, st->edit_buf + e, (size_t)(st->edit_len - e));
                st->edit_len -= (e - s);
                st->edit_sel_anchor = st->edit_cursor;
                st->edit_buf[st->edit_len] = '\0';
                scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
                note_autosize(st, st->edit_handle);
                caret_refresh_goal(st);
            }
        } else if (key == GLFW_KEY_ENTER) {
            selection_delete(st);                            /* type-over with a newline */
            if (st->edit_len + 1 < EDIT_BUF_CAP) {
                memmove(st->edit_buf + st->edit_cursor + 1,
                        st->edit_buf + st->edit_cursor,
                        (size_t)(st->edit_len - st->edit_cursor));
                st->edit_buf[st->edit_cursor] = '\n';
                st->edit_len++;
                st->edit_cursor++;
                st->edit_sel_anchor = st->edit_cursor;
                st->edit_buf[st->edit_len] = '\0';
                scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
                note_autosize(st, st->edit_handle);
                caret_refresh_goal(st);
            }
        } else if (key == GLFW_KEY_LEFT) {
            int shift = (mods & GLFW_MOD_SHIFT) != 0;
            if (!shift && edit_has_sel(st)) {
                st->edit_cursor = st->edit_sel_anchor = edit_sel_lo(st);
            } else if (st->edit_cursor > 0) {
                st->edit_cursor--;
                while (st->edit_cursor > 0 &&
                       ((unsigned char)st->edit_buf[st->edit_cursor] & 0xC0u) == 0x80u)
                    st->edit_cursor--;
                if (!shift) st->edit_sel_anchor = st->edit_cursor;
            }
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_RIGHT) {
            int shift = (mods & GLFW_MOD_SHIFT) != 0;
            if (!shift && edit_has_sel(st)) {
                st->edit_cursor = st->edit_sel_anchor = edit_sel_hi(st);
            } else if (st->edit_cursor < st->edit_len) {
                st->edit_cursor++;
                while (st->edit_cursor < st->edit_len &&
                       ((unsigned char)st->edit_buf[st->edit_cursor] & 0xC0u) == 0x80u)
                    st->edit_cursor++;
                if (!shift) st->edit_sel_anchor = st->edit_cursor;
            }
            caret_refresh_goal(st);
        } else if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN) {
            int shift = (mods & GLFW_MOD_SHIFT) != 0;
            if (!shift && edit_has_sel(st)) {
                st->edit_cursor = st->edit_sel_anchor =
                    (key == GLFW_KEY_UP) ? edit_sel_lo(st) : edit_sel_hi(st);
            } else {
                SceneObject *o = scene_get(&st->scene, st->edit_handle);
                if (o && st->ui_font) {
                    float      lh     = font_line_height(st->ui_font);
                    float      cw     = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
                    float      usable = cw - 3.0f * 0.025f;
                    float      bpx2m  = note_text_size(&st->scene, st->edit_handle) / lh;
                    CaretField cf;
                    int        slot, line, tgt;
                    caret_build(st->ui_font, st->edit_buf, bpx2m, usable, &cf);
                    slot = caret_slot_for_offset(&cf, st->edit_cursor);
                    line = (slot >= 0) ? caret_line_of_slot(&cf, slot) : 0;
                    if (key == GLFW_KEY_UP)   line = (line > 0) ? line - 1 : -1;
                    else                      line = (line + 1 < cf.line_count) ? line + 1 : -1;
                    if (line < 0) st->edit_cursor = (key == GLFW_KEY_UP) ? 0 : st->edit_len;
                    else {
                        tgt = caret_slot_nearest_x(&cf, line, st->edit_goal_x);
                        if (tgt >= 0) st->edit_cursor = cf.slots[tgt].src;
                    }
                    if (!shift) st->edit_sel_anchor = st->edit_cursor;
                }
            }
        } else if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_C && action == GLFW_PRESS) {
            note_clip_copy(st, window);                       /* copy selection (no-op if none) */
        } else if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_X && action == GLFW_PRESS) {
            if (note_clip_copy(st, window)) selection_delete(st);   /* cut = copy + delete */
        } else if ((mods & GLFW_MOD_SUPER) && key == GLFW_KEY_V && action == GLFW_PRESS) {
            note_clip_paste(st, window);                      /* paste at the caret */
        }
        return;                                 /* everything else stays quiet */
    }

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        if (st && st->reader_state != READER_IDLE)
            reader_close(st);                   /* put the book back first (even in board view) */
        else if (st && (st->board_view != 0 || st->bv_t < 1.0f))
            board_view_exit(st);                /* leave board view (or absorb Esc mid-glide-out) */
        else
            glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    /* Enter = ENTER THE THING: a NOTE opens for typing, a codex or a
       FILE/ALIAS card opens for reading (item 9) */
    if (st && key == GLFW_KEY_ENTER && action == GLFW_PRESS &&
        st->selected_handle != 0) {
        SceneObject *o = scene_get(&st->scene, st->selected_handle);
        if (o && o->kind == KIND_NOTE)
            note_edit_begin(st, st->selected_handle);
        else if (object_is_board(&st->scene, st->selected_handle)) {
            if (st->board_view == 0) board_view_enter(st);
            /* already in board view: Enter on the board itself does nothing */
        }
        else if (o)
            reader_open(st, st->selected_handle);   /* a file/alias card opens to
                                                       read — works in board view too;
                                                       reader_open ignores unreadable kinds */
    }
}

int main(void) {
    GLFWwindow *window;
    AppState state = {0};
    double last;
    state.bv_t = 1.0f;   /* board-view glide starts settled (no tween at boot) */
    state.hover_corner = -1;   /* no resize-corner hovered yet */

    asset_store_init(&g_mesh_assets,    mesh_asset_destroy,   NULL);
    asset_store_init(&g_tex_assets,     tex_asset_destroy,    NULL);
    asset_store_init(&g_glbpart_assets, glb_part_destroy,     NULL);
    asset_store_init(&g_texgen_assets,  texgen_asset_destroy, NULL);
    load_material_overrides();   /* the kinds' voice, before the first resolve */

    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return EXIT_FAILURE;
    }

    rhi_configure_window();   /* backend sets the API/context hints */

    window = glfwCreateWindow(960, 540, "solarium", NULL, NULL);
    if (!window) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwSetWindowUserPointer(window, &state);   /* bridge callbacks -> state */
    glfwSetKeyCallback(window, on_key);         /* platform: input (buttons) */
    glfwSetCharCallback(window, on_char);       /* platform: input (text) */
    glfwSetScrollCallback(window, on_scroll);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);  /* first-person: capture */
    state.mouse_skip = 2;                /* swallow the first deltas (no baseline yet) */

    if (!rhi_init(window)) {              /* backend: context + GL info */
        fprintf(stderr, "rhi_init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    if (!init_scene(&state)) {
        fprintf(stderr, "scene init failed\n");
        rhi_shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

#ifndef NDEBUG
    /* teardown self-test (item 1 acceptance): create/destroy in a loop must be
       ASan-clean and bounded — the free-list reuses one slot the whole time */
    {
        int n;
        sol_f32 tmp = 0.0f;
        for (n = 0; n < 5000; n++) {
            RhiBuffer b = rhi_create_buffer(RHI_BUFFER_VERTEX, &tmp, sizeof tmp);
            rhi_destroy_buffer(b);
        }
        printf("teardown selftest: 5000 create/destroy cycles ok\n");
    }

    /* persistence self-check (P3 item 1 acceptance): the scene.stml written
       by init_scene must load back and RECONSTRUCT geometry, not just data.
       Load into a second Scene, resolve, verify every ref'd object got real
       geometry, then free it. THE TEARDOWN GOES THROUGH THE REGISTRY (P4
       item 4): the check's resolve SHARES meshes with the live palace (same
       keys, by design), so a raw destroy here would tear the buffers out
       from under the live scene — release gives the refs back instead. */
    {
        Scene   check;
        sol_u32 i, refs = 0, solid = 0;
        if (!scene_load(&check, "scene.stml")) {
            fprintf(stderr, "persistence self-check: scene.stml failed to load\n");
            rhi_shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_FAILURE;
        }
        scene_resolve_meshes(&check);
        for (i = 0; i < check.count; i++) {
            SceneObject *o = &check.objects[i];
            if (!o->mesh_ref) continue;
            if (strcmp(o->mesh_ref, "arrow") == 0) continue;
            /* ^ arrows are SCENE-derived (item 8): their geometry comes from
               arrows_rebuild, not the registry — and a dangling edge
               legitimately draws nothing, so "every ref must reconstruct"
               is the registry's invariant, not theirs (iotest covers their
               rel round-trip instead) */
            refs++;
            if (o->mesh.index_count > 0) solid++;
            else {
                /* item 8 made honest emptiness legal: a church_roof at
                   ruin 0.6 builds NOTHING because every roof has fallen
                   — that IS its reconstruction. The drift this check
                   exists to catch is an unknown/broken ref, so a ref
                   the registry still knows counts as reconstructed. */
                const char *const *nm; const float *df;
                if (mesh_ref_schema(o->mesh_ref, &nm, &df) >= 0) solid++;
            }
        }
        scene_release_meshes(&check);     /* shared shapes go BACK, not down */
        /* counts may legitimately differ now: scene.stml can be the user's
           saved arrangement (item 4), not a mirror of the built default —
           the hard requirement is that every ref RECONSTRUCTS */
        if (refs == 0 || solid != refs) {
            fprintf(stderr, "persistence self-check FAILED: %u/%u refs reconstructed\n",
                    (unsigned)solid, (unsigned)refs);
            scene_free(&check);
            rhi_shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_FAILURE;
        }
        printf("persistence self-check: %u objects (%u live), %u/%u meshes reconstructed ok\n",
               (unsigned)check.count, (unsigned)state.scene.count,
               (unsigned)solid, (unsigned)refs);
        scene_free(&check);
    }
#endif

    if (!ui_init()) {                     /* the 2D overlay (P3 item 2) */
        fprintf(stderr, "ui_init failed\n");
        rhi_shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    /* the UI font (P3 item 3): bake the SDF atlas once at startup. A missing
       font degrades (no text) rather than failing the launch — ui_font stays
       NULL and text paths skip. */
    state.ui_font = font_load("fonts/DejaVuSans.ttf", 48.0f);
    if (!state.ui_font)
        fprintf(stderr, "font: fonts/DejaVuSans.ttf failed to load — text disabled\n");
    state.mono_font = font_load("fonts/DejaVuSansMono.ttf", 48.0f);
    if (!state.mono_font)
        fprintf(stderr, "font: fonts/DejaVuSansMono.ttf failed to load — mono disabled\n");

    if (!wtext_init())                    /* world text (item 8): same atlas, MVP ride */
        fprintf(stderr, "wtext_init failed — card text disabled\n");

    /* audio (P4 item 8): the callback starts now; failure degrades to a
       silent palace, never a dead one. The bank mints every buffer from
       presets + sounds.stml overrides; the wind starts breathing; the
       watcher makes the knobs live. */
    if (!audio_init())
        fprintf(stderr, "audio: no output device — the palace stays silent\n");
    load_sound_overrides();
    sound_bank_mint();
    audio_loops_restart();
    {
        RhiTexture none;
        none.id = 0;
        watch_add("sounds.stml", WATCH_SND, none);
        watch_add("materials.stml", WATCH_MAT, none);
    }

    last = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        double      now, dt, y1, y2;
        CameraInput in;
        glfwPollEvents();

        now = glfwGetTime();
        dt  = now - last;
        last = now;
        yardstick_ms(&state.t_frame, dt);         /* the true frame period */
        if (dt > 0.1) dt = 0.1;   /* clamp: a long stall pauses motion, never lurches */

        glfwGetFramebufferSize(window, &state.fb_width, &state.fb_height);

        read_input(window, &in, dt, &state);          /* poll GLFW -> CameraInput */
        state.camera.ground_y = ground_under(&state, state.camera.pos,
                                             &state.current_terrain);
        {   /* architecture may claim the ground too (P6 item 9): the
               pavement above the island, the porch steps, a broken
               wall stump — the highest top within the step treaty */
            vec3  sfeet = state.camera.pos;
            float gs;
            sfeet.y -= CAMERA_EYE_HEIGHT;
            gs = collide_stand(&state.colliders, sfeet, COLLIDE_RADIUS);
            if (gs > state.camera.ground_y) state.camera.ground_y = gs;
        }
        /* the plan overlay follows you island to island (P6 item 3);
           a reload's stale handle drops it, the next step rebuilds it */
        if (state.plan_on) {
            if (state.plan_plot && !scene_get(&state.scene, state.plan_plot))
                plan_overlay_drop(&state);
            if (state.current_terrain != 0 &&
                state.current_terrain != state.plan_plot) {
                SceneObject *isl = scene_get(&state.scene,
                                             state.current_terrain);
                if (isl) plan_overlay_build(&state, isl);
            }
        }
        {
            /* collision (P4 item 1): camera_update PROPOSES, the world
               DISPOSES. The desired move is read back as a delta so
               camera.c stays pure kinematics; the capsule hangs from the
               eye (feet = eye - eye height; the crown sits just above the
               eye). Lateral resolves against the derived walls; vertical
               stays the ground seam's except fly's clamp at undersides
               and tops. Orbit is an inspection camera and ghost is the
               debug out — both skip. */
            vec3 before = state.camera.pos;
            camera_update(&state.camera, &in, (float)dt);
            board_view_update(&state, (float)dt);   /* overrides camera_update */
            if (!state.ghost && state.camera.mode != CAMERA_ORBIT) {
                vec3 move = vec3_sub(state.camera.pos, before);
                vec3 feet = before;
                vec3 lat  = move;
                feet.y -= CAMERA_EYE_HEIGHT;
                lat.y   = 0.0f;
                feet = collide_slide(&state.colliders, feet, lat,
                                     COLLIDE_RADIUS, COLLIDE_HEIGHT);
                if (state.camera.mode == CAMERA_FLY)
                    move.y = collide_clamp_y(&state.colliders, feet, move.y,
                                             COLLIDE_RADIUS, COLLIDE_HEIGHT);
                feet.y += move.y;
                state.camera.pos   = feet;
                state.camera.pos.y += CAMERA_EYE_HEIGHT;
            }
        }
        y1 = glfwGetTime();
        /* THE ONE WIND (P7 item 9): one evaluation per frame at the
           camera, read by every consumer this frame */
        wind_at((float)now, state.camera.pos.x, state.camera.pos.z,
                &state.wind_dx, &state.wind_dz, &state.wind_gust);
        update(&state, dt);                           /* animate the scene */
        portal_update(&state);
        carry_update(&state);
        components_update(&state.scene, (float)now, (float)dt);  /* overlays
                                                         rewrite BEFORE the tree
                                                         and render read poses */
        {   /* particle drift rides the wind: motes + falling leaves blow
               downwind, harder in a gust (light dust, stronger push) */
            vec3 wind = vec3_make(state.wind_dx * (0.4f + 1.8f * state.wind_gust),
                                  0.0f,
                                  state.wind_dz * (0.4f + 1.8f * state.wind_gust));
            particles_update(&state.particles, (float)dt, wind);
        }
        update_audio(&state, (float)dt);              /* wind by containment,
                                                         lantern crackles in 3D,
                                                         footsteps (item 8) */
        reader_update(&state, (float)dt);             /* the book's flight (item 9) */
        if (now - g_watch_last >= 0.5) {              /* the watcher (P4 item 4):
                                                         a handful of stats twice
                                                         a second, reloads in place */
            g_watch_last = now;
            watch_poll(&state);
        }
        bvh_refresh(&state);                          /* boxes current AFTER the
                                                         animations move things —
                                                         culling reads them next */
        ornament_sync(&state);                        /* balustrades' copies follow
                                                         their params (P6 item 10) */
        yardstick_ms(&state.t_update, glfwGetTime() - y1);
        render(&state);

        y2 = glfwGetTime();
        yardstick_ms(&state.t_cpu, y2 - now);         /* all the work we control */
        rhi_present();
        yardstick_ms(&state.t_swap, glfwGetTime() - y2);  /* the vsync block */
        {   /* P8 item 1: the GPU yardstick — Metal reports the completed
               frame's GPU time (a few frames deferred); GL returns < 0 (its
               timer queries are stubbed). Snap on the first real sample so the
               -1/0 sentinel doesn't smear into the smoothed value. */
            double gpu = rhi_timer_frame_ms();
            if (gpu >= 0.0) {
                if (state.t_frame_gpu <= 0.0f) state.t_frame_gpu = (float)gpu;
                else state.t_frame_gpu += ((float)gpu - state.t_frame_gpu) * 0.08f;
            } else {
                state.t_frame_gpu = -1.0f;
            }
        }
    }

    plan_overlay_drop(&state);
    mesh_destroy(&state.caret_mesh);
    mesh_destroy(&state.folderbook_leaves_mesh);
    font_destroy(state.mono_font);
    font_destroy(state.ui_font);
    wtext_shutdown();
    audio_shutdown();                   /* stop the other thread FIRST: no
                                           render in flight while buffers die */
    ui_shutdown();
    scene_free(&state.scene);
    collide_set_free(&state.colliders);
    bvh_free(&state.bvh);
    free(state.bvh_ids);
    free(state.bvh_boxes);
    free(state.vis);
    asset_store_free(&g_mesh_assets);   /* sweeps the living, before rhi dies */
    asset_store_free(&g_glbpart_assets);
    asset_store_free(&g_texgen_assets); /* synthesized maps: nobody names them */
    asset_store_free(&g_tex_assets);    /* textures LAST: parts may name them */
    if (state.hdr_rt.id) rhi_destroy_render_target(state.hdr_rt);
    {
        int sc;
        for (sc = 0; sc < SHADOW_CASCADES; sc++)
            if (state.shadow_rt[sc].id) rhi_destroy_render_target(state.shadow_rt[sc]);
    }
    if (state.spot_shadow_rt.id) rhi_destroy_render_target(state.spot_shadow_rt);
    rhi_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}

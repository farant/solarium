#include <stdio.h>
#include <stdlib.h>
#include <string.h>              /* strcmp — room-shell refs (item 7) */
#include <math.h>                /* cosf — spot-light cone cosines (item 9a) */
#include <time.h>                /* time — seeds the codex mint (P3 item 9) */

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>          /* platform: window, input, time — not GL */

#include "rhi.h"                 /* the graphics seam — no GL above here */
#include "mesh.h"
#include "scene.h"
#include "sol_math.h"
#include "camera.h"
#include "image.h"
#include "glb.h"
#include "ui.h"                  /* the 2D overlay (P3 item 2) */
#include "mirror.h"              /* mirror rooms reflect real folders (P3 item 6) */
#include "font.h"                /* the SDF glyph atlas (P3 item 3) */
#include "text.h"                /* text_shape seam + ui_text (P3 item 3b) */
#include "wtext.h"               /* world-space SDF text — note cards (P3 item 8) */
#include "platform_fs.h"         /* fs_read_file — the reader's pages (P3 item 9) */

#define LOOK_SPEED        1.5f     /* radians/sec for keyboard look           */
#define MOUSE_SENSITIVITY 0.0025f  /* radians per pixel; NOT dt-scaled        */

#define SHADOW_MAP_SIZE   2048     /* the light's depth-map resolution (item 9b) */
#define SHADOW_NEAR       1.0f     /* light frustum near/far: bracket the scene  */
#define SHADOW_FAR        20.0f    /* (also set depth precision)                 */

#define ENV_CUBE_SIZE   1024   /* per-face resolution of the environment cubemap */
#define IRRADIANCE_SIZE 32     /* irradiance is very low-frequency: tiny is plenty */
#define PREFILTER_SIZE  128    /* specular prefilter base (mip 0 = sharpest reflection) */
#define PREFILTER_MIPS  5      /* roughness levels 0..1 across mips 0..4 */
#define BRDF_LUT_SIZE   512    /* BRDF integration LUT (NoV x roughness) */

/* --- shaders: GLSL source handed to the backend (still app-authored) --- */
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
    "uniform float uMetallic;\n"
    "uniform float uRoughness;\n"
    "uniform vec3  uLightPos;\n"                             /* spot light: position (item 9a) */
    "uniform vec3  uLightDir;\n"                             /* normalized aim, light -> scene */
    "uniform vec3  uLightColor;\n"
    "uniform float uLightIntensity;\n"
    "uniform float uCosInner;\n"                             /* cos(inner half-angle) */
    "uniform float uCosOuter;\n"                             /* cos(outer half-angle) */
    "uniform mat4  uLightVP;\n"                              /* light proj*view (item 9c); == the shadow pass's */
    "uniform sampler2D uShadowMap;\n"                        /* depth from the light's POV (item 9b) */
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
    "vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float rough) {\n"  /* ambient F (no half-vector): view angle + roughness (B3) */
    "    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - cosTheta, 5.0);\n"
    "}\n"
    "float shadowFactor(vec3 worldPos, float NoL) {\n"       /* 0 = lit, 1 = fully shadowed (item 9c) */
    "    vec4 lpos = uLightVP * vec4(worldPos, 1.0);\n"
    "    vec3 proj = lpos.xyz / lpos.w;\n"                    /* perspective divide -> NDC */
    "    proj = proj * 0.5 + 0.5;\n"                          /* -> [0,1] for sampling + compare */
    "    if (proj.z > 1.0) return 0.0;\n"                     /* beyond the light's far plane = lit */
    "    float current = proj.z;\n"
    "    float bias = max(0.0025 * (1.0 - NoL), 0.0008);\n"   /* slope-scaled: more at grazing angles (tunable) */
    "    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));\n"
    "    float sum = 0.0;\n"                                  /* 3x3 PCF: soften the edge */
    "    int x, y;\n"
    "    for (x = -1; x <= 1; ++x)\n"
    "        for (y = -1; y <= 1; ++y) {\n"
    "            float closest = texture(uShadowMap, proj.xy + vec2(x, y) * texel).r;\n"
    "            sum += (current - bias > closest) ? 1.0 : 0.0;\n"  /* 1 = something closer to light */
    "        }\n"
    "    return sum / 9.0;\n"
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
    "    if (uUseNormalTex > 0.5) {\n"
    "        vec3 T = normalize(vTangent.xyz - dot(vTangent.xyz, N) * N);\n"  /* re-orthogonalize vs N */
    "        vec3 B = cross(N, T) * vTangent.w;\n"             /* bitangent (handedness) */
    "        vec3 n = texture(uNormalTex, vUV).rgb * 2.0 - 1.0;\n"  /* decode [0,1] -> [-1,1] */
    "        n.xy *= uNormalScale;\n"                          /* bump strength */
    "        N = normalize(mat3(T, B, N) * n);\n"              /* tangent space -> world */
    "    }\n"
    "    vec3 V = normalize(uViewPos - vWorldPos);\n"          /* direction TO the camera */
    "    vec3 toLight = uLightPos - vWorldPos;\n"
    "    float dist = length(toLight);\n"
    "    vec3 L = toLight / dist;\n"                            /* spot: direction TO light */
    "    vec3 H = normalize(L + V);\n"
    "    float NoV = max(dot(N, V), 0.0001);\n"
    "    float NoL = max(dot(N, L), 0.0);\n"
    "    float NoH = max(dot(N, H), 0.0);\n"
    "    float HoV = max(dot(H, V), 0.0);\n"
    "\n"
    "    vec3 F0 = mix(vec3(0.04), albedo, metallic);\n"      /* dielectric 4% vs metal-tinted */
    "    float D = distributionGGX(NoH, roughness);\n"
    "    float G = geometrySmith(NoV, NoL, roughness);\n"
    "    vec3  F = fresnelSchlick(HoV, F0);\n"
    "    vec3  specular = (D * G) * F / (4.0 * NoV * NoL + 0.0001);\n"
    "\n"
    "    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);\n"     /* energy: no diffuse where it reflected / for metals */
    "    vec3 diffuse = kD * albedo / PI;\n"
    "\n"
    "    float atten = 1.0 / (dist * dist);\n"                 /* inverse-square falloff */
    "    float theta = dot(-L, uLightDir);\n"                  /* cos angle off the spot axis */
    "    float cone  = smoothstep(uCosOuter, uCosInner, theta);\n"  /* 1 inside, 0 past outer */
    "    vec3 radiance = uLightColor * uLightIntensity * atten * cone;\n"
    "    float shadow = shadowFactor(vWorldPos, NoL);\n"      /* item 9c: only direct light is shadowed */
    "    vec3 Lo = (diffuse + specular) * radiance * NoL * (1.0 - shadow);\n"
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
    "    vec3 color = ambient * uAmbientScale + Lo;\n"          /* sealed rooms dim the sky's ambient (item 7) */
    "    color = mix(color, vec3(1.0, 0.85, 0.30), uHighlight * 0.5);\n"  /* selection tint (linear) */
    "    FragColor = vec4(color, 1.0);\n"                     /* LINEAR -> the HDR buffer; 7c tonemaps + encodes */
    "}\n";

/* --- the fullscreen tonemap/encode pass (item 7b): samples the HDR buffer and
   writes the display image to the window. The vertex shader synthesizes one
   screen-covering triangle from gl_VertexID, so it needs no vertex buffer. --- */
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
    "uniform float uExposure;\n"
    "out vec4 FragColor;\n"
    "vec3 aces(vec3 x) {\n"                                  /* Narkowicz ACES filmic fit */
    "    return clamp((x * (2.51*x + 0.03)) / (x * (2.43*x + 0.59) + 0.14), 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    vec3 hdr    = texture(uHdr, vUV).rgb * uExposure;\n"  /* linear radiance * exposure */
    "    vec3 mapped = aces(hdr);\n"                           /* tonemap: roll off HDR -> [0,1] */
    "    vec3 ldr    = pow(mapped, vec3(1.0 / 2.2));\n"        /* linear -> sRGB for display */
    "    FragColor   = vec4(ldr, 1.0);\n"
    "}\n";

/* --- the shadow (depth-only) pass (item 9b): render the scene from the light's
   POV, writing only depth into the shadow map. No color work; position only,
   reading just attr 0 of the 12-float vertex. --- */
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

/* --- debug view (item 9b): show the shadow map full-screen as linearized
   grayscale (near=dark, far=white), so we can confirm the light's-eye depth
   render is correct before 9c samples it. Reuses the fullscreen-triangle VS. --- */
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

/* --- the skybox (Phase A2): a fullscreen triangle that samples the
   equirectangular HDR by the per-pixel world-space view direction. Drawn FIRST
   into the HDR target with depth off, so it fills the background; objects then
   draw over it. Writes linear radiance — the post pass tonemaps it like the
   rest of the scene. --- */
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

/* --- the on-screen skybox once we have a cubemap (B1): identical to the equirect
   version but samples a samplerCube by the view direction. Switching the live
   skybox to this is the self-check — if the sky looks the same, the equirect->
   cube conversion is correct. --- */
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

/* --- the diffuse irradiance convolution (B2): for each output direction N (a
   surface normal), integrate the cosine-weighted hemisphere of incoming light
   from the env cubemap. Run once per face into a tiny 32^2 cubemap. cos*sin =
   Lambert cosine * solid-angle Jacobian; the pi folds in the 1/pi BRDF so the
   lighting pass is just diffuse = irradiance * albedo. --- */
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

/* --- specular prefilter convolution (C1): for each output direction (= the
   reflection vector R, assuming V=N=R), GGX-importance-sample the env cube and
   average. Rendered once per mip, each at a higher roughness -> the roughness
   mip chain. Hammersley + GGX inverse-CDF gives samples clustered in the lobe
   (far fewer needed than uniform). The per-sample mip pick (solid-angle ratio)
   pre-blurs bright spots so the sun can't firefly a glossy reflection. --- */
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

/* --- the BRDF integration LUT (C2): the second split-sum factor. For each
   (NoV, roughness) it integrates the specular geometry+Fresnel over the GGX
   lobe, factored so F0 separates -> R = scale on F0, G = bias. A 2D fullscreen
   render; environment-independent (bake once). Uses the same Hammersley + GGX
   importance sampling as C1, but the IBL geometry remap (k = rough^2 / 2). --- */
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

#define EDIT_BUF_CAP 2048   /* a note's text, while it is being typed */

typedef struct {
    int         fb_width, fb_height;
    RhiPipeline pipeline;
    RhiPipeline post_pipeline;  /* fullscreen tonemap/encode pass (item 7b) */
    RhiTexture  albedo_tex;     /* decoded page image (item 5b); 0 if load failed */
    Scene       scene;
    sol_u32     box_handle;     /* so update() can animate the box object */
    sol_u32     anchor_handle;  /* the empty the box is parented to */
    sol_u32     page_handle;    /* the readable parchment surface (item 5d) */
    sol_u32     sword_handle;   /* the showcase sword: stood upright, spun in update() */
    sol_u32     sword_precess_handle;  /* invisible anchor the sword orbits (precession) */
    float       angle;
    Camera      camera;
    /* offscreen HDR pass (item 7) */
    RhiRenderTarget hdr_rt;          /* scene renders here, then to the window */
    int             rt_width, rt_height;  /* size hdr_rt was built at; recreate on resize */
    float           exposure;        /* HDR exposure (item 7c); '[' / ']' scrub it live */
    /* the one shadow-casting spot light (item 9a). Defined once here so 9b's
       shadow matrix reads the SAME pos/dir/cone — lit cone and shadow agree. */
    vec3        light_pos;
    vec3        light_target;    /* aim point; dir = normalize(target - pos) */
    vec3        light_color;
    float       light_intensity; /* high: inverse-square falloff at room scale */
    float       light_inner_deg; /* full-bright within this half-angle */
    float       light_outer_deg; /* fades to black by this half-angle */
    /* shadow map (item 9b): depth from the light's POV; fixed size, not resized */
    RhiRenderTarget shadow_rt;
    RhiPipeline shadow_pipeline;       /* the depth-only pass */
    RhiPipeline shadow_debug_pipeline; /* fullscreen depth-map inspector */
    sol_bool    show_shadow_map;       /* 'M' toggles the inspector view */
    sol_bool    m_was_down;            /* edge-detect the inspector toggle */
    /* environment / skybox (Phase A): equirectangular HDR, linear radiance */
    RhiTexture  skybox_tex;            /* equirect HDR; 0 if the .hdr failed to load */
    RhiPipeline skybox_pipeline;       /* fullscreen-triangle equirect draw (A2; also the equirect->cube tool) */
    RhiTexture  env_cubemap;           /* equirect baked into a cubemap (B1); 0 if none */
    RhiPipeline skybox_cube_pipeline;  /* on-screen skybox sampling the cubemap (B1) */
    RhiTexture  irradiance_cubemap;    /* diffuse irradiance convolution (B2) */
    RhiPipeline irradiance_pipeline;   /* the convolution pass */
    sol_bool    show_irradiance;       /* 'I' toggles the skybox source: env vs irradiance */
    sol_bool    i_was_down;            /* edge-detect the inspector toggle */
    RhiTexture  prefilter_cubemap;     /* specular prefilter, roughness mip chain (C1) */
    RhiPipeline prefilter_pipeline;    /* the GGX importance-sampling pass */
    sol_bool    show_prefilter;        /* 'P' cycles the prefilter-mip inspector */
    int         prefilter_mip;         /* which roughness level the inspector shows */
    sol_bool    p_was_down;
    RhiRenderTarget brdf_lut_rt;       /* BRDF integration LUT, 2D RG (C2) */
    RhiPipeline     brdf_lut_pipeline;
    sol_bool    f_was_down;     /* edge-detect the walk/fly toggle key */
    sol_bool    l_was_down;     /* edge-detect the scene-reload key (P3 item 1) */
    sol_bool    r_was_down;     /* edge-detect the mirror-rescan key (P3 item 6c) */
    sol_bool    bs_was_down;    /* edge-detect tombstone dismissal (Backspace) */
    sol_bool    g_was_down;     /* edge-detect gather-to-workspace (P3 item 6d) */
    /* room graph (P3 item 7) */
    sol_u32     current_room;   /* containing room's anchor handle; 0 = outside (derived per frame) */
    float       ambient_scale;  /* eased toward the room's ambient (sealed = dim) */
    /* text (P3 item 3) */
    Font       *ui_font;        /* DejaVu Sans, SDF atlas; NULL if load failed */
    Font       *mono_font;      /* DejaVu Sans Mono — code + aligned readouts */
    int         text_inspect;   /* 'T' cycles: 0 off, 1 atlas, 2 type specimen */
    sol_bool    t_was_down;
    /* mouse-look / cursor state (item 3c/3d) */
    double      mouse_last_x, mouse_last_y;
    int         mouse_skip;        /* swallow N frames of delta after a cursor-mode change */
    sol_bool    tab_was_down;
    double      scroll_accum;      /* scroll events accumulate here, drained per frame */
    /* picking / selection (item 4) */
    sol_u32     selected_handle;   /* 0 = none */
    sol_bool    lmb_was_down;
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
    sol_u32     drag_board;        /* board hosting the carry; 0 = ground mode */
    float       drag_board_ox;     /* grab offset in board-local XY            */
    float       drag_board_oy;
    sol_bool    b_was_down;        /* edge-detect spawn-board (B) */
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
    sol_bool    v_was_down;        /* edge-detect mint-codex (V, item 9) */
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
    const Font *reader_font;       /* mono for code, sans for prose */
    float       reader_px2m;       /* body text scale, meters per font px */
    sol_bool    arrow_l_was, arrow_r_was;     /* page-turn edges */
} AppState;

#define READER_IDLE      0
#define READER_RISING    1
#define READER_OPEN      2
#define READER_RETURNING 3
#define READER_RISE_SECS 0.45f
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
static void glb_attach_parts(AppState *state, sol_u32 anchor, const GlbModel *model) {
    Aabb    b      = union_bounds(model);
    vec3    center = vec3_scale(vec3_add(b.min, b.max), 0.5f);
    sol_u32 m;
    for (m = 0; m < model->count; m++) {
        sol_u32 h = scene_add(&state->scene, anchor, model->parts[m].mesh,
                              vec3_make(-center.x, -center.y, -center.z),
                              quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_material_set(&state->scene, h, model->parts[m].material);
        scene_meta_set(&state->scene, h, "derived", "1");
    }
}

/* Load a glTF .glb, auto-fit it (longest side ~1.2 units), and stand it on the
   floor centered at (x,z). Node transforms are already baked by the loader.
   Returns the group anchor's handle (0 on failure) so the caller can re-pose it. */
static sol_u32 add_glb_to_scene(AppState *state, const char *path, float x, float z) {
    GlbModel model;
    Aabb     b;
    vec3     center, ext;
    float    maxdim, scale;
    sol_u32  anchor;
    Mesh     empty = {0};

    if (!glb_load(path, &model)) { fprintf(stderr, "glb load failed: %s\n", path); return 0; }

    b      = union_bounds(&model);
    center = vec3_scale(vec3_add(b.min, b.max), 0.5f);
    ext    = vec3_sub(b.max, b.min);
    maxdim = ext.x;
    if (ext.y > maxdim) maxdim = ext.y;
    if (ext.z > maxdim) maxdim = ext.z;
    scale  = (maxdim > 0.0001f) ? (1.2f / maxdim) : 1.0f;

    printf("glb: %s -> %u part(s), autoscale %.4f\n", path, (unsigned)model.count, scale);

    /* group the parts under one empty anchor placed at the model's CENTER (so the
       anchor's rotation pivots the model in place); the anchor's y lifts the model
       so it sits on the floor at (x,z). The anchor carries the glb ref (6e):
       it is the durable identity; the parts are derived data. */
    anchor = scene_add(&state->scene, 0, empty,
                       vec3_make(x, scale * (center.y - b.min.y), z),
                       quat_identity(), vec3_make(scale, scale, scale));
    glb_attach_parts(state, anchor, &model);
    scene_meta_set(&state->scene, anchor, "name", path);   /* the pin's tag (item 6) */
    scene_meta_set(&state->scene, anchor, "glb", path);    /* geometry by reference (6e) */
    glb_free(&model);
    return anchor;
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
        if (!path || !glb_load(path, &model)) {
            fprintf(stderr, "glb re-import failed: %s — anchor kept, body missing\n",
                    path ? path : "(no path)");
            continue;                       /* the placed anchor outlives its asset */
        }
        glb_attach_parts(state, anchors[i], &model);
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

/* The top-level ancestor of an object: walk the parent chain to the root. For a
   grouped import this is the model anchor (so every part shares one group root);
   for a root-level object it's the object itself. Drives group highlighting. */
static sol_u32 group_root(Scene *s, sol_u32 handle) {
    SceneObject *o = scene_get(s, handle);
    while (o && o->parent != 0) {
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
    if (st->camera.mode == CAMERA_ORBIT && ww > 0 && wh > 0) {
        glfwGetCursorPos(w, &mx, &my);
        nx = 2.0f * (float)mx / (float)ww - 1.0f;
        ny = 1.0f - 2.0f * (float)my / (float)wh;
    }
    return camera_ray(&st->camera, nx, ny, aspect);
}

/* Side-effect-free pick through an NDC point: the nearest hit's handle (0 =
   none). do_pick adds the selection/focus behavior on top; the drag path
   (item 4) needs just the hit. */
static sol_u32 pick_at(AppState *st, GLFWwindow *w, float ndc_x, float ndc_y, float *t_out) {
    int   ww, wh;
    float aspect;
    Ray   ray;
    glfwGetWindowSize(w, &ww, &wh);                 /* cursor is in window coords */
    aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
    ray = camera_ray(&st->camera, ndc_x, ndc_y, aspect);
    return scene_pick(&st->scene, ray, t_out);
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

/* Cast a pick ray through a screen point (NDC) and select the nearest object,
   reporting its stable handle + nid. In orbit, a hit re-targets the pivot. */
static void do_pick(AppState *st, GLFWwindow *w, float ndc_x, float ndc_y) {
    float   t;
    sol_u32 hit;

    hit = pick_at(st, w, ndc_x, ndc_y, &t);
    st->selected_handle = hit;
    if (hit) {
        SceneObject *o = scene_get(&st->scene, hit);
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
    if (ho->kind != KIND_PLAIN) {
        target = hit;       /* cards (item 6) move INDIVIDUALLY, though parented
                               to a room or a board */
    } else {
        if (object_is_arrow(&st->scene, hit)) return;   /* derived geometry: an
                                                           arrow follows its cards,
                                                           it is never dragged */
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
    if (!ray_vs_plane(r, vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 1.0f, 0.0f), &t) ||
        t > DRAG_MAX_DIST)
        return;                                  /* no floor under the cursor: look
                                                    down a little to start a drag */
    st->drag_handle    = target;
    st->drag_offset    = vec3_sub(wpos, vec3_add(r.origin, vec3_scale(r.dir, t)));
    st->drag_start_pos = wpos;                   /* world, throughout */
    st->drag_moved     = SOL_FALSE;
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
static void apply_kind_materials(Scene *s);    /* likewise */
static int  rescan_mirrors(AppState *st);      /* likewise */
static sol_bool load_palace(AppState *st);     /* likewise */
static void note_edit_end(AppState *st);       /* defined with on_key below */

/* The codex mint's tiny LCG (item 9): varied-but-PERSISTENT books — the
   drawn parameters land in the parts' mesh attrs, so a minted book keeps
   its build forever; the generator is only consulted at mint time. */
static unsigned g_mint_rng = 0;
static float mint_range(float lo, float hi) {
    g_mint_rng = g_mint_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)((g_mint_rng >> 16) & 0x7FFF) / 32767.0f;
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

static void reader_free_text(AppState *st) {
    free(st->reader_text);
    free(st->reader_line_off);
    st->reader_text       = (char *)0;
    st->reader_line_off   = (int *)0;
    st->reader_line_count = 0;
    st->reader_text_len   = 0;
    st->reader_spread     = 0;
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
    st->reader_font = reader_wants_mono(st, path) ? st->mono_font : st->ui_font;
    if (!st->reader_font) return;

    wb = bp[0] - bp[4];
    zh = bp[1] * 0.5f - bp[4];
    mg = wb * 0.06f;
    xf = wb * BOOK_GUTTER_FRAC;
    field_w = wb - xf - 2.0f * mg;
    field_h = 2.0f * zh - 2.0f * mg;
    st->reader_px2m = (bp[1] * 0.026f) / font_line_height(st->reader_font);
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
    reader_load_content(st, o->content);       /* the card's file; a codex
                                                  carries none (yet) */
    printf("reading '%s' — Esc or click to put it back; left/right turn pages\n",
           object_label(s, root, lbuf));
}

/* Send the book home — from wherever it is, even mid-rise. */
static void reader_close(AppState *st) {
    if (st->reader_state == READER_IDLE || st->reader_state == READER_RETURNING)
        return;
    st->reader_a_pos = st->reader_pos;
    st->reader_a_rot = st->reader_rot;
    st->reader_b_pos = st->reader_rest_pos;
    st->reader_b_rot = st->reader_rest_rot;
    st->reader_t     = 0.0f;
    st->reader_state = READER_RETURNING;
}

/* Per-frame: ease along the current arc (position lerp, rotation slerp,
   both under one smoothstep so they arrive together). */
static void reader_update(AppState *st, float dt) {
    float s;
    if (st->reader_state == READER_IDLE || st->reader_state == READER_OPEN)
        return;
    st->reader_t += dt / READER_RISE_SECS;
    if (st->reader_t > 1.0f) st->reader_t = 1.0f;
    s = sol_smoothstep(st->reader_t);
    st->reader_pos = vec3_add(st->reader_a_pos,
                     vec3_scale(vec3_sub(st->reader_b_pos, st->reader_a_pos), s));
    st->reader_rot = quat_slerp(st->reader_a_rot, st->reader_b_rot, s);
    if (st->reader_t >= 1.0f) {
        if (st->reader_state == READER_RISING) {
            st->reader_state = READER_OPEN;
        } else {                                   /* landed: the rig dies */
            st->reader_state  = READER_IDLE;
            st->reader_source = 0;
            mesh_destroy(&st->reader_cover);
            mesh_destroy(&st->reader_block);
            reader_free_text(st);
        }
    }
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

static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {
    float    look = (float)dt * LOOK_SPEED;
    sol_bool f_now, tab_now, m_now, i_now, p_now, l_now, dragging, fp;
    double   mx, my;

    fp = (st->camera.mode != CAMERA_ORBIT);

    /* FOCUS (item 8 piece 5): while a note is open for typing, the keyboard
       is TEXT — chars route to the note (on_char/on_key) and every button
       below goes quiet, so 'w' writes a letter instead of walking. A click
       blurs (saving) without also picking — the standard text-field deal:
       the first click leaves the field, the next one acts. */
    if (st->edit_handle != 0) {
        in->forward = in->back = in->left = in->right = SOL_FALSE;
        in->up = in->down = SOL_FALSE;
        in->look_dx = 0.0f;
        in->look_dy = 0.0f;
        in->zoom    = 0.0f;
        st->scroll_accum = 0.0;
        glfwGetCursorPos(w, &mx, &my);
        {
            sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            if (lmb && !st->lmb_was_down) note_edit_end(st);
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
        if (rnow && !st->arrow_r_was && st->reader_spread + 1 < spreads)
            st->reader_spread++;
        if (lnow && !st->arrow_l_was && st->reader_spread > 0)
            st->reader_spread--;
        st->arrow_l_was = lnow;
        st->arrow_r_was = rnow;
    } else {
        if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
        if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
        if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) in->look_dy += look;
        if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) in->look_dy -= look;
    }

    /* Tab toggles first-person <-> orbit (edge); cursor mode follows */
    tab_now = glfwGetKey(w, GLFW_KEY_TAB) == GLFW_PRESS;
    if (tab_now && !st->tab_was_down) {
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
    } else if (fp || (dragging && st->drag_handle == 0)) {
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

        if (lmb && !st->lmb_was_down) {                 /* ---- press ---- */
            st->press_x = mx;
            st->press_y = my;
            if (st->reader_state != READER_IDLE) {
                reader_close(st);                       /* click-away, like blur:
                                                           this press only closes */
            } else if (fp) {
                do_pick(st, w, 0.0f, 0.0f);             /* select on press, as before */
                if (try_connect(st, st->selected_handle)) {
                    /* the press completed a connection — no drag */
                } else if (st->selected_handle != 0 && st->selected_handle != st->page_handle)
                    drag_begin(st, w, st->selected_handle);
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
                    if (o->kind == KIND_ALIAS || o->kind == KIND_NOTE)
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
                    } else {                           /* ---- ground mode ---- */
                        float t;
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
                            st->drag_offset = vec3_make(0.0f, 0.0f, 0.0f);
                            arrows_rebuild(st);        /* its edges go dormant */
                        }
                        if (ray_vs_plane(r, vec3_make(0.0f, 0.0f, 0.0f),
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
            if (st->drag_handle != 0 && st->drag_moved) {
                SceneObject *o = scene_get(&st->scene, st->drag_handle);
                if (o && o->kind != KIND_PLAIN) {       /* a dragged card is PLACED:
                                                           the tray's claim ends here */
                    const char *u = scene_meta_get(&st->scene, st->drag_handle, "unplaced");
                    if (u && strcmp(u, "1") == 0) {
                        scene_meta_set(&st->scene, st->drag_handle, "unplaced", "0");
                        apply_kind_materials(&st->scene);   /* its tray glow goes out */
                    }
                }
                if (o && (o->kind == KIND_FILE || o->kind == KIND_FOLDER) && o->content) {
                    /* a mirror's record never leaves its room (§1.3: membership
                       follows disk) — dropping it on a board snaps the record
                       home and pins an ALIAS at the drop point instead */
                    vec3    blocal;
                    sol_u32 board = board_under_ray(st, pick_ray(st, w), &blocal);
                    if (board != 0) {
                        const char *cpath = o->content;     /* heap string: the
                                                               pointer survives
                                                               the scene_add */
                        char        lbuf[16];
                        const char *nm = object_label(&st->scene, st->drag_handle, lbuf);
                        Mesh        empty;
                        vec3        one = vec3_make(1.0f, 1.0f, 1.0f);
                        float       ch  = mesh_ref_param("card", (const float *)0, 0, "h");
                        sol_u32     a;
                        o->parent = st->drag_prev_parent;   /* snap home */
                        o->pos    = st->drag_prev_pos;
                        o->rot    = st->drag_prev_rot;
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
                        st->selected_handle = a;
                        printf("pinned alias '%s' to the board — the record stays home\n", nm);
                        o = scene_get(&st->scene, st->drag_handle);  /* re-fetch:
                                                               scene_add may move
                                                               the objects array */
                    }
                }
                if (scene_save(&st->scene, "scene.stml"))
                    printf("placed #%u at (%.2f, %.2f, %.2f) — saved\n",
                           (unsigned)st->drag_handle,
                           o ? (double)o->pos.x : 0.0,
                           o ? (double)o->pos.y : 0.0,
                           o ? (double)o->pos.z : 0.0);
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
        }
        st->lmb_was_down = lmb;
    }

    /* F toggles walk/fly in first person (edge) */
    f_now = glfwGetKey(w, GLFW_KEY_F) == GLFW_PRESS;
    if (f_now && !st->f_was_down && st->camera.mode != CAMERA_ORBIT)
        st->camera.mode = (st->camera.mode == CAMERA_WALK) ? CAMERA_FLY : CAMERA_WALK;
    st->f_was_down = f_now;

    /* M toggles the shadow-map inspector (item 9b, edge) */
    m_now = glfwGetKey(w, GLFW_KEY_M) == GLFW_PRESS;
    if (m_now && !st->m_was_down)
        st->show_shadow_map = !st->show_shadow_map;
    st->m_was_down = m_now;

    /* I toggles the skybox source: env cubemap vs irradiance map (B2, edge) */
    i_now = glfwGetKey(w, GLFW_KEY_I) == GLFW_PRESS;
    if (i_now && !st->i_was_down && st->irradiance_cubemap.id)
        st->show_irradiance = !st->show_irradiance;
    st->i_was_down = i_now;

    /* P cycles the prefilter inspector: off -> roughness 0 -> .. -> 1 -> off (C1) */
    p_now = glfwGetKey(w, GLFW_KEY_P) == GLFW_PRESS;
    if (p_now && !st->p_was_down && st->prefilter_cubemap.id) {
        if (!st->show_prefilter) { st->show_prefilter = SOL_TRUE; st->prefilter_mip = 0; }
        else {
            st->prefilter_mip++;
            if (st->prefilter_mip >= PREFILTER_MIPS) st->show_prefilter = SOL_FALSE;
        }
    }
    st->p_was_down = p_now;

    /* T cycles the text inspectors (P3 item 3): off -> SDF atlas -> specimen */
    {
        sol_bool t_now = glfwGetKey(w, GLFW_KEY_T) == GLFW_PRESS;
        if (t_now && !st->t_was_down && st->ui_font)
            st->text_inspect = (st->text_inspect + 1) % 3;
        st->t_was_down = t_now;
    }

    /* R reconciles every mirror to its disk (item 6c, edge): new files tray
       up unplaced, vanished files tombstone (your notes survive), returned
       files resurrect in place. Membership changes are real -> saved. */
    {
        sol_bool r_now = glfwGetKey(w, GLFW_KEY_R) == GLFW_PRESS;
        if (r_now && !st->r_was_down) {
            int total = rescan_mirrors(st);
            if (total > 0) scene_save(&st->scene, "scene.stml");
            printf("rescan: %d change(s)\n", total);
        }
        st->r_was_down = r_now;
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

    /* B spawns a whiteboard facing you (item 8): plain furniture that
       persists like everything else; drag ALIAS/NOTE cards onto its face. */
    {
        sol_bool b_now = glfwGetKey(w, GLFW_KEY_B) == GLFW_PRESS;
        if (b_now && !st->b_was_down) {
            Mesh    empty;
            vec3    f = camera_forward(&st->camera);
            vec3    pos, one;
            float   yaw;
            sol_u32 h;
            f.y = 0.0f;
            if (vec3_dot(f, f) < 1e-6f) f = vec3_make(0.0f, 0.0f, -1.0f);
            f   = vec3_normalize(f);
            pos = vec3_add(st->camera.pos, vec3_scale(f, 2.2f));
            pos.y = 0.9f;                  /* bottom-origin: face center ~1.5 */
            yaw = atan2f(-f.x, -f.z);      /* board +Z looks back at you */
            one = vec3_make(1.0f, 1.0f, 1.0f);
            memset(&empty, 0, sizeof empty);
            h = scene_add(&st->scene, 0, empty, pos,
                          quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw), one);
            scene_mesh_ref_set(&st->scene, h, "board");
            scene_meta_set(&st->scene, h, "name", "board");
            scene_resolve_meshes(&st->scene);
            st->selected_handle = h;
            scene_save(&st->scene, "scene.stml");
            printf("board #%u spawned — drag cards onto it\n", (unsigned)h);
        }
        st->b_was_down = b_now;
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
        if (n_now && !st->n_was_down) {
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
                pos.y = 0.0f;                  /* bottom-origin card on the floor */
                yaw = atan2f(-f.x, -f.z);      /* facing you */
                h = scene_add(&st->scene, 0, empty, pos,
                              quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw), one);
            }
            scene_kind_set(&st->scene, h, KIND_NOTE);
            scene_meta_set(&st->scene, h, "name", "note");
            scene_meta_set(&st->scene, h, "text",
                           "press Enter to edit me");
            scene_mesh_ref_set(&st->scene, h, "card");
            if (board != 0) {
                SceneObject *no = scene_get(&st->scene, h);
                float ch = mesh_ref_param("card", (const float *)0, 0, "h");
                if (no) no->pos = board_pin_pos(&st->scene, board, h,
                                                blocal, 0.0f, -0.5f * ch);
            }
            scene_resolve_meshes(&st->scene);
            apply_kind_materials(&st->scene);
            st->selected_handle = h;
            scene_save(&st->scene, "scene.stml");
            printf("note #%u spawned%s\n", (unsigned)h,
                   board ? " on the board" : "");
        }
        st->n_was_down = n_now;
    }

    /* V mints a CODEX in front of you (item 9): a procedural bound book —
       cover + page block as a small GROUP, each part wearing its own
       material. Proportions draw from real binding ranges and PERSIST
       (they live in the parts' mesh attrs); press it a few times for a
       shelf of individuals. */
    {
        sol_bool v_now = glfwGetKey(w, GLFW_KEY_V) == GLFW_PRESS;
        if (v_now && !st->v_was_down) {
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
            pos.y = p[1] * 0.5f;                  /* standing: height spans +-h/2 */
            yaw = atan2f(-f.z, f.x);              /* the spine (-x) faces you */
            memset(&empty, 0, sizeof empty);
            anchor = scene_add(&st->scene, 0, empty, pos,
                               quat_mul(quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw),
                                        quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f),
                                                             sol_radians(-90.0f))),
                               one);
            scene_meta_set(&st->scene, anchor, "name", "codex");
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
        st->v_was_down = v_now;
    }

    /* Backspace dismisses a selected TOMBSTONE — manual, deliberate (the 6c
       decision): the system never throws away the marker for you. Item 8
       extends it to ARROWS: deleting the edge object deletes the relation. */
    {
        sol_bool bs_now = glfwGetKey(w, GLFW_KEY_BACKSPACE) == GLFW_PRESS;
        if (bs_now && !st->bs_was_down && st->selected_handle != 0) {
            SceneObject *o = scene_get(&st->scene, st->selected_handle);
            if (o && o->kind == KIND_TOMBSTONE) {
                char        lbuf[16];
                const char *label = object_label(&st->scene, st->selected_handle, lbuf);
                printf("dismissed tombstone: %s\n", label);
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
            }
        }
        st->bs_was_down = bs_now;
    }

    /* L reverts to the palace on disk mid-session (since 6e the startup
       loads it automatically; L remains the manual "reload my save" key).
       Old GPU meshes are not destroyed (glb parts may share buffers) — a
       bounded leak for a manual key; the asset registry owns this later. */
    l_now = glfwGetKey(w, GLFW_KEY_L) == GLFW_PRESS;
    if (l_now && !st->l_was_down) {
        if (load_palace(st))
            printf("reloaded scene.stml: %u objects\n", (unsigned)st->scene.count);
        else
            fprintf(stderr, "scene.stml did not load — keeping the live scene\n");
    }
    st->l_was_down = l_now;

    /* exposure scrub: '[' down, ']' up (held; dt-scaled). The readout lives
       in the debug panel now (3c) — the window-title hack is retired. */
    {
        float erate = (float)dt * 1.5f;
        if (glfwGetKey(w, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) st->exposure += erate;
        if (glfwGetKey(w, GLFW_KEY_LEFT_BRACKET)  == GLFW_PRESS) st->exposure -= erate;
        if (st->exposure < 0.1f) st->exposure = 0.1f;
        if (st->exposure > 8.0f) st->exposure = 8.0f;
    }
}

/* Decode a page image with stb (via image.c) and upload it as an sRGB texture.
   Returns a zero handle if the file is missing/undecodable. */
static RhiTexture load_texture(const char *path) {
    Image      img;
    RhiTexture tex;
    tex.id = 0;
    if (image_load(path, &img)) {
        tex = rhi_create_texture(img.pixels, img.w, img.h, RHI_TEX_SRGB8);
        image_free(&img);
    } else {
        fprintf(stderr, "image load failed: %s\n", path);
    }
    return tex;
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

/* The mesh-ref resolver, GPU half (P3 item 1): realize geometry for every
   object whose ref names a generator and whose mesh is still empty. Runs
   AFTER rhi_init (it uploads) and after scene_load or scene-building — the
   data phase stays pure (headless iotest keeps working), realization happens
   here. An unknown ref leaves the object as a transform-only empty, warned:
   placed data must outlive a missing generator (it re-saves intact). */
static void scene_resolve_meshes(Scene *s) {
    sol_u32 i;
    for (i = 0; i < s->count; i++) {
        SceneObject *o = &s->objects[i];
        MeshBuilder  mb;
        if (!o->mesh_ref || o->mesh.index_count != 0) continue;
        if (strcmp(o->mesh_ref, "arrow") == 0) continue;   /* scene-derived:
                                                              arrows_rebuild owns it */
        mb_init(&mb);
        if (mesh_ref_build(o->mesh_ref, o->mesh_params, o->mesh_param_count, &mb)) {
            o->mesh = mesh_from_builder(&mb);
        } else {
            fprintf(stderr, "scene: unknown mesh ref \"%s\" on %s — left empty\n",
                    o->mesh_ref, o->nid ? o->nid : "(no nid)");
        }
        mb_free(&mb);
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

/* Load the palace from scene.stml and bring it fully to life (6e): glb
   bodies re-imported from their anchors' refs, procedural meshes resolved,
   kind colors applied, runtime handles re-found by name, the page's image
   (a runtime texture handle) rebound, and the mirrors reconciled against
   today's disk — the folder may have changed while the palace slept.
   SOL_FALSE if the file is absent or won't parse. */
static sol_bool load_palace(AppState *st) {
    Scene fresh;
    int   changes;
    if (!scene_load(&fresh, "scene.stml")) return SOL_FALSE;
    scene_free(&st->scene);
    st->scene = fresh;
    scene_reimport_glbs(st);
    scene_resolve_meshes(&st->scene);
    arrows_rebuild(st);              /* edges re-derive from the loaded cards */
    apply_kind_materials(&st->scene);
    bind_runtime_handles(st);
    {
        SceneObject *p = scene_get(&st->scene, st->page_handle);
        if (p) p->material.albedo_tex = st->albedo_tex;
    }
    changes = rescan_mirrors(st);
    if (changes > 0) {
        scene_save(&st->scene, "scene.stml");
        printf("reconciled on load: %d change(s)\n", changes);
    }
    return SOL_TRUE;
}

/* First-run population: the default demo palace. Runs only when there is
   no scene.stml to load (6e) — after that, the file IS the world. */
static void populate_default_scene(AppState *state) {
    Mesh    empty = {0};            /* zero mesh -> an empty (transform-only) */
    sol_u32 anchor, floor, page;

    /* scene: floor (root), an empty anchor (root), and the box as the
       anchor's child — so spinning the anchor makes the box orbit it.
       Geometry is BY REFERENCE even at build time (P3 item 1): objects are
       added empty carrying a mesh ref, and one resolver pass realizes them —
       the exact path a scene loaded from disk takes, so the built world and
       the reloaded world cannot drift apart. */
    scene_init(&state->scene);
    /* the grid sits a hair above the item-5 room's floor: two coplanar quads
       at y=0 would z-fight (equal depth -> per-pixel flicker). It reads as a
       rug now that the room provides the actual floor. */
    floor = scene_add(&state->scene, 0, empty,
              vec3_make(0.0f, 0.02f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    anchor = scene_add(&state->scene, 0, empty,
              vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    state->anchor_handle = anchor;
    state->box_handle = scene_add(&state->scene, anchor, empty,
              vec3_make(1.5f, 1.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));

    /* the parchment reading surface: an upright page quad facing +Z (~3:4,
       matching paper-picture.png; the emitter lives in mesh.c now) */
    page = scene_add(&state->scene, 0, empty,
              vec3_make(-2.0f, 1.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
    state->page_handle = page;

    scene_mesh_ref_set(&state->scene, floor, "grid");
    scene_mesh_ref_set(&state->scene, state->box_handle, "box");
    scene_mesh_ref_set(&state->scene, page, "page");

    /* The palace's first ROOMS (items 5+7): three §1.10 anchors and the two
       edge EMBODIMENTS that connect them — a shared thick doorway wall (the
       hall's open east side into a sealed cell) and a path across the void
       (the hall's open north side out to a bare folly platform). The graph
       lives in the existing relation slots: each connector carries
       `connects` edges to both rooms. Geometry children parent to their
       anchor (drag-excluded via room_type); connectors parent to the hall. */
    {
        Material stone = material_default();
        Material wood  = material_default();
        sol_u32  hall, cell, folly, sh, sc, sf, doorwall, path;
        float    hall_p[8], cell_p[8], folly_p[8], wall_p[6], path_p[3];
        quat     yaw90 = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f),
                                              sol_radians(90.0f));

        hall_p[0] = 12.0f; hall_p[1] = 10.0f; hall_p[2] = 3.5f;
        hall_p[3] = 0.0f;  /* north open: the path leaves here */
        hall_p[4] = 0.0f;  /* east open: the doorway wall closes it */
        hall_p[5] = 1.0f;  hall_p[6] = 1.0f;  hall_p[7] = 1.0f;

        cell_p[0] = 6.0f;  cell_p[1] = 6.0f;  cell_p[2] = 2.5f;
        cell_p[3] = 1.0f;  cell_p[4] = 1.0f;  cell_p[5] = 1.0f;
        cell_p[6] = 0.0f;  /* west open: the shared doorway wall is there */
        cell_p[7] = 1.0f;

        folly_p[0] = 5.0f; folly_p[1] = 5.0f; folly_p[2] = 3.0f;   /* h = its volume only */
        folly_p[3] = 0.0f; folly_p[4] = 0.0f; folly_p[5] = 0.0f;   /* a bare platform */
        folly_p[6] = 0.0f; folly_p[7] = 0.0f;

        wall_p[0] = 10.0f; wall_p[1] = 3.5f;                  /* spans the hall's east side */
        wall_p[2] = 4.5f;  wall_p[3] = 1.0f; wall_p[4] = 2.2f;
        wall_p[5] = 0.15f;

        path_p[0] = 4.5f;  path_p[1] = 1.5f; path_p[2] = 0.15f;

        hall = scene_add(&state->scene, 0, empty,
                  vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_meta_set(&state->scene, hall, "room_type", "room");
        scene_meta_set(&state->scene, hall, "name", "the hall");
        scene_meta_set(&state->scene, hall, "source_path", "");   /* item 6 fills this */
        sh = scene_add(&state->scene, hall, empty,
                  vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&state->scene, sh, "room");
        scene_mesh_params_set(&state->scene, sh, hall_p, 8);

        /* the cell: interior x in [6.15, 12.15] — past the wall's thickness.
           It is the palace's first MIRROR (item 6): it reflects the solarium
           repo itself. Membership follows disk; arrangement follows you. */
        cell = scene_add(&state->scene, 0, empty,
                  vec3_make(9.15f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_meta_set(&state->scene, cell, "room_type", "mirror");
        scene_meta_set(&state->scene, cell, "name", "the archive");
        scene_meta_set(&state->scene, cell, "source_path", ".");
        scene_meta_set(&state->scene, cell, "ambient", "0.45");   /* doorway-sealed: its own
                  shell has an open side (the shared wall closes it), so the
                  flags-derived answer would be "open" — the builder knows better */
        sc = scene_add(&state->scene, cell, empty,
                  vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&state->scene, sc, "room");
        scene_mesh_params_set(&state->scene, sc, cell_p, 8);

        /* the folly: a platform floating north, past the path */
        folly = scene_add(&state->scene, 0, empty,
                  vec3_make(0.0f, 0.0f, -12.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_meta_set(&state->scene, folly, "room_type", "workspace");
        scene_meta_set(&state->scene, folly, "name", "the folly");   /* the first workspace (6d) */
        sf = scene_add(&state->scene, folly, empty,
                  vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&state->scene, sf, "room");
        scene_mesh_params_set(&state->scene, sf, folly_p, 8);

        /* edge 1: the shared doorway wall (rotated into the YZ plane at the
           hall/cell boundary). ONE wall — two abutting walls would be the
           z-fight lesson at architecture scale. */
        doorwall = scene_add(&state->scene, hall, empty,
                  vec3_make(6.075f, 0.0f, 0.0f), yaw90, vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&state->scene, doorwall, "wall");
        scene_mesh_params_set(&state->scene, doorwall, wall_p, 6);
        scene_rel_add(&state->scene, doorwall, "connects", hall);
        scene_rel_add(&state->scene, doorwall, "connects", cell);

        /* edge 2: the path across the void (length runs local X; rotated to
           span world Z from the hall's north opening to the folly) */
        path = scene_add(&state->scene, hall, empty,
                  vec3_make(0.0f, 0.0f, -7.25f), yaw90, vec3_make(1.0f, 1.0f, 1.0f));
        scene_mesh_ref_set(&state->scene, path, "path");
        scene_mesh_params_set(&state->scene, path, path_p, 3);
        scene_rel_add(&state->scene, path, "connects", hall);
        scene_rel_add(&state->scene, path, "connects", folly);

        stone.base_color = vec3_make(0.58f, 0.55f, 0.50f);   /* warm gray stone */
        stone.roughness  = 0.92f;
        scene_material_set(&state->scene, sh, stone);
        scene_material_set(&state->scene, sc, stone);
        scene_material_set(&state->scene, sf, stone);
        stone.base_color = vec3_make(0.62f, 0.58f, 0.52f);   /* the wall a shade lighter */
        scene_material_set(&state->scene, doorwall, stone);
        wood.base_color = vec3_make(0.46f, 0.36f, 0.27f);    /* the path: timber */
        wood.roughness  = 0.85f;
        scene_material_set(&state->scene, path, wood);

        /* the mirror reflects THIS repo: one card per disk entry, waiting in
           the tray. The room's crowding IS the folder's size, felt (item 6). */
        {
            int n = room_mirror_scan(&state->scene, cell, ".");
            if (n >= 0) printf("mirror 'the archive' <- '.': %d cards\n", n);
            else        fprintf(stderr, "mirror: could not scan '.'\n");
        }
    }

    scene_resolve_meshes(&state->scene);
    apply_kind_materials(&state->scene);
    state->floor_handle = floor;             /* room structure: drag excludes it */

    /* PBR materials for the procedural objects (item 8a) */
    {
        Material m = material_default();
        m.base_color = vec3_make(0.85f, 0.45f, 0.35f);   /* the box keeps its warm orange */
        m.roughness  = 0.5f;
        scene_material_set(&state->scene, state->box_handle, m);
    }
    {
        Material m = material_default();
        m.base_color = vec3_make(0.5f, 0.5f, 0.5f);      /* a neutral, fairly rough floor */
        m.roughness  = 0.85f;
        scene_material_set(&state->scene, floor, m);
    }
    {
        Material m = material_default();
        m.albedo_tex = state->albedo_tex;                /* the parchment image; matte */
        m.roughness  = 0.8f;
        scene_material_set(&state->scene, page, m);
    }

    /* item 6: real glTF models standing in the room */
    add_glb_to_scene(state, "book.glb",   2.0f,  0.0f);
    add_glb_to_scene(state, "candle.glb", 2.0f,  2.0f);   /* corner, clear of the centred sword */

    /* the showcase sword (item 8b/8d): it orbits an invisible point at a tight
       radius, leaning 15deg out, while spinning on its own long axis — so the
       metal sweeps the directional light. An outer 'precession' anchor does the
       orbit; the sword group hangs off it at a small radial offset (update() sets
       the precession spin + the sword's standup/lean/axial-spin). */
    {
        Mesh        empty   = {0};
        sol_u32     precess = scene_add(&state->scene, 0, empty,
                                        vec3_make(0.0f, 1.3f, 0.0f),   /* the invisible point */
                                        quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        SceneObject *sw;
        state->sword_precess_handle = precess;
        state->sword_handle = add_glb_to_scene(state, "sword.glb", 0.0f, 0.0f);
        sw = scene_get(&state->scene, state->sword_handle);
        if (sw) {
            sw->parent = precess;                       /* orbit the precession anchor */
            sw->pos    = vec3_make(0.3f, 0.0f, 0.0f);   /* tight radius (the cube's is 1.5) */
        }
    }

    /* names: the pin's tag (item 6) reads these */
    scene_meta_set(&state->scene, state->box_handle, "name", "the box");
    scene_meta_set(&state->scene, page, "name", "the page");

    /* overbuilt slots demo (mostly empty this phase) */
    scene_meta_set(&state->scene, state->box_handle, "title",  "Test Box");
    scene_meta_set(&state->scene, state->box_handle, "author", "Solarium");
    scene_rel_add(&state->scene, state->box_handle, "orbits", state->anchor_handle);
    scene_content_set(&state->scene, state->box_handle, "notes/box.txt");

    /* Persist the fresh default — unless a scene.stml EXISTS but failed to
       load (corrupt?): never overwrite what might be the user's palace. */
    {
        FILE *probe = fopen("scene.stml", "rb");
        if (probe) {
            fclose(probe);
            printf("scene.stml exists but did not load — NOT overwriting it\n");
        } else if (scene_save(&state->scene, "scene.stml")) {
            printf("saved scene -> scene.stml\n");
        }
    }
}

/* One-time setup: build the pipeline + meshes, populate the scene. */
static int init_scene(AppState *state) {
    RhiShader shader;
    RhiPipelineDesc desc = {0};  /* {0} all descs: a future field defaults off, not garbage */

    shader = rhi_create_shader(VERTEX_SRC, FRAGMENT_SRC);
    if (!shader.id) return 0;

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

    /* the fullscreen post pass: no vertex attributes (gl_VertexID builds the
       triangle), no depth test (it's a screen-space overlay) */
    {
        RhiShader       post_shader;
        RhiPipelineDesc post_desc = {0};
        post_shader = rhi_create_shader(POST_VERTEX_SRC, POST_FRAGMENT_SRC);
        if (!post_shader.id) return 0;
        post_desc.shader     = post_shader;
        post_desc.attr_count = 0;
        post_desc.stride     = 0;
        post_desc.depth_test = SOL_FALSE;
        post_desc.blend      = SOL_FALSE;
        state->post_pipeline = rhi_create_pipeline(&post_desc);
    }

    /* the shadow map + its two pipelines (item 9b): a depth-only pass that reads
       just position, and a fullscreen inspector that shows the depth map. */
    {
        RhiShader       sh_shader, dbg_shader;
        RhiPipelineDesc sh_desc = {0}, dbg_desc = {0};

        state->shadow_rt = rhi_create_depth_target(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
        if (!state->shadow_rt.id) return 0;

        sh_shader = rhi_create_shader(SHADOW_VERTEX_SRC, SHADOW_FRAGMENT_SRC);
        if (!sh_shader.id) return 0;
        sh_desc.shader     = sh_shader;
        sh_desc.attrs[0].location = 0; sh_desc.attrs[0].format = RHI_FORMAT_FLOAT3; sh_desc.attrs[0].offset = 0;
        sh_desc.attr_count = 1;                 /* position only */
        sh_desc.stride     = 12 * sizeof(float);  /* same VBO; skip the other 9 floats */
        sh_desc.depth_test = SOL_TRUE;
        sh_desc.blend      = SOL_FALSE;
        state->shadow_pipeline = rhi_create_pipeline(&sh_desc);

        dbg_shader = rhi_create_shader(POST_VERTEX_SRC, SHADOW_DEBUG_FRAGMENT_SRC);
        if (!dbg_shader.id) return 0;
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
        if (!sky_shader.id) return 0;
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
        if (!cube_shader.id) return 0;
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
        if (!irr_shader.id) return 0;
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
        if (!pre_shader.id) return 0;
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
        if (!brdf_shader.id) return 0;
        brdf_desc.shader     = brdf_shader;
        brdf_desc.attr_count = 0;
        brdf_desc.stride     = 0;
        brdf_desc.depth_test = SOL_FALSE;
        brdf_desc.blend      = SOL_FALSE;
        state->brdf_lut_pipeline = rhi_create_pipeline(&brdf_desc);
    }

    state->albedo_tex = load_texture("paper-picture.png");   /* item 5b: decode via stb */

    /* THE PALACE REMEMBERS ITSELF (6e): an existing scene.stml IS the world —
       loaded and brought fully to life. The default demo palace builds only
       on first run (or after deleting the file to re-mint); L remains the
       manual mid-session revert. */
    if (load_palace(state)) {
        printf("palace loaded from scene.stml (%u objects)\n",
               (unsigned)state->scene.count);
    } else {
        populate_default_scene(state);
    }

    /* spawn standing at the south edge of the scene, facing -Z at eye height
       with a slight downward tilt (2.5 was above the 2.2 doorway lintels) */
    camera_init(&state->camera, vec3_make(0.0f, CAMERA_EYE_HEIGHT, 5.0f),
                sol_radians(-90.0f), sol_radians(-10.0f));
    state->f_was_down = SOL_FALSE;
    state->exposure   = 1.0f;
    state->ambient_scale = 1.0f;   /* zero-init would fade up from black */

    /* the spot light: warm, aimed at the table. Moved INSIDE the item-5 room
       (it sat at y=5, above the 3.5m ceiling — which would have shadowed the
       entire interior); intensity drops with the shorter throw (inverse
       square). */
    state->light_pos       = vec3_make(2.2f, 3.1f, 2.2f);
    state->light_target    = vec3_make(0.0f, 0.5f, 0.0f);
    state->light_color     = vec3_make(1.0f, 0.95f, 0.85f);
    state->light_intensity = 70.0f;
    state->light_inner_deg = 22.0f;
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
        } else {
            printf("skybox HDR: load failed (skybox disabled)\n");
        }
    }

    printf("scene: %u objects (1 empty anchor)\n", (unsigned)state->scene.count);
    printf("box meta: title=\"%s\", author=\"%s\"; %u relations\n",
           scene_meta_get(&state->scene, state->box_handle, "title"),
           scene_meta_get(&state->scene, state->box_handle, "author"),
           (unsigned)scene_get(&state->scene, state->box_handle)->rel_count);
    return 1;
}

static void update(AppState *state, double dt) {
    SceneObject *box, *anchor, *sword;
    quat qy, qx;

    state->angle += (float)dt * 0.8f;

    /* item 7: containment is DERIVED state, recomputed each frame — walking
       through a doorway "transitions" only in the sense that this query's
       answer changes. The ambient eases toward the room's level (an
       exponential glide; no pop at the threshold). */
    {
        float target;
        state->current_room = room_containing(&state->scene, state->camera.pos);
        target = state->current_room
               ? room_ambient(&state->scene, state->current_room) : 1.0f;
        state->ambient_scale += (target - state->ambient_scale)
                              * (1.0f - (float)exp(-dt * 5.0));
    }

    /* spin the anchor -> its child (the box) orbits the origin */
    anchor = scene_get(&state->scene, state->anchor_handle);
    if (anchor) anchor->rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), state->angle);

    /* the box's OWN tumble (local rotation), faster than the orbit */
    box = scene_get(&state->scene, state->box_handle);
    if (box) {
        qy = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), state->angle * 1.5f);
        qx = quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), state->angle * 0.75f);
        box->rot = quat_normalize(quat_mul(qy, qx));
    }

    /* the sword's precession: orbit the invisible point (tight radius) */
    {
        SceneObject *precess = scene_get(&state->scene, state->sword_precess_handle);
        if (precess) precess->rot = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f),
                                                         state->angle * 0.5f);
    }

    /* the sword itself: stand upright (blade +Z -> world +Y), spin on that long
       axis, then lean 15deg out (toward +X = radially outward as the parent
       precesses). Compose tilt * spin * standup (applied standup -> spin -> tilt). */
    sword = scene_get(&state->scene, state->sword_handle);
    if (sword) {
        quat standup = quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), sol_radians(-90.0f));
        quat spin    = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), state->angle * 0.8f);
        quat tilt    = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), sol_radians(-15.0f));
        sword->rot = quat_normalize(quat_mul(quat_mul(tilt, spin), standup));
    }
}

/* The light's view-projection (item 9b). Both the shadow pass and 9c's lighting
   pass call this — they MUST use the identical matrix or the cast shadow won't
   line up with the lit cone. The spot's perspective frustum IS the light matrix:
   fovy = the cone's full angle (+ slack), square map, near/far bracket the scene. */
static mat4 light_view_proj(const AppState *state) {
    mat4  lview = mat4_look_at(state->light_pos, state->light_target,
                               vec3_make(0.0f, 1.0f, 0.0f));
    float lfovy = sol_radians(2.0f * state->light_outer_deg + 6.0f);
    mat4  lproj = mat4_perspective(lfovy, 1.0f, SHADOW_NEAR, SHADOW_FAR);
    return mat4_mul(lproj, lview);
}

static void draw_mesh(const AppState *state, Mesh mesh, mat4 model,
                      mat4 view, mat4 proj, vec3 eye, float highlight,
                      Material mat) {
    mat3 nrm = mat3_normal_matrix(model);

    rhi_set_pipeline(state->pipeline);
    rhi_set_uniform_mat4("uModel",        model.m);
    rhi_set_uniform_mat4("uView",         view.m);
    rhi_set_uniform_mat4("uProj",         proj.m);
    rhi_set_uniform_mat3("uNormalMatrix", nrm.m);
    rhi_set_uniform_vec3("uViewPos",      eye.x, eye.y, eye.z);
    rhi_set_uniform_float("uHighlight",   highlight);

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

        /* shadow map (item 9c): the SAME light matrix as the depth pass, plus the
           depth texture on unit 4 (0-3 are albedo/MR/AO/normal). */
        {
            mat4 lvp = light_view_proj(state);
            rhi_set_uniform_mat4("uLightVP", lvp.m);
            rhi_bind_texture(rhi_render_target_depth_texture(state->shadow_rt), 4);
            rhi_set_uniform_int("uShadowMap", 4);
        }

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

    rhi_bind_vertex_buffer(mesh.vbuffer);
    rhi_bind_index_buffer(mesh.ibuffer);
    rhi_draw_indexed(0, mesh.index_count);
}

/* Ensure the HDR target matches the window's framebuffer size, recreating it on
   resize (the one path that both first-creates and resizes it). A minimized
   window reports 0 — clamp so we never make a zero-size framebuffer. */
static void ensure_render_target(AppState *state, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (state->hdr_rt.id != 0 && state->rt_width == w && state->rt_height == h)
        return;                                   /* already the right size */
    if (state->hdr_rt.id != 0)
        rhi_destroy_render_target(state->hdr_rt);
    state->hdr_rt    = rhi_create_render_target(w, h, RHI_TEX_RGBA16F);
    state->rt_width  = w;
    state->rt_height = h;
}

static void render(AppState *state) {
    float   aspect;
    float   us;        /* UI scale: sizes track the framebuffer (see pass 3) */
    vec3    eye;
    mat4    view, proj;
    sol_u32 i;
    sol_u32 sel_root;

    ensure_render_target(state, state->fb_width, state->fb_height);

    sel_root = selection_root(&state->scene, state->selected_handle);  /* 0 if nothing selected */

    /* ---- pass 0: depth from the light's POV -> the shadow map (item 9b) ---- */
    {
        mat4 lvp = light_view_proj(state);
        rhi_begin_pass(state->shadow_rt, RHI_CLEAR_ALL, 0.0f, 0.0f, 0.0f, 1.0f);  /* clears depth to 1.0 */
        rhi_set_pipeline(state->shadow_pipeline);
        rhi_set_uniform_mat4("uLightVP", lvp.m);
        for (i = 0; i < state->scene.count; i++) {
            const SceneObject *o = &state->scene.objects[i];
            mat4 model;
            if (o->mesh.index_count == 0) continue;   /* empties cast nothing */
            model = scene_world_matrix(&state->scene, o);
            rhi_set_uniform_mat4("uModel", model.m);
            rhi_bind_vertex_buffer(o->mesh.vbuffer);
            rhi_bind_index_buffer(o->mesh.ibuffer);
            rhi_draw_indexed(0, o->mesh.index_count);
        }
        rhi_end_pass();
    }

    /* ---- pass 1: render the scene into the offscreen HDR target ---- */
    rhi_begin_pass(state->hdr_rt, RHI_CLEAR_ALL, 0.10f, 0.12f, 0.15f, 1.0f);

    aspect = (state->fb_height > 0)
           ? (float)state->fb_width / (float)state->fb_height
           : 1.0f;
    eye  = state->camera.pos;                       /* camera drives the view now */
    view = camera_view(&state->camera);
    proj = camera_proj(&state->camera, aspect);

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
        if (state->reader_source != 0 &&
            group_root(&state->scene, o->handle) == state->reader_source)
            continue;                             /* its book is aloft (item 9) */
        model = scene_world_matrix(&state->scene, o);
        hl    = (sel_root != 0 && selection_root(&state->scene, o->handle) == sel_root)
              ? 1.0f : 0.0f;                       /* light the whole selected group */
        draw_mesh(state, o->mesh, model, view, proj, eye, hl, o->material);
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
    }

    /* world text (item 8): card labels + note bodies, drawn as depth-tested
       INK after the opaque geometry, still inside the HDR pass — it rides
       through ACES like everything else. Same atlas as the HUD; the SDF
       fwidth threshold keeps it crisp at any distance. */
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
            float wb    = bp[0] - bp[4];
            float zh    = bp[1] * 0.5f - bp[4];
            float stack = (bp[2] - 2.0f * bp[3]) * 0.5f;
            float xf, fy, mg;
            mat4  bm, page;
            if (stack < 0.004f) stack = 0.004f;
            xf   = wb * BOOK_GUTTER_FRAC;
            fy   = bp[3] + stack + 0.0012f;
            mg   = wb * 0.06f;
            bm   = mat4_from_trs(state->reader_pos, state->reader_rot,
                                 vec3_make(1.0f, 1.0f, 1.0f));
            page = mat4_mul(bm, mat4_mul(mat4_translate(vec3_make(0.0f, fy, 0.0f)),
                       quat_to_mat4(quat_from_axis_angle(
                           vec3_make(1.0f, 0.0f, 0.0f), sol_radians(-90.0f)))));
            if (state->reader_text) {
                int L      = state->reader_lines_per_page;
                int pages  = (state->reader_line_count + L - 1) / L;
                int pl     = state->reader_spread * 2;
                reader_draw_page(state, vp, page, pl,     -wb + mg, zh - mg);
                reader_draw_page(state, vp, page, pl + 1,  xf + mg, zh - mg);
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
            cw = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "w");
            ch = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "h");
            ct = mesh_ref_param("card", o->mesh_params, o->mesh_param_count, "t");
            face = mat4_mul(scene_world_matrix(&state->scene, o),
                            mat4_translate(vec3_make(0.0f, 0.0f, ct * 0.5f + 0.0008f)));
            if (o->kind == KIND_TOMBSTONE) {                /* light ink on slate */
                ink_r = 0.82f; ink_g = 0.82f; ink_b = 0.86f;
            } else {                                        /* dark ink on paper */
                ink_r = 0.10f; ink_g = 0.09f; ink_b = 0.08f;
            }
            margin = 0.025f;
            usable = cw - 2.0f * margin;
            /* the label: the card's name across the top, shrunk to fit */
            nm = object_label(&state->scene, o->handle, lbuf);
            px2m = 0.038f / lh;                             /* ~3.8cm line */
            text_measure(uf, nm, 1.0f, &name_w, (float *)0);
            if (name_w * px2m > usable && name_w > 0.0f)
                px2m = usable / name_w;                     /* shrink, don't clip */
            wtext_block(uf, vp, face, nm,
                        -cw * 0.5f + margin, ch - margin, px2m, 0.0f,
                        ink_r, ink_g, ink_b);
            /* the note body: the inline text meta, wrapped to the card.
               While the note has focus, a caret tails the text — the meta
               mirrors every keystroke, so this renders the typing live. */
            if (o->kind == KIND_NOTE) {
                const char *txt = scene_meta_get(&state->scene, o->handle, "text");
                char        ebuf[EDIT_BUF_CAP + 2];
                if (state->edit_handle == o->handle) {
                    size_t tn = txt ? strlen(txt) : 0;
                    if (tn > sizeof(ebuf) - 2) tn = sizeof(ebuf) - 2;
                    memcpy(ebuf, txt ? txt : "", tn);
                    ebuf[tn]     = '_';
                    ebuf[tn + 1] = '\0';
                    txt = ebuf;
                }
                if (txt && txt[0]) {
                    float bpx2m = 0.028f / lh;              /* ~2.8cm body lines */
                    wtext_block(uf, vp, face, txt,
                                -cw * 0.5f + margin, ch - margin - 0.055f,
                                bpx2m, usable, ink_r, ink_g, ink_b);
                }
            }
        }
    }

    rhi_end_pass();

    /* ---- pass 2: fullscreen pass to the window — sample the HDR buffer, encode
       linear->sRGB (7c adds the tonemap). One triangle, no vertex buffer. ---- */
    {
        RhiRenderTarget screen = {0};   /* {0} = default framebuffer (declaration init, not a compound literal) */
        rhi_begin_pass(screen, RHI_CLEAR_ALL, 0.0f, 0.0f, 0.0f, 1.0f);
        if (state->show_shadow_map) {
            /* inspector: the shadow map as linearized grayscale (item 9b debug) */
            rhi_set_pipeline(state->shadow_debug_pipeline);
            rhi_bind_texture(rhi_render_target_depth_texture(state->shadow_rt), 0);
            rhi_set_uniform_int("uDepth", 0);
            rhi_set_uniform_float("uNear", SHADOW_NEAR);
            rhi_set_uniform_float("uFar",  SHADOW_FAR);
        } else {
            rhi_set_pipeline(state->post_pipeline);
            rhi_bind_texture(rhi_render_target_texture(state->hdr_rt), 0);
            rhi_set_uniform_int("uHdr", 0);             /* sampler -> texture unit 0 */
            rhi_set_uniform_float("uExposure", state->exposure);
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
       retired the window-title sprintf hack). ui_text positions BASELINES. */
    ui_quad(8.0f * us, 8.0f * us, 300.0f * us, 124.0f * us, 0.05f, 0.07f, 0.10f, 0.55f);
    ui_quad_outline(8.0f * us, 8.0f * us, 300.0f * us, 124.0f * us,
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
            sprintf(line, "cam %s  exposure %.2f", mode, (double)state->exposure);
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 1.0f, 1.0f, 1.0f, 0.85f);
            mb += font_line_height(state->mono_font) * ms;
            {
                const char *rn = state->current_room
                    ? scene_meta_get(&state->scene, state->current_room, "name") : (const char *)0;
                sprintf(line, "room %s", rn ? rn
                        : (state->current_room ? "(unnamed)" : "outside"));
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
#ifdef __clang__
#pragma clang diagnostic pop
#endif
            ui_text(state->mono_font, line, 20.0f * us, mb, ms, 1.0f, 1.0f, 1.0f, 0.85f);
        }
    }

    /* first-person crosshair at the pick point (screen centre, via the
       viewport-relative units) — orbit mode picks at the cursor instead */
    if (state->camera.mode != CAMERA_ORBIT) {
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
    if (state->selected_handle != 0 &&
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
    ui_end();
}

/* ---- note editing (item 8 piece 5) ---- */

/* Open a note for typing: seed the buffer from its text meta. */
static void note_edit_begin(AppState *st, sol_u32 handle) {
    const char *t = scene_meta_get(&st->scene, handle, "text");
    size_t      n = t ? strlen(t) : 0;
    if (n >= EDIT_BUF_CAP) n = EDIT_BUF_CAP - 1;
    memcpy(st->edit_buf, t ? t : "", n);
    st->edit_buf[n] = '\0';
    st->edit_len    = (int)n;
    st->edit_handle = handle;
    printf("editing note — type away; Enter = newline, Esc or click = done\n");
}

/* Blur: the buffer already mirrors into the meta per keystroke (the card
   renders live); the blur is what makes it durable — save on blur, the
   same reflex as save-on-release. */
static void note_edit_end(AppState *st) {
    if (st->edit_handle == 0) return;
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
    if (!st || st->edit_handle == 0) return;
    n = utf8_encode(cp, enc);
    if (n <= 0 || st->edit_len + n >= EDIT_BUF_CAP) return;
    memcpy(st->edit_buf + st->edit_len, enc, (size_t)n);
    st->edit_len += n;
    st->edit_buf[st->edit_len] = '\0';
    scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
}

/* KEYS are buttons. While a note has focus the only buttons that mean
   anything are the text-control ones — and they get GLFW_REPEAT for free
   here (held Backspace erases a run), which polling cannot give. */
static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods) {
    AppState *st = (AppState *)glfwGetWindowUserPointer(window);
    (void)scancode;
    (void)mods;

    if (st && st->edit_handle != 0) {
        if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            note_edit_end(st);
        } else if (key == GLFW_KEY_BACKSPACE && st->edit_len > 0) {
            st->edit_len--;                     /* drop ONE codepoint: walk back
                                                   over UTF-8 continuation bytes */
            while (st->edit_len > 0 &&
                   ((unsigned char)st->edit_buf[st->edit_len] & 0xC0u) == 0x80u)
                st->edit_len--;
            st->edit_buf[st->edit_len] = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
        } else if (key == GLFW_KEY_ENTER && st->edit_len + 1 < EDIT_BUF_CAP) {
            st->edit_buf[st->edit_len++] = '\n';
            st->edit_buf[st->edit_len]   = '\0';
            scene_meta_set(&st->scene, st->edit_handle, "text", st->edit_buf);
        }
        return;                                 /* everything else stays quiet */
    }

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        if (st && st->reader_state != READER_IDLE)
            reader_close(st);                   /* put the book back first */
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
        else if (o)
            reader_open(st, st->selected_handle);
    }
}

int main(void) {
    GLFWwindow *window;
    AppState state = {0};
    double last;

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
       geometry, then free it — including the check meshes, which are safe to
       destroy per-object here because they were just resolved (uniquely
       owned), unlike the live scene's possibly-instanced glb buffers. */
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
            if (o->mesh.index_count > 0) {
                solid++;
                rhi_destroy_buffer(o->mesh.vbuffer);
                rhi_destroy_buffer(o->mesh.ibuffer);
            }
        }
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

    last = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        double      now, dt;
        CameraInput in;
        glfwPollEvents();

        now = glfwGetTime();
        dt  = now - last;
        last = now;
        if (dt > 0.1) dt = 0.1;   /* clamp: a long stall pauses motion, never lurches */

        glfwGetFramebufferSize(window, &state.fb_width, &state.fb_height);

        read_input(window, &in, dt, &state);          /* poll GLFW -> CameraInput */
        camera_update(&state.camera, &in, (float)dt);
        update(&state, dt);                           /* animate the scene */
        reader_update(&state, (float)dt);             /* the book's flight (item 9) */
        render(&state);

        rhi_present();
    }

    font_destroy(state.mono_font);
    font_destroy(state.ui_font);
    wtext_shutdown();
    ui_shutdown();
    scene_free(&state.scene);
    if (state.hdr_rt.id) rhi_destroy_render_target(state.hdr_rt);
    if (state.shadow_rt.id) rhi_destroy_render_target(state.shadow_rt);
    rhi_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}

#include <stdio.h>
#include <stdlib.h>
#include <math.h>                /* cosf — spot-light cone cosines (item 9a) */

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
    "    vec3 color = ambient + Lo;\n"
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
    /* mouse-look / cursor state (item 3c/3d) */
    double      mouse_last_x, mouse_last_y;
    int         mouse_skip;        /* swallow N frames of delta after a cursor-mode change */
    sol_bool    tab_was_down;
    double      scroll_accum;      /* scroll events accumulate here, drained per frame */
    /* picking / selection (item 4) */
    sol_u32     selected_handle;   /* 0 = none */
    sol_bool    lmb_was_down;
    double      press_x, press_y;  /* left-press position, for orbit tap-vs-drag */
} AppState;

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

/* Load a glTF .glb, auto-fit it (longest side ~1.2 units), and stand it on the
   floor centered at (x,z). Node transforms are already baked by the loader.
   Returns the group anchor's handle (0 on failure) so the caller can re-pose it. */
static sol_u32 add_glb_to_scene(AppState *state, const char *path, float x, float z) {
    GlbModel model;
    Aabb     b;
    vec3     center, ext;
    float    maxdim, scale;
    sol_u32  m, anchor;
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
       so it sits on the floor at (x,z). Parts are children offset by -center, so
       each part's WORLD matrix is unchanged from a corner-anchored layout. */
    anchor = scene_add(&state->scene, 0, empty,
                       vec3_make(x, scale * (center.y - b.min.y), z),
                       quat_identity(), vec3_make(scale, scale, scale));
    for (m = 0; m < model.count; m++) {
        sol_u32 h = scene_add(&state->scene, anchor, model.parts[m].mesh,
                              vec3_make(-center.x, -center.y, -center.z),
                              quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
        scene_material_set(&state->scene, h, model.parts[m].material);
    }
    glb_free(&model);
    return anchor;
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

/* Cast a pick ray through a screen point (NDC) and select the nearest object,
   reporting its stable handle + nid. In orbit, a hit re-targets the pivot. */
static void do_pick(AppState *st, GLFWwindow *w, float ndc_x, float ndc_y) {
    int     ww, wh;
    float   aspect, t;
    Ray     ray;
    sol_u32 hit;

    glfwGetWindowSize(w, &ww, &wh);                 /* cursor is in window coords */
    aspect = (wh > 0) ? (float)ww / (float)wh : 1.0f;
    ray = camera_ray(&st->camera, ndc_x, ndc_y, aspect);
    hit = scene_pick(&st->scene, ray, &t);
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
static void scene_resolve_meshes(Scene *s);   /* defined with init_scene below */

static void read_input(GLFWwindow *w, CameraInput *in, double dt, AppState *st) {
    float    look = (float)dt * LOOK_SPEED;
    sol_bool f_now, tab_now, m_now, i_now, p_now, l_now, dragging, fp;
    double   mx, my;

    fp = (st->camera.mode != CAMERA_ORBIT);

    /* movement (held; ignored by the camera in orbit) */
    in->forward = glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS;
    in->back    = glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS;
    in->left    = glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS;
    in->right   = glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS;
    in->up      = glfwGetKey(w, GLFW_KEY_SPACE)        == GLFW_PRESS;
    in->down    = glfwGetKey(w, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;

    /* keyboard look (held -> a rate -> dt-scaled) */
    in->look_dx = 0.0f;
    in->look_dy = 0.0f;
    if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) in->look_dx += look;
    if (glfwGetKey(w, GLFW_KEY_LEFT)  == GLFW_PRESS) in->look_dx -= look;
    if (glfwGetKey(w, GLFW_KEY_UP)    == GLFW_PRESS) in->look_dy += look;
    if (glfwGetKey(w, GLFW_KEY_DOWN)  == GLFW_PRESS) in->look_dy -= look;

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
    } else if (fp || dragging) {
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

    /* left button -> pick. FP: a press picks through screen center. Orbit: a
       tap (release with little movement) picks at the cursor; a drag rotates. */
    {
        sol_bool lmb = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (fp) {
            if (lmb && !st->lmb_was_down) do_pick(st, w, 0.0f, 0.0f);
        } else {
            if (lmb && !st->lmb_was_down) {
                st->press_x = mx;
                st->press_y = my;
            } else if (!lmb && st->lmb_was_down) {
                double ddx = mx - st->press_x;
                double ddy = my - st->press_y;
                if (ddx*ddx + ddy*ddy < 25.0) {         /* moved < 5px -> a tap */
                    int ww, wh;
                    glfwGetWindowSize(w, &ww, &wh);
                    do_pick(st, w, 2.0f*(float)mx/(float)ww - 1.0f,
                                   1.0f - 2.0f*(float)my/(float)wh);
                }
            }
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

    /* L reloads scene.stml from disk (P3 item 1: the persistence proof, edge).
       Procedural geometry reconstructs via the resolver; glb objects and
       materials are not in the file yet (items 5/6), so they come back as
       empties/defaults — the honest current boundary. The old scene's GPU
       meshes are NOT destroyed: glb parts may share buffers (instanced
       meshes), so per-object destroy could double-free a slot. A debug key
       leaking boundedly is acceptable; the asset registry owns this later. */
    l_now = glfwGetKey(w, GLFW_KEY_L) == GLFW_PRESS;
    if (l_now && !st->l_was_down) {
        Scene fresh;
        if (scene_load(&fresh, "scene.stml")) {
            scene_resolve_meshes(&fresh);
            scene_free(&st->scene);
            st->scene = fresh;
            st->selected_handle = 0;       /* the old selection died with its scene */
            printf("reloaded scene.stml: %u objects\n", (unsigned)st->scene.count);
        } else {
            fprintf(stderr, "scene.stml did not load — keeping the live scene\n");
        }
    }
    st->l_was_down = l_now;

    /* exposure scrub: '[' down, ']' up (held; dt-scaled), with a live title readout */
    {
        float erate = (float)dt * 1.5f;
        char  title[48];
        if (glfwGetKey(w, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) st->exposure += erate;
        if (glfwGetKey(w, GLFW_KEY_LEFT_BRACKET)  == GLFW_PRESS) st->exposure -= erate;
        if (st->exposure < 0.1f) st->exposure = 0.1f;
        if (st->exposure > 8.0f) st->exposure = 8.0f;
        sprintf(title, "solarium  exposure %.2f", (double)st->exposure);
        glfwSetWindowTitle(w, title);
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
        mb_init(&mb);
        if (mesh_ref_build(o->mesh_ref, &mb)) {
            o->mesh = mesh_from_builder(&mb);
        } else {
            fprintf(stderr, "scene: unknown mesh ref \"%s\" on %s — left empty\n",
                    o->mesh_ref, o->nid ? o->nid : "(no nid)");
        }
        mb_free(&mb);
    }
}

/* One-time setup: build the pipeline + meshes, populate the scene. */
static int init_scene(AppState *state) {
    RhiShader shader;
    RhiPipelineDesc desc = {0};  /* {0} all descs: a future field defaults off, not garbage */
    Mesh empty = {0};            /* zero mesh -> an empty (transform-only) */
    sol_u32 anchor, floor, page;

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

    /* scene: floor (root), an empty anchor (root), and the box as the
       anchor's child — so spinning the anchor makes the box orbit it.
       Geometry is BY REFERENCE even at build time (P3 item 1): objects are
       added empty carrying a mesh ref, and one resolver pass realizes them —
       the exact path a scene loaded from disk takes, so the built world and
       the reloaded world cannot drift apart. */
    scene_init(&state->scene);
    floor = scene_add(&state->scene, 0, empty,
              vec3_make(0.0f, 0.0f, 0.0f), quat_identity(), vec3_make(1.0f, 1.0f, 1.0f));
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
    scene_resolve_meshes(&state->scene);

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

    /* overbuilt slots demo (mostly empty this phase) */
    scene_meta_set(&state->scene, state->box_handle, "title",  "Test Box");
    scene_meta_set(&state->scene, state->box_handle, "author", "Solarium");
    scene_rel_add(&state->scene, state->box_handle, "orbits", state->anchor_handle);
    scene_content_set(&state->scene, state->box_handle, "notes/box.txt");

    /* persist the room once at startup (item 2.5c) — inspect ./scene.stml */
    if (scene_save(&state->scene, "scene.stml"))
        printf("saved scene -> scene.stml\n");

    /* place the camera where the old fixed view sat: back + raised, facing the
       scene (-Z) with a slight downward tilt (item 3b) */
    camera_init(&state->camera, vec3_make(0.0f, 2.5f, 5.0f),
                sol_radians(-90.0f), sol_radians(-20.0f));
    state->f_was_down = SOL_FALSE;
    state->exposure   = 1.0f;

    /* the spot light: elevated over the cluster, warm, aimed at the table (9a) */
    state->light_pos       = vec3_make(2.5f, 5.0f, 2.5f);
    state->light_target    = vec3_make(0.0f, 0.5f, 0.0f);
    state->light_color     = vec3_make(1.0f, 0.95f, 0.85f);
    state->light_intensity = 120.0f;
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
    vec3    eye;
    mat4    view, proj;
    sol_u32 i;
    sol_u32 sel_root;

    ensure_render_target(state, state->fb_width, state->fb_height);

    sel_root = group_root(&state->scene, state->selected_handle);  /* 0 if nothing selected */

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
        model = scene_world_matrix(&state->scene, o);
        hl    = (sel_root != 0 && group_root(&state->scene, o->handle) == sel_root)
              ? 1.0f : 0.0f;                       /* light the whole selected group */
        draw_mesh(state, o->mesh, model, view, proj, eye, hl, o->material);
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
       the tonemap on purpose: ACES would roll UI white down to ~0.8 gray. ---- */
    ui_begin(state->fb_width, state->fb_height);

    /* reserved debug-readout panel (item 3 puts text in it) + its border */
    ui_quad(8.0f, 8.0f, 280.0f, 84.0f, 0.05f, 0.07f, 0.10f, 0.55f);
    ui_quad_outline(8.0f, 8.0f, 280.0f, 84.0f, 1.0f, 0.95f, 0.80f, 0.45f, 0.9f);

    /* first-person crosshair at the pick point (screen centre, via the
       viewport-relative units) — orbit mode picks at the cursor instead */
    if (state->camera.mode != CAMERA_ORBIT) {
        float cx = ui_vw(50.0f), cy = ui_vh(50.0f);
        ui_line(cx - 9.0f, cy, cx + 9.0f, cy, 1.5f, 1.0f, 1.0f, 1.0f, 0.7f);
        ui_line(cx, cy - 9.0f, cx, cy + 9.0f, 1.5f, 1.0f, 1.0f, 1.0f, 0.7f);
    }

    /* a billboarded mark pinned to the selected object: its WORLD position is
       projected through the same view-proj the scene used, then drawn in
       screen space — constant pixel size, always crisp, faces the camera by
       construction. Culled when behind the camera (w<=0: see
       mat4_project_point). The tag quad is the item-3 label's seat. */
    if (state->selected_handle != 0 &&
        scene_get(&state->scene, state->selected_handle)) {
        vec3 world = object_world_pos(&state->scene, state->selected_handle);
        vec3 ndc;
        if (mat4_project_point(mat4_mul(proj, view), world, &ndc)) {
            float sx = (ndc.x * 0.5f + 0.5f) * (float)state->fb_width;
            float sy = (0.5f - ndc.y * 0.5f) * (float)state->fb_height;   /* NDC y-up -> UI y-down */
            float tw = 64.0f, th = 20.0f;                /* the tag, sat above the point */
            float tx = sx - tw * 0.5f, ty = sy - 46.0f;
            ui_line(sx, sy, sx, ty + th, 1.5f, 0.95f, 0.80f, 0.45f, 0.9f);   /* leader */
            ui_quad(tx, ty, tw, th, 0.05f, 0.07f, 0.10f, 0.75f);
            ui_quad_outline(tx, ty, tw, th, 1.0f, 0.95f, 0.80f, 0.45f, 0.9f);
        }
    }
    ui_end();
}

static void on_key(GLFWwindow *window, int key, int scancode, int action, int mods) {
    (void)scancode;
    (void)mods;
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
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
    glfwSetKeyCallback(window, on_key);         /* platform: input */
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
            refs++;
            if (o->mesh.index_count > 0) {
                solid++;
                rhi_destroy_buffer(o->mesh.vbuffer);
                rhi_destroy_buffer(o->mesh.ibuffer);
            }
        }
        if (refs == 0 || solid != refs || check.count != state.scene.count) {
            fprintf(stderr, "persistence self-check FAILED: %u/%u refs reconstructed, "
                    "%u loaded vs %u live objects\n",
                    (unsigned)solid, (unsigned)refs,
                    (unsigned)check.count, (unsigned)state.scene.count);
            scene_free(&check);
            rhi_shutdown();
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_FAILURE;
        }
        printf("persistence self-check: %u objects, %u/%u meshes reconstructed ok\n",
               (unsigned)check.count, (unsigned)solid, (unsigned)refs);
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
        render(&state);

        rhi_present();
    }

    ui_shutdown();
    scene_free(&state.scene);
    if (state.hdr_rt.id) rhi_destroy_render_target(state.hdr_rt);
    if (state.shadow_rt.id) rhi_destroy_render_target(state.shadow_rt);
    rhi_shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}

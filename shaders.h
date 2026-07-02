/* shaders.h — every GLSL + MSL shader twin source, included ONLY by main.c.
   Moved verbatim out of the hub so it stays navigable. Each #ifdef
   SOL_RHI_METAL block keeps the MSL twin beside its GLSL under one comment
   (item 10's approved decision) — edit the pair together. */

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

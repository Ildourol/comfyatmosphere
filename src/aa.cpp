// aa: post-process Anti-Aliasing (FXAA 3.11 Quality & 3-pass SMAA 1x) + optional RCAS sharpening
// at the world -> UI boundary (D3D9 SM 3.0 for Turtle WoW tw11817979 / DXVK 2.6.1).
//
// Modes ([aa] mode / comfyAA):
//   0 = Off  (runs standalone RCAS only if sharpen > 0; otherwise 0.00 ms bypass)
//   1 = FXAA (NVIDIA FXAA 3.11 quality edge search + subpixel anti-aliasing + optional RCAS)
//   2 = SMAA (3-pass Subpixel Morphological AA: color edge detect -> analytical pattern weights ->
//             neighborhood blend + optional RCAS)
//
// Runs strictly before UI draws so chat, tooltips, nameplates, and action bars stay 100% razor-sharp.
// Resolves multisampled (e.g. 8x MSAA) backbuffers safely via full-size StretchRect(..., D3DTEXF_NONE).

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>

#include "aa.h"
#include "client.h"
#include "common.h"
#include "config.h"
#include "shadercache.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // ---------------------------------------------------------------------------------------------
    // 1. FXAA 3.11 Quality + optional RCAS sharpening (ps_3_0)
    // ---------------------------------------------------------------------------------------------
    const char* kFxaaHlsl = R"HLSL(
sampler2D sScene : register(s0);
float4 gTexel  : register(c0);   // xy: 1/width, 1/height
float4 gParams : register(c1);   // x: subpix (0..1), y: edgeThreshold, z: edgeThresholdMin, w: steps (4/8/12)
float4 gSharp  : register(c2);   // x: rcas strength (0..1)

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float3 ApplyRcas(float2 uv, float3 e, float strength)
{
    float3 b = tex2Dlod(sScene, float4(uv + float2( 0.0,       -gTexel.y), 0.0, 0.0)).rgb;
    float3 d = tex2Dlod(sScene, float4(uv + float2(-gTexel.x,   0.0     ), 0.0, 0.0)).rgb;
    float3 f = tex2Dlod(sScene, float4(uv + float2( gTexel.x,   0.0     ), 0.0, 0.0)).rgb;
    float3 h = tex2Dlod(sScene, float4(uv + float2( 0.0,        gTexel.y), 0.0, 0.0)).rgb;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float  lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * strength;
    return saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 rgbM = tex2Dlod(sScene, float4(uv, 0.0, 0.0)).rgb;
    float  lM   = Luma(rgbM);
    float  lN   = Luma(tex2Dlod(sScene, float4(uv + float2( 0.0,       -gTexel.y), 0.0, 0.0)).rgb);
    float  lS   = Luma(tex2Dlod(sScene, float4(uv + float2( 0.0,        gTexel.y), 0.0, 0.0)).rgb);
    float  lW   = Luma(tex2Dlod(sScene, float4(uv + float2(-gTexel.x,   0.0     ), 0.0, 0.0)).rgb);
    float  lE   = Luma(tex2Dlod(sScene, float4(uv + float2( gTexel.x,   0.0     ), 0.0, 0.0)).rgb);

    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float lRange = lMax - lMin;

    float3 outCol = rgbM;
    [branch] if (lRange >= max(gParams.z, lMax * gParams.y))
    {
        float lNW = Luma(tex2Dlod(sScene, float4(uv + float2(-gTexel.x, -gTexel.y), 0.0, 0.0)).rgb);
        float lNE = Luma(tex2Dlod(sScene, float4(uv + float2( gTexel.x, -gTexel.y), 0.0, 0.0)).rgb);
        float lSW = Luma(tex2Dlod(sScene, float4(uv + float2(-gTexel.x,  gTexel.y), 0.0, 0.0)).rgb);
        float lSE = Luma(tex2Dlod(sScene, float4(uv + float2( gTexel.x,  gTexel.y), 0.0, 0.0)).rgb);

        float lNS = lN + lS;
        float lWE = lW + lE;
        float subpixRcpRange = 1.0 / max(lRange, 1e-5);
        float subpixNSWE = lNS + lWE;
        float edgeH = abs(-2.0 * lW + (lNW + lSW)) + abs(-2.0 * lM + lNS) * 2.0 + abs(-2.0 * lE + (lNE + lSE));
        float edgeV = abs(-2.0 * lN + (lNW + lNE)) + abs(-2.0 * lM + lWE) * 2.0 + abs(-2.0 * lS + (lSW + lSE));
        bool  isHorz = (edgeH >= edgeV);

        float l1 = isHorz ? lN : lW;
        float l2 = isHorz ? lS : lE;
        float grad1 = abs(l1 - lM);
        float grad2 = abs(l2 - lM);
        bool  is1Steepest = (grad1 >= grad2);
        float gradScaled = 0.25 * max(grad1, grad2);

        float stepLen = isHorz ? gTexel.y : gTexel.x;
        float lLocalAvg = 0.0;
        if (is1Steepest)
        {
            stepLen = -stepLen;
            lLocalAvg = 0.5 * (l1 + lM);
        }
        else
        {
            lLocalAvg = 0.5 * (l2 + lM);
        }

        float2 curUv = uv;
        if (isHorz) curUv.y += stepLen * 0.5;
        else        curUv.x += stepLen * 0.5;

        float2 offset = isHorz ? float2(gTexel.x, 0.0) : float2(0.0, gTexel.y);
        float2 uv1 = curUv - offset;
        float2 uv2 = curUv + offset;
        float  lEnd1 = Luma(tex2Dlod(sScene, float4(uv1, 0.0, 0.0)).rgb) - lLocalAvg;
        float  lEnd2 = Luma(tex2Dlod(sScene, float4(uv2, 0.0, 0.0)).rgb) - lLocalAvg;
        bool   done1 = abs(lEnd1) >= gradScaled;
        bool   done2 = abs(lEnd2) >= gradScaled;

        int maxSteps = (int)gParams.w;
        [loop] for (int i = 1; i < 12; ++i)
        {
            if (i >= maxSteps || (done1 && done2))
                break;
            float mult = (i < 4) ? 1.0 : ((i < 8) ? 2.0 : 4.0);
            if (!done1)
            {
                uv1 -= offset * mult;
                lEnd1 = Luma(tex2Dlod(sScene, float4(uv1, 0.0, 0.0)).rgb) - lLocalAvg;
                done1 = abs(lEnd1) >= gradScaled;
            }
            if (!done2)
            {
                uv2 += offset * mult;
                lEnd2 = Luma(tex2Dlod(sScene, float4(uv2, 0.0, 0.0)).rgb) - lLocalAvg;
                done2 = abs(lEnd2) >= gradScaled;
            }
        }

        float dist1 = isHorz ? (uv.x - uv1.x) : (uv.y - uv1.y);
        float dist2 = isHorz ? (uv2.x - uv.x) : (uv2.y - uv.y);
        bool  isDir1 = (dist1 < dist2);
        float distMin = min(dist1, dist2);
        float edgeThick = max(dist1 + dist2, 1e-5);
        float pixelOffset = -distMin / edgeThick + 0.5;

        bool isMSmaller = (lM < lLocalAvg);
        bool goodSpan = ((isDir1 ? lEnd1 : lEnd2) < 0.0) != isMSmaller;
        float finalOffset = goodSpan ? pixelOffset : 0.0;

        float lAvg = (1.0 / 12.0) * (2.0 * subpixNSWE + lNW + lNE + lSW + lSE);
        float subpix1 = saturate(abs(lAvg - lM) * subpixRcpRange);
        float subpix2 = (-2.0 * subpix1 + 3.0) * subpix1 * subpix1;
        float subpixOffset = subpix2 * subpix2 * gParams.x;
        finalOffset = max(finalOffset, subpixOffset);

        float2 finalUv = uv;
        if (isHorz) finalUv.y += finalOffset * stepLen;
        else        finalUv.x += finalOffset * stepLen;
        outCol = tex2Dlod(sScene, float4(finalUv, 0.0, 0.0)).rgb;
    }

    [branch] if (gSharp.x > 0.001)
        outCol = ApplyRcas(uv, outCol, gSharp.x);

    return float4(saturate(outCol), 1.0);
}
)HLSL";

    // ---------------------------------------------------------------------------------------------
    // 2. SMAA 1x (3-pass Morphological Anti-Aliasing, ps_3_0)
    // ---------------------------------------------------------------------------------------------
    const char* kSmaaEdgeHlsl = R"HLSL(
sampler2D sScene : register(s0);
float4 gTexel  : register(c0);   // xy: 1/width, 1/height
float4 gParams : register(c1);   // x: edgeThreshold (e.g. 0.08..0.15)

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 c  = tex2Dlod(sScene, float4(uv, 0.0, 0.0)).rgb;
    float3 cL = tex2Dlod(sScene, float4(uv + float2(-gTexel.x,  0.0     ), 0.0, 0.0)).rgb;
    float3 cT = tex2Dlod(sScene, float4(uv + float2( 0.0,      -gTexel.y), 0.0, 0.0)).rgb;

    float3 dL = abs(c - cL);
    float3 dT = abs(c - cT);
    float  deltaL = max(dL.r, max(dL.g, dL.b));
    float  deltaT = max(dT.r, max(dT.g, dT.b));

    float2 edges = step(gParams.x, float2(deltaL, deltaT));
    if (edges.x + edges.y < 0.5)
        return float4(0.0, 0.0, 0.0, 0.0);

    // Local contrast adaptation: suppress weak edges next to much stronger edges.
    float3 cR  = tex2Dlod(sScene, float4(uv + float2( gTexel.x,  0.0     ), 0.0, 0.0)).rgb;
    float3 cB  = tex2Dlod(sScene, float4(uv + float2( 0.0,       gTexel.y), 0.0, 0.0)).rgb;
    float3 cLL = tex2Dlod(sScene, float4(uv + float2(-2.0 * gTexel.x, 0.0), 0.0, 0.0)).rgb;
    float3 cTT = tex2Dlod(sScene, float4(uv + float2(0.0, -2.0 * gTexel.y), 0.0, 0.0)).rgb;

    float3 dR  = abs(c - cR);
    float3 dB  = abs(c - cB);
    float3 dLL = abs(cL - cLL);
    float3 dTT = abs(cT - cTT);

    float maxDelta = max(max(deltaL, deltaT), max(max(max(dR.r, max(dR.g, dR.b)), max(dB.r, max(dB.g, dB.b))),
                     max(max(dLL.r, max(dLL.g, dLL.b)), max(dTT.r, max(dTT.g, dTT.b)))));
    edges *= step(maxDelta * 0.5, float2(deltaL, deltaT));
    return float4(edges, 0.0, 1.0);
}
)HLSL";

    const char* kSmaaWeightHlsl = R"HLSL(
sampler2D sEdges : register(s0);
float4 gTexel  : register(c0);   // xy: 1/width, 1/height
float4 gParams : register(c1);   // x: maxSearchSteps (6/10/16)

// Analytical SMAA area approximation for an edge span (dLeft, dRight) with crossing edges (e1, e2).
float2 CalcArea(float d1, float d2, float e1, float e2)
{
    float total = max(d1 + d2, 1.0);
    // Relative distance of current pixel along the edge (0 at left/top end, 1 at right/bottom end)
    float u1 = d1 / total;
    float u2 = d2 / total;
    float2 w = float2(0.0, 0.0);

    // L-shape or Z-shape crossings determine whether pixel blends to side A (.x) or side B (.y)
    bool has1 = (e1 > 0.5);
    bool has2 = (e2 > 0.5);
    if (has1 && !has2)
    {
        // Linear ramp down from crossing end 1
        float a = saturate(0.5 - u1);
        w.x = a;
    }
    else if (!has1 && has2)
    {
        float a = saturate(0.5 - u2);
        w.y = a;
    }
    else if (has1 && has2)
    {
        // Both ends cross: blend toward nearer crossing
        if (d1 < d2) w.x = saturate(0.5 - u1);
        else         w.y = saturate(0.5 - u2);
    }
    return w;
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float2 e = tex2Dlod(sEdges, float4(uv, 0.0, 0.0)).rg;
    float4 weights = float4(0.0, 0.0, 0.0, 0.0);
    int maxSteps = (int)gParams.x;

    // Horizontal edge (e.g = top edge): search left & right along e.g, check crossing e.r
    [branch] if (e.g > 0.5)
    {
        float dL = 0.0;
        [loop] for (int i = 1; i <= 16; ++i)
        {
            if (i > maxSteps) break;
            float eg = tex2Dlod(sEdges, float4(uv - float2(gTexel.x * i, 0.0), 0.0, 0.0)).g;
            if (eg < 0.5) break;
            dL = (float)i;
        }
        float dR = 0.0;
        [loop] for (int j = 1; j <= 16; ++j)
        {
            if (j > maxSteps) break;
            float eg = tex2Dlod(sEdges, float4(uv + float2(gTexel.x * j, 0.0), 0.0, 0.0)).g;
            if (eg < 0.5) break;
            dR = (float)j;
        }
        float cL = tex2Dlod(sEdges, float4(uv - float2(gTexel.x * dL, 0.0), 0.0, 0.0)).r;
        float cR = tex2Dlod(sEdges, float4(uv + float2(gTexel.x * (dR + 1.0), 0.0), 0.0, 0.0)).r;
        float cLT = tex2Dlod(sEdges, float4(uv - float2(gTexel.x * dL, gTexel.y), 0.0, 0.0)).r;
        float cRT = tex2Dlod(sEdges, float4(uv + float2(gTexel.x * (dR + 1.0), -gTexel.y), 0.0, 0.0)).r;
        weights.rg = CalcArea(dL + 0.5, dR + 0.5, max(cL, cLT), max(cR, cRT));
    }

    // Vertical edge (e.r = left edge): search up & down along e.r, check crossing e.g
    [branch] if (e.r > 0.5)
    {
        float dU = 0.0;
        [loop] for (int i = 1; i <= 16; ++i)
        {
            if (i > maxSteps) break;
            float er = tex2Dlod(sEdges, float4(uv - float2(0.0, gTexel.y * i), 0.0, 0.0)).r;
            if (er < 0.5) break;
            dU = (float)i;
        }
        float dD = 0.0;
        [loop] for (int j = 1; j <= 16; ++j)
        {
            if (j > maxSteps) break;
            float er = tex2Dlod(sEdges, float4(uv + float2(0.0, gTexel.y * j), 0.0, 0.0)).r;
            if (er < 0.5) break;
            dD = (float)j;
        }
        float cU  = tex2Dlod(sEdges, float4(uv - float2(0.0, gTexel.y * dU), 0.0, 0.0)).g;
        float cD  = tex2Dlod(sEdges, float4(uv + float2(0.0, gTexel.y * (dD + 1.0)), 0.0, 0.0)).g;
        float cUL = tex2Dlod(sEdges, float4(uv - float2(gTexel.x, gTexel.y * dU), 0.0, 0.0)).g;
        float cDL = tex2Dlod(sEdges, float4(uv + float2(-gTexel.x, gTexel.y * (dD + 1.0)), 0.0, 0.0)).g;
        weights.ba = CalcArea(dU + 0.5, dD + 0.5, max(cU, cUL), max(cD, cDL));
    }

    return weights;
}
)HLSL";

    const char* kSmaaBlendHlsl = R"HLSL(
sampler2D sScene   : register(s0);
sampler2D sWeights : register(s1);
float4 gTexel  : register(c0);   // xy: 1/width, 1/height
float4 gSharp  : register(c1);   // x: rcas strength (0..1)

float3 ApplyRcas(float2 uv, float3 e, float strength)
{
    float3 b = tex2Dlod(sScene, float4(uv + float2( 0.0,       -gTexel.y), 0.0, 0.0)).rgb;
    float3 d = tex2Dlod(sScene, float4(uv + float2(-gTexel.x,   0.0     ), 0.0, 0.0)).rgb;
    float3 f = tex2Dlod(sScene, float4(uv + float2( gTexel.x,   0.0     ), 0.0, 0.0)).rgb;
    float3 h = tex2Dlod(sScene, float4(uv + float2( 0.0,        gTexel.y), 0.0, 0.0)).rgb;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float  lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * strength;
    return saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
}

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float4 a;
    a.x = tex2Dlod(sWeights, float4(uv + float2(gTexel.x, 0.0), 0.0, 0.0)).a; // right
    a.y = tex2Dlod(sWeights, float4(uv + float2(0.0, gTexel.y), 0.0, 0.0)).g; // bottom
    a.wz = tex2Dlod(sWeights, float4(uv, 0.0, 0.0)).xz;                      // top, left

    float3 outCol;
    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5)
    {
        outCol = tex2Dlod(sScene, float4(uv, 0.0, 0.0)).rgb;
    }
    else
    {
        bool horiz = max(a.x, a.z) > max(a.y, a.w);
        float4 off = float4(0.0, 0.0, 0.0, 0.0);
        float2 w;
        if (horiz)
        {
            off.xz = float2(a.x, -a.z);
            w = a.xz;
        }
        else
        {
            off.yw = float2(a.y, -a.w);
            w = a.yw;
        }
        w /= max(w.x + w.y, 1e-5);
        float3 c1 = tex2Dlod(sScene, float4(uv + off.xy * gTexel.xy, 0.0, 0.0)).rgb;
        float3 c2 = tex2Dlod(sScene, float4(uv + off.zw * gTexel.xy, 0.0, 0.0)).rgb;
        outCol = c1 * w.x + c2 * w.y;
    }

    [branch] if (gSharp.x > 0.001)
        outCol = ApplyRcas(uv, outCol, gSharp.x);

    return float4(saturate(outCol), 1.0);
}
)HLSL";

    // ---------------------------------------------------------------------------------------------
    // 3. Standalone RCAS Contrast-Adaptive Sharpening (when AA mode == 0 and sharpen > 0)
    // ---------------------------------------------------------------------------------------------
    const char* kSharpenHlsl = R"HLSL(
sampler2D sScene : register(s0);
float4 gTexel : register(c0);   // xy: 1/width, 1/height
float4 gSharp : register(c1);   // x: rcas strength (0..1)

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 e = tex2Dlod(sScene, float4(uv, 0.0, 0.0)).rgb;
    float3 b = tex2Dlod(sScene, float4(uv + float2( 0.0,       -gTexel.y), 0.0, 0.0)).rgb;
    float3 d = tex2Dlod(sScene, float4(uv + float2(-gTexel.x,   0.0     ), 0.0, 0.0)).rgb;
    float3 f = tex2Dlod(sScene, float4(uv + float2( gTexel.x,   0.0     ), 0.0, 0.0)).rgb;
    float3 h = tex2Dlod(sScene, float4(uv + float2( 0.0,        gTexel.y), 0.0, 0.0)).rgb;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float  lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * gSharp.x;
    return float4(saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0)), 1.0);
}
)HLSL";

    struct Target
    {
        IDirect3DTexture9* tex  = nullptr;
        IDirect3DSurface9* surf = nullptr;
    };

    Target g_scene, g_edges, g_weights;
    UINT   g_w = 0, g_h = 0;
    D3DFORMAT g_fmt = D3DFMT_UNKNOWN;
    bool   g_haveSmaaTargets = false;

    IDirect3DPixelShader9* g_psFxaa       = nullptr;
    IDirect3DPixelShader9* g_psSmaaEdge   = nullptr;
    IDirect3DPixelShader9* g_psSmaaWeight = nullptr;
    IDirect3DPixelShader9* g_psSmaaBlend  = nullptr;
    IDirect3DPixelShader9* g_psSharpen    = nullptr;
    IDirect3DStateBlock9*  g_sb           = nullptr;
    bool                   g_failed       = false;
    bool                   g_loggedResolveFail = false;
    int                    g_loggedMode   = -1;
    int                    g_loggedQual   = -1;
    float                  g_loggedSharp  = -1.0f;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    void ReleaseTarget(Target& t)
    {
        SafeRelease(t.surf);
        SafeRelease(t.tex);
    }

    bool MakeTarget(IDirect3DDevice9* dev, Target& t, UINT w, UINT h, D3DFORMAT fmt)
    {
        ReleaseTarget(t);
        HRESULT hr = dev->lpVtbl->CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &t.tex, nullptr);
        if (SUCCEEDED(hr) && t.tex)
            hr = t.tex->lpVtbl->GetSurfaceLevel(t.tex, 0, &t.surf);
        if (FAILED(hr) || !t.surf)
        {
            ReleaseTarget(t);
            return false;
        }
        return true;
    }

    IDirect3DPixelShader9* CompilePs(IDirect3DDevice9* dev, const char* src, const char* name)
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        if (!compile || FAILED(compile(src, strlen(src), name, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &errs)) || !code)
            Log("aa: %s failed to compile: %s", name,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no compiler)");
        else if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &ps)))
            ps = nullptr;
        if (code) code->lpVtbl->Release(code);
        if (errs) errs->lpVtbl->Release(errs);
        return ps;
    }

    bool EnsureTargets(IDirect3DDevice9* dev, const D3DSURFACE_DESC& d, bool needSmaa)
    {
        if (g_scene.surf && g_w == d.Width && g_h == d.Height && g_fmt == d.Format && (!needSmaa || g_haveSmaaTargets))
            return true;
        if (g_w != d.Width || g_h != d.Height || g_fmt != d.Format)
        {
            ReleaseTarget(g_scene);
            ReleaseTarget(g_edges);
            ReleaseTarget(g_weights);
            g_haveSmaaTargets = false;
        }
        if (!g_scene.surf && !MakeTarget(dev, g_scene, d.Width, d.Height, d.Format))
            return false;
        if (needSmaa && !g_haveSmaaTargets)
        {
            if (!MakeTarget(dev, g_edges, d.Width, d.Height, D3DFMT_A8R8G8B8) ||
                !MakeTarget(dev, g_weights, d.Width, d.Height, D3DFMT_A8R8G8B8))
                return false;
            g_haveSmaaTargets = true;
        }
        g_w = d.Width;
        g_h = d.Height;
        g_fmt = d.Format;
        return true;
    }

    bool EnsureShaders(IDirect3DDevice9* dev, int mode)
    {
        if (mode == 1)
        {
            if (!g_psFxaa) g_psFxaa = CompilePs(dev, kFxaaHlsl, "aa_fxaa");
            return g_psFxaa != nullptr;
        }
        if (mode == 2)
        {
            if (!g_psSmaaEdge)   g_psSmaaEdge   = CompilePs(dev, kSmaaEdgeHlsl,   "aa_smaa_edge");
            if (!g_psSmaaWeight) g_psSmaaWeight = CompilePs(dev, kSmaaWeightHlsl, "aa_smaa_weight");
            if (!g_psSmaaBlend)  g_psSmaaBlend  = CompilePs(dev, kSmaaBlendHlsl,  "aa_smaa_blend");
            return g_psSmaaEdge && g_psSmaaWeight && g_psSmaaBlend;
        }
        if (!g_psSharpen) g_psSharpen = CompilePs(dev, kSharpenHlsl, "aa_sharpen");
        return g_psSharpen != nullptr;
    }

    bool DebugViewOn()
    {
        const Settings& s = g_cfg;
        return s.volume.debug || s.sunShadows.debug || s.lamps.debug || s.rays.debugView || s.fog.debug ||
               s.water.debug || s.clouds.debug;
    }

    struct QuadVertex { float x, y, z, rhw, u, v; };

    void DrawQuad(IDirect3DDevice9* dev, UINT w, UINT h)
    {
        const float fw = static_cast<float>(w) - 0.5f;
        const float fh = static_cast<float>(h) - 0.5f;
        const QuadVertex q[4] = {
            { -0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f },
            {  fw,   -0.5f, 0.0f, 1.0f, 1.0f, 0.0f },
            { -0.5f,  fh,   0.0f, 1.0f, 0.0f, 1.0f },
            {  fw,    fh,   0.0f, 1.0f, 1.0f, 1.0f },
        };
        dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
    }

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE,
        D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);

    bool Run(IDirect3DDevice9* dev, IDirect3DSurface9* bb, UINT w, UINT h, int mode)
    {
        auto* d = dev->lpVtbl;

        // Full-size resolve with D3DTEXF_NONE: works identically on 1x, 2x, 4x, and 8x MSAA backbuffers.
        // Check before touching device state so a transient resolve failure leaves the backbuffer intact.
        const HRESULT hrResolve = d->StretchRect(dev, bb, nullptr, g_scene.surf, nullptr, D3DTEXF_NONE);
        if (FAILED(hrResolve))
        {
            if (!g_loggedResolveFail)
            {
                g_loggedResolveFail = true;
                Log("aa: StretchRect resolve failed (hr=0x%08lx), skipping AA pass", static_cast<unsigned long>(hrResolve));
            }
            return false;
        }

        // --- save ---
        IDirect3DSurface9* oldRT = nullptr;
        IDirect3DSurface9* oldDS = nullptr;
        d->GetRenderTarget(dev, 0, &oldRT);
        d->GetDepthStencilSurface(dev, &oldDS);
        g_sb->lpVtbl->Capture(g_sb);
        DWORD saved[kTouchedCount];
        for (int i = 0; i < kTouchedCount; ++i)
            d->GetRenderState(dev, kTouched[i], &saved[i]);
        IDirect3DBaseTexture9*       oldTex0 = nullptr;
        IDirect3DBaseTexture9*       oldTex1 = nullptr;
        IDirect3DVertexShader9*      oldVS   = nullptr;
        IDirect3DPixelShader9*       oldPS   = nullptr;
        IDirect3DVertexDeclaration9* oldDecl = nullptr;
        DWORD                        oldFVF  = 0;
        d->GetTexture(dev, 0, &oldTex0);
        d->GetTexture(dev, 1, &oldTex1);
        d->GetVertexShader(dev, &oldVS);
        d->GetPixelShader(dev, &oldPS);
        d->GetVertexDeclaration(dev, &oldDecl);
        d->GetFVF(dev, &oldFVF);

        const bool began = SUCCEEDED(d->BeginScene(dev));
        d->SetDepthStencilSurface(dev, nullptr);
        d->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_FALSE);
        d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
        d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
        d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  FALSE);
        d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
        d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
        d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
        d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
        d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
        for (DWORD s = 0; s < 2; ++s)
        {
            d->SetSamplerState(dev, s, D3DSAMP_ADDRESSU,    D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, s, D3DSAMP_ADDRESSV,    D3DTADDRESS_CLAMP);
            d->SetSamplerState(dev, s, D3DSAMP_MINFILTER,   D3DTEXF_LINEAR);
            d->SetSamplerState(dev, s, D3DSAMP_MAGFILTER,   D3DTEXF_LINEAR);
            d->SetSamplerState(dev, s, D3DSAMP_MIPFILTER,   D3DTEXF_NONE);
            d->SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, 0);
        }
        d->SetVertexShader(dev, nullptr);
        d->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);

        const float texel[4] = { 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), 0.0f, 0.0f };
        const float sharp[4] = { g_cfg.aa.sharpen, 0.0f, 0.0f, 0.0f };
        const int   q = g_cfg.aa.quality;   // 1 = Low, 2 = Medium, 3 = High

        if (mode == 1)
        {
            // FXAA 3.11 Quality
            const float fxParams[4] = {
                q == 1 ? 0.50f : (q == 2 ? 0.75f : 0.85f),
                q == 1 ? 0.166f : (q == 2 ? 0.125f : 0.063f),
                q == 1 ? 0.0833f : (q == 2 ? 0.0625f : 0.0312f),
                q == 1 ? 4.0f : (q == 2 ? 8.0f : 12.0f)
            };
            d->SetRenderTarget(dev, 0, bb);
            d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                           D3DCOLORWRITEENABLE_BLUE);
            d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_scene.tex));
            d->SetPixelShader(dev, g_psFxaa);
            d->SetPixelShaderConstantF(dev, 0, texel,    1);
            d->SetPixelShaderConstantF(dev, 1, fxParams, 1);
            d->SetPixelShaderConstantF(dev, 2, sharp,    1);
            DrawQuad(dev, w, h);
        }
        else if (mode == 2)
        {
            // SMAA Pass 1: Edge Detection -> g_edges
            const float edgeParams[4] = { q == 1 ? 0.14f : (q == 2 ? 0.10f : 0.06f), 0.0f, 0.0f, 0.0f };
            d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetRenderTarget(dev, 0, g_edges.surf);
            d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_scene.tex));
            d->SetPixelShader(dev, g_psSmaaEdge);
            d->SetPixelShaderConstantF(dev, 0, texel,      1);
            d->SetPixelShaderConstantF(dev, 1, edgeParams, 1);
            DrawQuad(dev, w, h);

            // SMAA Pass 2: Blending Weight Calculation -> g_weights
            const float weightParams[4] = { q == 1 ? 6.0f : (q == 2 ? 10.0f : 16.0f), 0.0f, 0.0f, 0.0f };
            d->SetRenderTarget(dev, 0, g_weights.surf);
            d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_edges.tex));
            d->SetPixelShader(dev, g_psSmaaWeight);
            d->SetPixelShaderConstantF(dev, 0, texel,        1);
            d->SetPixelShaderConstantF(dev, 1, weightParams, 1);
            DrawQuad(dev, w, h);

            // SMAA Pass 3: Neighborhood Blend (+ optional RCAS) -> bb
            d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(dev, 1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetRenderTarget(dev, 0, bb);
            d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                           D3DCOLORWRITEENABLE_BLUE);
            d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_scene.tex));
            d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(g_weights.tex));
            d->SetPixelShader(dev, g_psSmaaBlend);
            d->SetPixelShaderConstantF(dev, 0, texel, 1);
            d->SetPixelShaderConstantF(dev, 1, sharp, 1);
            DrawQuad(dev, w, h);
        }
        else
        {
            // Standalone RCAS Sharpening
            d->SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            d->SetRenderTarget(dev, 0, bb);
            d->SetRenderState(dev, D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                           D3DCOLORWRITEENABLE_BLUE);
            d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_scene.tex));
            d->SetPixelShader(dev, g_psSharpen);
            d->SetPixelShaderConstantF(dev, 0, texel, 1);
            d->SetPixelShaderConstantF(dev, 1, sharp, 1);
            DrawQuad(dev, w, h);
        }

        if (began)
            d->EndScene(dev);

        // --- restore ---
        for (int i = 0; i < kTouchedCount; ++i)
            d->SetRenderState(dev, kTouched[i], saved[i]);
        d->SetTexture(dev, 0, oldTex0);
        d->SetTexture(dev, 1, oldTex1);
        d->SetVertexShader(dev, oldVS);
        d->SetPixelShader(dev, oldPS);
        d->SetFVF(dev, oldFVF);
        if (oldDecl)
            d->SetVertexDeclaration(dev, oldDecl);
        d->SetRenderTarget(dev, 0, oldRT);
        d->SetDepthStencilSurface(dev, oldDS);
        g_sb->lpVtbl->Apply(g_sb);
        SafeRelease(oldRT);
        SafeRelease(oldDS);
        SafeRelease(oldTex0);
        SafeRelease(oldTex1);
        SafeRelease(oldVS);
        SafeRelease(oldPS);
        SafeRelease(oldDecl);
        return true;
    }
}

bool AaBeforeUI(IDirect3DDevice9* dev)
{
    const int mode = g_cfg.aa.mode;
    const bool wantSharp = g_cfg.aa.sharpen > 0.001f;
    if (!g_cfg.master || (mode == 0 && !wantSharp) || g_failed || DebugViewOn())
        return false;
    float pl[3];
    if (!ClientPlayer(pl))
        return false;

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return false;
    D3DSURFACE_DESC desc = {};
    bb->lpVtbl->GetDesc(bb, &desc);
    bool drawn = false;
    if (!EnsureShaders(dev, mode) || !EnsureTargets(dev, desc, mode == 2) ||
        (!g_sb && (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb)))
    {
        g_failed = true;
        Log("aa: resources unavailable: anti-aliasing disabled until next device reset");
    }
    else
    {
        drawn = Run(dev, bb, desc.Width, desc.Height, mode);
        if (drawn && (g_loggedMode != mode || g_loggedQual != g_cfg.aa.quality ||
                      fabsf(g_loggedSharp - g_cfg.aa.sharpen) > 0.001f))
        {
            const char* label = (mode == 1) ? "FXAA" : (mode == 2 ? "SMAA" : "RCAS");
            const unsigned msaa = desc.MultiSampleType > 1 ? static_cast<unsigned>(desc.MultiSampleType) : 1u;
            Log("aa: active %s (quality %d, sharpen %.2f, %ux%u, MSAA %ux)",
                label, g_cfg.aa.quality, g_cfg.aa.sharpen, desc.Width, desc.Height, msaa);
            g_loggedMode  = mode;
            g_loggedQual  = g_cfg.aa.quality;
            g_loggedSharp = g_cfg.aa.sharpen;
        }
    }
    bb->lpVtbl->Release(bb);
    return drawn;
}

void AaReset()
{
    ReleaseTarget(g_scene);
    ReleaseTarget(g_edges);
    ReleaseTarget(g_weights);
    SafeRelease(g_sb);
    SafeRelease(g_psFxaa);
    SafeRelease(g_psSmaaEdge);
    SafeRelease(g_psSmaaWeight);
    SafeRelease(g_psSmaaBlend);
    SafeRelease(g_psSharpen);
    g_w = g_h = 0;
    g_fmt = D3DFMT_UNKNOWN;
    g_haveSmaaTargets = false;
    g_failed = false;
    g_loggedResolveFail = false;
    g_loggedMode = -1;
    g_loggedQual = -1;
    g_loggedSharp = -1.0f;
}

void AaShaderList()
{
    ShaderPrecompile("aa_fxaa",        kFxaaHlsl,       "ps_3_0");
    ShaderPrecompile("aa_smaa_edge",   kSmaaEdgeHlsl,   "ps_3_0");
    ShaderPrecompile("aa_smaa_weight", kSmaaWeightHlsl, "ps_3_0");
    ShaderPrecompile("aa_smaa_blend",  kSmaaBlendHlsl,  "ps_3_0");
    ShaderPrecompile("aa_sharpen",     kSharpenHlsl,    "ps_3_0");
}

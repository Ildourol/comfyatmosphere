// sundot: a debug view of where the effects take the sun (2026-10-14, the owner: "a dot where atmos thinks the sun
// is"). [sun] debug 1, or the Debug View that sets it, draws over the finished world, before the UI:
//
//   yellow  the light the effects follow (SunDirection): the volumetric light, the fog's sunlight, the water's glint
//   red     the sun the shadow maps are drawn with (ShadowSunDirection): below [sunshadows] riseFrom it climbs
//           back toward riseTo, and [sunshadows] lock tilts it, so it can stand apart from the yellow dot
//   blue    the other moon at night (SunSecondDirection), which casts rays of its own
//   white   each sun-shaped sprite the sky drew this frame. With the table right (sunpath.cpp), the white dot of
//           the body followed sits inside the yellow one
//
// A dot off screen, or behind the camera, is drawn at half its size at the edge of the screen, toward it, and
// above the action bars at the bottom.
// Fixed function, no shader. The device state is saved and put back as in grade.cpp.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "config.h"
#include "sun.h"
#include "sundot.h"

#include <cmath>

namespace
{
    IDirect3DStateBlock9* g_sb = nullptr;
    bool                  g_failed = false;   // the state block could not be made: off until the next Reset

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    struct Vertex { float x, y, z, rhw; DWORD colour; };

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_CULLMODE,
        D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
        D3DRS_LIGHTING,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);

    // A world direction to the screen, in pixels. Off screen or behind the camera: on the edge, toward it.
    void Project(const float d[3], const D3DMATRIX& v, const D3DMATRIX& p, float w, float h, float& x, float& y,
                 bool& edge)
    {
        float c[3];
        for (int j = 0; j < 3; ++j)
            c[j] = d[0] * v.m[0][j] + d[1] * v.m[1][j] + d[2] * v.m[2][j];
        float nx = c[0] * p.m[0][0] + c[1] * p.m[1][0] + c[2] * p.m[2][0];
        float ny = c[0] * p.m[0][1] + c[1] * p.m[1][1] + c[2] * p.m[2][1];
        const float cw = c[0] * p.m[0][3] + c[1] * p.m[1][3] + c[2] * p.m[2][3];
        if (cw > 1e-4f)
        {
            nx /= cw;
            ny /= cw;
        }
        // The box the edge dots stay in: clear of the action bars at the bottom of the screen.
        constexpr float kSide = 0.94f, kTop = 0.94f, kBottom = 0.70f;
        edge = cw <= 1e-4f || fabsf(nx) > kSide || ny > kTop || ny < -kBottom;
        if (edge)
        {
            const float sx = fabsf(nx) > 1e-6f ? kSide / fabsf(nx) : 1e6f;
            const float sy = ny > 1e-6f ? kTop / ny : ny < -1e-6f ? kBottom / -ny : 1e6f;
            const float k = (std::fmin)(sx, sy);
            nx *= k;
            ny *= k;
        }
        x = (nx * 0.5f + 0.5f) * w;
        y = (0.5f - ny * 0.5f) * h;
    }

    // A round dot of radius r with a black ring round it, as two fans of 16 sides.
    void Dot(IDirect3DDevice9* dev, float x, float y, float r, DWORD colour)
    {
        constexpr int kSides = 16;
        Vertex fan[kSides + 2];
        for (int pass = 0; pass < 2; ++pass)
        {
            const float rr = pass == 0 ? r + 2.0f : r;
            const DWORD c = pass == 0 ? 0xFF000000u : colour;
            fan[0] = { x, y, 0.0f, 1.0f, c };
            for (int i = 0; i <= kSides; ++i)
            {
                const float a = 6.2831853f * static_cast<float>(i) / kSides;
                fan[i + 1] = { x + rr * cosf(a), y + rr * sinf(a), 0.0f, 1.0f, c };
            }
            dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, kSides, fan, sizeof(Vertex));
        }
    }
}

void SunDotDraw(IDirect3DDevice9* dev)
{
    if (g_cfg.sun.debug != 1 || g_failed)
        return;
    D3DMATRIX view, proj;
    if (!SunCamera(view, proj))
        return;
    if (!g_sb && (FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)) || !g_sb))
    {
        g_failed = true;
        Log("sundot: no state block: no sun dots until the next device reset");
        return;
    }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb)
        return;
    D3DSURFACE_DESC desc = {};
    bb->lpVtbl->GetDesc(bb, &desc);
    const float w = static_cast<float>(desc.Width), h = static_cast<float>(desc.Height);

    // What to mark, the sky's sprites first so the effects' dots are drawn over them.
    struct Mark { float d[3]; DWORD colour; float r; };
    Mark marks[8];
    int n = 0;
    float d[3];
    const bool sun = SunDirection(d);   // first: it takes in this frame's sky sprites
    float quads[4][3];
    const int nq = SunSkyQuads(quads, 4);
    for (int q = 0; q < nq && n < 8; ++q)
        marks[n++] = { { quads[q][0], quads[q][1], quads[q][2] }, 0xFFFFFFFFu, 4.0f };
    if (sun)
        marks[n++] = { { d[0], d[1], d[2] }, 0xFFFFE040u, 12.0f };
    if (ShadowSunDirection(d) && n < 8)
        marks[n++] = { { d[0], d[1], d[2] }, 0xFFFF4020u, 7.0f };
    if (SunSecondDirection(d) && n < 8)
        marks[n++] = { { d[0], d[1], d[2] }, 0xFF60B0FFu, 7.0f };

    auto* dv = dev->lpVtbl;
    IDirect3DSurface9* oldRT = nullptr;
    IDirect3DSurface9* oldDS = nullptr;
    dv->GetRenderTarget(dev, 0, &oldRT);
    dv->GetDepthStencilSurface(dev, &oldDS);
    g_sb->lpVtbl->Capture(g_sb);
    DWORD saved[kTouchedCount];
    for (int i = 0; i < kTouchedCount; ++i)
        dv->GetRenderState(dev, kTouched[i], &saved[i]);
    IDirect3DBaseTexture9*       oldTex0 = nullptr;
    IDirect3DVertexShader9*      oldVS   = nullptr;
    IDirect3DPixelShader9*       oldPS   = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD                        oldFVF  = 0;
    dv->GetTexture(dev, 0, &oldTex0);
    dv->GetVertexShader(dev, &oldVS);
    dv->GetPixelShader(dev, &oldPS);
    dv->GetVertexDeclaration(dev, &oldDecl);
    dv->GetFVF(dev, &oldFVF);

    const bool began = SUCCEEDED(dv->BeginScene(dev));   // fails harmlessly mid-scene
    dv->SetRenderTarget(dev, 0, bb);
    dv->SetDepthStencilSurface(dev, nullptr);
    const D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
    dv->SetViewport(dev, &vp);
    dv->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_FALSE);
    dv->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
    dv->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
    dv->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  FALSE);
    dv->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
    dv->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
    dv->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
    dv->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    dv->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                     D3DCOLORWRITEENABLE_BLUE);
    dv->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
    dv->SetRenderState(dev, D3DRS_LIGHTING,          FALSE);
    dv->SetTexture(dev, 0, nullptr);
    dv->SetTextureStageState(dev, 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dv->SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dv->SetTextureStageState(dev, 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dv->SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dv->SetTextureStageState(dev, 1, D3DTSS_COLOROP,   D3DTOP_DISABLE);
    dv->SetTextureStageState(dev, 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
    dv->SetVertexShader(dev, nullptr);
    dv->SetPixelShader(dev, nullptr);
    dv->SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    for (int i = 0; i < n; ++i)
    {
        float x, y;
        bool edge;
        Project(marks[i].d, view, proj, w, h, x, y, edge);
        Dot(dev, x, y, edge ? marks[i].r * 0.5f : marks[i].r, marks[i].colour);
    }
    if (began)
        dv->EndScene(dev);

    // Through the vtable first, so every hook's mirror sees the client's values again; then the state block puts the
    // device back exactly.
    for (int i = 0; i < kTouchedCount; ++i)
        dv->SetRenderState(dev, kTouched[i], saved[i]);
    dv->SetTexture(dev, 0, oldTex0);
    dv->SetVertexShader(dev, oldVS);
    dv->SetPixelShader(dev, oldPS);
    dv->SetFVF(dev, oldFVF);
    if (oldDecl)
        dv->SetVertexDeclaration(dev, oldDecl);   // after SetFVF, which would otherwise replace it
    dv->SetRenderTarget(dev, 0, oldRT);
    dv->SetDepthStencilSurface(dev, oldDS);
    g_sb->lpVtbl->Apply(g_sb);
    SafeRelease(oldRT);
    SafeRelease(oldDS);
    SafeRelease(oldTex0);
    SafeRelease(oldVS);
    SafeRelease(oldPS);
    SafeRelease(oldDecl);
    bb->lpVtbl->Release(bb);
}

void SunDotReset()
{
    SafeRelease(g_sb);
    g_failed = false;
}

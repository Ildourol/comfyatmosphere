// clouds: our clouds, and now and then one with a shape (2026-10-08).
//
// The game's clouds are one textured strip that scrolls across the sky dome: one picture that tiles and moves as a
// sheet, so it cannot hold a shape. This pass is drawn in its place (comfyatmos.cpp, at the game's cloud draw, which
// it then skips). The sun sprite, the sky dome, the stars and the moons are drawn before it, and the world after it,
// so the world, the grass and the far mountains stay in front, and a cloud before the sun cuts the sun rays (the rays
// take the darker of the sky before the clouds and the finished frame). Where a frame draws no cloud layer, the pass
// is drawn at the end of the sky instead.
//
// The clouds. Each is a sheet that faces the camera with a silhouette's distance field on it (cloudshapes.cpp): a
// plain cumulus made in code, or now and then a picture (a kodo, a turtle). Plain clouds and shapes are drawn the
// same way, so a shape is one of the clouds (the owner, 2026-10-08: "we would need to add more random clouds that
// work like the animals too"; the first build drew the plain clouds as one flat layer, and a shape on it looked laid
// on top). Billows of the sheet's own push the edge about and eat it, more for a plain cloud than for a shape, whose
// outline has to survive ([clouds] erode, puffBillows). A read toward the sun across the sheet shades the far side,
// the thick middle is darker and so is the foot. The sheet faces the camera fully, so a cloud overhead is seen too:
// a kodo overhead is the kodo you see lying in the grass. The plain clouds near a shape fade by [clouds] clearing, so
// its outline shows. The sheets are drawn far ones first.
//
// The high layer. Behind the clouds, a thin layer: each pixel's line of sight meets a flat base [clouds] layerHeight
// yards over the camera, where four reads of one tiling noise texture give three sizes of billow and one of cover,
// each moving with the wind at its own speed. Every read is tex2Dgrad, with the gradients taken before the branch
// that skips the pixels under the horizon.
//
// Where they are. The sky is the same for every player at one place: the clock is the real one (UTC, which every
// computer keeps to within seconds), not the game's, which comfytime sets for each player. The layer's place is the
// wind times that clock. The clouds come from grids over each map: a plain cloud from patches of puffCell yards, each
// rolling [clouds] cover for a cloud in each window of puffLife seconds; a shape from patches of 6000 yards, each
// rolling [clouds] shapes percent in each window of shapeLife seconds. The windows are shifted per patch. A cloud
// forms, drifts with the wind through its rolled point at mid-life, and breaks up.
//
// The colours follow the sun: white by day, warm as the sun sets, grey-blue at night (Night Darkness then darkens
// it with the sky). The shade takes some of the game's fog colour, which the game sets for the zone and the hour.
// In rain the clouds and the layer close up and darken, and no shape forms.
//
// Both draws are ps_3_0 with a vs_3_0, blended premultiplied (ONE, INVSRCALPHA): alpha is lowered in the shade, so
// some of the sky's colour shows through it. [clouds] debug 1 (Debug View 31) shows the shapes red, the plain clouds
// green and the high layer blue.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "cloudshapes.h"
#include "clouds.h"
#include "common.h"
#include "config.h"
#include "shadercache.h"
#include "sun.h"
#include "sunshadows.h"
#include "water.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    const char* kVsHlsl = R"HLSL(
float4 gHalf : register(c0);   // D3D9 half-pixel offset, in clip units
struct O { float4 pos : POSITION; float2 uv : TEXCOORD0; };
O main(float3 pos : POSITION, float2 uv : TEXCOORD0)
{
    O o;
    o.pos = float4(pos.xy + gHalf.xy, 0.0, 1.0);
    o.uv  = uv;
    return o;
}
)HLSL";

    // The high layer, full screen.
    const char* kPsHlsl = R"HLSL(
sampler2D sNoise  : register(s0);   // tiling noise: r, g, b billows at three sizes, a the cover's
float4 gInv0  : register(c0);       // rows of inverse(view-projection): clip -> camera-relative world
float4 gInv1  : register(c1);
float4 gInv2  : register(c2);
float4 gInv3  : register(c3);
float4 gSun   : register(c4);       // xyz the world direction to the sun, w the cover
float4 gLit   : register(c5);       // rgb the lit side, w the opacity
float4 gShade : register(c6);       // rgb the shaded side, w the share of sky seen through the shade
float4 gP     : register(c7);       // x the base's height over the camera, y the edge's width, w debug
float4 gL0    : register(c8);       // per layer: xy its offset, z 1 / its size in yards
float4 gL1    : register(c9);
float4 gL2    : register(c10);
float4 gL3    : register(c11);      // the cover's layer
float4 gHaze  : register(c12);      // rgb the horizon's colour, w 1 / the yards at which clouds fade into it
float4 gMisc  : register(c13);      // x the horizon's sine

struct I { float2 uv : TEXCOORD0; };

float4 Read(float2 p, float2 gx, float2 gy, float4 l)
{
    return tex2Dgrad(sNoise, p * l.z + l.xy, gx * l.z, gy * l.z);
}

float4 main(I i) : COLOR
{
    float4 h = (i.uv.x * 2.0 - 1.0) * gInv0 + (1.0 - i.uv.y * 2.0) * gInv1 + 0.5 * gInv2 + gInv3;
    float3 d = normalize(h.xyz / h.w);
    float  t = gP.x / max(d.z, 0.01);
    float2 p = d.xy * t;               // yards across the base from under the camera
    float2 gx = ddx(p), gy = ddy(p);

    float fade = smoothstep(0.0, gMisc.x, d.z);
    [branch] if (fade <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);   // under the horizon: the ground is drawn over it

    float a = Read(p, gx, gy, gL0).r;
    float b = Read(p, gx, gy, gL1).g;
    float c = Read(p, gx, gy, gL2).b;
    float big = Read(p, gx, gy, gL3).a;
    float cover = saturate(gSun.w + (big - 0.5) * 0.6);
    float f = a * 0.55 + b * 0.30 + c * 0.15;
    float dens = saturate((f - (1.0 - cover)) / gP.y);
    // Toward the sun across the base: the cloud the light comes through.
    float2 toSun = gSun.xy * 350.0;
    float o = saturate((Read(p + toSun, gx, gy, gL0).r * 0.55 + Read(p + toSun, gx, gy, gL1).g * 0.30 + 0.075
                        - (1.0 - cover)) / gP.y);
    float thick = saturate((f - (1.0 - cover)) / (gP.y * 3.0));
    float L = (1.0 - 0.35 * thick) * (1.0 - 0.40 * o);

    float3 col = lerp(gShade.rgb, gLit.rgb, L);
    float fwd = pow(saturate(dot(d, gSun.xyz)), 8.0);
    col += gLit.rgb * fwd * (1.0 - dens) * 0.8;                // thin edges toward the sun glow
    float hz = saturate(t * gHaze.w);
    hz *= hz;
    col = lerp(col, gHaze.rgb, hz * 0.8);
    float alpha = dens * gLit.w * fade * (1.0 - hz * 0.5);

    if (gP.w > 0.0)
        return float4(0.0, 0.0, dens * fade, 1.0);
    return float4(col * alpha, alpha * (1.0 - gShade.w * (1.0 - L)));
}
)HLSL";

    // The clouds: one sheet each, its corners placed on the CPU, camera-relative.
    const char* kSheetVsHlsl = R"HLSL(
float4 gVP0 : register(c0);   // rows of view-projection: camera-relative world -> clip
float4 gVP1 : register(c1);
float4 gVP2 : register(c2);
float4 gVP3 : register(c3);
struct V { float3 pos : POSITION; float2 s : TEXCOORD0; float4 cell : TEXCOORD1; float4 more : TEXCOORD2; };
struct O { float4 pos : POSITION; float2 s : TEXCOORD0; float4 cell : TEXCOORD1; float4 more : TEXCOORD2;
           float3 rel : TEXCOORD3; };
O main(V v)
{
    O o;
    o.pos = v.pos.x * gVP0 + v.pos.y * gVP1 + v.pos.z * gVP2 + gVP3;
    o.pos.z = 0.5 * o.pos.w;      // past the game's far plane, and depth is off: keep it inside
    o.s = v.s;
    o.cell = v.cell;
    o.more = v.more;
    o.rel = v.pos;
    return o;
}
)HLSL";

    const char* kSheetPsHlsl = R"HLSL(
sampler2D sNoise  : register(s0);   // tiling noise
sampler2D sShapes : register(s1);   // the silhouettes' distance fields, 4 x 4 cells, 0.5 at the edge
float4 gSun   : register(c0);       // xyz the world direction to the sun
float4 gLit   : register(c1);       // rgb the lit side, w the opacity
float4 gShade : register(c2);       // rgb the shaded side, w the share of sky seen through the shade
float4 gP     : register(c3);       // x the edge's width, y the billows' drift, z debug, w the horizon's sine
float4 gHaze  : register(c4);       // rgb the horizon's colour, w 1 / the yards at which clouds fade into it

// s: the sheet, -1..1 (x turned for a cloud that faces left); cell: xy the cell's corner, z 1 a plain cloud and
// 2 a shape, w how formed; more: xy toward the sun across the sheet, z the billows' bite, w the cloud's own seed.
struct I { float2 s : TEXCOORD0; float4 cell : TEXCOORD1; float4 more : TEXCOORD2; float3 rel : TEXCOORD3; };

float4 main(I i) : COLOR
{
    float bite = i.more.z;
    float4 w = tex2D(sNoise, i.s * 0.35 + i.more.ww + gP.yy);
    float4 v = tex2D(sNoise, i.s * 0.9 + i.more.ww * 1.7 - gP.yy);
    float2 s = i.s + (w.rg - 0.5) * bite * 0.45;             // the billows push the edge about
    float2 lo = i.cell.xy + 0.002, hi = i.cell.xy + 0.248;
    float2 cuv = clamp(i.cell.xy + (float2(s.x, -s.y) * 0.5 + 0.5) * 0.25, lo, hi);
    float2 cs  = clamp(cuv + float2(i.more.x, -i.more.y) * 0.125, lo, hi);
    float sd  = tex2D(sShapes, cuv).r;
    float sd2 = tex2D(sShapes, cs).r;
    // Sheet units: half the width is 1, and the field holds 0.75 of that to either side of the edge.
    float puff = v.g * 0.6 + v.b * 0.4;
    float e  = (sd - 0.5) * 0.75 + (puff - 0.5) * bite;      // and they eat it
    float dk = saturate(e / gP.x) * i.cell.w;
    // The thick middle darker, darker again where the cloud lies toward the sun, and a darker foot.
    float lk = (1.0 - 0.35 * saturate(e / 0.35)) * (1.0 - 0.40 * saturate((sd2 - 0.5) * 0.75 / 0.15));
    lk *= lerp(0.80, 1.0, saturate(i.s.y * 0.8 + 0.6));

    float dist = length(i.rel);
    float3 d = i.rel / dist;
    float fade = smoothstep(0.0, gP.w, d.z);
    float3 col = lerp(gShade.rgb, gLit.rgb, lk);
    col += gLit.rgb * pow(saturate(dot(d, gSun.xyz)), 8.0) * (1.0 - dk) * 0.8;   // thin edges toward the sun glow
    float hz = saturate(dist * gHaze.w);
    hz *= hz;
    col = lerp(col, gHaze.rgb, hz * 0.8);
    float a = dk * gLit.w * fade * (1.0 - hz * 0.5);

    if (gP.z > 0.0)
        return float4((i.cell.z > 1.5 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0)) * a, a);
    return float4(col * a, a * (1.0 - gShade.w * (1.0 - lk)));
}
)HLSL";

    constexpr int    kNoise     = 256;      // the noise texture's size a side
    constexpr float  kShapeCell = 6000.0f;  // yards a side of a shape's patch
    constexpr int    kShapeReach = 2;       // shape patches to each side of the camera's
    constexpr int    kMaxClouds = 192;      // sheets drawn at once, the nearest

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps = nullptr;
    IDirect3DVertexShader9* g_vsSheet = nullptr;
    IDirect3DPixelShader9*  g_psSheet = nullptr;
    IDirect3DTexture9*      g_noise = nullptr;
    IDirect3DTexture9*      g_atlas = nullptr;   // made when the loader is done
    IDirect3DStateBlock9*   g_sb = nullptr;
    bool                    g_failed = false;     // off until the next Reset
    bool                    g_probe = false;

    template <typename T> void SafeRelease(T*& p)
    {
        if (p) { p->lpVtbl->Release(p); p = nullptr; }
    }

    // --- numbers -----------------------------------------------------------------------------------

    uint32_t Mix(uint32_t a)
    {
        a ^= a >> 16; a *= 0x7FEB352Du;
        a ^= a >> 15; a *= 0x846CA68Bu;
        a ^= a >> 16;
        return a;
    }
    uint32_t Hash(int x, int y, uint32_t k, uint32_t salt)
    {
        return Mix(static_cast<uint32_t>(x) * 0x8DA6B343u ^ Mix(static_cast<uint32_t>(y) * 0xD8163841u ^ Mix(k ^ salt)));
    }
    float U(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); }

    float Smooth(float e0, float e1, float x)
    {
        float t = (x - e0) / (e1 - e0);
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return t * t * (3.0f - 2.0f * t);
    }

    // Seconds since 1970, UTC: the same on every computer whose clock is set.
    double UtcSeconds()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<double>(u.QuadPart - 116444736000000000ULL) * 1e-7;
    }

    double Frac(double v) { return v - floor(v); }

    void Mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    bool Invert(const D3DMATRIX& src, D3DMATRIX& out)
    {
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j)
                a[i][j] = j < 4 ? src.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int p = c;
            for (int r = c + 1; r < 4; ++r)
                if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
            if (fabs(a[p][c]) < 1e-12)
                return false;
            if (p != c)
                for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
            const double inv = 1.0 / a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] *= inv;
            for (int r = 0; r < 4; ++r)
                if (r != c && a[r][c] != 0.0)
                {
                    const double f = a[r][c];
                    for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
                }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out.m[i][j] = static_cast<float>(a[i][j + 4]);
        return true;
    }

    // --- the noise texture -------------------------------------------------------------------------

    // Gradient noise that tiles every `period` lattice cells.
    float Perlin(float x, float y, int period, uint32_t seed)
    {
        const int x0 = static_cast<int>(floorf(x)), y0 = static_cast<int>(floorf(y));
        const float fx = x - x0, fy = y - y0;
        auto grad = [&](int ix, int iy, float dx, float dy) {
            ix = ((ix % period) + period) % period;
            iy = ((iy % period) + period) % period;
            const float a = U(Hash(ix, iy, seed, 0x51ED270Bu)) * 6.2831853f;
            return cosf(a) * dx + sinf(a) * dy;
        };
        const float u = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
        const float v = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
        const float n00 = grad(x0, y0, fx, fy), n10 = grad(x0 + 1, y0, fx - 1.0f, fy);
        const float n01 = grad(x0, y0 + 1, fx, fy - 1.0f), n11 = grad(x0 + 1, y0 + 1, fx - 1.0f, fy - 1.0f);
        const float nx0 = n00 + (n10 - n00) * u, nx1 = n01 + (n11 - n01) * u;
        return nx0 + (nx1 - nx0) * v;
    }

    // A sum of octaves; billow: each octave folded (1 - |n|), which gives round, puffed shapes.
    float Fbm(float x, float y, int freq, int octaves, uint32_t seed, bool billow)
    {
        float sum = 0.0f, amp = 1.0f;
        for (int o = 0; o < octaves; ++o)
        {
            const int f = freq << o;
            float n = Perlin(x * f, y * f, f, seed + o * 101u);
            if (billow)
                n = 0.7f - fabsf(n) * 2.0f;
            sum += n * amp;
            amp *= 0.5f;
        }
        return sum;
    }

    // Each channel equalised to an even spread over 0..1, so [clouds] cover reads as the share of sky covered.
    void Equalise(std::vector<float>& v)
    {
        std::vector<int> idx(v.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return v[a] < v[b]; });
        std::vector<float> out(v.size());
        for (size_t r = 0; r < idx.size(); ++r)
            out[idx[r]] = (r + 0.5f) / idx.size();
        v.swap(out);
    }

    // The noise's levels as B, G, R, A bytes, built on a thread of its own: about 400 ms, a freeze on the game's
    // thread at the first frame in the world. Until it is done the pass draws nothing.
    std::vector<std::vector<unsigned char>> g_noiseLevels;
    std::atomic<int> g_noiseState{ 0 };   // 0 not started, 1 building, 2 done

    DWORD WINAPI BuildNoise(void*)
    {
        const double t0 = Now();
        const int n = kNoise;
        std::vector<float> cur[4];
        struct Spec { int freq, octaves; uint32_t seed; bool billow; };
        const Spec spec[4] = { { 4, 5, 11, false }, { 4, 5, 23, true }, { 8, 4, 37, true }, { 2, 4, 53, false } };
        for (int c = 0; c < 4; ++c)
        {
            cur[c].resize(static_cast<size_t>(n) * n);
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                    cur[c][y * n + x] = Fbm((x + 0.5f) / n, (y + 0.5f) / n, spec[c].freq, spec[c].octaves, spec[c].seed,
                                            spec[c].billow);
            Equalise(cur[c]);
        }
        // Level 0 from the channels, each next level the mean of four texels of the one before.
        for (int size = n; size >= 1; size /= 2)
        {
            std::vector<unsigned char> level(static_cast<size_t>(size) * size * 4);
            for (int i = 0; i < size * size; ++i)
            {
                // A8R8G8B8 is B, G, R, A in memory.
                level[i * 4 + 2] = static_cast<unsigned char>(cur[0][i] * 255.0f + 0.5f);
                level[i * 4 + 1] = static_cast<unsigned char>(cur[1][i] * 255.0f + 0.5f);
                level[i * 4 + 0] = static_cast<unsigned char>(cur[2][i] * 255.0f + 0.5f);
                level[i * 4 + 3] = static_cast<unsigned char>(cur[3][i] * 255.0f + 0.5f);
            }
            g_noiseLevels.push_back(std::move(level));
            const int half = size / 2;
            for (int c = 0; c < 4 && half > 0; ++c)
            {
                std::vector<float> next(static_cast<size_t>(half) * half);
                for (int y = 0; y < half; ++y)
                    for (int x = 0; x < half; ++x)
                        next[y * half + x] = 0.25f * (cur[c][(2 * y) * size + 2 * x] + cur[c][(2 * y) * size + 2 * x + 1] +
                                                      cur[c][(2 * y + 1) * size + 2 * x] + cur[c][(2 * y + 1) * size + 2 * x + 1]);
                cur[c].swap(next);
            }
        }
        Log("clouds: noise made, %dx%d, %d levels, %.0f ms", n, n, static_cast<int>(g_noiseLevels.size()),
            (Now() - t0) * 1000.0);
        g_noiseState = 2;
        return 0;
    }

    // Starts the build once; uploads it when done. False until there is a texture.
    bool MakeNoise(IDirect3DDevice9* dev)
    {
        int expected = 0;
        if (g_noiseState.compare_exchange_strong(expected, 1))
        {
            if (HANDLE t = CreateThread(nullptr, 0, BuildNoise, nullptr, 0, nullptr))
                CloseHandle(t);
            else
                BuildNoise(nullptr);
        }
        if (g_noiseState != 2)
            return false;
        const int levels = static_cast<int>(g_noiseLevels.size());
        if (FAILED(dev->lpVtbl->CreateTexture(dev, kNoise, kNoise, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_noise, nullptr)))
            return false;
        for (int l = 0, size = kNoise; l < levels; ++l, size /= 2)
        {
            D3DLOCKED_RECT lr = {};
            if (FAILED(g_noise->lpVtbl->LockRect(g_noise, l, &lr, nullptr, 0)))
            {
                SafeRelease(g_noise);
                return false;
            }
            for (int y = 0; y < size; ++y)
                memcpy(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, &g_noiseLevels[l][static_cast<size_t>(y) * size * 4],
                       static_cast<size_t>(size) * 4);
            g_noise->lpVtbl->UnlockRect(g_noise, l);
        }
        return true;
    }

    // The silhouettes' fields, as the loader left them. Five levels: a sixth would mix neighbouring cells.
    bool MakeAtlas(IDirect3DDevice9* dev)
    {
        const unsigned char* src = CloudShapesAtlas();
        if (!src)
            return false;
        const int levels = 5;
        if (FAILED(dev->lpVtbl->CreateTexture(dev, kCloudAtlas, kCloudAtlas, levels, 0, D3DFMT_L8, D3DPOOL_MANAGED,
                                              &g_atlas, nullptr)))
        {
            Log("clouds: the shapes' texture could not be made: no shaped clouds");
            return false;
        }
        std::vector<float> cur(src, src + static_cast<size_t>(kCloudAtlas) * kCloudAtlas);
        int size = kCloudAtlas;
        for (int l = 0; l < levels; ++l)
        {
            D3DLOCKED_RECT lr = {};
            if (FAILED(g_atlas->lpVtbl->LockRect(g_atlas, l, &lr, nullptr, 0)))
                return false;
            for (int y = 0; y < size; ++y)
            {
                unsigned char* row = static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch;
                for (int x = 0; x < size; ++x)
                    row[x] = static_cast<unsigned char>(cur[y * size + x] + 0.5f);
            }
            g_atlas->lpVtbl->UnlockRect(g_atlas, l);
            const int half = size / 2;
            std::vector<float> next(static_cast<size_t>(half) * half);
            for (int y = 0; y < half; ++y)
                for (int x = 0; x < half; ++x)
                    next[y * half + x] = 0.25f * (cur[(2 * y) * size + 2 * x] + cur[(2 * y) * size + 2 * x + 1] +
                                                  cur[(2 * y + 1) * size + 2 * x] + cur[(2 * y + 1) * size + 2 * x + 1]);
            cur.swap(next);
            size = half;
        }
        return true;
    }

    OgBlob* Compile(const char* src, const char* name, const char* profile)
    {
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        if (!compile || FAILED(compile(src, strlen(src), name, nullptr, nullptr, "main", profile, 0, 0, &code, &errs)) || !code)
        {
            Log("clouds: %s failed to compile: %s", name,
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no compiler)");
            if (code) code->lpVtbl->Release(code);
            code = nullptr;
        }
        if (errs) errs->lpVtbl->Release(errs);
        return code;
    }


    IDirect3DVertexShader9* MakeVS(IDirect3DDevice9* dev, const char* src, const char* name)
    {
        IDirect3DVertexShader9* out = nullptr;
        if (OgBlob* code = Compile(src, name, "vs_3_0"))
        {
            if (FAILED(dev->lpVtbl->CreateVertexShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &out)))
                out = nullptr;
            code->lpVtbl->Release(code);
        }
        return out;
    }

    IDirect3DPixelShader9* MakePS(IDirect3DDevice9* dev, const char* src, const char* name)
    {
        IDirect3DPixelShader9* out = nullptr;
        if (OgBlob* code = Compile(src, name, "ps_3_0"))
        {
            if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)), &out)))
                out = nullptr;
            code->lpVtbl->Release(code);
        }
        return out;
    }

    bool MakeShaders(IDirect3DDevice9* dev)
    {
        if (!g_vs)      g_vs      = MakeVS(dev, kVsHlsl, "clouds_vs");
        if (!g_ps)      g_ps      = MakePS(dev, kPsHlsl, "clouds_ps");
        if (!g_vsSheet) g_vsSheet = MakeVS(dev, kSheetVsHlsl, "clouds_sheet_vs");
        if (!g_psSheet) g_psSheet = MakePS(dev, kSheetPsHlsl, "clouds_sheet_ps");
        return g_vs && g_ps && g_vsSheet && g_psSheet;
    }

    // --- the clouds --------------------------------------------------------------------------------

    struct Cloud
    {
        float rel[3];     // the sheet's centre, camera-relative
        float half;       // half its width, yards
        int   cell;       // which silhouette (atlas cell)
        float flip;       // +1 faces right, -1 left
        float weight;     // how formed, 0..1
        bool  shape;      // a picture, not a plain cloud
        float bite;       // how much the billows eat the edge
        float seed;       // the cloud's own place in the noise
        float dist;       // yards from the camera
    };

    uint32_t MapSalt()
    {
        char map[64] = {};
        uint32_t h = 0x9E3779B9u;
        if (ClientMapName(map, sizeof(map)))
            for (const char* c = map; *c; ++c)
                h = Mix(h ^ static_cast<unsigned char>(*c));
        return h;
    }

    float Life(float age, float life, float form)
    {
        return Smooth(0.0f, form, age) * (1.0f - Smooth(life - form, life, age));
    }

    // Every cloud round the camera now: the shapes, the plain clouds, and the plain clouds faded near a shape.
    void FindClouds(const CloudSettings& cl, const float cam[3], double now, const float wind[2], float rain,
                    const D3DMATRIX& view, std::vector<Cloud>& out)
    {
        out.clear();
        const int animals = CloudShapesCount(), puffs = CloudPuffCount();
        const uint32_t salt = MapSalt();

        // The shapes.
        const float half = cl.shapeSize;
        if (animals > 0 && cl.testShape >= 0)
        {
            // Formed, 3200 yards ahead of the camera, for screenshots and tuning.
            float fx = view.m[0][2], fy = view.m[1][2];
            const float len = sqrtf(fx * fx + fy * fy);
            if (len < 1e-3f) { fx = 1.0f; fy = 0.0f; } else { fx /= len; fy /= len; }
            Cloud c = { { fx * 3200.0f, fy * 3200.0f, cl.height + half * 0.74f }, half, cl.testShape % animals, 1.0f,
                        1.0f, true, 0.14f * cl.erode, 0.37f, 3200.0f };
            out.push_back(c);
        }
        else if (animals > 0 && cl.shapes > 0.0f && rain < 1.0f)
        {
            const double life = cl.shapeLife;
            const float  form = std::min(cl.shapeForm, cl.shapeLife * 0.5f);
            const int cx = static_cast<int>(floorf(cam[0] / kShapeCell)), cy = static_cast<int>(floorf(cam[1] / kShapeCell));
            for (int dy = -kShapeReach; dy <= kShapeReach; ++dy)
                for (int dx = -kShapeReach; dx <= kShapeReach; ++dx)
                {
                    const int ix = cx + dx, iy = cy + dy;
                    const double shift = U(Hash(ix, iy, 0u, salt ^ 0xA5A5u)) * life;
                    const double k = floor((now + shift) / life);
                    const float age = static_cast<float>(now + shift - k * life);
                    const uint32_t h = Hash(ix, iy, static_cast<uint32_t>(static_cast<int64_t>(k)), salt);
                    if (U(Mix(h ^ 1u)) * 100.0f >= cl.shapes)
                        continue;
                    Cloud c = {};
                    c.shape = true;
                    c.cell = static_cast<int>(Mix(h ^ 2u) % static_cast<uint32_t>(animals));
                    c.flip = (Mix(h ^ 5u) & 1u) ? 1.0f : -1.0f;
                    c.half = half * (0.85f + 0.3f * U(Mix(h ^ 6u)));
                    c.bite = 0.14f * cl.erode;
                    c.seed = U(Mix(h ^ 8u)) * 7.0f;
                    // Placed so that it drifts through the rolled point at the middle of its life.
                    const float drift = age - static_cast<float>(life) * 0.5f;
                    c.rel[0] = (ix + U(Mix(h ^ 3u))) * kShapeCell + wind[0] * drift - cam[0];
                    c.rel[1] = (iy + U(Mix(h ^ 4u))) * kShapeCell + wind[1] * drift - cam[1];
                    c.rel[2] = cl.height + c.half * 0.74f;   // the silhouette's foot near the base
                    c.weight = Life(age, static_cast<float>(life), form) * (1.0f - rain);
                    if (c.weight > 0.002f)
                        out.push_back(c);
                }
        }
        const size_t shapes = out.size();

        // The plain clouds.
        if (puffs > 0)
        {
            const float pc = cl.puffCell;
            const int reach = static_cast<int>(ceilf(cl.reach / pc));
            const float cover = cl.cover + (0.9f - cl.cover) * rain;
            const float life = cl.puffLife, form = std::min(cl.puffForm, cl.puffLife * 0.5f);
            const int cx = static_cast<int>(floorf(cam[0] / pc)), cy = static_cast<int>(floorf(cam[1] / pc));
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx)
                {
                    const int ix = cx + dx, iy = cy + dy;
                    const double shift = U(Hash(ix, iy, 0u, salt ^ 0x5EEDu)) * life;
                    const double k = floor((now + shift) / life);
                    const float age = static_cast<float>(now + shift - k * life);
                    const uint32_t h = Hash(ix, iy, static_cast<uint32_t>(static_cast<int64_t>(k)), salt ^ 0xC10Du);
                    if (U(Mix(h ^ 1u)) >= cover)
                        continue;
                    Cloud c = {};
                    c.cell = kCloudPuffFirst + static_cast<int>(Mix(h ^ 2u) % static_cast<uint32_t>(puffs));
                    c.flip = (Mix(h ^ 5u) & 1u) ? 1.0f : -1.0f;
                    c.half = cl.puffSize * (0.55f + 0.75f * U(Mix(h ^ 6u)));
                    c.bite = 0.30f * cl.puffBillows;
                    c.seed = U(Mix(h ^ 8u)) * 7.0f;
                    const float drift = age - life * 0.5f;
                    c.rel[0] = (ix + U(Mix(h ^ 3u))) * pc + wind[0] * drift - cam[0];
                    c.rel[1] = (iy + U(Mix(h ^ 4u))) * pc + wind[1] * drift - cam[1];
                    c.rel[2] = cl.height + (U(Mix(h ^ 7u)) - 0.5f) * 300.0f + c.half * 0.35f;
                    const float flat = sqrtf(c.rel[0] * c.rel[0] + c.rel[1] * c.rel[1]);
                    c.weight = Life(age, life, form) * (1.0f - Smooth(cl.reach * 0.8f, cl.reach, flat));
                    // Faded near a shape, so its outline shows.
                    for (size_t s = 0; s < shapes; ++s)
                    {
                        const Cloud& a = out[s];
                        const float ddx = c.rel[0] - a.rel[0], ddy = c.rel[1] - a.rel[1], ddz = c.rel[2] - a.rel[2];
                        const float d = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz), r = a.half + c.half;
                        c.weight *= 1.0f - cl.clearing * a.weight * (1.0f - Smooth(r * 0.7f, r * 1.3f, d));
                    }
                    if (c.weight > 0.002f)
                        out.push_back(c);
                }
        }

        for (Cloud& c : out)
            c.dist = sqrtf(c.rel[0] * c.rel[0] + c.rel[1] * c.rel[1] + c.rel[2] * c.rel[2]);
        // The nearest kMaxClouds, then far ones first, for the blend.
        std::sort(out.begin(), out.end(), [](const Cloud& a, const Cloud& b) { return a.dist < b.dist; });
        if (out.size() > static_cast<size_t>(kMaxClouds))
            out.resize(kMaxClouds);
        std::reverse(out.begin(), out.end());
    }

    struct SheetVertex { float x, y, z, s, t, cellU, cellV, kind, weight, sunX, sunY, bite, seed; };

    // Two triangles a cloud, facing the camera, upright.
    void BuildSheets(const std::vector<Cloud>& clouds, const float sun[3], std::vector<SheetVertex>& out)
    {
        out.clear();
        out.reserve(clouds.size() * 6);
        for (const Cloud& c : clouds)
        {
            const float f[3] = { c.rel[0] / c.dist, c.rel[1] / c.dist, c.rel[2] / c.dist };   // from the camera
            // The viewer's right is f x up; straight overhead, any level line will do.
            float r[3] = { f[1], -f[0], 0.0f };
            float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
            if (rl < 1e-3f) { r[0] = 1.0f; r[1] = 0.0f; rl = 1.0f; }
            r[0] /= rl; r[1] /= rl;
            const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
            float sx = (sun[0] * r[0] + sun[1] * r[1] + sun[2] * r[2]) * c.flip;
            float sy = sun[0] * u[0] + sun[1] * u[1] + sun[2] * u[2];
            const float sl = sqrtf(sx * sx + sy * sy);
            sx = sl > 1e-4f ? sx / sl * 0.10f : 0.0f;
            sy = sl > 1e-4f ? sy / sl * 0.10f : 0.10f;
            const float cu = (c.cell % kCloudCellsSide) * 0.25f, cv = (c.cell / kCloudCellsSide) * 0.25f;
            const float corner[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { -1.0f, 1.0f }, { 1.0f, 1.0f } };
            SheetVertex q[4];
            for (int i = 0; i < 4; ++i)
            {
                const float a = corner[i][0] * c.half, b = corner[i][1] * c.half;
                q[i] = { c.rel[0] + r[0] * a + u[0] * b, c.rel[1] + r[1] * a + u[1] * b, c.rel[2] + r[2] * a + u[2] * b,
                         corner[i][0] * c.flip, corner[i][1], cu, cv, c.shape ? 2.0f : 1.0f, c.weight, sx, sy, c.bite,
                         c.seed };
            }
            const int order[6] = { 0, 1, 2, 2, 1, 3 };
            for (int i : order)
                out.push_back(q[i]);
        }
    }

    // --- the pass ----------------------------------------------------------------------------------

    struct ClipVertex { float x, y, z, u, v; };

    const D3DRENDERSTATETYPE kTouched[] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_CULLMODE, D3DRS_FOGENABLE,
        D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SRGBWRITEENABLE,
        D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_CLIPPING,
    };
    constexpr int kTouchedCount = sizeof(kTouched) / sizeof(kTouched[0]);

    void Lerp3(const float a[3], const float b[3], float t, float out[3])
    {
        for (int i = 0; i < 3; ++i)
            out[i] = a[i] + (b[i] - a[i]) * t;
    }

    std::vector<Cloud>       g_clouds;
    std::vector<SheetVertex> g_sheets;
}

bool CloudsDraw(IDirect3DDevice9* dev)
{
    const CloudSettings& cl = g_cfg.clouds;
    const bool logThis = g_probe;
    g_probe = false;
    float cam[3];
    D3DMATRIX view, proj;
    if (g_failed || !(ClientCamera(cam) || ClientPlayer(cam)) || !SunCamera(view, proj))
    {
        if (logThis)
            Log("clouds: skipped: %s", g_failed ? "resources failed" : "no camera yet");
        return false;
    }
    CloudShapesStart();
    if (!g_sb && FAILED(dev->lpVtbl->CreateStateBlock(dev, D3DSBT_ALL, &g_sb)))
        g_sb = nullptr;
    if (!g_sb || !MakeShaders(dev))
    {
        // A missing effect, never a black sky.
        g_failed = true;
        Log("clouds: resources unavailable: no clouds until the next device reset");
        return false;
    }
    if (!g_noise && !MakeNoise(dev))
    {
        if (logThis)
            Log("clouds: skipped: the noise is still being made");
        return false;
    }
    if (!g_atlas && CloudShapesAtlas() && !MakeAtlas(dev))
        SafeRelease(g_atlas);

    IDirect3DSurface9* rt = nullptr;
    dev->lpVtbl->GetRenderTarget(dev, 0, &rt);
    if (!rt)
        return false;
    D3DSURFACE_DESC rd = {};
    rt->lpVtbl->GetDesc(rt, &rd);
    rt->lpVtbl->Release(rt);

    D3DMATRIX vp, inv;
    Mul(view, proj, vp);
    if (!Invert(vp, inv))
        return false;

    // The sun, the hour and the rain.
    float sun[3] = { 0.5f, 0.3f, 0.81f };
    SunDirection(sun);
    float hour = 12.0f;
    const float night = ClientHour(hour) ? NightWeight(hour) : Smooth(0.05f, -0.15f, sun[2]);
    const float rain = std::clamp(WaterRainAmount(), 0.0f, 1.0f);
    const float warm = (1.0f - night) * Smooth(0.35f, 0.03f, sun[2]);

    // The colours.
    const float dayLit[3]    = { 1.00f, 0.985f, 0.95f };
    const float sunsetLit[3] = { 1.00f, 0.70f, 0.48f };
    const float nightLit[3]  = { 0.30f, 0.34f, 0.45f };
    float lit[3], tmp[3];
    Lerp3(dayLit, sunsetLit, warm, tmp);
    Lerp3(tmp, nightLit, night, lit);
    const float dim = cl.brightness * (1.0f - 0.35f * rain);
    for (float& v : lit) v *= dim;
    DWORD fogCol = 0;
    float fog[3] = { lit[0] * 0.75f, lit[1] * 0.78f, lit[2] * 0.85f };
    if (WorldFogColor(fogCol))
    {
        fog[0] = ((fogCol >> 16) & 0xFF) / 255.0f;
        fog[1] = ((fogCol >> 8) & 0xFF) / 255.0f;
        fog[2] = (fogCol & 0xFF) / 255.0f;
    }
    float shade[3];
    const float cool[3] = { 0.92f, 0.95f, 1.05f }, pink[3] = { 0.10f, 0.02f, 0.06f };
    for (int i = 0; i < 3; ++i)
        shade[i] = (lit[i] * 0.52f * cool[i]) * 0.65f + fog[i] * 0.85f * 0.35f + pink[i] * warm;

    // The high layer's place: the camera and the wind, from the real clock. A billow fixed in the air that drifts
    // with the wind is at the same texel while (its place - the camera) / size + offset holds, so the offset takes
    // the camera and gives back the wind. It was added once, and the layer drifted against the clouds (the owner).
    const double now = UtcSeconds();
    const float wr = cl.windDeg * 0.0174532925f;
    const float wind[2] = { cosf(wr) * cl.windSpeed, sinf(wr) * cl.windSpeed };
    const float  size[4]  = { cl.scale, cl.scale * 0.43f, cl.scale * 0.19f, cl.scale * 3.7f };
    const float  speed[4] = { 1.0f, 1.25f, 1.6f, 0.7f };
    float layers[16];
    for (int l = 0; l < 4; ++l)
    {
        layers[l * 4 + 0] = static_cast<float>(Frac((cam[0] - wind[0] * speed[l] * now) / size[l]));
        layers[l * 4 + 1] = static_cast<float>(Frac((cam[1] - wind[1] * speed[l] * now) / size[l]));
        layers[l * 4 + 2] = 1.0f / size[l];
        layers[l * 4 + 3] = 0.0f;
    }
    const float layerCover = cl.layerCover + (0.97f - cl.layerCover) * rain;
    const bool  drawLayer = layerCover > 0.0f && cl.layerOpacity > 0.0f;

    if (g_atlas)
        FindClouds(cl, cam, now, wind, rain, view, g_clouds);
    else
        g_clouds.clear();
    BuildSheets(g_clouds, sun, g_sheets);

    // --- constants ---------------------------------------------------------------------------------
    float pc[14 * 4] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[r * 4 + c] = inv.m[r][c];
    pc[16] = sun[0]; pc[17] = sun[1]; pc[18] = sun[2]; pc[19] = layerCover;
    pc[20] = lit[0]; pc[21] = lit[1]; pc[22] = lit[2]; pc[23] = cl.layerOpacity;
    pc[24] = shade[0]; pc[25] = shade[1]; pc[26] = shade[2]; pc[27] = cl.shadeSky;
    pc[28] = cl.layerHeight; pc[29] = 0.02f + cl.softness * 0.25f; pc[31] = cl.debug ? 1.0f : 0.0f;
    memcpy(&pc[32], layers, sizeof(layers));
    pc[48] = fog[0]; pc[49] = fog[1]; pc[50] = fog[2]; pc[51] = 1.0f / cl.haze;
    pc[52] = cl.horizon;

    const float sheetPc[5 * 4] = {
        sun[0], sun[1], sun[2], 0.0f,
        lit[0] * cl.puffBrightness, lit[1] * cl.puffBrightness, lit[2] * cl.puffBrightness, cl.opacity,
        shade[0], shade[1], shade[2], cl.shadeSky,
        0.015f + cl.softness * 0.10f, static_cast<float>(Frac(now * 0.004)), cl.debug ? 1.0f : 0.0f, cl.horizon,
        fog[0], fog[1], fog[2], 1.0f / cl.haze,
    };
    float vpRows[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            vpRows[r * 4 + c] = vp.m[r][c];

    if (logThis)
    {
        int shapes = 0;
        for (const Cloud& c : g_clouds)
            shapes += c.shape ? 1 : 0;
        Log("clouds: %d clouds (%d shapes) of %d shapes and %d plain kinds; high layer cover %.2f; rain %.2f, night %.2f, "
            "warm %.2f, sun (%.2f %.2f %.2f), lit (%.2f %.2f %.2f), shade (%.2f %.2f %.2f), fog (%.2f %.2f %.2f), utc %.0f",
            static_cast<int>(g_clouds.size()), shapes, CloudShapesCount(), CloudPuffCount(), layerCover, rain, night, warm,
            sun[0], sun[1], sun[2], lit[0], lit[1], lit[2], shade[0], shade[1], shade[2], fog[0], fog[1], fog[2], now);
        for (const Cloud& c : g_clouds)
            if (c.shape)
                Log("clouds:   shape %s at (%.0f %.0f %.0f) yards from the camera, %.0f wide, formed %.2f",
                    CloudShapeName(c.cell), c.rel[0], c.rel[1], c.rel[2], c.half * 2.0f, c.weight);
    }

    // --- save --------------------------------------------------------------------------------------
    auto* d = dev->lpVtbl;
    g_sb->lpVtbl->Capture(g_sb);
    DWORD saved[kTouchedCount];
    for (int i = 0; i < kTouchedCount; ++i)
        d->GetRenderState(dev, kTouched[i], &saved[i]);
    IDirect3DBaseTexture9*       oldTex[2] = {};
    IDirect3DVertexShader9*      oldVS   = nullptr;
    IDirect3DPixelShader9*       oldPS   = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD                        oldFVF  = 0;
    d->GetTexture(dev, 0, &oldTex[0]);
    d->GetTexture(dev, 1, &oldTex[1]);
    d->GetVertexShader(dev, &oldVS);
    d->GetPixelShader(dev, &oldPS);
    d->GetVertexDeclaration(dev, &oldDecl);
    d->GetFVF(dev, &oldFVF);

    // --- the pass ----------------------------------------------------------------------------------
    d->SetRenderState(dev, D3DRS_ZENABLE,           D3DZB_FALSE);
    d->SetRenderState(dev, D3DRS_ZWRITEENABLE,      FALSE);
    d->SetRenderState(dev, D3DRS_ALPHATESTENABLE,   FALSE);
    d->SetRenderState(dev, D3DRS_ALPHABLENDENABLE,  TRUE);
    d->SetRenderState(dev, D3DRS_SRCBLEND,          D3DBLEND_ONE);
    d->SetRenderState(dev, D3DRS_DESTBLEND,         D3DBLEND_INVSRCALPHA);
    d->SetRenderState(dev, D3DRS_BLENDOP,           D3DBLENDOP_ADD);
    d->SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_CULLMODE,          D3DCULL_NONE);
    d->SetRenderState(dev, D3DRS_FOGENABLE,         FALSE);
    d->SetRenderState(dev, D3DRS_STENCILENABLE,     FALSE);
    d->SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(dev, D3DRS_CLIPPING,          TRUE);
    d->SetRenderState(dev, D3DRS_COLORWRITEENABLE,  D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                    D3DCOLORWRITEENABLE_BLUE);
    d->SetRenderState(dev, D3DRS_SRGBWRITEENABLE,   FALSE);
    for (DWORD s = 0; s < 2; ++s)
    {
        const DWORD address = s == 0 ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
        d->SetSamplerState(dev, s, D3DSAMP_ADDRESSU,  address);
        d->SetSamplerState(dev, s, D3DSAMP_ADDRESSV,  address);
        d->SetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(dev, s, D3DSAMP_MAXMIPLEVEL, 0);
        d->SetSamplerState(dev, s, D3DSAMP_MIPMAPLODBIAS, 0);
        d->SetSamplerState(dev, s, D3DSAMP_SRGBTEXTURE, 0);
    }
    d->SetTexture(dev, 0, reinterpret_cast<IDirect3DBaseTexture9*>(g_noise));
    d->SetTexture(dev, 1, reinterpret_cast<IDirect3DBaseTexture9*>(g_atlas ? g_atlas : g_noise));

    // The high layer, full screen.
    if (drawLayer || cl.debug)
    {
        d->SetVertexShader(dev, g_vs);
        d->SetPixelShader(dev, g_ps);
        d->SetPixelShaderConstantF(dev, 0, pc, 14);
        const float halfPx[4] = { -1.0f / rd.Width, 1.0f / rd.Height, 0.0f, 0.0f };
        d->SetVertexShaderConstantF(dev, 0, halfPx, 1);
        const ClipVertex q[4] = {
            { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
            {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
            { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        };
        d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1);
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(ClipVertex));
    }

    // The clouds, far ones first.
    if (!g_sheets.empty())
    {
        d->SetVertexShader(dev, g_vsSheet);
        d->SetPixelShader(dev, g_psSheet);
        d->SetVertexShaderConstantF(dev, 0, vpRows, 4);
        d->SetPixelShaderConstantF(dev, 0, sheetPc, 5);
        d->SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX3 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE4(1) |
                       D3DFVF_TEXCOORDSIZE4(2));
        d->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, static_cast<UINT>(g_sheets.size() / 3), g_sheets.data(),
                           sizeof(SheetVertex));
    }

    // --- restore -----------------------------------------------------------------------------------
    // Through the vtable first, so every hook's mirror sees the client's values again; then the state block
    // puts the device back exactly.
    for (int i = 0; i < kTouchedCount; ++i)
        d->SetRenderState(dev, kTouched[i], saved[i]);
    d->SetTexture(dev, 0, oldTex[0]);
    d->SetTexture(dev, 1, oldTex[1]);
    d->SetVertexShader(dev, oldVS);
    d->SetPixelShader(dev, oldPS);
    d->SetFVF(dev, oldFVF);
    if (oldDecl)
        d->SetVertexDeclaration(dev, oldDecl);   // after SetFVF, which would otherwise replace it
    g_sb->lpVtbl->Apply(g_sb);
    SafeRelease(oldTex[0]);
    SafeRelease(oldTex[1]);
    SafeRelease(oldVS);
    SafeRelease(oldPS);
    SafeRelease(oldDecl);
    return true;
}

void CloudsReset()
{
    SafeRelease(g_sb);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_vsSheet);
    SafeRelease(g_psSheet);
    SafeRelease(g_noise);
    SafeRelease(g_atlas);
    g_failed = false;
}

void CloudsProbe()
{
    g_probe = true;
}

// The shaders this pass compiles, as it compiles them, for the cache's worker (shadercache.cpp).
void CloudsShaderList()
{
    ShaderPrecompile("clouds_vs", kVsHlsl, "vs_3_0");
    ShaderPrecompile("clouds_ps", kPsHlsl, "ps_3_0");
    ShaderPrecompile("clouds_sheet_vs", kSheetVsHlsl, "vs_3_0");
    ShaderPrecompile("clouds_sheet_ps", kSheetPsHlsl, "ps_3_0");
}

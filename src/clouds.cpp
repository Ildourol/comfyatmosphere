// clouds: our clouds, and now and then one with a shape (2026-10-08).
//
// The game's clouds are one textured strip that scrolls across the sky dome: one picture that tiles and moves as a
// sheet, so it cannot hold a shape. This pass is drawn in its place (comfyatmos.cpp, at the game's cloud draw, which
// it then skips). The sun sprite, the sky dome, the stars and the moons are drawn before it, and the world after it,
// so the world, the grass and the far mountains stay in front, and a cloud before the sun cuts the sun rays (the rays
// take the darker of the sky before the clouds and the finished frame). Where a frame draws no cloud layer, the pass
// is drawn at the end of the sky instead.
//
// One layer (2026-10-10, the owner: one layer, the low clouds removed). Each pixel's line of sight meets a flat
// base [clouds] layerHeight yards over the camera, where four reads of one tiling noise texture give three sizes of
// billow and one of cover, each moving with the wind at its own speed. [clouds] layerSoftness reads coarser levels of
// the noise, so the layer is big soft masses. Every read is tex2Dgrad, with the gradients taken before the branch
// that skips the pixels under the horizon. From 2026-10-08 to 2026-10-10 there were also low clouds: a sheet each,
// facing the camera, in front of this layer, which was then thin. Two kinds of cloud, lit two ways, came out a mix of
// white and grey.
//
// The shapes (a kodo, a turtle) are in the layer. The layer's shader takes the eight nearest, each a sheet facing
// the camera at layerHeight: a line of sight meets the sheet, the silhouette's distance field (cloudshapes.cpp)
// gives the outline, the layer's own billows at that pixel eat its edge ([clouds] erode), the layer thins round it
// ([clouds] clearing), and the two are joined and shaded alike. A kodo overhead is the kodo you see lying in the grass.
//
// Where they are. The sky is the same for every player at one place: the clock is the real one (UTC, which every
// computer keeps to within seconds), not the game's, which comfytime sets for each player. The layer's place is the
// wind times that clock. The shapes come from patches on each map, a grid fixed in the air that the wind carries as it
// carries the layer, each patch rolling [clouds] shapes percent once for a shape. The wind brings them in from the far
// edge of the sky; they cross and leave at the other side.
//
// The colours follow the sun: white by day, warm as the sun sets, grey-blue at night (Night Darkness then darkens
// it with the sky). The shade takes some of the game's fog colour, which the game sets for the zone and the hour.
// In rain the clouds swell into each other until they cover the sky, thickest and darkest where they meet, and no
// shape forms. The clouds follow the rain over about 12 seconds (the overcast), not at once as the water does.
//
// The draw is ps_3_0 with a vs_3_0, blended premultiplied (ONE, INVSRCALPHA): alpha is lowered in the shade, so some
// of the sky's colour shows through it. [clouds] debug 1 (Debug View 31) shows the shapes red and the layer blue.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "cloudshapes.h"
#include "clouds.h"
#include "weather.h"
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

    // The layer, full screen.
    const char* kPsHlsl = R"HLSL(
sampler2D sNoise  : register(s0);   // tiling noise: r, g billows at two sizes, b round puffs, a the cover
float4 gInv0  : register(c0);       // rows of inverse(view-projection): clip -> camera-relative world
float4 gInv1  : register(c1);
float4 gInv2  : register(c2);
float4 gInv3  : register(c3);
float4 gSun   : register(c4);       // xyz the world direction to the sun, w the cover
float4 gLit   : register(c5);       // rgb the lit side, w the opacity
float4 gShade : register(c6);       // rgb the shaded side, w the share of sky seen through the shade
float4 gP     : register(c7);       // x the base's height over the camera, y the edge's width, z the reads'
                                    // footprint (1..16, softer), w debug
float4 gL0    : register(c8);       // per layer: xy its offset, z 1 / its size in yards
float4 gL1    : register(c9);
float4 gL2    : register(c10);
float4 gL3    : register(c11);      // the cover's layer
float4 gHaze  : register(c12);      // rgb the horizon's colour, w 1 / the yards at which clouds fade into it
float4 gMisc  : register(c13);      // x the horizon's sine, y layerSoftness, z Cloud Puffiness
// The shapes in the layer (2026-10-10), eight at most (four until 2026-10-11), four rows each:
// the way to its centre (a unit vector) and its distance over its half-width; its right (turned for one that faces
// left) and how formed; its up; the atlas cell's corner, how much the billows bite and the cutout's width.
// An unused one is formed 0.
float4 gS[32] : register(c14);
float4 gQ     : register(c46);      // x a shape's edge, y Cloud Clearing, z a shape's opacity, w overcast (rain)

struct I { float2 uv : TEXCOORD0; };
sampler2D sShapes : register(s1);   // the silhouettes' fields (cloudshapes.cpp): r the field, 0.5 at the edge;
                                    // a the cutout round each shape: an oval with puffs, as a distance

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
    // [clouds] layerSoftness (2026-10-10, the owner: softer, big blurred masses, not torn bits): the reads take
    // coarser levels of the noise, gP.z times the pixel's own, which leaves out the fine billows.
    float2 gx = ddx(p) * gP.z, gy = ddy(p) * gP.z;

    float fade = smoothstep(0.0, gMisc.x, d.z);
    [branch] if (fade <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);   // under the horizon: the ground is drawn over it
    if (gP.w > 1.5)
        return float4(1.0, 1.0, 1.0, 1.0);   // [clouds] debug 2: the whole sky flat white, for what is drawn over it

    float soft = gMisc.y;
    float4 t0 = Read(p, gx, gy, gL0);
    float a = t0.r;
    float b = Read(p, gx, gy, gL1).g;
    float c = Read(p, gx, gy, gL2).b;
    float big = Read(p, gx, gy, gL3).a;
    // Over 1 in rain (2026-10-12): the clouds swell past full cover and grow thick where they meet.
    float cover = max(0.0, gSun.w + (big - 0.5) * 0.6);
    // Soft: the smallest billows out, the contrast the coarser levels lose put back (so the cover holds), and a
    // wider edge.
    float k = 1.0 + soft * 0.6;
    // Cumulus (Cloud Puffiness, 2026-10-11): the big billows say where the clouds are, the round puffs (b, read
    // with them at their size, so about 520 and 260 yards at Cloud Scale 2600) give them round, lumpy edges and tops.
    float fOld = lerp(a * 0.55 + b * 0.30 + c * 0.15, a * 0.65 + b * 0.35, soft);
    float f = 0.5 + (lerp(fOld, a * 0.45 + t0.b * 0.55, gMisc.z) - 0.5) * k;
    float edge = gP.y * (1.0 + soft * 3.0);
    float dens = saturate((f - (1.0 - cover)) / edge) * step(0.0001, gSun.w);   // Cloud Cover 0: shapes alone

    // The shapes (2026-10-10, the owner: blend them in; until then they were sheets among the low clouds). Each is
    // a sheet facing the camera, met by this line of sight, with its outline from the field. The layer's own
    // billows at this pixel eat the edge, and the layer thins round it by Cloud Clearing, so the outline shows.
    float shapeA = 0.0, near = 0.0, shapeThick = 0.0;
    // The layer's lumps carried into a shape (2026-10-11, the owner: a shape "just got stamped into the clouds map"):
    // thin where the layer's billows are low, thick and shaded where they are high, as the layer round it is.
    float lump = smoothstep(0.2, 0.8, f);
    [unroll] for (int n = 0; n < 8; ++n)   // unrolled: a loop indexed the constants by a chain of compares
    {
        float4 s0 = gS[n * 4], s1 = gS[n * 4 + 1], s2 = gS[n * 4 + 2], s3 = gS[n * 4 + 3];
        // The sheet faces the camera, so its right and up are square to the way to its centre: where this line of
        // sight meets it is d x (distance / dn), and on the sheet (d . right, d . up) times that over the half-width.
        float  dn = dot(d, s0.xyz);
        float2 s  = float2(dot(d, s1.xyz), dot(d, s2.xyz)) * (s0.w / max(dn, 0.05));
        float2 cuv = clamp(s3.xy + (float2(s.x, -s.y) * 0.5 + 0.5) * 0.25, s3.xy + 0.002, s3.xy + 0.248);
        float  sd = tex2Dlod(sShapes, float4(cuv, 0.0, 0.5)).r;
        float  e0 = (sd - 0.5) * 0.75;   // sheet units from the outline, + inside: 0.375 reach either way
        float  e  = e0 + (f - 0.5) * s3.z;
        float  w  = s1.w * step(0.05, dn);
        shapeA = 1.0 - (1.0 - shapeA) * (1.0 - saturate(e / gQ.x) * w * lerp(0.55, 1.0, lump));
        shapeThick = max(shapeThick, saturate(e / (gQ.x * 3.0)) * w * lump);
        // The cutout (2026-10-11, the owner: "draw a circle around the shape, then circles around the edge of that
        // circle in various sizes"): a, made by cloudshapes.cpp, is the distance in from an oval round the shape with
        // puffs on its rim; 1 in a is 1 in sheet units (32 cell texels each way). s3.w (Cloud Shape Cutout Width)
        // grows or shrinks it, and the layer's own billows push the rim a little. Tries before this followed the
        // shape's own outline, grown or blurred, and read as a stamp of it (the owner's screenshots of a fish, a cat).
        float  cut = tex2Dlod(sShapes, float4(cuv, 0.0, 0.0)).a - 0.5;
        // The rim fades on an S-curve from just outside the cutout's edge to 0.35 of the half-width inside it, so the
        // clouds thin all the way in (2026-10-11, the owner: "much smoother", and "it should start inside the cutout";
        // until then a straight ramp 0.06 wide, then a curve centred on the edge).
        near = max(near, w * smoothstep(-0.05, 0.35, cut + s3.w + (f - 0.5) * 0.12));
    }
    dens *= 1.0 - gQ.y * near;
    // Rain (2026-10-12, the owner: the clouds should thicken and flow together, not lie on a dark background): the
    // cover passes 1 (gSun.w), so each cloud swells into its neighbours and the gaps close last. From 2026-10-10
    // the density and the thickness were set flat in rain, and the layer read as grey laid behind the clouds.

    // Toward the sun across the base: the cloud the light comes through.
    float2 toSun = gSun.xy * 350.0;
    float4 t2 = Read(p + toSun, gx, gy, gL0);
    float a2 = t2.r, b2 = Read(p + toSun, gx, gy, gL1).g;
    float f2 = 0.5 + (lerp(lerp(a2 * 0.55 + b2 * 0.30 + 0.075, a2 * 0.65 + b2 * 0.35, soft), a2 * 0.45 + t2.b * 0.55,
                            gMisc.z) - 0.5) * k;
    float o = saturate((f2 - (1.0 - cover)) / edge);
    // The thick middle darker: grey bellies and bright thin parts, more so when soft.
    // A shape is shaded as the layer is: its thick middle as the layer's.
    float thick = max(saturate((f - (1.0 - cover)) / (edge * 3.0)) * step(0.0001, gSun.w), shapeThick);
    float L = (1.0 - lerp(0.35, 0.5, soft) * thick) * (1.0 - 0.40 * o);

    float3 col = lerp(gShade.rgb, gLit.rgb, L);
    float fwd = pow(saturate(dot(d, gSun.xyz)), 8.0);
    col += gLit.rgb * fwd * (1.0 - max(dens, shapeA)) * 0.8;  // thin edges toward the sun glow
    float hz = saturate(t * gHaze.w);
    hz *= hz;
    col = lerp(col, gHaze.rgb, hz * 0.8);
    // A rain cloud is dark, darkest where it is thickest (2026-10-12, the owner: darker). After the haze: the horizon's
    // colour is a light grey in rain, and the deck read paler than the sky behind it.
    col *= 1.0 - gQ.w * (0.3 + 0.3 * thick);
    // The layer and the shapes joined, each with its own opacity.
    float alpha = (1.0 - (1.0 - dens * gLit.w) * (1.0 - shapeA * gQ.z)) * fade * (1.0 - hz * 0.5);

    if (gP.w > 0.0)
        return float4(shapeA * fade, 0.0, dens * fade, 1.0);
    // The shade lets some sky through; a rain cloud does not.
    return float4(col * alpha, alpha * (1.0 - gShade.w * (1.0 - gQ.w * 0.8) * (1.0 - L)));
}
)HLSL";

    constexpr int    kNoise    = 256;      // the noise texture's size a side
    constexpr float  kShapeRange = 15000.0f;   // yards round the camera that shapes are seen to: low over the horizon
    constexpr int    kShapesInLayer = 8;    // shapes the layer's shader takes (gS); 4 until 2026-10-11

    IDirect3DVertexShader9* g_vs = nullptr;
    IDirect3DPixelShader9*  g_ps = nullptr;
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

    // Round puffs that tile every `period` cells (2026-10-11, the owner: the clouds read as cirrus or stratus, "more
    // like cumulus, round and puffy"): cellular noise, one point a cell, and over each point a dome, sqrt(1 - d^2) of
    // the distance d to the nearest point in cells, so each puff is round on top.
    float Puffs(float x, float y, int period, uint32_t seed)
    {
        const float px = x * period, py = y * period;
        const int cx = static_cast<int>(floorf(px)), cy = static_cast<int>(floorf(py));
        float best = 4.0f;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int ix = cx + dx, iy = cy + dy;
                const int wx = ((ix % period) + period) % period, wy = ((iy % period) + period) % period;
                const float fx = ix + 0.15f + 0.7f * U(Hash(wx, wy, seed, 0x9E37u));
                const float fy = iy + 0.15f + 0.7f * U(Hash(wx, wy, seed, 0x79B9u));
                best = std::min(best, (px - fx) * (px - fx) + (py - fy) * (py - fy));
            }
        return sqrtf(std::max(0.0f, 1.0f - best));
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
                {
                    const float u = (x + 0.5f) / n, v = (y + 0.5f) / n;
                    // b: the round puffs, two sizes (until 2026-10-11 small billows, Fbm at 8).
                    cur[c][y * n + x] = c == 2 ? Puffs(u, v, 8, 37u) * 0.65f + Puffs(u, v, 16, 41u) * 0.35f
                                               : Fbm(u, v, spec[c].freq, spec[c].octaves, spec[c].seed, spec[c].billow);
                }
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

    // The silhouettes' fields (r, as L) and their fluffy blobs (a, as A), as the loader left them: A8L8, two bytes
    // a texel (L8, the field alone, until 2026-10-11). Five levels: a sixth would mix neighbouring cells.
    bool MakeAtlas(IDirect3DDevice9* dev)
    {
        const unsigned char* src = CloudShapesAtlas();
        if (!src)
            return false;
        const int levels = 5;
        if (FAILED(dev->lpVtbl->CreateTexture(dev, kCloudAtlas, kCloudAtlas, levels, 0, D3DFMT_A8L8, D3DPOOL_MANAGED,
                                              &g_atlas, nullptr)))
        {
            Log("clouds: the shapes' texture could not be made: no shaped clouds");
            return false;
        }
        std::vector<float> cur(src, src + static_cast<size_t>(kCloudAtlas) * kCloudAtlas * 2);
        int size = kCloudAtlas;
        for (int l = 0; l < levels; ++l)
        {
            D3DLOCKED_RECT lr = {};
            if (FAILED(g_atlas->lpVtbl->LockRect(g_atlas, l, &lr, nullptr, 0)))
                return false;
            for (int y = 0; y < size; ++y)
            {
                unsigned char* row = static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch;
                for (int x = 0; x < size * 2; ++x)
                    row[x] = static_cast<unsigned char>(cur[static_cast<size_t>(y) * size * 2 + x] + 0.5f);
            }
            g_atlas->lpVtbl->UnlockRect(g_atlas, l);
            const int half = size / 2;
            std::vector<float> next(static_cast<size_t>(half) * half * 2);
            for (int y = 0; y < half; ++y)
                for (int x = 0; x < half; ++x)
                    for (int c = 0; c < 2; ++c)
                    {
                        auto at = [&](int yy, int xx) { return cur[(static_cast<size_t>(yy) * size + xx) * 2 + c]; };
                        next[(static_cast<size_t>(y) * half + x) * 2 + c] =
                            0.25f * (at(2 * y, 2 * x) + at(2 * y, 2 * x + 1) + at(2 * y + 1, 2 * x) + at(2 * y + 1, 2 * x + 1));
                    }
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
        return g_vs && g_ps;
    }

    // --- the clouds --------------------------------------------------------------------------------

    struct Cloud
    {
        float rel[3];     // the sheet's centre, camera-relative
        float half;       // half its width, yards
        int   cell;       // which silhouette (atlas cell)
        float flip;       // +1 faces right, -1 left
        float weight;     // how formed, 0..1
        float bite;       // how much the layer's billows eat the edge
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

    // The shapes round the camera now, the kShapesInLayer nearest.
    void FindShapes(const CloudSettings& cl, const float cam[3], double now, const float wind[2], float rain,
                    const D3DMATRIX& view, std::vector<Cloud>& out)
    {
        out.clear();
        const int animals = CloudShapesCount();
        const uint32_t salt = MapSalt();
        const float half = cl.shapeSize;
        if (animals > 0 && cl.testShape >= 0)
        {
            // Formed, 3200 yards ahead of the camera, for screenshots and tuning.
            float fx = view.m[0][2], fy = view.m[1][2];
            const float len = sqrtf(fx * fx + fy * fy);
            if (len < 1e-3f) { fx = 1.0f; fy = 0.0f; } else { fx /= len; fy /= len; }
            Cloud c = { { fx * 3200.0f, fy * 3200.0f, cl.layerHeight }, half, cl.testShape % animals, 1.0f,
                        1.0f, 0.18f * cl.erode, 0.0f };
            out.push_back(c);
        }
        else if (animals > 0 && cl.shapesOn && cl.shapes > 0.0f && rain < 1.0f)
        {
            // The patches ride the wind: their grid is fixed in the air, as the layer's billows are, and the camera's
            // place in the air is its place less the wind times the clock. A patch holds its shape, or none, for good,
            // and the wind brings new patches in from upwind: a shape comes in from the far edge of the sky, fading
            // in over its last 2500 yards, crosses, and fades out at the other side (2026-10-11, the owner: "shapes
            // should come in from back where the skybox starts"). Until then each patch rolled again every 6 minutes,
            // and a shape formed and broke up in the middle of the sky; before that the grid was fixed to the ground.
            // The patches are sized so that about kShapesInLayer of them lie within kShapeRange: Cloud Shapes 100
            // fills the sky with as many as the shader draws. And at least 2.6 half-widths a side, so the spacing
            // below keeps neighbours apart.
            const double cell = std::max(kShapeRange * sqrt(3.14159265 / kShapesInLayer), cl.shapeSize * 2.6);
            const double ax = cam[0] - static_cast<double>(wind[0]) * now, ay = cam[1] - static_cast<double>(wind[1]) * now;
            const int reach = std::min(8, static_cast<int>(ceil(kShapeRange / cell)) + 1);
            const int cx = static_cast<int>(floor(ax / cell)), cy = static_cast<int>(floor(ay / cell));
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx)
                {
                    const int ix = cx + dx, iy = cy + dy;
                    const uint32_t h = Hash(ix, iy, 0u, salt);
                    if (U(Mix(h ^ 1u)) * 100.0f >= cl.shapes)
                        continue;
                    Cloud c = {};
                    c.cell = static_cast<int>(Mix(h ^ 2u) % static_cast<uint32_t>(animals));
                    c.flip = (Mix(h ^ 5u) & 1u) ? 1.0f : -1.0f;
                    c.half = half * (0.85f + 0.3f * U(Mix(h ^ 6u)));
                    c.bite = 0.18f * cl.erode;   // the layer's billows (0.14 of the sheet's own until 2026-10-10)
                    // At its rolled point in the air, which the wind carries past the camera. The point keeps the
                    // shape's half-width (and a little) from the patch's edges, so two neighbours never overlap
                    // (2026-10-11, the owner: shapes "super stacked"). A choice by the view, tried first, left one of
                    // two overlapping shapes out by their distances, and as the wind moved them they traded places: a
                    // shape "changing into different variations".
                    const double m = std::min(0.45, c.half * 1.1 / cell);
                    c.rel[0] = static_cast<float>((ix + m + (1.0 - 2.0 * m) * U(Mix(h ^ 3u))) * cell - ax);
                    c.rel[1] = static_cast<float>((iy + m + (1.0 - 2.0 * m) * U(Mix(h ^ 4u))) * cell - ay);
                    c.rel[2] = cl.layerHeight;   // in the layer (at the low clouds' base until 2026-10-10)
                    const float flat = sqrtf(c.rel[0] * c.rel[0] + c.rel[1] * c.rel[1]);
                    c.weight = Smooth(kShapeRange, kShapeRange - 2500.0f, flat) * (1.0f - rain);
                    if (c.weight > 0.002f)
                        out.push_back(c);
                }
        }
        for (Cloud& c : out)
            c.dist = sqrtf(c.rel[0] * c.rel[0] + c.rel[1] * c.rel[1] + c.rel[2] * c.rel[2]);
        // The kShapesInLayer nearest in the sky, by distance alone, so turning the camera changes nothing (2026-10-11,
        // the owner: "max shapes in the sky, not in view"; a choice of the shapes in front first made them blink as
        // the camera turned).
        std::sort(out.begin(), out.end(), [](const Cloud& a, const Cloud& b) { return a.dist < b.dist; });
        // Only the last one taken fades, by how near the next one is: as they come to change places both are near
        // 0. Fading every one within 1500 yards of the next (the first try) left most of them faint once the shapes
        // were many and close together (the owner: shapes "very faded").
        if (out.size() > static_cast<size_t>(kShapesInLayer))
            out[kShapesInLayer - 1].weight *= Smooth(0.0f, 1500.0f, out[kShapesInLayer].dist - out[kShapesInLayer - 1].dist);
        if (out.size() > static_cast<size_t>(kShapesInLayer))
            out.resize(kShapesInLayer);
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

    std::vector<Cloud>       g_shapes;      // in the layer
    float                    g_overcast = 0.0f;   // the rain, as the clouds follow it
    double                   g_overcastLast = 0.0;
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
    // The overcast: the rain, followed over about 12 s, so the sky closes as a shower comes on and does not jump
    // with how much rain is in view. Light rain covers it too. The server's weather from its packet (weather.cpp,
    // 2026-10-12), so the clouds gather as the weather turns, before the drops show, and clear as it ends: rain and
    // snow, by their grade. Before a packet (the patch not in, or a hot reload), the rain drawn (water.cpp, eased
    // over 0.8 s), as until 2026-10-12.
    {
        const double t = Now();
        int wtype = 0;
        float wgrade = 0.0f;
        const float amount = WeatherServer(wtype, wgrade) ? (wtype == 1 || wtype == 2 ? wgrade : 0.0f)
                                                           : WaterRainAmount();
        const float target = std::min(1.0f, std::clamp(amount, 0.0f, 1.0f) * 2.0f);
        const float ease = g_overcastLast > 0.0 ? static_cast<float>(1.0 - exp(-(t - g_overcastLast) / cl.transitionTime)) : 1.0f;
        g_overcast += (target - g_overcast) * std::min(1.0f, ease);
        g_overcastLast = t;
    }
    const float rain = g_overcast;
    const float warm = (1.0f - night) * Smooth(0.35f, 0.03f, sun[2]);

    // The colours.
    const float dayLit[3]    = { 1.00f, 0.985f, 0.95f };
    const float sunsetLit[3] = { 1.00f, 0.70f, 0.48f };
    const float nightLit[3]  = { 0.30f, 0.34f, 0.45f };
    float lit[3], tmp[3];
    Lerp3(dayLit, sunsetLit, warm, tmp);
    Lerp3(tmp, nightLit, night, lit);
    const float dim = cl.brightness * (1.0f - 0.5f * rain);   // 0.35 until 2026-10-12 (the owner: darker)
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

    // The layer's place: the camera and the wind, from the real clock. A billow fixed in the air that drifts
    // with the wind is at the same texel while (its place - the camera) / size + offset holds, so the offset takes
    // the camera and gives back the wind. It was added once, and the layer drifted against the clouds (the owner).
    const double now = UtcSeconds();
    const float wr = cl.windDeg * 0.0174532925f;
    const float wind[2] = { cosf(wr) * cl.windSpeed, sinf(wr) * cl.windSpeed };
    // Soft, the billows are up to twice as big (layerSoftness, 2026-10-10: big masses).
    const float  big = cl.scale * (1.0f + cl.layerSoftness);
    const float  size[4]  = { big, big * 0.43f, big * 0.19f, cl.scale * 3.7f };
    // Each pattern's speed against the wind: apart, they slide over each other and the clouds change shape and thin
    // as they drift. Cloud Reshaping (2026-10-11, the owner: the clouds "reshape and fade" too much as they move)
    // sets how far apart: 0 moves all four together, 1 is 1, 1.25, 1.6 and 0.7 as until then.
    const float  base[4]  = { 1.0f, 1.25f, 1.6f, 0.7f };
    float speed[4];
    for (int l = 0; l < 4; ++l)
        speed[l] = 1.0f + (base[l] - 1.0f) * cl.reshape;
    float layers[16];
    for (int l = 0; l < 4; ++l)
    {
        layers[l * 4 + 0] = static_cast<float>(Frac((cam[0] - wind[0] * speed[l] * now) / size[l]));
        layers[l * 4 + 1] = static_cast<float>(Frac((cam[1] - wind[1] * speed[l] * now) / size[l]));
        layers[l * 4 + 2] = 1.0f / size[l];
        layers[l * 4 + 3] = 0.0f;
    }
    // In rain the cover passes full (2026-10-12): the clouds swell into each other (the shader's cover).
    const float layerCover = cl.cover + (1.5f - cl.cover) * rain;

    if (g_atlas)
        FindShapes(cl, cam, now, wind, rain, view, g_shapes);
    else
        g_shapes.clear();
    const bool drawLayer = (layerCover > 0.0f && cl.opacity > 0.0f) || !g_shapes.empty();

    // --- constants ---------------------------------------------------------------------------------
    float pc[47 * 4] = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc[r * 4 + c] = inv.m[r][c];
    pc[16] = sun[0]; pc[17] = sun[1]; pc[18] = sun[2]; pc[19] = layerCover;
    pc[20] = lit[0]; pc[21] = lit[1]; pc[22] = lit[2]; pc[23] = cl.opacity + (0.95f - cl.opacity) * rain;
    pc[24] = shade[0]; pc[25] = shade[1]; pc[26] = shade[2]; pc[27] = cl.shadeSky;
    pc[28] = cl.layerHeight; pc[29] = 0.02f + cl.softness * 0.25f; pc[30] = 1.0f + cl.layerSoftness * 15.0f;
    pc[31] = static_cast<float>(cl.debug);
    memcpy(&pc[32], layers, sizeof(layers));
    pc[48] = fog[0]; pc[49] = fog[1]; pc[50] = fog[2]; pc[51] = 1.0f / cl.haze;
    pc[52] = cl.horizon; pc[53] = cl.layerSoftness; pc[54] = cl.puffiness;
    // The shapes in the layer: each a sheet facing the camera, upright; the viewer's right is f x up (straight
    // overhead, any level line will do).
    for (size_t n = 0; n < g_shapes.size(); ++n)
    {
        const Cloud& c = g_shapes[n];
        float* q = &pc[56 + n * 16];
        const float f[3] = { c.rel[0] / c.dist, c.rel[1] / c.dist, c.rel[2] / c.dist };
        float r[3] = { f[1], -f[0], 0.0f };
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
        if (rl < 1e-3f) { r[0] = 1.0f; r[1] = 0.0f; rl = 1.0f; }
        r[0] /= rl; r[1] /= rl;
        const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
        q[0] = f[0]; q[1] = f[1]; q[2] = f[2]; q[3] = c.dist / c.half;
        q[4] = r[0] * c.flip; q[5] = r[1] * c.flip; q[6] = 0.0f; q[7] = c.weight;
        q[8] = u[0]; q[9] = u[1]; q[10] = u[2]; q[11] = 0.0f;
        q[12] = (c.cell % kCloudCellsSide) * 0.25f; q[13] = (c.cell / kCloudCellsSide) * 0.25f; q[14] = c.bite;
        q[15] = -0.15f + cl.cutoutWidth * 0.3f;   // sheet units the cutout is grown by: -0.15 shrunk to +0.15 grown
    }
    for (size_t n = g_shapes.size(); n < static_cast<size_t>(kShapesInLayer); ++n)
        pc[56 + n * 16 + 2] = 1.0f;   // unused: formed 0, and a way that is not 0
    // A shape's edge, in half-widths of its sheet: Cloud Softness alone sets it (2026-10-11, the owner: the shapes'
    // edges "fade too wide"; until then Cloud Layer Softness added 0.2 of it, and at 60 the edge was a fifth of the
    // shape, more so once the shapes grew).
    pc[184] = 0.02f + cl.softness * 0.15f;
    pc[185] = cl.clearing;
    pc[186] = cl.opacity;
    pc[187] = rain;

    if (logThis)
    {
        const int shapes = static_cast<int>(g_shapes.size());
        Log("clouds: %d shapes of %d in the sky; cover %.2f; rain %.2f, night %.2f, "
            "warm %.2f, sun (%.2f %.2f %.2f), lit (%.2f %.2f %.2f), shade (%.2f %.2f %.2f), fog (%.2f %.2f %.2f), utc %.0f",
            shapes, CloudShapesCount(), layerCover, rain, night, warm,
            sun[0], sun[1], sun[2], lit[0], lit[1], lit[2], shade[0], shade[1], shade[2], fog[0], fog[1], fog[2], now);
        for (const Cloud& c : g_shapes)
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

    // The layer, full screen.
    if (drawLayer || cl.debug)
    {
        d->SetVertexShader(dev, g_vs);
        d->SetPixelShader(dev, g_ps);
        d->SetPixelShaderConstantF(dev, 0, pc, 47);
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
}

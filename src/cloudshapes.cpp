// cloudshapes: the silhouettes the shaped clouds take (2026-10-08).
//
// Each picture in Interface\AddOns\ComfyAtmosphere\clouds\ (PNG, or any format Windows decodes) is one shape:
// dark and opaque is the cloud, light or see-through is sky. Draw it from the side, facing right: the pass shows it
// in the cloud layer (clouds.cpp), on a sheet that faces the camera, and turns it to face left at random. The files
// are taken in name order, 16 at most. Until 2026-10-10 the files took 8 cells, and the other 8 held plain clouds
// made here, drawn as the pictures were, below the layer.
//
// Each picture becomes a signed distance field in one 128 x 128 cell of a 512 x 512 atlas (clouds.cpp uploads it):
// the silhouette is fitted to its cell with a margin, made a 256 x 256 mask, measured with an exact Euclidean
// distance transform (Felzenszwalb and Huttenlocher), and halved. A texel holds 128 at the edge, more inside and
// less outside, kCloudRange cell texels to either end. The field, not the mask, lets the pass make the edge soft and
// eat it with the layer's billows.
//
// The work runs on a thread of its own at the first cloud frame (about 20 ms a shape). Windows Imaging Component
// decodes the files; the thread joins COM's multithreaded apartment for that, and leaves it after.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <wincodec.h>

#include "cloudshapes.h"
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace
{
    constexpr int kWork = kCloudCell * 2;     // the mask's size a side, before halving
    constexpr int kMargin = 40;               // mask texels kept clear on each side
    constexpr float kBig = 1e20f;

    std::atomic<bool> g_started{ false };
    std::atomic<bool> g_ready{ false };
    int                       g_count = 0;
    std::vector<unsigned char> g_atlas;
    std::vector<std::string>   g_names;

    // A picture as coverage, 0..1: how much of each pixel is cloud.
    bool Decode(IWICImagingFactory* f, const std::wstring& path, std::vector<float>& cov, UINT& w, UINT& h)
    {
        IWICBitmapDecoder*     dec   = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter*   conv  = nullptr;
        bool ok = false;
        if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                   &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 4096 && h <= 4096)
        {
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())))
            {
                cov.resize(static_cast<size_t>(w) * h);
                for (size_t i = 0; i < cov.size(); ++i)
                {
                    const unsigned char* p = &px[i * 4];   // B, G, R, A
                    const float lum = (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) / 255.0f;
                    cov[i] = (p[3] / 255.0f) * (1.0f - lum);
                }
                ok = true;
            }
        }
        if (conv)  conv->Release();
        if (frame) frame->Release();
        if (dec)   dec->Release();
        return ok;
    }

    // One row or column of the squared distance transform: d[q] = min over p of (q - p)^2 + f[p].
    void Edt1d(const float* f, int n, float* d, int* v, float* z)
    {
        int k = 0;
        v[0] = 0;
        z[0] = -kBig;
        z[1] = kBig;
        for (int q = 1; q < n; ++q)
        {
            float s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0f * q - 2.0f * v[k]);
            while (s <= z[k])
            {
                --k;
                s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0f * q - 2.0f * v[k]);
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = kBig;
        }
        k = 0;
        for (int q = 0; q < n; ++q)
        {
            while (z[k + 1] < q)
                ++k;
            d[q] = (q - v[k]) * static_cast<float>(q - v[k]) + f[v[k]];
        }
    }

    // The distance from each texel to the nearest texel where feature is set, in texels.
    void Edt(const std::vector<bool>& feature, std::vector<float>& out)
    {
        const int n = kWork;
        std::vector<float> g(static_cast<size_t>(n) * n), f(n), d(n), z(n + 1);
        std::vector<int> v(n);
        for (int i = 0; i < n * n; ++i)
            g[i] = feature[i] ? 0.0f : kBig;
        for (int x = 0; x < n; ++x)
        {
            for (int y = 0; y < n; ++y) f[y] = g[y * n + x];
            Edt1d(f.data(), n, d.data(), v.data(), z.data());
            for (int y = 0; y < n; ++y) g[y * n + x] = d[y];
        }
        out.resize(static_cast<size_t>(n) * n);
        for (int y = 0; y < n; ++y)
        {
            Edt1d(&g[y * n], n, d.data(), v.data(), z.data());
            for (int x = 0; x < n; ++x) out[y * n + x] = sqrtf(d[x]);
        }
    }

    // The picture's coverage at a point, by its four nearest pixels.
    float Sample(const std::vector<float>& cov, UINT w, UINT h, float x, float y)
    {
        x -= 0.5f;
        y -= 0.5f;
        const int x0 = static_cast<int>(floorf(x)), y0 = static_cast<int>(floorf(y));
        const float fx = x - x0, fy = y - y0;
        auto at = [&](int px, int py) {
            if (px < 0 || py < 0 || px >= static_cast<int>(w) || py >= static_cast<int>(h))
                return 0.0f;
            return cov[static_cast<size_t>(py) * w + px];
        };
        return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) +
               (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    }

    // One picture into cell `slot` of the atlas. False when it holds no cloud.
    bool MakeField(const std::vector<float>& cov, UINT w, UINT h, int slot)
    {
        // The silhouette's box, so every shape fills its cell alike whatever the picture's margins.
        int x0 = static_cast<int>(w), y0 = static_cast<int>(h), x1 = -1, y1 = -1;
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x)
                if (cov[static_cast<size_t>(y) * w + x] > 0.5f)
                {
                    x0 = std::min(x0, static_cast<int>(x)); x1 = std::max(x1, static_cast<int>(x));
                    y0 = std::min(y0, static_cast<int>(y)); y1 = std::max(y1, static_cast<int>(y));
                }
        if (x1 < x0)
            return false;
        const float bw = static_cast<float>(x1 - x0 + 1), bh = static_cast<float>(y1 - y0 + 1);
        const float room = static_cast<float>(kWork - 2 * kMargin);
        const float k = std::min(room / bw, room / bh);              // mask texels a picture pixel
        const float ox = (kWork - bw * k) * 0.5f, oy = (kWork - bh * k) * 0.5f;

        std::vector<bool> in(static_cast<size_t>(kWork) * kWork), out(in.size());
        for (int y = 0; y < kWork; ++y)
            for (int x = 0; x < kWork; ++x)
            {
                float c = 0.0f;   // 2 x 2 samples a texel
                for (int s = 0; s < 4; ++s)
                {
                    const float mx = x + 0.25f + 0.5f * (s & 1), my = y + 0.25f + 0.5f * (s >> 1);
                    c += Sample(cov, w, h, x0 + (mx - ox) / k, y0 + (my - oy) / k);
                }
                const bool inside = c * 0.25f > 0.5f;
                in[y * kWork + x] = inside;
                out[y * kWork + x] = !inside;
            }
        std::vector<float> toIn, toOut;
        Edt(in, toIn);     // from outside, the way to the cloud
        Edt(out, toOut);   // from inside, the way out
        const int cx = (slot % kCloudCellsSide) * kCloudCell, cy = (slot / kCloudCellsSide) * kCloudCell;
        for (int y = 0; y < kCloudCell; ++y)
            for (int x = 0; x < kCloudCell; ++x)
            {
                float sd = 0.0f;   // mask texels, + inside
                for (int s = 0; s < 4; ++s)
                {
                    const int i = (y * 2 + (s >> 1)) * kWork + x * 2 + (s & 1);
                    sd += in[i] ? toOut[i] - 0.5f : -(toIn[i] - 0.5f);
                }
                sd *= 0.25f * 0.5f;   // the mean, in cell texels
                const float v = 0.5f + sd / (2.0f * kCloudRange);
                g_atlas[(static_cast<size_t>(cy + y) * kCloudAtlas + cx + x) * 2] =
                    static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }

        // The cutout round the shape (2026-10-11, the owner: "draw a circle around the shape, then draw circles around
        // the edge of that circle in various sizes; it's okay if the circles go into the shape"). An oval round the
        // shape's box with some room (an oval, so a long whale gets no round hole wider than its cell), and kPuffs
        // circles of various sizes on its rim, placed by the picture's own numbers, so each shape keeps its own
        // puffy outline. Kept as a distance, + inside, so Cloud Shape Cutout Width grows or shrinks it. Two tries
        // before this followed the shape's own outline, grown or blurred, and read as a stamp of it.
        {
            int bx0 = kWork, by0 = kWork, bx1 = -1, by1 = -1;
            for (int y = 0; y < kWork; ++y)
                for (int x = 0; x < kWork; ++x)
                    if (in[y * kWork + x])
                    {
                        bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
                        by0 = std::min(by0, y); by1 = std::max(by1, y);
                    }
            const float ocx = (bx0 + bx1 + 1) * 0.5f, ocy = (by0 + by1 + 1) * 0.5f;
            float ea = (bx1 - bx0 + 1) * 0.5f * 1.1f, eb = (by1 - by0 + 1) * 0.5f * 1.1f;
            ea = std::max(ea, eb * 0.55f);   // not too thin either way
            eb = std::max(eb, ea * 0.55f);
            const float minAxis = std::min(ea, eb);
            constexpr int kPuffs = 10;
            struct Puff { float x, y, r; };
            Puff puffs[kPuffs];
            uint32_t r = 0x2545F491u * (slot + 7);
            auto rnd = [&r]() { r = r * 1664525u + 1013904223u; return (r >> 8) * (1.0f / 16777216.0f); };
            for (int k = 0; k < kPuffs; ++k)
            {
                const float t = (k + (rnd() - 0.5f) * 0.6f) * 6.2831853f / kPuffs;
                puffs[k] = { ocx + cosf(t) * ea * 0.9f, ocy + sinf(t) * eb * 0.9f, minAxis * (0.22f + 0.18f * rnd()) };
            }
            std::vector<bool> cin(static_cast<size_t>(kWork) * kWork), cout(cin.size());
            for (int y = 0; y < kWork; ++y)
                for (int x = 0; x < kWork; ++x)
                {
                    const float px = x + 0.5f, py = y + 0.5f;
                    bool c = (px - ocx) * (px - ocx) / (ea * ea) + (py - ocy) * (py - ocy) / (eb * eb) <= 1.0f;
                    for (int k = 0; k < kPuffs && !c; ++k)
                        c = (px - puffs[k].x) * (px - puffs[k].x) + (py - puffs[k].y) * (py - puffs[k].y) <=
                            puffs[k].r * puffs[k].r;
                    cin[y * kWork + x] = c;
                    cout[y * kWork + x] = !c;
                }
            std::vector<float> cToIn, cToOut;
            Edt(cin, cToIn);
            Edt(cout, cToOut);
            for (int y = 0; y < kCloudCell; ++y)
                for (int x = 0; x < kCloudCell; ++x)
                {
                    float sd = 0.0f;   // mask texels, + inside the cutout
                    for (int q = 0; q < 4; ++q)
                    {
                        const int i2 = (y * 2 + (q >> 1)) * kWork + x * 2 + (q & 1);
                        sd += cin[i2] ? cToOut[i2] - 0.5f : -(cToIn[i2] - 0.5f);
                    }
                    sd *= 0.25f * 0.5f;   // the mean, in cell texels
                    const float v = 0.5f + sd / (2.0f * kCloudCutRange);
                    g_atlas[(static_cast<size_t>(cy + y) * kCloudAtlas + cx + x) * 2 + 1] =
                        static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
        }
        return true;
    }

    // The atlas as a picture, Logs\comfyatmos_cloudshapes.bmp beside WoW.exe (2026-10-11): each shape white, its
    // cutout grey, so the shapes and their cutouts can be looked at outside the game. Rewritten at each load.
    void WriteAtlasPicture(const wchar_t* exePath)
    {
        std::wstring path = exePath;
        path.resize(path.find_last_of(L"\\/") + 1);
        path += L"Logs\\comfyatmos_cloudshapes.bmp";
        const int n = kCloudAtlas, row = n * 3;
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + row * n;
        ih.biSize = sizeof(ih);
        ih.biWidth = n;
        ih.biHeight = -n;   // top-down
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        std::vector<unsigned char> px(static_cast<size_t>(row) * n);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const unsigned char field = g_atlas[(static_cast<size_t>(y) * n + x) * 2];
                const unsigned char cut = g_atlas[(static_cast<size_t>(y) * n + x) * 2 + 1];
                const unsigned char v = field >= 128 ? 255 : (cut >= 128 ? 120 : 30);
                unsigned char* p = &px[static_cast<size_t>(y) * row + x * 3];
                p[0] = p[1] = p[2] = v;
            }
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return;
        DWORD wrote = 0;
        WriteFile(h, &fh, sizeof(fh), &wrote, nullptr);
        WriteFile(h, &ih, sizeof(ih), &wrote, nullptr);
        WriteFile(h, px.data(), static_cast<DWORD>(px.size()), &wrote, nullptr);
        CloseHandle(h);
    }

    DWORD WINAPI Load(void*)
    {
        const double t0 = Now();
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir.resize(dir.find_last_of(L"\\/") + 1);
        dir += L"Interface\\AddOns\\ComfyAtmosphere\\clouds\\";

        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    files.push_back(fd.cFileName);
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });

        g_atlas.assign(static_cast<size_t>(kCloudAtlas) * kCloudAtlas * 2, 0);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))))
            Log("clouds: the image decoder is not available: no shaped clouds");
        else
        {
            const int maxShapes = kCloudAnimals;
            for (const std::wstring& name : files)
            {
                std::string narrow;
                for (wchar_t c : name.substr(0, name.size() - 4))
                    narrow += static_cast<char>(c < 128 ? c : '?');
                if (g_count >= maxShapes)
                {
                    Log("clouds: %s left out: %d shapes at most", narrow.c_str(), maxShapes);
                    continue;
                }
                std::vector<float> cov;
                UINT w = 0, ht = 0;
                if (!Decode(f, dir + name, cov, w, ht))
                    Log("clouds: %s could not be read", narrow.c_str());
                else if (!MakeField(cov, w, ht, g_count))
                    Log("clouds: %s has no dark, opaque part: left out", narrow.c_str());
                else
                {
                    g_names.push_back(narrow);
                    ++g_count;
                }
            }
            f->Release();
        }
        if (SUCCEEDED(co))
            CoUninitialize();
        std::string list;
        for (const std::string& n : g_names)
            list += (list.empty() ? "" : ", ") + n;
        Log("clouds: %d shapes in %.0f ms (%s)", g_count, (Now() - t0) * 1000.0,
        g_count ? list.c_str() : "none");
        WriteAtlasPicture(exe);
        g_ready = true;
        return 0;
    }
}

void CloudShapesStart()
{
    if (g_started.exchange(true))
        return;
    if (HANDLE t = CreateThread(nullptr, 0, Load, nullptr, 0, nullptr))
        CloseHandle(t);
    else
        g_ready = true;
}

bool CloudShapesReady() { return g_ready; }
int  CloudShapesCount() { return g_ready ? g_count : 0; }
const unsigned char* CloudShapesAtlas() { return g_ready && g_count ? g_atlas.data() : nullptr; }
const char* CloudShapeName(int i) { return g_ready && i >= 0 && i < g_count ? g_names[i].c_str() : ""; }

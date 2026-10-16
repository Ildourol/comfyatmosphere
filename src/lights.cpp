// lights: the game's light tables, read from the files (2026-10-16, the owner: the water's colour taken from what the
// game draws "has not worked in the past and isn't going to work now. We have the map files.").
//
// The client colours its sky, fog and water from three tables in DBFilesClient:
//
//   Light.dbc        light zones on each map: a place, an inner and an outer radius, and a LightParams row for each
//                    kind of weather (clear first). A map's default zone has no place and no radii.
//   LightParams.dbc  for each set: the sky box, the glow, the water's alpha values, and so on.
//   LightIntBand.dbc 18 colour bands for each set (row id = (set - 1) x 18 + band + 1), each a curve of up to 16
//                    keys over the day, in half-minutes from midnight (0 to 2880).
//
// Step 1 reads them and works the bands out where you stand, for the probe, to be checked against what the client
// drew (NOTES.md, the zone's sky and water colours). The water takes its sky, its glow and its body's colour from
// them, and the mist its glow (water.cpp, SkyGlowColour). Checked (2026-10-16) at the Goldshire
// pool (the map's default zone), the Westfall coast, Booty Bay and Auberdine at noon: the sky's six rings and the
// water's ramp within 2 of 255 inside a zone or in the default, 1 to 5 where two zones fade into each other.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "lights.h"
#include "client.h"
#include "common.h"
#include "mapterrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr int kBands = 18;

    struct Dbc
    {
        std::vector<uint8_t> data;
        uint32_t rows = 0, fields = 0, size = 0, strings = 0;
        bool Parse()
        {
            if (data.size() < 20 || memcmp(data.data(), "WDBC", 4) != 0)
                return false;
            memcpy(&rows, &data[4], 4);
            memcpy(&fields, &data[8], 4);
            memcpy(&size, &data[12], 4);
            memcpy(&strings, &data[16], 4);
            return size >= 4 && 20 + static_cast<size_t>(rows) * size + strings <= data.size();
        }
        uint32_t U(uint32_t r, uint32_t f) const
        {
            uint32_t v;
            memcpy(&v, &data[20 + static_cast<size_t>(r) * size + f * 4], 4);
            return v;
        }
        float F(uint32_t r, uint32_t f) const
        {
            float v;
            memcpy(&v, &data[20 + static_cast<size_t>(r) * size + f * 4], 4);
            return v;
        }
        const char* S(uint32_t off) const
        {
            const size_t base = 20 + static_cast<size_t>(rows) * size;
            return off < strings ? reinterpret_cast<const char*>(&data[base + off]) : "";
        }
    };

    enum { kMap, kLight, kParams, kBand, kFiles };
    const char* const kNames[kFiles] = { "DBFilesClient\\Map.dbc", "DBFilesClient\\Light.dbc",
                                         "DBFilesClient\\LightParams.dbc", "DBFilesClient\\LightIntBand.dbc" };
    Dbc  g_dbc[kFiles];
    int  g_state[kFiles] = {};   // 0 asked, 1 read, -1 failed
    bool g_ready = false;

    struct Light
    {
        uint32_t id = 0, map = 0;
        float    raw[3] = {}, start = 0.0f, end = 0.0f;   // as stored
        uint32_t params[8] = {};
        int      nParams = 0;
    };
    std::vector<Light> g_lights;
    struct Band { uint32_t n = 0; uint32_t t[16] = {}; uint32_t v[16] = {}; };
    std::unordered_map<uint32_t, Band> g_bands;   // by row id
    std::unordered_map<std::string, uint32_t> g_mapIds;   // by internal name, upper case

    std::string Upper(const char* s)
    {
        std::string u = s;
        for (char& c : u)
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        return u;
    }

    void Build()
    {
        const Dbc& m = g_dbc[kMap];
        for (uint32_t r = 0; r < m.rows; ++r)
            g_mapIds[Upper(m.S(m.U(r, 1)))] = m.U(r, 0);
        const Dbc& l = g_dbc[kLight];
        for (uint32_t r = 0; r < l.rows && l.fields >= 8; ++r)
        {
            Light x;
            x.id = l.U(r, 0);
            x.map = l.U(r, 1);
            for (int i = 0; i < 3; ++i)
                x.raw[i] = l.F(r, 2 + i);
            x.start = l.F(r, 5);
            x.end = l.F(r, 6);
            x.nParams = (std::min)(static_cast<int>(l.fields) - 7, 8);
            for (int i = 0; i < x.nParams; ++i)
                x.params[i] = l.U(r, 7 + i);
            g_lights.push_back(x);
        }
        const Dbc& b = g_dbc[kBand];
        for (uint32_t r = 0; r < b.rows && b.fields >= 34; ++r)
        {
            Band x;
            x.n = (std::min)(b.U(r, 1), 16u);
            for (int i = 0; i < 16; ++i)
            {
                x.t[i] = b.U(r, 2 + i);
                x.v[i] = b.U(r, 18 + i);
            }
            g_bands[b.U(r, 0)] = x;
        }
    }

    // A light's place in world coordinates: Light.dbc stores it as the map files store places, in inches (36 to the
    // yard) from the map's corner, the axes turned: Stormwind's zone #77 lands at (-8801 578).
    void Place(const Light& l, float out[3])
    {
        out[0] = 17066.666f - l.raw[2] / 36.0f;
        out[1] = 17066.666f - l.raw[0] / 36.0f;
        out[2] = l.raw[1] / 36.0f;
    }

    bool IsDefault(const Light& l)
    {
        return l.raw[0] == 0.0f && l.raw[1] == 0.0f && l.raw[2] == 0.0f && l.end == 0.0f;
    }

    // A band's colour at a time in half-minutes, between the keys around it, round midnight too.
    bool BandAt(uint32_t params, int band, float halfMinutes, float rgb[3])
    {
        auto it = g_bands.find((params - 1) * kBands + static_cast<uint32_t>(band) + 1);
        if (params == 0 || it == g_bands.end() || it->second.n == 0)
            return false;
        const Band& b = it->second;
        auto col = [](uint32_t v, float c[3]) {
            c[0] = ((v >> 16) & 0xFF) / 255.0f; c[1] = ((v >> 8) & 0xFF) / 255.0f; c[2] = (v & 0xFF) / 255.0f;
        };
        if (b.n == 1)
        {
            col(b.v[0], rgb);
            return true;
        }
        uint32_t i = 0;
        while (i + 1 < b.n && b.t[i + 1] <= halfMinutes)
            ++i;
        const uint32_t j = (i + 1) % b.n;
        float t0 = static_cast<float>(b.t[i]), t1 = static_cast<float>(b.t[j]), t = halfMinutes;
        if (t < t0) t += 2880.0f;
        if (t1 <= t0) t1 += 2880.0f;
        const float f = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
        float a[3], c[3];
        col(b.v[i], a);
        col(b.v[j], c);
        for (int k = 0; k < 3; ++k)
            rgb[k] = a[k] + (c[k] - a[k]) * f;
        return true;
    }

    bool MapId(uint32_t& id)
    {
        char name[64] = {};
        if (!ClientMapName(name, sizeof(name)))
            return false;
        auto it = g_mapIds.find(Upper(name));
        if (it == g_mapIds.end())
            return false;
        id = it->second;
        return true;
    }

    float HalfMinutes()
    {
        float hour = 12.0f;
        ClientHour(hour);
        return hour * 120.0f;
    }

    // The bands where you stand: the map's default zone, then each zone you are within, the larger first, mixed in by
    // how far inside its outer radius you are (all of it inside the inner radius). Clear weather.
    bool Blend(const float pl[3], uint32_t map, float halfMinutes, float out[kBands][3])
    {
        const Light* def = nullptr;
        std::vector<std::pair<const Light*, float>> in;
        for (const Light& l : g_lights)
        {
            if (l.map != map)
                continue;
            if (IsDefault(l))
            {
                def = &l;
                continue;
            }
            float p[3];
            Place(l, p);
            const float dx = p[0] - pl[0], dy = p[1] - pl[1], dz = p[2] - pl[2];
            const float d = sqrtf(dx * dx + dy * dy + dz * dz), s = l.start / 36.0f, e = l.end / 36.0f;
            if (d < e)
                in.push_back({ &l, d <= s ? 1.0f : (e > s ? (e - d) / (e - s) : 1.0f) });
        }
        if (!def && in.empty())
            return false;
        std::sort(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.first->end > b.first->end; });
        for (int b = 0; b < kBands; ++b)
        {
            float c[3] = {};
            if (def)
                BandAt(def->params[0], b, halfMinutes, c);
            for (const auto& w : in)
            {
                float x[3];
                if (BandAt(w.first->params[0], b, halfMinutes, x))
                    for (int k = 0; k < 3; ++k)
                        c[k] += (x[k] - c[k]) * w.second;
            }
            memcpy(out[b], c, sizeof(c));
        }
        return true;
    }

    // The bands where you stand, worked out four times a second (LightsFrame) for the water and the mist.
    float  g_now[kBands][3] = {};
    bool   g_haveNow = false;
    double g_nextNow = 0.0;

    unsigned Hex(const float c[3])
    {
        auto q = [](float v) { return static_cast<unsigned>((std::min)((std::max)(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
        return (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
    }
}

void LightsFrame()
{
    if (g_ready)
    {
        const double now = Now();
        if (now < g_nextNow)
            return;
        g_nextNow = now + 0.25;
        uint32_t map = 0;
        float pl[3];
        g_haveNow = ClientPlayer(pl) && MapId(map) && Blend(pl, map, HalfMinutes(), g_now);
        return;
    }
    bool all = true;
    for (int i = 0; i < kFiles; ++i)
    {
        if (g_state[i] == 0)
        {
            const int r = MapRequestFile(kNames[i], g_dbc[i].data);
            if (r != 0)
            {
                g_state[i] = r > 0 && g_dbc[i].Parse() ? 1 : -1;
                Log("lights: %s %s", kNames[i], g_state[i] > 0 ? "read" : "could not be read");
            }
        }
        all = all && g_state[i] != 0;
    }
    if (!all)
        return;
    if (g_state[kMap] > 0 && g_state[kLight] > 0 && g_state[kBand] > 0)
        Build();
    g_ready = true;
}

bool LightsBand(int band, float rgb[3])
{
    if (!g_haveNow || band < 0 || band >= kBands)
        return false;
    memcpy(rgb, g_now[band], sizeof(g_now[band]));
    return true;
}

void LightsProbe()
{
    if (!g_ready)
    {
        Log("lights: the tables are not read yet (%d %d %d %d)", g_state[0], g_state[1], g_state[2], g_state[3]);
        return;
    }
    for (int i = 0; i < kFiles; ++i)
        Log("lights: %s: %u rows of %u fields (%u bytes)", kNames[i], g_dbc[i].rows, g_dbc[i].fields, g_dbc[i].size);
    uint32_t map = 0;
    float pl[3];
    const bool havePl = ClientPlayer(pl);
    const bool haveMap = MapId(map);
    const float hm = HalfMinutes();
    Log("lights: map id %d, you at (%.1f %.1f %.1f), %.0f half-minutes (%02d:%02d)", haveMap ? static_cast<int>(map) : -1,
        pl[0], pl[1], pl[2], hm, static_cast<int>(hm / 120.0f), static_cast<int>(fmodf(hm / 2.0f, 60.0f)));
    if (!havePl || !haveMap)
        return;
    // The zones nearest you, and the default one.
    std::vector<std::pair<float, const Light*>> nearest;
    for (const Light& l : g_lights)
    {
        if (l.map != map)
            continue;
        if (IsDefault(l))
        {
            Log("lights: default zone #%u, sets %u %u %u %u", l.id, l.params[0], l.params[1], l.params[2], l.params[3]);
            continue;
        }
        float p[3];
        Place(l, p);
        const float dx = p[0] - pl[0], dy = p[1] - pl[1], dz = p[2] - pl[2];
        nearest.push_back({ sqrtf(dx * dx + dy * dy + dz * dz), &l });
    }
    std::sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < nearest.size() && i < 4; ++i)
    {
        const Light& l = *nearest[i].second;
        float p[3];
        Place(l, p);
        Log("lights: zone #%u at (%.1f %.1f %.1f), %.0f yd from you, radii %.0f / %.0f yd, sets %u %u %u %u", l.id, p[0],
            p[1], p[2], nearest[i].first, l.start / 36.0f, l.end / 36.0f, l.params[0], l.params[1], l.params[2],
            l.params[3]);
    }
    float out[kBands][3];
    if (!Blend(pl, map, hm, out))
        return;
    // 2 to 7 the sky from the zenith down to the fog at the horizon, 14 and 15 the ocean shallow and deep, 16 and 17
    // a river or a lake shallow and deep (matched against the drawn sky rings and water ramps).
    char line[400];
    int k = snprintf(line, sizeof(line), "lights: the bands where you stand:");
    for (int b = 0; b < kBands && k > 0 && k < static_cast<int>(sizeof(line)); ++b)
        k += snprintf(line + k, sizeof(line) - k, " %d %06X", b, Hex(out[b]));
    Log("%s", line);
}

// sun: where the sun is and where the camera looks, for the shadow map and the volumetric light.
//
// The sun comes from the game clock and a measured table of where the sky draws the sun and the two moons
// (sunpath.cpp, 2026-10-14). By day the light follows the sun. At night it follows the larger moon while it is up,
// else the other, and the other moon casts rays of its own ([rays] secondMoon). [sun] fixed = 1 replaces it with a
// fixed direction instead.
//
// Until 2026-10-14 the sun was read from the sprite the sky draws for it: a quad under a view whose translation is
// the sun's camera-space position (comfyatmos.cpp, NoteSkySun). The sky draws a sprite only where the camera can
// see it, so the sun followed whatever sprite was in view, a moon among them, and the shadows turned as the camera
// turned. The sprites are still handed here (SunSetView): [sun] measure logs them for the table, and the probe
// compares them with it (SunProbe).
//
// At night the rays and the light follow a moon as if it were the sun. NightScale turns them down by
// the game clock.
//
// This code was part of the screen-space sun rays (rays.cpp) until the rays were removed.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "common.h"
#include "config.h"
#include "sun.h"
#include "sunpath.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    D3DMATRIX g_view = {}, g_proj = {};
    bool      g_haveView = false, g_haveProj = false;

    unsigned g_sunFrame  = 0;       // counts frames, so the glide below advances once a frame
    // The sun-shaped quads of this frame's sky, in camera space, and their textures' widths: the sun's is 128, the
    // moons' 512 and 256 (tests/sun-table.json).
    constexpr int kMaxQuads = 4;
    float    g_quads[kMaxQuads][3] = {};
    unsigned g_quadTex[kMaxQuads] = {};
    int      g_nQuads       = 0;
    bool     g_quadsFresh   = false;
    // The last sky's quads in the world, for the probe.
    float    g_lastQuads[kMaxQuads][3] = {};
    unsigned g_lastTex[kMaxQuads] = {};
    int      g_nLast        = 0;
    unsigned g_lastQuadFrame = 0;

    // This frame's light from the table (SunPathLight), worked out once a frame.
    float    g_tableDir[3]   = { 0.0f, 0.0f, 1.0f };
    float    g_secondDir[3]  = { 0.0f, 0.0f, 1.0f };   // the other moon, world space, for the rays
    bool     g_haveSecond    = false;
    bool     g_haveTable     = false;
    SkyBody  g_tableBody     = kBodySun;
    unsigned g_tableFrame    = ~0u;
    SkyBody  g_loggedBody    = kBodies;

    // For the stats (SunStatsText, 2026-10-03): this frame's measurement and where it came from, the glided
    // sun, the shadows' held sun, and how often the held sun stepped.
    float    g_statMeas[3]  = { 0.0f, 0.0f, 1.0f };
    int      g_statSrc      = -1;      // -1 none yet, 0 the table, 2 [sun] fixed
    float    g_statGlide[3] = { 0.0f, 0.0f, 1.0f };
    float    g_statHeld[3]  = { 0.0f, 0.0f, 1.0f };
    bool     g_statHaveHeld = false;
    unsigned g_heldSteps    = 0;       // since the last stats line
    double   g_heldStepAt   = 0.0;

    // Camera space -> world space. The camera matrix is a pure rotation here (this client folds the
    // camera position into every world matrix), so its inverse is its transpose: for D3D's row vectors,
    // world[i] = sum_j view_dir[j] * V[i][j].
    bool ViewToWorld(const float v[3], float d[3])
    {
        for (int i = 0; i < 3; ++i)
            d[i] = v[0] * g_view.m[i][0] + v[1] * g_view.m[i][1] + v[2] * g_view.m[i][2];
        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(len > 1e-4f))
            return false;
        for (int i = 0; i < 3; ++i)
            d[i] /= len;
        return true;
    }

    float Azimuth(const float d[3])   { return atan2f(d[1], d[0]) * 57.29578f; }
    float Elevation(const float d[3]) { return asinf(d[2] < -1.0f ? -1.0f : (d[2] > 1.0f ? 1.0f : d[2])) * 57.29578f; }

    const char* BodyName(SkyBody b)
    {
        return b == kBodySun ? "the sun" : b == kBodyMoon1 ? "the larger moon" : "the smaller moon";
    }

    // This frame's sky quads into the world, once a frame, for the probe and [sun] measure (2026-10-14): each
    // quad's world direction, its texture's width and the game time, at most four times a second.
    // tests/sun-table.json steps the hour and the camera, and tests/Sun-Table.ps1 turns these lines into the
    // table in sunpath.cpp.
    void NoteQuads()
    {
        if (!g_quadsFresh)
            return;
        g_quadsFresh = false;
        const int n = g_nQuads;
        if (!g_haveView || n == 0)
            return;
        g_nLast = 0;
        g_lastQuadFrame = g_sunFrame;
        for (int q = 0; q < n; ++q)
            if (ViewToWorld(g_quads[q], g_lastQuads[g_nLast]))
                g_lastTex[g_nLast++] = g_quadTex[q];
        if (!g_cfg.sun.measure)
            return;
        static double next = 0.0;
        const double now = Now();
        float hour = -1.0f;
        if (now < next || !ClientHour(hour))
            return;
        next = now + 0.25;
        char line[400];
        int at = snprintf(line, sizeof(line), "sun measure: hour %.4f, %d quad%s:", hour, g_nLast,
                          g_nLast == 1 ? "" : "s");
        for (int q = 0; q < g_nLast && at > 0 && at < static_cast<int>(sizeof(line)); ++q)
            at += snprintf(line + at, sizeof(line) - at, " %.2f/%.2f tex %u;", Azimuth(g_lastQuads[q]),
                           Elevation(g_lastQuads[q]), g_lastTex[q]);
        const float f[3] = { g_view.m[0][2], g_view.m[1][2], g_view.m[2][2] };
        if (at > 0 && at < static_cast<int>(sizeof(line)))
            snprintf(line + at, sizeof(line) - at, " camera %.1f/%.1f", Azimuth(f), Elevation(f));
        Log("%s", line);
    }

    // This frame's light from the table, once a frame. Logged when the body it follows changes.
    bool TableLight()
    {
        if (g_tableFrame == g_sunFrame)
            return g_haveTable;
        g_tableFrame = g_sunFrame;
        float hour = 0.0f;
        if (!ClientHour(hour))
        {
            g_haveTable = g_haveSecond = false;
            return false;
        }
        g_tableBody = SunPathLight(hour, NightWeight(hour) >= 0.5f, g_tableDir, g_secondDir, g_haveSecond);
        g_haveTable = true;
        if (g_tableBody != g_loggedBody)
        {
            g_loggedBody = g_tableBody;
            Log("sun: the light follows %s from the clock's table, at azimuth %.1f, elevation %.1f, game time %02d:%02d",
                BodyName(g_tableBody), Azimuth(g_tableDir), Elevation(g_tableDir), static_cast<int>(hour),
                static_cast<int>(hour * 60.0f) % 60);
        }
        return true;
    }
}

bool SunDirection(float dir[3])
{
    NoteQuads();
    // [sun] fixed = 1 pins the sun for the shadow map and the volumetric light, whatever the game's clock
    // says.
    const SunSettings& s = g_cfg.sun;
    if (s.fixed)
    {
        const float az = s.azimuth * 0.01745329f, el = s.elevation * 0.01745329f;
        dir[0] = cosf(el) * cosf(az); dir[1] = cosf(el) * sinf(az); dir[2] = sinf(el);
        memcpy(g_statMeas, dir, sizeof(g_statMeas));
        memcpy(g_statGlide, dir, sizeof(g_statGlide));
        g_statSrc = 2;
        return true;
    }
    if (!TableLight())
        return false;
    const float* meas = g_tableDir;

    // The sprite is measured afresh every frame and the measurement is noisy: standing still, with the
    // camera still and the time pinned, the direction wandered in the fifth decimal. Everything here is
    // built around it, so that wander turned the shadow map a little each frame. Until 2026-09-29 the
    // direction was taken only once it had moved 0.05 degrees: the map then held still and jumped, and
    // about every 30 seconds the long shadows of a low sun moved some ten pixels at once. Now it glides
    // toward the measurement with a time constant of 10 seconds: the noise averages out, the real sun is
    // followed a hair each frame, and a change of more than 1.5 degrees (the time set by hand) lands at once.
    // The 10 seconds are [sun] glide since 2026-09-30 (the Sun Smoothing control); 0 follows at once.
    // 1.5 degrees since 2026-10-03, 5 until then: a comfytime step moves the sky 2.8 degrees, and glided it
    // barely showed, until a few steps passed 5 and the light, the shadows and the glint jumped at once.
    // At a steady speed since 2026-10-03, not an ease. The sky moves its sun 0.56 degrees once a game minute,
    // and an ease of 3 seconds made most of that move in its first second: with [shadow] sunStep 0 every
    // shadow gave a small snap once a minute. Now a step of the sky (more than 0.1 degrees: the wander is a
    // thousandth) starts a straight move from where the sun is to the new place, as long as the time since
    // the sky's last step and at least glide seconds. The sun then moves on all the time at the sky's own
    // pace, one step behind it.
    // Since 2026-10-14 the measurement is the table (sunpath.cpp), which has no wander and moves with the clock's
    // day fraction. The glide still smooths a time stepped by hand.
    static float  stable[3] = { 0.0f, 0.0f, 0.0f };
    static bool   have = false;
    static double last = 0.0;
    static unsigned frame = ~0u;
    // Once a frame: the shadow map, the sun shadows, the light and the rays all ask, and must agree.
    if (have && frame == g_sunFrame)
    {
        memcpy(dir, stable, sizeof(stable));
        return true;
    }
    frame = g_sunFrame;
    static float  raw[3] = { 0.0f, 0.0f, 0.0f };
    static int    stepLogs = 0;
    const double  now = Now();
    // Log the measurement's own jumps: a time set by hand, or the light going from the sun to a moon.
    const float rawDot = raw[0] * meas[0] + raw[1] * meas[1] + raw[2] * meas[2];
    if (have && rawDot < 0.9999939f && stepLogs < 60)   // > 0.2 degrees in one frame
    {
        ++stepLogs;
        Log("sun: the table's light moved %.2f degrees in one frame, at %.1f s", acosf(rawDot > 1.0f ? 1.0f : rawDot) * 57.29578f,
            now);
    }
    memcpy(raw, meas, sizeof(raw));
    static float  from[3] = { 0.0f, 0.0f, 1.0f }, to[3] = { 0.0f, 0.0f, 1.0f };
    static double moveAt = 0.0, moveFor = 0.0, stepAt = 0.0;
    const double glide = g_cfg.sun.glide;
    const float dot = stable[0] * meas[0] + stable[1] * meas[1] + stable[2] * meas[2];
    if (!have || dot < 0.99966f || glide <= 0.0)   // more than 1.5 degrees, or no glide: at once
    {
        memcpy(stable, meas, sizeof(stable));
        memcpy(from, meas, sizeof(from));
        memcpy(to, meas, sizeof(to));
        moveFor = 0.0;
        stepAt = now;
        have = true;
    }
    else
    {
        if (to[0] * meas[0] + to[1] * meas[1] + to[2] * meas[2] < 0.9999985f)   // the sky stepped: > 0.1 degrees
        {
            memcpy(from, stable, sizeof(from));
            memcpy(to, meas, sizeof(to));
            const double since = now - stepAt;
            moveAt  = now;
            moveFor = since > glide ? (since < 120.0 ? since : 120.0) : glide;
            stepAt  = now;
        }
        const float t = moveFor > 0.0 ? static_cast<float>((now - moveAt) / moveFor) : 1.0f;
        const float k = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        float len = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            stable[i] = from[i] + (to[i] - from[i]) * k;
            len += stable[i] * stable[i];
        }
        len = sqrtf(len);
        if (len > 1e-6f)
            for (int i = 0; i < 3; ++i)
                stable[i] /= len;
    }
    last = now;
    memcpy(dir, stable, sizeof(stable));
    memcpy(g_statMeas, meas, sizeof(g_statMeas));
    memcpy(g_statGlide, stable, sizeof(g_statGlide));
    g_statSrc = 0;
    return true;
}

// How dark it is by the game clock: 0 by day, 1 at night, a straight ramp over [night] fade hours from
// dusk and from dawn.
float NightWeight(float hour)
{
    const NightSettings& n = g_cfg.night;
    const auto since = [](float from, float to) { const float t = fmodf(to - from, 24.0f); return t < 0.0f ? t + 24.0f : t; };
    const float span = since(n.dusk, n.dawn);                    // hours from dusk to dawn
    float fade = n.fade;
    if (fade > span)         fade = span;
    if (fade > 24.0f - span) fade = 24.0f - span;
    const float t = since(n.dusk, hour);
    if (t < span)
        return fade > 0.0f && t < fade ? t / fade : 1.0f;
    const float u = t - span;                                    // hours since dawn
    return fade > 0.0f && u < fade ? 1.0f - u / fade : 0.0f;
}

float NightScale()
{
    return NightScale(g_cfg.night.strength);
}

float NightScale(float percent)
{
    const float night = percent * 0.01f;
    float hour = 0.0f;
    if (night >= 1.0f || !ClientHour(hour))
        return 1.0f;
    return 1.0f + (night - 1.0f) * NightWeight(hour);
}

// The shadows' sun: the sun, or with [sunshadows] lock the sun's azimuth at lockTilt degrees from straight
// down. With the sun overhead, whose azimuth says nothing, the client's 45 degrees.
// Below [sunshadows] riseFrom degrees the shadows' light climbs back toward [sunshadows] highest as the real
// light sinks (2026-09-30), on a smoothstep: at the horizon and under it, the shadows are short, as at noon.
// Over 12 degrees of the real light's height from riseFrom down (2026-10-14, the owner). The climb ended at the
// horizon, so with riseFrom 1 it went from the real sun to its end within one degree, a few game minutes. The
// span is now the same whatever riseFrom is: from 12 (the default until 2026-10-14) it still ends at the horizon,
// from 1 at -11, where the sun is still the light followed (the moons take over about 21:45).
// The shadows' light never stands higher than highest (Highest Sun, 2026-10-15, the owner): the sun reaches 85
// degrees at noon, and a character's shadow was a dot under its feet. The climb ends there too. It was its own
// setting (riseTo, Low Sun Height, 89) for a day.
constexpr float kRiseSpan = 12.0f;
static bool ShadowTarget(float dir[3])
{
    if (!SunDirection(dir))
        return false;
    const SunShadowSettings& ss = g_cfg.sunShadows;
    const float h = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
    const float ax = h > 1e-4f ? dir[0] / h : 0.70710678f, ay = h > 1e-4f ? dir[1] / h : 0.70710678f;
    if (!ss.lock)
    {
        const float el = asinf(dir[2] < -1.0f ? -1.0f : dir[2] > 1.0f ? 1.0f : dir[2]) * 57.29578f;
        float up = el;
        if (ss.riseFrom > 0.0f && el < ss.riseFrom)
        {
            float t = (ss.riseFrom - el) / kRiseSpan;
            t = t > 1.0f ? 1.0f : t;
            t = t * t * (3.0f - 2.0f * t);
            const float lifted = el > 0.0f ? el : 0.0f;
            up = lifted + (ss.highest - lifted) * t;
        }
        up = (std::min)(up, ss.highest);
        if (up == el)
            return true;
        up *= 0.01745329f;
        dir[0] = cosf(up) * ax;
        dir[1] = cosf(up) * ay;
        dir[2] = sinf(up);
        return true;
    }
    const float tilt = ss.lockTilt * 0.01745329f;
    dir[0] = sinf(tilt) * ax;
    dir[1] = sinf(tilt) * ay;
    dir[2] = cosf(tilt);
    return true;
}

// Turned toward ShadowTarget at most kTurnRate degrees a second, once a frame (2026-09-30). A moon can
// first show well above the horizon, where the shadows' light stood at highest, and the shadows turned at
// once: the sun's own glide in SunDirection lets a change of more than 5 degrees through at once, for the
// time set by hand. Here such a change sweeps over. A slow change passes as it is, so [sun] glide alone
// sets how far the shadows lag the sun. This was an ease of 1.5 seconds first, and with Sun Smoothing at 2
// the shadows still took about 6 seconds to settle.
bool ShadowSunDirection(float dir[3])
{
    static float    cur[3] = { 0.0f, 0.0f, 1.0f };
    static bool     have = false;
    static unsigned frame = ~0u;
    static double   last = 0.0;
    static float    held[3] = { 0.0f, 0.0f, 1.0f };   // what is handed out: see below
    if (have && frame == g_sunFrame)
    {
        memcpy(dir, held, sizeof(held));
        return true;
    }
    float target[3];
    if (!ShadowTarget(target))
        return false;
    frame = g_sunFrame;
    const double now = Now();
    if (!have)
        memcpy(cur, target, sizeof(cur));
    else
    {
        constexpr float kTurnRate = 30.0f;   // degrees a second
        const double dt   = now - last;
        const float  step = kTurnRate * 0.01745329f * static_cast<float>(dt > 0.0 && dt < 1.0 ? dt : 0.0);
        float d = cur[0] * target[0] + cur[1] * target[1] + cur[2] * target[2];
        d = d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d;
        const float angle = acosf(d);
        if (angle <= step || angle < 1e-4f || sinf(angle) < 1e-4f)
            memcpy(cur, target, sizeof(cur));
        else
        {
            // Along the great circle from cur to target, by step.
            const float a = sinf(angle - step) / sinf(angle), b = sinf(step) / sinf(angle);
            for (int i = 0; i < 3; ++i)
                cur[i] = cur[i] * a + target[i] * b;
        }
    }
    have = true;
    last = now;
    // Held, and moved on in steps of [shadow] sunStep degrees (2026-10-02). The maps are turned to face the
    // sun, and the sun moves a little every frame as the game's clock runs. With the maps held on whole
    // texels of a grid fixed in the world ([shadow] snap, nearSnap), a turn of a millionth of a radian about
    // the world's origin, thousands of yards off, moved the grid under the player by a sixth of a texel a
    // frame: the jagged shadow edges crawled while standing still. Held, the grids stand still between
    // steps, and a step moves the tip of a 10-yard shadow a few hundredths of a yard.
    static bool  haveHeld = false;
    const float  hold = g_cfg.shadow.sunStep * 0.01745329f;
    float hd = held[0] * cur[0] + held[1] * cur[1] + held[2] * cur[2];
    hd = hd > 1.0f ? 1.0f : hd < -1.0f ? -1.0f : hd;
    if (!haveHeld || hold <= 0.0f || acosf(hd) >= hold)
    {
        if (haveHeld && memcmp(held, cur, sizeof(held)) != 0)
        {
            ++g_heldSteps;
            g_heldStepAt = Now();
        }
        memcpy(held, cur, sizeof(held));
        haveHeld = true;
    }
    memcpy(dir, held, sizeof(held));
    memcpy(g_statHeld, held, sizeof(g_statHeld));
    g_statHaveHeld = true;
    return true;
}

// The sun for the on-screen stats (/atmos stats), as name=value; pairs: azimuth and elevation in degrees of the
// measurement (the clock's table, or [sun] fixed), of the glided sun that the light, the rays,
// the water and the fog read (SunDirection), and of the held sun the shadow maps are drawn with
// (ShadowSunDirection); the game time; and how often the held sun stepped in the last second.
void SunStatsText(std::string& out)
{
    const auto azel = [](const float d[3], float& az, float& el) {
        az = atan2f(d[1], d[0]) * 57.29578f;
        el = asinf(d[2] < -1.0f ? -1.0f : (d[2] > 1.0f ? 1.0f : d[2])) * 57.29578f;
    };
    float maz, mel, gaz, gel, haz, hel;
    azel(g_statMeas, maz, mel);
    azel(g_statGlide, gaz, gel);
    azel(g_statHeld, haz, hel);
    float hour = -1.0f;
    const bool clock = ClientHour(hour);
    const int gm = clock ? static_cast<int>(hour * 60.0f) : -1;
    char line[400];
    snprintf(line, sizeof(line),
             "sunsrc=%s;sunmaz=%.3f;sunmel=%.3f;sungaz=%.3f;sungel=%.3f;sunheld=%d;sunhaz=%.3f;sunhel=%.3f;"
             "sunsteps=%u;sunstepago=%.1f;sunstep=%.3f;gametime=%s%02d:%02d;",
             g_statSrc == 2 ? "fixed" : g_statSrc == 0 ? "table" : "none", maz, mel, gaz, gel,
             g_statHaveHeld ? 1 : 0, haz, hel, g_heldSteps, g_heldStepAt > 0.0 ? Now() - g_heldStepAt : -1.0,
             g_cfg.shadow.sunStep, clock ? "" : "?", gm >= 0 ? gm / 60 : 0, gm >= 0 ? gm % 60 : 0);
    out += line;
    g_heldSteps = 0;
    // Where the camera looks (2026-10-04), for wow-test-tool's face command: the heading in degrees
    // counter-clockwise from +x, as the sun's azimuth, and the pitch, up positive. The view's third column is
    // the camera's forward direction in the world.
    if (g_haveView)
    {
        const float f[3] = { g_view.m[0][2], g_view.m[1][2], g_view.m[2][2] };
        float yaw, pitch;
        azel(f, yaw, pitch);
        snprintf(line, sizeof(line), "camyaw=%.2f;campitch=%.2f;", yaw, pitch);
        out += line;
    }
    // The camera's distance from the player (2026-10-04), for wow-test-tool's camdist: a test sets the camera
    // as far back as the owner had it.
    float cam[3], pl[3];
    if (ClientCamera(cam) && ClientPlayer(pl))
    {
        const float dx = cam[0] - pl[0], dy = cam[1] - pl[1], dz = cam[2] - pl[2];
        snprintf(line, sizeof(line), "camdist=%.2f;", sqrtf(dx * dx + dy * dy + dz * dz));
        out += line;
    }
}

bool SunSecondDirection(float dir[3])
{
    if (g_cfg.sun.fixed || !TableLight() || !g_haveSecond)
        return false;
    memcpy(dir, g_secondDir, sizeof(g_secondDir));
    return true;
}

void SunFrameStart()
{
    ++g_sunFrame;
    g_nQuads = 0;
    g_quadsFresh = false;
}

int SunSkyQuads(float dirs[][3], int max)
{
    if (g_lastQuadFrame != g_sunFrame)
        return 0;
    int n = 0;
    for (; n < g_nLast && n < max; ++n)
        memcpy(dirs[n], g_lastQuads[n], sizeof(dirs[n]));
    return n;
}

// The probe (2026-10-14): the table's sun and moons at this time, the body the light follows, and the quads the last
// sky drew, each with how far it is from the table's body of its texture. A client whose sky differs from the
// table shows here.
void SunProbe()
{
    float hour = 0.0f;
    if (!ClientHour(hour))
    {
        Log("sun: no game clock at [client] clockAddr, so no sun from the table%s", g_cfg.sun.fixed ? " ([sun] fixed is on)" : "");
        return;
    }
    char line[600];
    int at = snprintf(line, sizeof(line), "sun: the table at %02d:%02d:", static_cast<int>(hour), static_cast<int>(hour * 60.0f) % 60);
    float body[kBodies][3];
    for (int b = 0; b < kBodies && at > 0 && at < static_cast<int>(sizeof(line)); ++b)
    {
        const bool drawn = SunPathBody(hour, static_cast<SkyBody>(b), body[b]);
        at += snprintf(line + at, sizeof(line) - at, "%s %s %.1f/%.1f%s", b ? "," : "", BodyName(static_cast<SkyBody>(b)),
                       Azimuth(body[b]), Elevation(body[b]), drawn ? "" : " (not drawn)");
    }
    if (at > 0 && at < static_cast<int>(sizeof(line)))
        at += snprintf(line + at, sizeof(line) - at, "; the light follows %s%s", g_haveTable ? BodyName(g_tableBody) : "nothing yet",
                       g_cfg.sun.fixed ? " ([sun] fixed is on: the fixed sun)" : "");
    Log("%s", line);
    if (!g_nLast)
    {
        Log("sun: the sky has drawn no sun-shaped quad yet");
        return;
    }
    at = snprintf(line, sizeof(line), "sun: the last sky with sun-shaped quads (%u frames ago) drew %d:", g_sunFrame - g_lastQuadFrame,
                  g_nLast);
    for (int q = 0; q < g_nLast && at > 0 && at < static_cast<int>(sizeof(line)); ++q)
    {
        const SkyBody b = g_lastTex[q] == 128 ? kBodySun : g_lastTex[q] == 512 ? kBodyMoon1 : g_lastTex[q] == 256 ? kBodyMoon2 : kBodies;
        at += snprintf(line + at, sizeof(line) - at, "%s %.1f/%.1f, texture %u", q ? ";" : "", Azimuth(g_lastQuads[q]),
                       Elevation(g_lastQuads[q]), g_lastTex[q]);
        if (b != kBodies && at > 0 && at < static_cast<int>(sizeof(line)))
        {
            const float* d = body[b];
            float dot = d[0] * g_lastQuads[q][0] + d[1] * g_lastQuads[q][1] + d[2] * g_lastQuads[q][2];
            dot = dot > 1.0f ? 1.0f : dot < -1.0f ? -1.0f : dot;
            at += snprintf(line + at, sizeof(line) - at, " (%s, %.1f degrees from the table)", BodyName(b), acosf(dot) * 57.29578f);
        }
    }
    Log("%s", line);
}

bool SunCamera(D3DMATRIX& view, D3DMATRIX& proj)
{
    if (!g_haveView || !g_haveProj)
        return false;
    view = g_view;
    proj = g_proj;
    return true;
}

void SunSetView(const float v[3], unsigned texWidth)
{
    const float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 1e-4f))
        return;
    g_quadsFresh = true;
    if (g_nQuads < kMaxQuads)
    {
        for (int i = 0; i < 3; ++i)
            g_quads[g_nQuads][i] = v[i] / len;
        g_quadTex[g_nQuads] = texWidth;
        ++g_nQuads;
    }
}

void SunSetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* m)
{
    if (!m)
        return;

    // Only the world camera matters. The UI sets its own transforms after the world, so an identity view
    // or an orthographic projection is ignored rather than allowed to replace the camera. So is any view
    // with a translation: the world camera never has one in this client, and the sky's sun sprite is
    // drawn under a special view whose translation IS the sun's position. Taking that for the camera
    // would turn every later sun direction wrong.
    if (state == D3DTS_VIEW)
    {
        bool identity = true;
        for (int r = 0; r < 4 && identity; ++r)
            for (int c = 0; c < 4; ++c)
                if (fabsf(m->m[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-5f) { identity = false; break; }
        const bool translated = fabsf(m->m[3][0]) > 1e-3f || fabsf(m->m[3][1]) > 1e-3f || fabsf(m->m[3][2]) > 1e-3f;
        if (!identity && !translated)
        {
            g_view     = *m;
            g_haveView = true;
        }
    }
    else if (state == D3DTS_PROJECTION)
    {
        const bool perspective = fabsf(m->m[2][3] - 1.0f) < 1e-3f && fabsf(m->m[3][3]) < 1e-3f;
        if (perspective)
        {
            g_proj     = *m;
            g_haveProj = true;
        }
    }
}

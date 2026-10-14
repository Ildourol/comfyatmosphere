// sunpath: where the sky draws the sun and the two moons at a time of day (2026-10-14).
//
// Until 2026-10-14 the effects read the sun from the sprite the sky draws (sun.cpp). The sky draws a sprite only
// where the camera can see it. With the sun out of view and a moon in view, the moon was taken for the sun, and the
// shadows turned as the camera turned (a WOW-HC player's probe: the light 135 degrees from the sun at 11:45).
//
// The sky places them by the game clock alone, so they are read from this table instead. tests/sun-table.json
// measured it in the test client: [sun] measure logged every sprite the sky drew, the camera pointed in 18
// directions over the whole sky, and the hour stepped by half hours. tests/Sun-Table.ps1 wrote the rows.
//
// What the table shows:
// - The sun stands at azimuth 45 all day. It rises 14.80 degrees an hour from the horizon at 06:10 to 85 at 11:55,
//   holds 85 until 12:05, and sinks 10.09 degrees an hour to the horizon at 20:30.
// - The larger moon also stands at azimuth 45. It rises 33.9 degrees an hour from 22:15 to 55 at 23:55, holds 55
//   until 00:05, and sinks 16.6 degrees an hour to the horizon at 03:25. The smaller moon has the same height, at
//   azimuth 135 at 00:00, 15 degrees an hour less before and 3.75 an hour more after.
// - The sky draws a body down to about 2.5 degrees under the horizon. From 03:30 to 06:00 and from 20:30 to 22:30
//   it draws none.
// The rows at 11:55, 12:05, 23:55 and 00:05 are not measured: they are where the measured lines meet the 85 and 55
// caps, so that the line between the rows is exact there too. The WOW-HC client (a plain 1.12.1 WoW.exe) agrees:
// its probe had the sun at 82.7 at 11:45, the table gives 82.6.
//
// Between two rows, the azimuth and the height each follow a straight line in time, as the sky's do. A row where
// the sky drew a body not at all takes the line of its nearest two rows that it did draw, carried on.

#include "sunpath.h"

#include <cmath>

namespace
{
    struct Place { float az, el; bool drawn; };
    struct Row   { float hour; Place body[kBodies]; };

    // The hour (the game clock's, from its day fraction), then the sun, the larger moon and the smaller: azimuth and
    // height in degrees, and whether the sky drew it.
    constexpr Row kRows[] = {
        {  0.0065f, { {    0.00f,   0.00f, false }, {   45.00f,  55.00f, true  }, {  135.02f,  55.00f, true  } } },
        {  0.0835f, { {    0.00f,   0.00f, false }, {   45.00f,  55.00f, true  }, {  135.31f,  55.00f, true  } } },   // the cap's end
        {  0.5095f, { {    0.00f,   0.00f, false }, {   45.00f,  47.93f, true  }, {  136.91f,  47.93f, true  } } },
        {  1.0090f, { {    0.00f,   0.00f, false }, {   45.00f,  39.64f, true  }, {  138.78f,  39.64f, true  } } },
        {  1.5083f, { {    0.00f,   0.00f, false }, {   45.00f,  31.35f, true  }, {  140.66f,  31.36f, true  } } },
        {  2.0086f, { {    0.00f,   0.00f, false }, {   45.00f,  23.05f, true  }, {  142.53f,  23.05f, true  } } },
        {  2.5083f, { {    0.00f,   0.00f, false }, {   45.00f,  14.76f, true  }, {  144.41f,  14.76f, true  } } },
        {  3.0080f, { {    0.00f,   0.00f, false }, {   45.00f,   6.46f, true  }, {  146.28f,   6.47f, true  } } },
        {  3.5071f, { {    0.00f,   0.00f, false }, {   45.00f,  -1.82f, true  }, {  148.15f,  -1.82f, true  } } },
        {  4.0000f, { {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  4.5000f, { {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  5.0000f, { {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  5.5000f, { {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  6.0071f, { {   45.00f,  -2.49f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  6.5073f, { {   45.00f,   4.91f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  7.0074f, { {   45.00f,  12.32f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  7.5075f, { {   45.00f,  19.72f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  8.0074f, { {   45.00f,  27.12f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  8.5079f, { {   45.00f,  34.53f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  9.0080f, { {   45.00f,  41.94f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        {  9.5082f, { {   45.00f,  49.34f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 10.0082f, { {   45.00f,  56.74f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 10.5084f, { {   45.00f,  64.15f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 11.0085f, { {   45.00f,  71.55f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 11.5088f, { {   45.00f,  78.96f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 11.9168f, { {   45.00f,  85.00f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },   // the cap's start
        { 12.0088f, { {   45.00f,  85.00f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 12.0837f, { {   45.00f,  85.00f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },   // the cap's end
        { 12.5089f, { {   45.00f,  80.71f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 13.0090f, { {   45.00f,  75.66f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 13.5092f, { {   45.00f,  70.61f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 14.0091f, { {   45.00f,  65.57f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 14.5088f, { {   45.00f,  60.53f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 15.0085f, { {   45.00f,  55.49f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 15.5085f, { {   45.00f,  50.44f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 16.0080f, { {   45.00f,  45.41f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 16.5072f, { {   45.00f,  40.37f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 17.0065f, { {   45.00f,  35.33f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 17.5074f, { {   45.00f,  30.28f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 18.0080f, { {   45.00f,  25.23f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 18.5078f, { {   45.00f,  20.19f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 19.0082f, { {   45.00f,  15.14f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 19.5080f, { {   45.00f,  10.10f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 20.0079f, { {   45.00f,   5.05f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 20.5077f, { {   45.00f,   0.01f, true  }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 21.0000f, { {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 21.5000f, { {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 22.0000f, { {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false }, {    0.00f,   0.00f, false } } },
        { 22.5087f, { {    0.00f,   0.00f, false }, {   45.00f,   7.25f, true  }, {  157.37f,   7.25f, true  } } },
        { 23.0089f, { {    0.00f,   0.00f, false }, {   45.00f,  24.21f, true  }, {  149.87f,  24.21f, true  } } },
        { 23.5092f, { {    0.00f,   0.00f, false }, {   45.00f,  41.18f, true  }, {  142.36f,  41.18f, true  } } },
        { 23.9167f, { {    0.00f,   0.00f, false }, {   45.00f,  55.00f, true  }, {  136.25f,  55.00f, true  } } },   // the cap's start
    };
    constexpr int kN = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));

    // The sky draws a body down to about this height. Measured: drawn at -2.49 (the sun at 06:00), not drawn at
    // about -5 (the sun's line at 21:00, the moons' at 04:00).
    constexpr float kDrawnDown = -3.5f;

    // Hours from a to b, forward round the clock: 0..24.
    float Ahead(float a, float b)
    {
        const float d = fmodf(b - a, 24.0f);
        return d < 0.0f ? d + 24.0f : d;
    }

    // An azimuth difference brought into -180..180.
    float Turn(float d)
    {
        while (d > 180.0f)  d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return d;
    }

    // The body's place at the hour on the straight line through rows i and j (both drawn), within or past them.
    void OnLine(int i, int j, SkyBody b, float hour, float& az, float& el)
    {
        const Place& p = kRows[i].body[b];
        const Place& q = kRows[j].body[b];
        const float span = Ahead(kRows[i].hour, kRows[j].hour);
        // The hour from row i, -12..12, so a time just before row i comes out negative.
        float t = Ahead(kRows[i].hour, hour);
        if (t > 12.0f)
            t -= 24.0f;
        const float k = span > 1e-4f ? t / span : 0.0f;
        az = p.az + Turn(q.az - p.az) * k;
        el = p.el + (q.el - p.el) * k;
        el = el < -90.0f ? -90.0f : (el > 90.0f ? 90.0f : el);   // carried on far under the horizon
    }

    // The two drawn rows in a row next to each other nearest the hour: their line carries a body on where the sky
    // does not draw it. False when the body has no two such rows.
    bool NearestLine(SkyBody b, float hour, int& i, int& j)
    {
        float best = 1e9f;
        for (int a = 0; a < kN; ++a)
        {
            const int c = (a + 1) % kN;
            if (!kRows[a].body[b].drawn || !kRows[c].body[b].drawn)
                continue;
            const float d = (std::fmin)(Ahead(hour, kRows[a].hour), Ahead(kRows[c].hour, hour));
            if (d < best)
            {
                best = d;
                i = a;
                j = c;
            }
        }
        return best < 1e8f;
    }

    void ToDir(float az, float el, float dir[3])
    {
        const float a = az * 0.01745329f, e = el * 0.01745329f;
        dir[0] = cosf(e) * cosf(a);
        dir[1] = cosf(e) * sinf(a);
        dir[2] = sinf(e);
    }
}

bool SunPathBody(float hour, SkyBody body, float dir[3])
{
    hour = fmodf(hour, 24.0f);
    if (hour < 0.0f)
        hour += 24.0f;
    // The rows round the hour: the last at or before it, and the next.
    int i = kN - 1;
    for (int a = 0; a < kN; ++a)
        if (kRows[a].hour <= hour)
            i = a;
    const int j = (i + 1) % kN;
    float az = 0.0f, el = 0.0f;
    int li = i, lj = j;
    if (!(kRows[i].body[body].drawn && kRows[j].body[body].drawn) && !NearestLine(body, hour, li, lj))
    {
        ToDir(45.0f, -90.0f, dir);
        return false;
    }
    OnLine(li, lj, body, hour, az, el);
    ToDir(az, el, dir);
    return el > kDrawnDown;
}

SkyBody SunPathLight(float hour, bool night, float dir[3], float second[3], bool& haveSecond)
{
    float d[kBodies][3];
    bool  drawn[kBodies];
    for (int b = 0; b < kBodies; ++b)
        drawn[b] = SunPathBody(hour, static_cast<SkyBody>(b), d[b]);

    int pick = -1;
    if (night)
    {
        // The larger moon while it is up, else the other.
        for (int b = kBodyMoon1; b <= kBodyMoon2 && pick < 0; ++b)
            if (d[b][2] > 0.0f)
                pick = b;
    }
    else if (drawn[kBodySun])
        pick = kBodySun;
    if (pick < 0)
    {
        // Nothing up that this time of day follows (from 21:00 to 22:15 and from 03:30 to 06:00 the sky draws
        // nothing): the highest body. Under the horizon the shadows take a steep light ([sunshadows] riseFrom),
        // and the sun and the larger moon share azimuth 45, so the shadows do not turn.
        pick = kBodySun;
        for (int b = 1; b < kBodies; ++b)
            if (d[b][2] > d[pick][2])
                pick = b;
    }
    for (int i = 0; i < 3; ++i)
        dir[i] = d[pick][i];

    haveSecond = false;
    if (night && pick != kBodySun)
    {
        const int other = pick == kBodyMoon1 ? kBodyMoon2 : kBodyMoon1;
        if (drawn[other])
        {
            for (int i = 0; i < 3; ++i)
                second[i] = d[other][i];
            haveSecond = true;
        }
    }
    return static_cast<SkyBody>(pick);
}

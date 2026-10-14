// Where the sky draws the sun and the two moons at a time of day, from a measured table. See sunpath.cpp.
#pragma once

enum SkyBody { kBodySun, kBodyMoon1, kBodyMoon2, kBodies };   // kBodyMoon1 is the larger moon

// The body's direction at the hour (0..24), unit length, Z up. Under the horizon, where the sky does not draw it,
// the line it moves on is carried on. Returns whether the sky draws it then.
bool SunPathBody(float hour, SkyBody body, float dir[3]);

// The light the effects follow at the hour: the sun by day, the larger moon at night while it is up, else the other
// moon, else the highest body. second: the other moon at night while the sky draws it (the rays and the water's
// glint). Returns the body followed.
SkyBody SunPathLight(float hour, bool night, float dir[3], float second[3], bool& haveSecond);

// lights: the game's light tables for the player's place and hour (lights.cpp).
#pragma once

void LightsFrame();                          // once a frame: the tables asked for until read, then the bands where
                                             // you stand, four times a second
// One of the 18 colour bands where you stand (2 to 7 the sky from the zenith to the fog at the horizon, 14 and 15 the
// ocean, 16 and 17 a river or a lake, shallow and deep), as last worked out; false until the tables are read.
bool LightsBand(int band, float rgb[3]);
void LightsProbe();                          // the probe's lines: the tables, the lights round you, every band

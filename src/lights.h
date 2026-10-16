// lights: the game's light tables for the player's place and hour (lights.cpp).
#pragma once

void LightsFrame();                          // once a frame: asks the loader for the tables until they are read
bool LightsBand(int band, float rgb[3]);     // one of the 18 colour bands where you stand, now; false until read
void LightsProbe();                          // the probe's lines: the tables, the lights round you, every band

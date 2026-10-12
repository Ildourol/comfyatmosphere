// The server's weather, read from its packet as it arrives (SMSG_WEATHER). See weather.cpp.
#pragma once

void WeatherFrameEnd();                         // at Present: the packet handler's patch goes in, once
void WeatherDetach();                           // the hot reload: the patch taken out (at Present)
bool WeatherServer(int& type, float& grade);    // the last weather the server sent; false before the first
void WeatherProbe();                            // the probe (F12): the last weather and the patch

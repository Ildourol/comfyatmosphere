// A debug view: dots where the effects take the sun, the shadows' sun, the other moon and the sky's sprites. See
// sundot.cpp.
#pragma once

#include <d3d9.h>

void SunDotDraw(IDirect3DDevice9* dev);   // after the passes, before the UI; does nothing unless [sun] debug is 1
void SunDotReset();                       // before Reset: drop the state block

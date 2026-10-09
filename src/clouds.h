// Our clouds: a soft layer in the sky, and now and then a cloud with a shape. See clouds.cpp.
#pragma once

#include <d3d9.h>

bool CloudsDraw(IDirect3DDevice9* dev);   // in the sky, where the game's cloud layer is drawn; true if drawn
void CloudsReset();                       // before Reset: drop every D3DPOOL_DEFAULT object
void CloudsProbe();                       // log the next draw

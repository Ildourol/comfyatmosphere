// Post-process Anti-Aliasing (FXAA 3.11 & 3-pass SMAA 1x) + RCAS sharpening at the world -> UI boundary.
// See aa.cpp.
#pragma once

#include <d3d9.h>

bool AaBeforeUI(IDirect3DDevice9* dev);   // after grade, before UI; true if drawn
void AaReset();                           // before Reset: release every D3DPOOL_DEFAULT object
void AaShaderList();                      // background shader precompile list (shadercache.cpp)

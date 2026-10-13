// The game's Full Screen Glow, turned up or down (gameglow.cpp).
#pragma once

#include <d3d9.h>

// The shader to bind in place of ps: a copy of the client's glow composite that scales both of its glow amounts by
// `scale` (1 = the game's own), or ps itself when ps is not that shader or scale is 1.
IDirect3DPixelShader9* GameGlowSwap(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, float scale);
void GameGlowReset();   // before Reset, and for a new device

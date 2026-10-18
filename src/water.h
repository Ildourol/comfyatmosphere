// The water's own look: foam where it is shallow. See water.cpp.
#pragma once

#include <d3d9.h>

using WaterDrawFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

// One water chunk as the client draws it, with the transforms it set for it.
struct WaterChunk
{
    D3DPRIMITIVETYPE prim;
    INT              baseVertex;
    UINT             minIndex, numVertices, startIndex, primCount;
    const D3DMATRIX* world;   // camera-relative
    const D3DMATRIX* view;    // rotation only
    const D3DMATRIX* proj;
    bool             city = false;   // water in a building (a WMO's liquid): no depth in its vertices, no swell
};

bool WaterWanted();   // the foam draws this frame
// Before the client's draw of a water chunk: the first call of a frame copies the depth under the water.
void WaterBeforeDraw(IDirect3DDevice9* dev, const WaterChunk& c);
// Whether the client's own draw of the chunk is left out this frame: our pass covers it entirely (see water.cpp).
bool WaterHidesGame();
// Whether the liquid texture bound at stage 0 is water's: blue or grey, not lava's red or slime's green.
// Read once a texture, from its smallest level.
bool WaterTextureIsWater(IDirect3DDevice9* dev);
// A pixel shader of one flat colour, for the debug view of the game's own liquid: red for water, magenta else.
IDirect3DPixelShader9* WaterFlatShader(IDirect3DDevice9* dev, bool water);
// After it: the chunk is held for the foam over it, drawn with the rest of the run by WaterFlush (2026-10-11).
// `draw` is the device's DrawIndexedPrimitive below our hook.
void WaterAfterDraw(IDirect3DDevice9* dev, const WaterChunk& c, WaterDrawFn draw);
bool WaterPending();                     // chunks are held
// A held chunk reads this vertex or index buffer (2026-10-11): the client is about to write it, and the held chunks are
// drawn first. Turning the camera, the client writes the chunks coming into view into its shared water buffers in the
// middle of the run, and held chunks drawn after such a write flickered, with frames of up to 330 ms.
bool WaterPendingUses(const void* buffer);
void WaterFlush(IDirect3DDevice9* dev);  // draw the held chunks: at the client's next draw that is not water
// The probe: each water draw of the frame (water), and each other blended fixed-function draw through a
// pixel shader (not water), before the client's draw. `index` is the draw's place in the frame.
void WaterProbeDraw(IDirect3DDevice9* dev, const WaterChunk& c, unsigned index, bool water);
// A fixed-function draw about to be made: the game's own wake, ring or splash on the water (see water.cpp)?
bool WaterGameWake(IDirect3DDevice9* dev);
// The game's rain splashes (comfyatmos.cpp knows the draw by its texture): Begin binds a pixel shader that
// leaves out the ones under the water; true: make the client's draw, then End puts its state back.
bool WaterSplashBegin(IDirect3DDevice9* dev, bool debug = false);   // debug: Debug View 33's red and green
void WaterSplashEnd(IDirect3DDevice9* dev);
// The rain's splashes, the rain's 256 x 256 draw through the game's vertex shader: Begin binds a pixel shader that
// drops those under the water; true: make the client's draw, then End puts its state back.
bool WaterRainSplashBegin(IDirect3DDevice9* dev);
void WaterRainSplashEnd(IDirect3DDevice9* dev);
// The depth under the water this frame (copied before the first water draw), for the sun shadows; or null.
IDirect3DTexture9* WaterUnderDepth();
// The world is drawn: the water's textures come off samplers s10 to s15, before the body mask is built again into
// one of them. The water's constants and textures are set once a world (see water.cpp, g_frameSet).
void WaterWorldEnded(IDirect3DDevice9* dev);
void WaterFrameEnd(IDirect3DDevice9* dev);   // at Present; also takes the textures off for a world that never ended
// A fixed-function draw of 100 triangles or more: its world matrix's origin noted, to find the ships (Ship Wake).
void WaterNoteHull(IDirect3DDevice9* dev, const D3DMATRIX& world);
// Within reach of a ship under way (its hull followed from its draws): a model there is one of its parts.
bool WaterShipNear(const float p[3]);
// On the origin of a ship under way, within 2 yards: a fixed-function draw placed there is a piece of that ship.
bool WaterShipPart(const float p[3]);
// A ship under way within this many yards of p across the ground.
bool WaterShipWithin(const float p[3], float reach);
// The light the water takes from what lies under it a yard, by brightness, at the current colour and Clarity (0
// before the water was first drawn): the sun shadows fade the bed's shade by it.
float WaterBedFade();
// The sky's glow just over the horizon (the light tables' bands 5 and 6, lights.cpp) where you stand, for the mist's
// light (volume.cpp). False: the tables are not read yet.
bool SkyGlowColour(float rgb[3]);
void WaterNoteRain(IDirect3DDevice9* dev, UINT prims);   // a DrawPrimitive: counted if it is the game's rain
float WaterRainAmount();                                  // how hard it rains, 0..1, eased
void WaterReset();      // before Reset, and for a new device
void WaterProbe();      // log the next frame's water draws
bool WaterProbing();    // a probe frame is being logged
// The probe: any draw (water or not) using one of the water's textures, logged with its states.
void WaterProbeTexture(IDirect3DDevice9* dev, const char* call, UINT nv, UINT pc, unsigned index, bool water);

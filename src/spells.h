// The game's additive draws held until our shade is on the world (spells.cpp, 2026-10-12).
#pragma once

#include <d3d9.h>

// The device's own draw calls, past every hook (comfyatmos.cpp, at attach).
struct SpellCalls
{
    HRESULT (STDMETHODCALLTYPE* drawPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
    HRESULT (STDMETHODCALLTYPE* drawIdxPrim)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    HRESULT (STDMETHODCALLTYPE* drawPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
    HRESULT (STDMETHODCALLTYPE* drawIdxPrimUP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*,
                                               D3DFORMAT, const void*, UINT);
};
void SpellsAttach(const SpellCalls& calls);
void SpellsNoteConstants(UINT end);   // the client set vertex shader constants up to this register

// Whether the draw now set up is one to hold: blended, adding to what is there (DESTBLEND ONE), no depth writes.
bool SpellsIsAdditive(IDirect3DDevice9* dev);

// Each holds the draw and returns true, or returns false (the caller draws it): full, or no state block.
bool SpellsHoldDraw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT sv, UINT pc);
bool SpellsHoldIdx(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT bvi, UINT mvi, UINT nv, UINT si, UINT pc);
bool SpellsHoldUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT pc, UINT nv, const void* data, UINT stride);
bool SpellsHoldIdxUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT mvi, UINT nv, UINT pc, const void* idx,
                     D3DFORMAT fmt, const void* data, UINT stride);

// Every held draw drawn now, in the order the client made them, and let go. why: for the counts.
enum SpellFlush { kSpellsAtWorldEnd, kSpellsBufferWrite, kSpellsTarget, kSpellsOther, kSpellsFlushKinds };
void SpellsFlush(IDirect3DDevice9* dev, SpellFlush why);
bool SpellsPending();
bool SpellsPendingUses(const void* buffer);   // a vertex or index buffer one of them draws from
void SpellsDrop();                            // let go without drawing: a Reset, the hot reload

void SpellsFrameText(char* out, size_t size); // last frame's counts, for the probe
void SpellsFrameEnd();                        // at Present

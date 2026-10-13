// spells: the game's additive draws held until our shade is on the world (2026-10-12, the owner: spells darkened in
// caves and in shade).
//
// The sun shadows and the fog judge each pixel by the depth, and a spell writes none: drawn over it, they gave the
// spell the shade and the fog of the wall behind it. Our passes cannot go in before the spells, as the client draws
// the particles in one list with every model that has a blended batch, whole models among them (NOTES.md, *Spells
// dark in caves and in shade*). But an additive draw adds its light to what is there, and a sum does not depend on
// its order: so each additive draw the world makes is held here, and drawn when the world ends, after the sun
// shadows and the fog's shade (comfyatmos.cpp, WorldEnded). The client's Full Screen Glow then blooms them as
// before, and the light in the fog goes on after.
//
// A held draw is its device state and its call. The state is a state block recorded once with every state a draw
// reads (Record), and captured again for each draw: a D3DSBT_ALL block, made and applied for each, cost 26 and 18
// microseconds a draw, 3.6 ms for Stormwind's 83 (2026-10-12). The blocks are kept and reused; they hold references to
// the client's buffers and textures, and a D3DPOOL_DEFAULT object still referenced fails a Reset, so a Reset lets
// them all go (SpellsDrop). A draw from the client's memory (UP) is copied. A draw from a vertex or index buffer reads the buffer when it is
// drawn, so before the client writes to one of those buffers every held draw is drawn (the lock hooks), except for a
// write with D3DLOCK_NOOVERWRITE, by which the client promises to leave alone what it has drawn from. A change of
// render target or depth buffer draws them first too. Drawn early, a spell is shaded again, as before.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "spells.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    SpellCalls g_calls = {};

    enum Kind { kDraw, kIdx, kUP, kIdxUP };

    struct Held
    {
        Kind                  kind = kDraw;
        IDirect3DStateBlock9* sb = nullptr;
        D3DPRIMITIVETYPE      prim = D3DPT_TRIANGLELIST;
        INT                   bvi = 0;
        UINT                  mvi = 0, nv = 0, si = 0, sv = 0, pc = 0, stride = 0;
        D3DFORMAT             fmt = D3DFMT_INDEX16;
        std::vector<BYTE>     verts, idx;     // a UP draw's own copy
        const void*           bufs[5] = {};   // streams 0 to 3 and the indices: compared only, never dereferenced
    };

    constexpr size_t kMaxHeld = 512;
    std::vector<Held> g_held;
    // Recorded blocks; the first g_held.size() are in use. Each keeps the vertex shader constants up to the highest
    // register the client had set when it was recorded (g_vsEnd), not all 256: applying all 256 for each draw was most
    // of the cost of drawing them back. A block recorded with fewer than the client now sets is recorded again.
    struct Block
    {
        IDirect3DStateBlock9* sb = nullptr;
        UINT                  vs = 0;
    };
    std::vector<Block> g_pool;
    Block g_save;                                // the device as it was, around a flush
    UINT  g_vsEnd = 0;
    constexpr DWORD kStages = 4;                 // texture stages kept; the terrain uses 5, and is never held

    // Every render state a draw can read, but the point size: DXVK takes some point sizes as driver requests (RESZ).
    const D3DRENDERSTATETYPE kStates[] = {
        D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_LASTPIXEL,
        D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_CULLMODE, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
        D3DRS_DITHERENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_FOGENABLE, D3DRS_SPECULARENABLE, D3DRS_FOGCOLOR,
        D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE, D3DRS_STENCILENABLE,
        D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_STENCILFUNC, D3DRS_STENCILREF,
        D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TEXTUREFACTOR, D3DRS_WRAP0, D3DRS_WRAP1, D3DRS_WRAP2,
        D3DRS_WRAP3, D3DRS_WRAP4, D3DRS_WRAP5, D3DRS_WRAP6, D3DRS_WRAP7, D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_AMBIENT,
        D3DRS_FOGVERTEXMODE, D3DRS_COLORVERTEX, D3DRS_LOCALVIEWER, D3DRS_NORMALIZENORMALS,
        D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE,
        D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_VERTEXBLEND, D3DRS_CLIPPLANEENABLE, D3DRS_POINTSIZE_MIN,
        D3DRS_POINTSPRITEENABLE, D3DRS_POINTSCALEENABLE, D3DRS_POINTSCALE_A, D3DRS_POINTSCALE_B, D3DRS_POINTSCALE_C,
        D3DRS_MULTISAMPLEANTIALIAS, D3DRS_MULTISAMPLEMASK, D3DRS_POINTSIZE_MAX, D3DRS_INDEXEDVERTEXBLENDENABLE,
        D3DRS_COLORWRITEENABLE, D3DRS_TWEENFACTOR, D3DRS_BLENDOP, D3DRS_SCISSORTESTENABLE, D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_ANTIALIASEDLINEENABLE, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL,
        D3DRS_CCW_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3, D3DRS_BLENDFACTOR, D3DRS_SRGBWRITEENABLE, D3DRS_DEPTHBIAS,
        D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA,
    };
    const D3DTEXTURESTAGESTATETYPE kStageStates[] = {
        D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
        D3DTSS_BUMPENVMAT00, D3DTSS_BUMPENVMAT01, D3DTSS_BUMPENVMAT10, D3DTSS_BUMPENVMAT11, D3DTSS_TEXCOORDINDEX,
        D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_COLORARG0,
        D3DTSS_ALPHAARG0, D3DTSS_RESULTARG, D3DTSS_CONSTANT,
    };
    const D3DSAMPLERSTATETYPE kSamplerStates[] = {
        D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_BORDERCOLOR, D3DSAMP_MAGFILTER,
        D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY,
        D3DSAMP_SRGBTEXTURE,
    };
    const D3DTRANSFORMSTATETYPE kTransforms[] = {
        D3DTS_WORLD, D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0, D3DTS_TEXTURE1, D3DTS_TEXTURE2, D3DTS_TEXTURE3,
    };

    // A state block of every state above, each set to the value it has now. While recording, the device takes the
    // Set calls into the block and leaves its own state alone. The caller has its passes' flag up, so the hooks
    // pass these calls straight on.
    IDirect3DStateBlock9* Record(IDirect3DDevice9* dev, UINT vsEnd)
    {
        auto* d = dev->lpVtbl;
        if (FAILED(d->BeginStateBlock(dev)))
            return nullptr;
        for (D3DRENDERSTATETYPE rs : kStates)
        {
            DWORD v = 0;
            d->GetRenderState(dev, rs, &v);
            d->SetRenderState(dev, rs, v);
        }
        for (DWORD st = 0; st < kStages; ++st)
        {
            for (D3DTEXTURESTAGESTATETYPE ts : kStageStates)
            {
                DWORD v = 0;
                d->GetTextureStageState(dev, st, ts, &v);
                d->SetTextureStageState(dev, st, ts, v);
            }
            for (D3DSAMPLERSTATETYPE ss : kSamplerStates)
            {
                DWORD v = 0;
                d->GetSamplerState(dev, st, ss, &v);
                d->SetSamplerState(dev, st, ss, v);
            }
            IDirect3DBaseTexture9* t = nullptr;
            d->GetTexture(dev, st, &t);
            d->SetTexture(dev, st, t);
            if (t) t->lpVtbl->Release(t);
        }
        IDirect3DVertexShader9* vs = nullptr;
        d->GetVertexShader(dev, &vs);
        d->SetVertexShader(dev, vs);
        if (vs) vs->lpVtbl->Release(vs);
        IDirect3DPixelShader9* ps = nullptr;
        d->GetPixelShader(dev, &ps);
        d->SetPixelShader(dev, ps);
        if (ps) ps->lpVtbl->Release(ps);
        IDirect3DVertexDeclaration9* decl = nullptr;
        d->GetVertexDeclaration(dev, &decl);
        if (decl)
        {
            d->SetVertexDeclaration(dev, decl);
            decl->lpVtbl->Release(decl);
        }
        DWORD fvf = 0;
        d->GetFVF(dev, &fvf);
        d->SetFVF(dev, fvf);
        for (UINT s = 0; s < 4; ++s)
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            UINT off = 0, stride = 0, freq = 1;
            d->GetStreamSource(dev, s, &vb, &off, &stride);
            d->SetStreamSource(dev, s, vb, off, stride);
            if (vb) vb->lpVtbl->Release(vb);
            d->GetStreamSourceFreq(dev, s, &freq);
            d->SetStreamSourceFreq(dev, s, freq);
        }
        IDirect3DIndexBuffer9* ib = nullptr;
        d->GetIndices(dev, &ib);
        d->SetIndices(dev, ib);
        if (ib) ib->lpVtbl->Release(ib);
        static float f[256 * 4];
        static int   n[16 * 4];
        static BOOL  b[16];
        if (vsEnd > 0)
        {
            d->GetVertexShaderConstantF(dev, 0, f, vsEnd);
            d->SetVertexShaderConstantF(dev, 0, f, vsEnd);
        }
        d->GetVertexShaderConstantI(dev, 0, n, 16);
        d->SetVertexShaderConstantI(dev, 0, n, 16);
        d->GetVertexShaderConstantB(dev, 0, b, 16);
        d->SetVertexShaderConstantB(dev, 0, b, 16);
        d->GetPixelShaderConstantF(dev, 0, f, 32);
        d->SetPixelShaderConstantF(dev, 0, f, 32);
        d->GetPixelShaderConstantI(dev, 0, n, 16);
        d->SetPixelShaderConstantI(dev, 0, n, 16);
        d->GetPixelShaderConstantB(dev, 0, b, 16);
        d->SetPixelShaderConstantB(dev, 0, b, 16);
        for (D3DTRANSFORMSTATETYPE tt : kTransforms)
        {
            D3DMATRIX m = {};
            d->GetTransform(dev, tt, &m);
            d->SetTransform(dev, tt, &m);
        }
        D3DVIEWPORT9 vp = {};
        d->GetViewport(dev, &vp);
        d->SetViewport(dev, &vp);
        RECT sc = {};
        d->GetScissorRect(dev, &sc);
        d->SetScissorRect(dev, &sc);
        D3DMATERIAL9 mat = {};
        d->GetMaterial(dev, &mat);
        d->SetMaterial(dev, &mat);
        IDirect3DStateBlock9* sb = nullptr;
        if (FAILED(d->EndStateBlock(dev, &sb)))
            return nullptr;
        return sb;
    }

    // This frame's and last frame's counts.
    struct Counts
    {
        unsigned held = 0, notHeld = 0;              // not held: the list full, or no state block
        unsigned drawn[kSpellsFlushKinds] = {};      // drawn by each cause
        unsigned flushes[kSpellsFlushKinds] = {};    // times each cause drew the list
        double   holdMs = 0.0, drawMs = 0.0;         // our CPU: holding (the state blocks), and drawing them
    };
    Counts g_now, g_last;

    bool Begin(IDirect3DDevice9* dev, Held& h)
    {
        if (g_held.size() >= kMaxHeld)
            return false;
        const size_t i = g_held.size();
        if (i < g_pool.size() && g_pool[i].vs >= g_vsEnd)
        {
            h.sb = g_pool[i].sb;
            h.sb->lpVtbl->Capture(h.sb);
        }
        else
        {
            IDirect3DStateBlock9* sb = Record(dev, g_vsEnd);   // recorded with the values now: no capture needed
            if (!sb)
                return false;
            if (i < g_pool.size())
            {
                g_pool[i].sb->lpVtbl->Release(g_pool[i].sb);
                g_pool[i] = { sb, g_vsEnd };
            }
            else
                g_pool.push_back({ sb, g_vsEnd });
            h.sb = sb;
        }
        for (UINT s = 0; s < 4; ++s)
        {
            IDirect3DVertexBuffer9* vb = nullptr;
            UINT off = 0, stride = 0;
            if (SUCCEEDED(dev->lpVtbl->GetStreamSource(dev, s, &vb, &off, &stride)) && vb)
            {
                h.bufs[s] = vb;
                vb->lpVtbl->Release(vb);
            }
        }
        IDirect3DIndexBuffer9* ib = nullptr;
        if (SUCCEEDED(dev->lpVtbl->GetIndices(dev, &ib)) && ib)
        {
            h.bufs[4] = ib;
            ib->lpVtbl->Release(ib);
        }
        return true;
    }

    bool Keep(Held&& h, double t0)
    {
        g_held.push_back(std::move(h));
        ++g_now.held;
        g_now.holdMs += 1000.0 * (Now() - t0);
        return true;
    }

    bool Refuse(Held&)
    {
        ++g_now.notHeld;
        return false;
    }

    UINT IndexCount(D3DPRIMITIVETYPE prim, UINT pc)
    {
        switch (prim)
        {
        case D3DPT_POINTLIST:     return pc;
        case D3DPT_LINELIST:      return pc * 2;
        case D3DPT_LINESTRIP:     return pc + 1;
        case D3DPT_TRIANGLELIST:  return pc * 3;
        case D3DPT_TRIANGLESTRIP:
        case D3DPT_TRIANGLEFAN:   return pc + 2;
        default:                  return 0;
        }
    }
}

void SpellsAttach(const SpellCalls& calls)
{
    g_calls = calls;
}

bool SpellsIsAdditive(IDirect3DDevice9* dev)
{
    DWORD blend = 0, zwrite = 0, dst = 0, op = D3DBLENDOP_ADD;
    dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend)
        return false;
    dev->lpVtbl->GetRenderState(dev, D3DRS_ZWRITEENABLE, &zwrite);
    dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
    dev->lpVtbl->GetRenderState(dev, D3DRS_BLENDOP, &op);
    return !zwrite && dst == D3DBLEND_ONE && op == D3DBLENDOP_ADD;
}

bool SpellsHoldDraw(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT sv, UINT pc)
{
    const double t0 = Now();
    Held h;
    if (!Begin(dev, h))
        return Refuse(h);
    h.kind = kDraw; h.prim = prim; h.sv = sv; h.pc = pc;
    return Keep(std::move(h), t0);
}

bool SpellsHoldIdx(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, INT bvi, UINT mvi, UINT nv, UINT si, UINT pc)
{
    const double t0 = Now();
    Held h;
    if (!Begin(dev, h))
        return Refuse(h);
    h.kind = kIdx; h.prim = prim; h.bvi = bvi; h.mvi = mvi; h.nv = nv; h.si = si; h.pc = pc;
    return Keep(std::move(h), t0);
}

bool SpellsHoldUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT pc, UINT nv, const void* data, UINT stride)
{
    if (!data || !stride || !nv)
        return false;
    const double t0 = Now();
    Held h;
    if (!Begin(dev, h))
        return Refuse(h);
    h.kind = kUP; h.prim = prim; h.pc = pc; h.stride = stride;
    h.verts.assign(static_cast<const BYTE*>(data), static_cast<const BYTE*>(data) + static_cast<size_t>(nv) * stride);
    return Keep(std::move(h), t0);
}

bool SpellsHoldIdxUP(IDirect3DDevice9* dev, D3DPRIMITIVETYPE prim, UINT mvi, UINT nv, UINT pc, const void* idx,
                     D3DFORMAT fmt, const void* data, UINT stride)
{
    const UINT ni = IndexCount(prim, pc);
    if (!data || !idx || !stride || !ni)
        return false;
    const double t0 = Now();
    Held h;
    if (!Begin(dev, h))
        return Refuse(h);
    h.kind = kIdxUP; h.prim = prim; h.mvi = mvi; h.nv = nv; h.pc = pc; h.fmt = fmt; h.stride = stride;
    h.verts.assign(static_cast<const BYTE*>(data),
                   static_cast<const BYTE*>(data) + static_cast<size_t>(mvi + nv) * stride);
    const size_t isz = static_cast<size_t>(ni) * (fmt == D3DFMT_INDEX32 ? 4 : 2);
    h.idx.assign(static_cast<const BYTE*>(idx), static_cast<const BYTE*>(idx) + isz);
    return Keep(std::move(h), t0);
}

void SpellsFlush(IDirect3DDevice9* dev, SpellFlush why)
{
    if (g_held.empty())
        return;
    const double t0 = Now();
    // The device as it is now, put back after: each held draw sets its own state, but not the render target or the
    // depth buffer, which are the world's whenever they are drawn (a change of either draws them first).
    if (g_save.sb && g_save.vs >= g_vsEnd)
        g_save.sb->lpVtbl->Capture(g_save.sb);
    else
    {
        if (g_save.sb)
            g_save.sb->lpVtbl->Release(g_save.sb);
        g_save = { Record(dev, g_vsEnd), g_vsEnd };
    }
    for (Held& h : g_held)
    {
        h.sb->lpVtbl->Apply(h.sb);
        switch (h.kind)
        {
        case kDraw:  g_calls.drawPrim(dev, h.prim, h.sv, h.pc); break;
        case kIdx:   g_calls.drawIdxPrim(dev, h.prim, h.bvi, h.mvi, h.nv, h.si, h.pc); break;
        case kUP:    g_calls.drawPrimUP(dev, h.prim, h.pc, h.verts.data(), h.stride); break;
        case kIdxUP: g_calls.drawIdxPrimUP(dev, h.prim, h.mvi, h.nv, h.pc, h.idx.data(), h.fmt, h.verts.data(),
                                           h.stride); break;
        }
    }
    g_now.drawn[why] += static_cast<unsigned>(g_held.size());
    ++g_now.flushes[why];
    g_held.clear();
    if (g_save.sb)
        g_save.sb->lpVtbl->Apply(g_save.sb);
    g_now.drawMs += 1000.0 * (Now() - t0);
}

bool SpellsPending()
{
    return !g_held.empty();
}

bool SpellsPendingUses(const void* buffer)
{
    for (const Held& h : g_held)
        for (const void* b : h.bufs)
            if (b == buffer)
                return true;
    return false;
}

void SpellsDrop()
{
    g_held.clear();
    for (Block& b : g_pool)
        b.sb->lpVtbl->Release(b.sb);
    g_pool.clear();
    if (g_save.sb)
        g_save.sb->lpVtbl->Release(g_save.sb);
    g_save = Block();
}

void SpellsNoteConstants(UINT end)
{
    if (end > g_vsEnd)
        g_vsEnd = end > 256 ? 256 : end;
}

void SpellsFrameText(char* out, size_t size)
{
    const Counts& c = g_last;
    snprintf(out, size, "spells: last frame %u additive draws held (%u not: the list full or no state block); drawn "
             "%u at the world's end, %u before a write to their buffer (%u times), %u before a change of target (%u "
             "times), %u otherwise (%u times); our CPU %.3f ms to hold, %.3f ms to draw; %u vertex shader constants kept",
             c.held, c.notHeld, c.drawn[kSpellsAtWorldEnd], c.drawn[kSpellsBufferWrite], c.flushes[kSpellsBufferWrite],
             c.drawn[kSpellsTarget], c.flushes[kSpellsTarget], c.drawn[kSpellsOther], c.flushes[kSpellsOther],
             c.holdMs, c.drawMs, g_vsEnd);
}

void SpellsFrameEnd()
{
    g_last = g_now;
    g_now = Counts();
}

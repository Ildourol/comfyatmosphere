// gameglow: the game's Full Screen Glow, turned up or down (2026-10-13, the owner: "the full screen glow is quite
// bright").
//
// With Full Screen Glow on, the client shrinks the world's image to a quarter, blurs it twice, and draws world and
// blur onto the screen in one full-screen pass. The probe of 2026-10-13 shows the three pixel shaders. The last one:
//
//     texld r1, t0, s0              the world
//     texld r0, t1, s1              the blur
//     lrp   r2.xyz, v0.z, r0, r1    the world mixed toward the blur by v0.z: the haze
//     mul   r0.xyz, r0, r0
//     mad   r0.xyz, r0, v0.w, r2    plus the blur squared by v0.w: the glow on what is bright
//
// Both amounts come in the full-screen quad's vertex colour, set by the zone and the hour. No CVar sets them. So each
// new pixel shader the client binds is disassembled once, and the one that matches is swapped for a copy that scales
// v0.z and v0.w by [colour] gameGlow (Game Glow), in c30, which no client pixel shader uses (the highest is c9;
// terrainshade.cpp takes c31). At 1 the client's own shader is bound.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include "common.h"
#include "gameglow.h"

#include <string>

namespace
{
    struct Seen { IDirect3DPixelShader9* ps; bool glow; };
    Seen g_seen[256];
    int  g_seenCount = 0;
    IDirect3DPixelShader9* g_copy = nullptr;
    bool g_copyFailed = false;

    const char* kCopyHlsl =
        "sampler2D sWorld : register(s0);\n"
        "sampler2D sBlur  : register(s1);\n"
        "float4 gScale : register(c30);\n"
        "float4 main(float2 t0 : TEXCOORD0, float2 t1 : TEXCOORD1, float4 v0 : COLOR0) : COLOR\n"
        "{\n"
        "    float3 world = tex2D(sWorld, t0).rgb;\n"
        "    float3 blur  = tex2D(sBlur, t1).rgb;\n"
        "    float3 rgb   = lerp(world, blur, saturate(v0.z * gScale.x)) + blur * blur * (v0.w * gScale.x);\n"
        "    return float4(rgb, 1.0);\n"
        "}\n";

    std::string Disassemble(IDirect3DPixelShader9* ps)
    {
        auto dis = reinterpret_cast<PFN_D3DDisassemble>(CompilerProc("D3DDisassemble"));
        UINT size = 0;
        if (!dis || FAILED(ps->lpVtbl->GetFunction(ps, nullptr, &size)) || size == 0 || size > 65536)
            return std::string();
        std::string code(size, '\0');
        if (FAILED(ps->lpVtbl->GetFunction(ps, &code[0], &size)))
            return std::string();
        OgBlob* text = nullptr;
        if (FAILED(dis(code.data(), size, 0, nullptr, &text)) || !text)
            return std::string();
        std::string t(static_cast<const char*>(text->lpVtbl->GetBufferPointer(text)), text->lpVtbl->GetBufferSize(text));
        text->lpVtbl->Release(text);
        return t;
    }

    // The composite, by its three arithmetic lines and its two samplers.
    bool IsGlowComposite(const std::string& t)
    {
        return t.find("lrp r2.xyz, v0.z, r0, r1") != std::string::npos &&
               t.find("mul r0.xyz, r0, r0") != std::string::npos &&
               t.find("mad r0.xyz, r0, v0.w, r2") != std::string::npos &&
               t.find("dcl_2d s1") != std::string::npos && t.find("dcl_2d s2") == std::string::npos;
    }

    bool IsGlow(IDirect3DPixelShader9* ps)
    {
        for (int i = 0; i < g_seenCount; ++i)
            if (g_seen[i].ps == ps)
                return g_seen[i].glow;
        const bool glow = IsGlowComposite(Disassemble(ps));
        if (glow)
            Log("gameglow: pixel shader %p is the Full Screen Glow's composite", ps);
        if (g_seenCount < 256)
            g_seen[g_seenCount++] = { ps, glow };
        return glow;
    }

    IDirect3DPixelShader9* Copy(IDirect3DDevice9* dev)
    {
        if (g_copy || g_copyFailed)
            return g_copy;
        auto compile = reinterpret_cast<PFN_D3DCompile>(CompilerProc("D3DCompile"));
        OgBlob* code = nullptr;
        OgBlob* errs = nullptr;
        if (!compile || FAILED(compile(kCopyHlsl, strlen(kCopyHlsl), "gameglow", nullptr, nullptr, "main", "ps_2_0", 0, 0,
                                       &code, &errs)) || !code)
        {
            Log("gameglow: the copy failed to compile: %s",
                errs ? static_cast<const char*>(errs->lpVtbl->GetBufferPointer(errs)) : "(no compiler)");
            g_copyFailed = true;
        }
        else if (FAILED(dev->lpVtbl->CreatePixelShader(dev, static_cast<const DWORD*>(code->lpVtbl->GetBufferPointer(code)),
                                                       &g_copy)))
        {
            g_copy = nullptr;
            g_copyFailed = true;
            Log("gameglow: the copy could not be created");
        }
        else
            Log("gameglow: copy ready");
        if (code) code->lpVtbl->Release(code);
        if (errs) errs->lpVtbl->Release(errs);
        return g_copy;
    }
}

IDirect3DPixelShader9* GameGlowSwap(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, float scale)
{
    if (!ps || (scale > 0.999f && scale < 1.001f) || !IsGlow(ps))
        return ps;
    IDirect3DPixelShader9* copy = Copy(dev);
    if (!copy)
        return ps;
    const float k[4] = { scale < 0.0f ? 0.0f : scale, 0.0f, 0.0f, 0.0f };
    dev->lpVtbl->SetPixelShaderConstantF(dev, 30, k, 1);
    return copy;
}

bool GameGlowIsCopy(IDirect3DPixelShader9* ps)
{
    return ps && ps == g_copy;
}

void GameGlowReset()
{
    if (g_copy)
    {
        g_copy->lpVtbl->Release(g_copy);
        g_copy = nullptr;
    }
    g_copyFailed = false;
    g_seenCount = 0;
}

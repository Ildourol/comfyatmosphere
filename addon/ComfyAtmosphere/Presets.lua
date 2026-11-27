-- Presets.lua: Performance / Quality presets for ComfyAtmosphere (Vanilla WoW 1.12.1 / Lua 5.0).

ComfyAtmosphere_Presets = {
    {
        key = "verylow",
        label = "Very Low",
        desc = "Maximum FPS. Disables heavy volumetric, shadow, fog, water, and cloud passes; keeps light sun rays and grass.",
        cvars = {
            comfyAtmosphere = 1,
            comfyVolume = 0,
            comfyVolumeQuality = 1,
            comfyVolumeSteps = 16,
            comfyVolumeDownscale = 4,
            comfySunShadows = 0,
            comfyShadowResolution = 1,
            comfyShadowSoftness = 0,
            comfyShadowEvery = 4,
            comfyShadowRange = 180,
            comfyMist = 0,
            comfyLamps = 0,
            comfyLampMaxLights = 1,
            comfyRays = 1,
            comfyRaysPasses = 1,
            comfyClouds = 0,
            comfyWater = 0,
            comfyGrass = 1,
            comfyColor = 1,
            comfyAA = 0,
            comfyAAQuality = 1,
            comfySharpen = 0,
            farClip = 477,
            weatherDensity = 1,
        },
    },
    {
        key = "low",
        label = "Low",
        desc = "High FPS. Low-res volumetric light, 1024 shadows, fog, basic lamps, FXAA Low, and 627 view distance.",
        cvars = {
            comfyAtmosphere = 1,
            comfyVolume = 1,
            comfyVolumeQuality = 1,
            comfyVolumeSteps = 20,
            comfyVolumeDownscale = 4,
            comfySunShadows = 1,
            comfyShadowResolution = 1,
            comfyShadowSoftness = 1,
            comfyShadowEvery = 2,
            comfyShadowRange = 220,
            comfyMist = 1,
            comfyLamps = 1,
            comfyLampMaxLights = 8,
            comfyRays = 1,
            comfyRaysPasses = 1,
            comfyClouds = 0,
            comfyWater = 0,
            comfyGrass = 1,
            comfyColor = 1,
            comfyAA = 1,
            comfyAAQuality = 1,
            comfySharpen = 15,
            farClip = 627,
            weatherDensity = 2,
        },
    },
    {
        key = "medium",
        label = "Medium",
        desc = "Balanced performance and visuals. Medium volumetric light, 2048 shadows, water, clouds, FXAA Medium, and 777 view distance.",
        cvars = {
            comfyAtmosphere = 1,
            comfyVolume = 1,
            comfyVolumeQuality = 2,
            comfyVolumeSteps = 32,
            comfyVolumeDownscale = 2,
            comfySunShadows = 1,
            comfyShadowResolution = 2,
            comfyShadowSoftness = 2,
            comfyShadowEvery = 2,
            comfyShadowRange = 280,
            comfyMist = 1,
            comfyLamps = 1,
            comfyLampMaxLights = 16,
            comfyRays = 1,
            comfyRaysPasses = 2,
            comfyClouds = 1,
            comfyWater = 1,
            comfyGrass = 1,
            comfyColor = 1,
            comfyAA = 1,
            comfyAAQuality = 2,
            comfySharpen = 25,
            farClip = 777,
            weatherDensity = 3,
        },
    },
    {
        key = "high",
        label = "High",
        desc = "Rich visuals. High volumetric light, soft 2048 shadows, 24 lamps, 3-pass rays, SMAA Medium, and 977 view distance.",
        cvars = {
            comfyAtmosphere = 1,
            comfyVolume = 1,
            comfyVolumeQuality = 3,
            comfyVolumeSteps = 48,
            comfyVolumeDownscale = 2,
            comfySunShadows = 1,
            comfyShadowResolution = 2,
            comfyShadowSoftness = 4,
            comfyShadowEvery = 1,
            comfyShadowRange = 360,
            comfyMist = 1,
            comfyLamps = 1,
            comfyLampMaxLights = 24,
            comfyRays = 1,
            comfyRaysPasses = 3,
            comfyClouds = 1,
            comfyWater = 1,
            comfyGrass = 1,
            comfyColor = 1,
            comfyAA = 2,
            comfyAAQuality = 2,
            comfySharpen = 25,
            farClip = 977,
            weatherDensity = 3,
        },
    },
    {
        key = "ultra",
        label = "Ultra",
        desc = "Maximum fidelity (very heavy). Full-res volumetric light, 4096 soft shadows, 32 lamps, SMAA High, and 1277 view distance.",
        cvars = {
            comfyAtmosphere = 1,
            comfyVolume = 1,
            comfyVolumeQuality = 3,
            comfyVolumeSteps = 64,
            comfyVolumeDownscale = 1,
            comfySunShadows = 1,
            comfyShadowResolution = 3,
            comfyShadowSoftness = 6,
            comfyShadowEvery = 1,
            comfyShadowRange = 460,
            comfyMist = 1,
            comfyLamps = 1,
            comfyLampMaxLights = 32,
            comfyRays = 1,
            comfyRaysPasses = 3,
            comfyClouds = 1,
            comfyWater = 1,
            comfyGrass = 1,
            comfyColor = 1,
            comfyAA = 2,
            comfyAAQuality = 3,
            comfySharpen = 30,
            farClip = 1277,
            weatherDensity = 3,
        },
    },
}

function ComfyAtmosphere_ApplyPreset(key, onDone)
    local target = nil
    local i, n
    n = table.getn(ComfyAtmosphere_Presets)
    for i = 1, n do
        local entry = ComfyAtmosphere_Presets[i]
        if entry and entry.key == key then
            target = entry
            break
        end
    end
    if not target then
        return false
    end

    local cvar, val
    for cvar, val in pairs(target.cvars) do
        local ok = pcall(GetCVar, cvar)
        if ok then
            pcall(SetCVar, cvar, tostring(val))
        end
    end

    if type(onDone) == "function" then
        onDone()
    end

    if DEFAULT_CHAT_FRAME then
        DEFAULT_CHAT_FRAME:AddMessage("|cff88cc88comfyatmosphere|r: Applied " .. target.label .. " preset.")
    end
    return true
end

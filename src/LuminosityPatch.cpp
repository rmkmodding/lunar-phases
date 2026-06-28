#include <mutex>

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/Scripting/Natives/Generated/AtmosphereAreaSettings.hpp>
#include <RED4ext/Scripting/Natives/Generated/world/EnvAreaNotifier.hpp>
#include <RED4ext/Scripting/Natives/Generated/world/EnvironmentAreaParameters.hpp>
#include <RED4ext/Scripting/Natives/Generated/world/EnvironmentDefinition.hpp>
#include <RED4ext/Scripting/Natives/Generated/world/Node.hpp>
#include <RED4ext/Scripting/Natives/Generated/world/StreamingSector.hpp>
#include <RED4ext/Scripting/Natives/Generated/world/TriggerAreaNode.hpp>
#include <RED4ext/Scripting/Natives/Generated/LightAreaSettings.hpp>
#include <RED4ext/Scripting/Natives/Generated/WorldRenderAreaSettings.hpp>

#include "LuminosityPatch.hpp"
#include "LunarPhases.hpp"

namespace LunarPhases::LuminosityPatch
{

/// One discovered LightAreaSettings — controls the moon as a directional light source.
/// Scaling its moonColor dims both the moon light and the shadows it casts.
struct LightEntry
{
    RED4ext::SharedPtr<RED4ext::ResourceToken<>> token;
    RED4ext::LightAreaSettings*                  settings = nullptr;
    HDRCurveBaseline                             baseMoonColor;
};

// ─── Module state ─────────────────────────────────────────────────────────────

static const RED4ext::v1::Sdk*   s_sdk    = nullptr;
static RED4ext::v1::PluginHandle s_handle;

static std::mutex                s_mutex;
static std::vector<AtmosphereEntry> s_entries;

// Cached RTTI class pointers — populated lazily inside the mutex.
static RED4ext::CClass* s_envParamClass      = nullptr; // worldEnvironmentAreaParameters
static RED4ext::CClass* s_envDefinitionClass = nullptr; // worldEnvironmentDefinition
static RED4ext::CClass* s_sectorClass        = nullptr; // worldStreamingSector
static RED4ext::CClass* s_triggerNodeClass   = nullptr; // worldTriggerAreaNode
static RED4ext::CClass* s_envNotifierClass   = nullptr; // worldEnvAreaNotifier
static RED4ext::CClass*  s_atmosphereClass   = nullptr; // AtmosphereAreaSettings
static RED4ext::CClass*  s_lightClass        = nullptr; // LightAreaSettings
static std::vector<LightEntry> s_lightEntries;
static RED4ext::HDRColor s_colorTint{1.0f, 1.0f, 1.0f, 1.0f}; // per-channel tint multiplier
static bool              s_starDimming       = true;          // scale stars inversely to moon
static bool              s_moonLightScaling  = true;          // scale directional light + shadows with phase
static float             s_starBrightness    = 1.0f;          // manual multiplier on top of auto-dimming

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// Save all keyframes from a CurveData<float> into a FloatCurveBaseline.
static FloatCurveBaseline SaveFloatCurve(RED4ext::CurveData<float>& aCurve)
{
    FloatCurveBaseline baseline;
    const uint32_t size = aCurve.GetSize();
    baseline.times.reserve(size);
    baseline.values.reserve(size);
    for (uint32_t i = 0; i < size; ++i)
    {
        auto pt = aCurve.GetPoint(i);
        baseline.times.push_back(pt.point);
        baseline.values.push_back(pt.value);
    }
    return baseline;
}

/// Save all keyframes from a CurveData<HDRColor> into a HDRCurveBaseline.
static HDRCurveBaseline SaveHDRCurve(RED4ext::CurveData<RED4ext::HDRColor>& aCurve)
{
    HDRCurveBaseline baseline;
    const uint32_t size = aCurve.GetSize();
    baseline.times.reserve(size);
    baseline.colors.reserve(size);
    for (uint32_t i = 0; i < size; ++i)
    {
        auto pt = aCurve.GetPoint(i);
        baseline.times.push_back(pt.point);
        baseline.colors.push_back(pt.value);
    }
    return baseline;
}

/// Scale all keyframes of a CurveData<float> by aMultiplier, using saved baseline.
static void ApplyFloatCurve(RED4ext::CurveData<float>& aCurve,
                            const FloatCurveBaseline&  aBase,
                            float                      aMultiplier)
{
    const uint32_t size = static_cast<uint32_t>(aBase.times.size());
    for (uint32_t i = 0; i < size; ++i)
    {
        aCurve.SetPoint(i, aBase.times[i], aBase.values[i] * aMultiplier);
    }
}

/// Scale all keyframes of a CurveData<HDRColor> by aMultiplier (RGB only, Alpha unchanged).
static void ApplyHDRCurve(RED4ext::CurveData<RED4ext::HDRColor>& aCurve,
                           const HDRCurveBaseline&                aBase,
                           float                                  aMultiplier)
{
    const uint32_t size = static_cast<uint32_t>(aBase.times.size());
    for (uint32_t i = 0; i < size; ++i)
    {
        const auto& src = aBase.colors[i];
        RED4ext::HDRColor scaled{src.Red   * aMultiplier * s_colorTint.Red,
                                 src.Green * aMultiplier * s_colorTint.Green,
                                 src.Blue  * aMultiplier * s_colorTint.Blue,
                                 src.Alpha};
        aCurve.SetPoint(i, aBase.times[i], scaled);
    }
}

/// Scan one WorldRenderAreaSettings array and register any AtmosphereAreaSettings found.
/// Caller must hold s_mutex.
static void ScanAreaSettingsLocked(
    RED4ext::SharedPtr<RED4ext::ResourceToken<>>  aToken,
    RED4ext::WorldRenderAreaSettings&             aSettings,
    RED4ext::CClass*                              aAtmosphereClass,
    RED4ext::CClass*                              aLightClass,
    const char*                                   aSourceLabel)
{
    auto& params = aSettings.areaParameters;
    int32_t found = 0;
    for (uint32_t i = 0; i < params.Size(); ++i)
    {
        auto* areaSettings = params[i].instance;
        if (!areaSettings) continue;
        if (areaSettings->GetType() != aAtmosphereClass) continue;

        auto* atmo = static_cast<RED4ext::AtmosphereAreaSettings*>(areaSettings);

        // Skip duplicates — same AtmosphereAreaSettings* may come from both resource types.
        bool duplicate = false;
        for (const auto& existing : s_entries)
        {
            if (existing.settings == atmo) { duplicate = true; break; }
        }
        if (duplicate) continue;

        AtmosphereEntry entry;
        entry.token    = aToken;
        entry.settings = atmo;
        entry.baseMoonGlowIntensity = SaveFloatCurve(atmo->moonGlowIntensity);
        entry.baseMoonColor         = SaveHDRCurve(atmo->moonColor);
        entry.baseGalaxyIntensity   = SaveFloatCurve(atmo->galaxyIntensity);
        entry.baseStarMapIntensity  = SaveFloatCurve(atmo->starMapIntensity);

        // Apply the currently active phase immediately.
        const int32_t phase     = g_currentPhase.load(std::memory_order_relaxed);
        const float   scale     = kPhaseLuminosityScale[phase];
        const float   autoStarScale = s_starDimming ? 1.0f - scale * 0.8f : 1.0f;
        const float   starScale     = autoStarScale * s_starBrightness;
        if (!entry.baseMoonGlowIntensity.times.empty())
            ApplyFloatCurve(atmo->moonGlowIntensity, entry.baseMoonGlowIntensity, scale);
        if (!entry.baseMoonColor.times.empty())
            ApplyHDRCurve(atmo->moonColor, entry.baseMoonColor, scale);
        if (!entry.baseGalaxyIntensity.times.empty())
            ApplyFloatCurve(atmo->galaxyIntensity, entry.baseGalaxyIntensity, starScale);
        if (!entry.baseStarMapIntensity.times.empty())
            ApplyFloatCurve(atmo->starMapIntensity, entry.baseStarMapIntensity, starScale);

        s_entries.push_back(std::move(entry));
        ++found;
    }

    if (s_sdk && found > 0)
    {
        s_sdk->logger->InfoF(s_handle,
            "[Lum] [%s] Registered %d AtmosphereAreaSettings entries.", aSourceLabel, found);
    }

    // ── LightAreaSettings (directional light / shadow scale) ───────────────────────
    int32_t lightFound = 0;
    if (aLightClass)
    {
        for (uint32_t i = 0; i < params.Size(); ++i)
        {
            auto* areaSettings = params[i].instance;
            if (!areaSettings) continue;
            if (areaSettings->GetType() != aLightClass) continue;

            auto* light = static_cast<RED4ext::LightAreaSettings*>(areaSettings);

            bool duplicate = false;
            for (const auto& existing : s_lightEntries)
                if (existing.settings == light) { duplicate = true; break; }
            if (duplicate) continue;

            LightEntry le;
            le.token         = aToken;
            le.settings      = light;
            le.baseMoonColor = SaveHDRCurve(light->moonColor);

            const int32_t phase      = g_currentPhase.load(std::memory_order_relaxed);
            const float   scale      = kPhaseLuminosityScale[phase];
            const float   lightScale = s_moonLightScaling ? scale : 1.0f;
            if (!le.baseMoonColor.times.empty())
                ApplyHDRCurve(light->moonColor, le.baseMoonColor, lightScale);

            s_lightEntries.push_back(std::move(le));
            ++lightFound;
        }
        if (s_sdk && lightFound > 0)
            s_sdk->logger->InfoF(s_handle,
                "[Lum] [%s] Registered %d LightAreaSettings entries.", aSourceLabel, lightFound);
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────

void Init(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle)
{
    s_sdk    = aSdk;
    s_handle = aHandle;
}

void TryRegisterToken(RED4ext::SharedPtr<RED4ext::ResourceToken<>> aToken)
{
    if (!aToken.instance) return;
    if (!aToken.instance->IsLoaded()) return;

    auto* resource = aToken.instance->resource.instance;
    if (!resource) return;

    std::lock_guard<std::mutex> lock(s_mutex);

    // Lazy RTTI class cache (inside lock to avoid benign data race).
    auto* rtti = RED4ext::CRTTISystem::Get();
    if (!s_envParamClass)
        s_envParamClass      = rtti->GetClass("worldEnvironmentAreaParameters");
    if (!s_envDefinitionClass)
        s_envDefinitionClass = rtti->GetClass("worldEnvironmentDefinition");
    if (!s_sectorClass)
        s_sectorClass        = rtti->GetClass("worldStreamingSector");
    if (!s_triggerNodeClass)
        s_triggerNodeClass   = rtti->GetClass("worldTriggerAreaNode");
    if (!s_envNotifierClass)
        s_envNotifierClass   = rtti->GetClass("worldEnvAreaNotifier");
    if (!s_atmosphereClass)
        s_atmosphereClass = rtti->GetClass("AtmosphereAreaSettings");
    if (!s_lightClass)
        s_lightClass      = rtti->GetClass("LightAreaSettings");
    if (!s_atmosphereClass && !s_lightClass) return;

    auto* type = resource->GetType();

    if (type == s_envParamClass)
    {
        // Skip if we've already processed this resource.
        for (const auto& e : s_entries)
        {
            if (e.token.instance && e.token.instance->resource.instance == resource)
                return;
        }
        auto* env = static_cast<RED4ext::world::EnvironmentAreaParameters*>(resource);
        ScanAreaSettingsLocked(aToken, env->renderAreaSettings, s_atmosphereClass, s_lightClass, "envparam");
    }
    else if (type == s_envDefinitionClass)
    {
        // Skip if we've already processed this resource.
        for (const auto& e : s_entries)
        {
            if (e.token.instance && e.token.instance->resource.instance == resource)
                return;
        }
        auto* def = static_cast<RED4ext::world::EnvironmentDefinition*>(resource);
        ScanAreaSettingsLocked(aToken, def->worldRenderSettings, s_atmosphereClass, s_lightClass, "envdef");
    }
    else if (type == s_sectorClass)
    {
        // worldStreamingSector nodes are accessed via StreamingSectorNodeBuffer at
        // offset 0x40 (Codeware: Raw::StreamingSector::NodeBuffer).
        // nodes: DynArray<Handle<worldNode>> sits at +0x28 within that buffer.
        using NodeArrayT = RED4ext::DynArray<RED4ext::Handle<RED4ext::world::Node>>;
        auto* nodeArray = reinterpret_cast<NodeArrayT*>(
            reinterpret_cast<uint8_t*>(resource) + 0x40 + 0x28);

        for (uint32_t i = 0; i < nodeArray->Size(); ++i)
        {
            auto* node = (*nodeArray)[i].instance;
            if (!node) continue;
            if (node->GetType() != s_triggerNodeClass) continue;

            auto* trigger = static_cast<RED4ext::world::TriggerAreaNode*>(node);
            for (uint32_t j = 0; j < trigger->notifiers.Size(); ++j)
            {
                auto* notifier = trigger->notifiers[j].instance;
                if (!notifier) continue;
                if (static_cast<RED4ext::IScriptable*>(notifier)->GetType() != s_envNotifierClass)
                    continue;

                auto* envNotifier = static_cast<RED4ext::world::EnvAreaNotifier*>(notifier);
                ScanAreaSettingsLocked(aToken, envNotifier->params, s_atmosphereClass, s_lightClass, "sector");
            }
        }
    }
}

void ApplyPhase(int32_t aPhase)
{
    if (aPhase < 0) aPhase = 0;
    if (aPhase >= kPhaseCount) aPhase = kPhaseCount - 1;

    const float scale         = kPhaseLuminosityScale[aPhase];
    const float autoStarScale = s_starDimming ? 1.0f - scale * 0.8f : 1.0f;
    const float starScale     = autoStarScale * s_starBrightness;
    const float lightScale    = s_moonLightScaling ? scale : 1.0f;

    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& entry : s_entries)
    {
        if (!entry.settings) continue;
        if (!entry.baseMoonGlowIntensity.times.empty())
            ApplyFloatCurve(entry.settings->moonGlowIntensity, entry.baseMoonGlowIntensity, scale);
        if (!entry.baseMoonColor.times.empty())
            ApplyHDRCurve(entry.settings->moonColor, entry.baseMoonColor, scale);
        if (!entry.baseGalaxyIntensity.times.empty())
            ApplyFloatCurve(entry.settings->galaxyIntensity, entry.baseGalaxyIntensity, starScale);
        if (!entry.baseStarMapIntensity.times.empty())
            ApplyFloatCurve(entry.settings->starMapIntensity, entry.baseStarMapIntensity, starScale);
    }
    for (auto& le : s_lightEntries)
    {
        if (!le.settings) continue;
        if (!le.baseMoonColor.times.empty())
            ApplyHDRCurve(le.settings->moonColor, le.baseMoonColor, lightScale);
    }

    if (s_sdk && (!s_entries.empty() || !s_lightEntries.empty()))
    {
        s_sdk->logger->InfoF(s_handle,
            "[Lum] Phase %d applied (scale=%.2f, star=%.2f, light=%.2f) to %zu atmo + %zu light entries.",
            aPhase, scale, starScale, lightScale, s_entries.size(), s_lightEntries.size());
    }
}

void SetColorTint(float r, float g, float b)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_colorTint = {r, g, b, 1.0f};
    }
    ApplyPhase(g_currentPhase.load(std::memory_order_relaxed));
}

void SetStarDimming(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_starDimming = enabled;
    }
    ApplyPhase(g_currentPhase.load(std::memory_order_relaxed));
}

void SetMoonLightScaling(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_moonLightScaling = enabled;
    }
    ApplyPhase(g_currentPhase.load(std::memory_order_relaxed));
}

void SetStarBrightness(float multiplier)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_starBrightness = multiplier;
    }
    ApplyPhase(g_currentPhase.load(std::memory_order_relaxed));
}

void ReleaseAll()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_entries.clear();
    s_envParamClass      = nullptr;
    s_envDefinitionClass = nullptr;
    s_sectorClass        = nullptr;
    s_triggerNodeClass   = nullptr;
    s_envNotifierClass   = nullptr;
    s_atmosphereClass    = nullptr;
    s_lightEntries.clear();
    s_lightClass         = nullptr;
    s_colorTint          = {1.0f, 1.0f, 1.0f, 1.0f};
    s_starDimming        = false;
    s_moonLightScaling   = true;
    s_starBrightness     = 1.0f;
}

} // namespace LunarPhases::LuminosityPatch

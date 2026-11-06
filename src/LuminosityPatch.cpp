#include <mutex>
#include <optional>
#include <unordered_set>

#include <Windows.h>

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

/// One discovered LightAreaSettings - controls the moon as a directional light source.
/// Scaling its moonColor dims both the moon light and the shadows it casts.
struct LightEntry
{
    RED4ext::WeakHandle<RED4ext::IAreaSettings> handle;            // weak, see AtmosphereEntry
    RED4ext::LightAreaSettings*                 settings = nullptr; // valid only while handle is locked
    HDRCurveBaseline                            baseMoonColor;
};

/// The values the registered curves currently hold.  New entries are scaled as they register,
/// so this stays accurate for them too.
struct AppliedValues
{
    float moonScale;
    float starScale;
    float lightScale;
    float tintR;
    float tintG;
    float tintB;

    bool operator==(const AppliedValues&) const = default;
};

// ─── Module state ─────────────────────────────────────────────────────────────

static const RED4ext::v1::Sdk*   s_sdk    = nullptr;
static RED4ext::v1::PluginHandle s_handle;

static std::mutex                s_mutex;
static std::vector<AtmosphereEntry> s_entries;

// Cached RTTI class pointers - populated lazily inside the mutex.
static RED4ext::CClass* s_envParamClass      = nullptr; // worldEnvironmentAreaParameters
static RED4ext::CClass* s_envDefinitionClass = nullptr; // worldEnvironmentDefinition
static RED4ext::CClass* s_sectorClass        = nullptr; // worldStreamingSector
static RED4ext::CClass* s_triggerNodeClass   = nullptr; // worldTriggerAreaNode
static RED4ext::CClass* s_envNotifierClass   = nullptr; // worldEnvAreaNotifier
static RED4ext::CClass*  s_atmosphereClass   = nullptr; // AtmosphereAreaSettings
static RED4ext::CClass*  s_lightClass        = nullptr; // LightAreaSettings
static std::vector<LightEntry> s_lightEntries;
// Serial IDs (ISerializable::unk28, never reused) of resources already scanned.  A resource
// that unloads and loads again gets a new ID, so it is scanned again.
static std::unordered_set<uint64_t> s_scannedResources;
static RED4ext::HDRColor s_colorTint{1.0f, 1.0f, 1.0f, 1.0f}; // per-channel tint multiplier
static bool              s_starDimming       = true;          // scale stars inversely to moon
static bool              s_moonLightScaling  = true;          // scale directional light + shadows with phase
static float             s_starBrightness    = 1.0f;          // manual multiplier on top of auto-dimming
static std::optional<AppliedValues> s_lastApplied;            // empty until the first ApplyPhase

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// True if aAddress is inside a section of the game executable that is code (aExecutable)
/// or data (!aExecutable).
static bool IsInGameSection(uintptr_t aAddress, bool aExecutable)
{
    const auto  base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto* sec  = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        const uintptr_t start = base + sec->VirtualAddress;
        if (aAddress >= start && aAddress < start + sec->Misc.VirtualSize)
            return ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) == aExecutable;
    }
    return false;
}

/// GetType() for objects owned by a loaded sector, or nullptr if the object is not intact.
/// Sector objects have been seen with a non-vtable first qword while their sector is still
/// alive (a freed or reused block), and calling through it crashes the resource thread.
/// The range checks catch that without faulting; SEH covers memory changing under the checks.
static RED4ext::CClass* SafeGetType(RED4ext::ISerializable* aObject)
{
    __try
    {
        const auto* vtable = *reinterpret_cast<const uintptr_t* const*>(aObject);
        if (!IsInGameSection(reinterpret_cast<uintptr_t>(vtable), false))
            return nullptr;
        if (!IsInGameSection(vtable[1], true)) // slot 08: GetType
            return nullptr;
        return aObject->GetType();
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return nullptr;
    }
}

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

/// Drop entries whose settings object has been destroyed (its resource unloaded).
/// Caller must hold s_mutex.
static void ReapDeadEntriesLocked()
{
    std::erase_if(s_entries, [](const AtmosphereEntry& e) { return e.handle.Expired(); });
    std::erase_if(s_lightEntries, [](const LightEntry& e) { return e.handle.Expired(); });
}

/// Scan one WorldRenderAreaSettings array and register any AtmosphereAreaSettings found.
/// Caller must hold s_mutex.
static void ScanAreaSettingsLocked(
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
        auto* settingsType = SafeGetType(areaSettings);
        if (!settingsType || settingsType != aAtmosphereClass) continue;

        auto* atmo = static_cast<RED4ext::AtmosphereAreaSettings*>(areaSettings);

        // Skip duplicates - same AtmosphereAreaSettings* may come from both resource types.
        // An expired entry's address may have been reused, so it does not count.
        bool duplicate = false;
        for (const auto& existing : s_entries)
        {
            if (existing.settings == atmo && !existing.handle.Expired()) { duplicate = true; break; }
        }
        if (duplicate) continue;

        AtmosphereEntry entry;
        entry.handle   = params[i];
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
            if (SafeGetType(areaSettings) != aLightClass) continue;

            auto* light = static_cast<RED4ext::LightAreaSettings*>(areaSettings);

            bool duplicate = false;
            for (const auto& existing : s_lightEntries)
                if (existing.settings == light && !existing.handle.Expired()) { duplicate = true; break; }
            if (duplicate) continue;

            LightEntry le;
            le.handle        = params[i];
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
    if (type != s_envParamClass && type != s_envDefinitionClass && type != s_sectorClass)
        return;

    // Cached resources come back through the hook on every request; only scan each one once.
    if (!s_scannedResources.insert(resource->unk28).second)
        return;

    // A newly loaded resource usually means others streamed out; forget their entries.
    ReapDeadEntriesLocked();

    if (type == s_envParamClass)
    {
        auto* env = static_cast<RED4ext::world::EnvironmentAreaParameters*>(resource);
        ScanAreaSettingsLocked(env->renderAreaSettings, s_atmosphereClass, s_lightClass, "envparam");
    }
    else if (type == s_envDefinitionClass)
    {
        auto* def = static_cast<RED4ext::world::EnvironmentDefinition*>(resource);
        ScanAreaSettingsLocked(def->worldRenderSettings, s_atmosphereClass, s_lightClass, "envdef");
    }
    else // s_sectorClass
    {
        // worldStreamingSector nodes are accessed via worldCompiledSector at
        // offset 0x40 (Codeware: worldStreamingSector::compiled).
        // nodes: DynArray<Handle<worldNode>> sits at +0x30 within it (+0x28 before patch 2.5).
        using NodeArrayT = RED4ext::DynArray<RED4ext::Handle<RED4ext::world::Node>>;
        auto* nodeArray = reinterpret_cast<NodeArrayT*>(
            reinterpret_cast<uint8_t*>(resource) + 0x40 + 0x30);

        uint32_t invalidObjects = 0;
        for (uint32_t i = 0; i < nodeArray->Size(); ++i)
        {
            auto* node = (*nodeArray)[i].instance;
            if (!node) continue;
            auto* nodeType = SafeGetType(node);
            if (!nodeType) { ++invalidObjects; continue; }
            if (nodeType != s_triggerNodeClass) continue;

            auto* trigger = static_cast<RED4ext::world::TriggerAreaNode*>(node);
            for (uint32_t j = 0; j < trigger->notifiers.Size(); ++j)
            {
                auto* notifier = trigger->notifiers[j].instance;
                if (!notifier) continue;
                auto* notifierType = SafeGetType(static_cast<RED4ext::IScriptable*>(notifier));
                if (!notifierType) { ++invalidObjects; continue; }
                if (notifierType != s_envNotifierClass) continue;

                auto* envNotifier = static_cast<RED4ext::world::EnvAreaNotifier*>(notifier);
                ScanAreaSettingsLocked(envNotifier->params, s_atmosphereClass, s_lightClass, "sector");
            }
        }

        if (s_sdk && invalidObjects > 0)
        {
            s_sdk->logger->WarnF(s_handle,
                "[Lum] Skipped %u invalid objects in sector 0x%p.", invalidObjects, static_cast<void*>(resource));
        }
    }
}

void ApplyPhase(int32_t aPhase)
{
    if (aPhase < 0) aPhase = 0;
    if (aPhase >= kPhaseCount) aPhase = kPhaseCount - 1;

    std::lock_guard<std::mutex> lock(s_mutex);

    const float scale         = kPhaseLuminosityScale[aPhase];
    const float autoStarScale = s_starDimming ? 1.0f - scale * 0.8f : 1.0f;
    const float starScale     = autoStarScale * s_starBrightness;
    const float lightScale    = s_moonLightScaling ? scale : 1.0f;

    // Callers run up to once per frame during a time skip, so only touch the curves on a change.
    const AppliedValues values{scale, starScale, lightScale, s_colorTint.Red, s_colorTint.Green, s_colorTint.Blue};
    if (s_lastApplied == values)
        return;

    // The moon tint changes constantly as game time moves, so it does not get a log line.
    const bool brightnessChanged = !s_lastApplied || s_lastApplied->moonScale != scale ||
                                   s_lastApplied->starScale != starScale || s_lastApplied->lightScale != lightScale;
    s_lastApplied = values;

    ReapDeadEntriesLocked();

    for (auto& entry : s_entries)
    {
        // Holding the locked handle keeps the settings alive while the curves are written.
        const auto alive = entry.handle.Lock();
        if (!alive.instance || !entry.settings) continue;
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
        const auto alive = le.handle.Lock();
        if (!alive.instance || !le.settings) continue;
        if (!le.baseMoonColor.times.empty())
            ApplyHDRCurve(le.settings->moonColor, le.baseMoonColor, lightScale);
    }

    if (s_sdk && brightnessChanged && (!s_entries.empty() || !s_lightEntries.empty()))
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
    s_scannedResources.clear();
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
    s_lastApplied.reset();
}

void AbandonAll()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_entries.empty())
        new std::vector<AtmosphereEntry>(std::move(s_entries));
    if (!s_lightEntries.empty())
        new std::vector<LightEntry>(std::move(s_lightEntries));
}

} // namespace LunarPhases::LuminosityPatch

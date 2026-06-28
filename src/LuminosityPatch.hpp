#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Memory/SharedPtr.hpp>
#include <RED4ext/ResourceLoader.hpp>
#include <RED4ext/Scripting/Natives/Generated/AtmosphereAreaSettings.hpp>
#include <RED4ext/Scripting/Natives/Generated/HDRColor.hpp>

namespace LunarPhases
{

// ─── Phase luminosity multipliers ────────────────────────────────────────────

/// Luminosity scale per phase (0 = new moon … 15 = waning crescent).
/// Derived from the cosine illumination formula: f(i) = (1 - cos(i * π/8)) / 2
///   0  new moon        -   0 %
///   1  waxing crescent -   4 %
///   2  waxing crescent -  15 %
///   3  waxing crescent -  31 %
///   4  first quarter   -  50 %
///   5  waxing gibbous  -  69 %
///   6  waxing gibbous  -  85 %
///   7  waxing gibbous  -  96 %
///   8  full moon       - 100 %
///   9  waning gibbous  -  96 %
///  10  waning gibbous  -  85 %
///  11  waning gibbous  -  69 %
///  12  last quarter    -  50 %
///  13  waning crescent -  31 %
///  14  waning crescent -  15 %
///  15  waning crescent -   4 %
inline constexpr float kPhaseLuminosityScale[16] = {
    0.00f, 0.04f, 0.15f, 0.31f, 0.50f, 0.69f, 0.85f, 0.96f,
    1.00f, 0.96f, 0.85f, 0.69f, 0.50f, 0.31f, 0.15f, 0.04f};

// ─── Baseline storage ────────────────────────────────────────────────────────

/// Saved original keyframes for a single CurveData<float>.
struct FloatCurveBaseline
{
    std::vector<float> times;
    std::vector<float> values;
};

/// Saved original keyframes for a single CurveData<HDRColor>.
struct HDRCurveBaseline
{
    std::vector<float>             times;
    std::vector<RED4ext::HDRColor> colors;
};

/// One discovered AtmosphereAreaSettings, extracted from any loaded environment resource.
struct AtmosphereEntry
{
    RED4ext::SharedPtr<RED4ext::ResourceToken<>> token;   ///< keeps resource alive
    RED4ext::AtmosphereAreaSettings*             settings = nullptr;
    FloatCurveBaseline                           baseMoonGlowIntensity;
    HDRCurveBaseline                             baseMoonColor;
    FloatCurveBaseline                           baseGalaxyIntensity;
    FloatCurveBaseline                           baseStarMapIntensity;
};

// ─── Public API ──────────────────────────────────────────────────────────────

namespace LuminosityPatch
{

/// Store the SDK pointer and handle for logging.  Call before any other function.
void Init(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle);

/// Examine a resource token after load.  If it points to a
/// worldEnvironmentAreaParameters resource, extract and store any
/// AtmosphereAreaSettings entries it contains.
/// Thread-safe: may be called from the resource loading thread.
void TryRegisterToken(RED4ext::SharedPtr<RED4ext::ResourceToken<>> aToken);

/// Apply the luminosity scale for the given phase to all registered entries.
/// Call from the game thread (SetPhase, OnRunningEnter, etc.).
void ApplyPhase(int32_t aPhase);

/// Override the moon colour tint (per-channel multiplier on top of the phase scale).
/// (1, 1, 1) restores the original colours.  Thread-safe; re-applies immediately.
void SetColorTint(float r, float g, float b);

/// Enable or disable automatic star dimming.
/// When enabled, galaxyIntensity and starMapIntensity scale inversely with moon luminosity
/// so new-moon nights are fully starry and full-moon nights are washed out (~20% stars).
/// Thread-safe; re-applies immediately.
void SetStarDimming(bool enabled);

/// Enable or disable directional-light (moon light + shadow) scaling with the lunar phase.
/// Default true.  When false the moon's directional light stays at full intensity regardless
/// of phase (visual disc/glow still scales normally).
/// Thread-safe; re-applies immediately.
void SetMoonLightScaling(bool enabled);

/// Apply a manual multiplier to star/galaxy brightness on top of any auto-dimming.
/// 1.0 = default (no change), 0.0 = stars off, 2.0 = double brightness.
/// Stacks multiplicatively with SetStarDimming auto-scaling.
/// Thread-safe; re-applies immediately.
void SetStarBrightness(float multiplier);

/// Release all stored entries.  Call on plugin Unload.
void ReleaseAll();

} // namespace LuminosityPatch

} // namespace LunarPhases

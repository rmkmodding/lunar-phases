#pragma once

#include <atomic>
#include <cstdint>

#include <RED4ext/ResourceLoader.hpp>
#include <RED4ext/ResourcePath.hpp>

namespace LunarPhases
{

// ─── Constants ───────────────────────────────────────────────────────────────

inline constexpr int32_t kPhaseCount = 16;

/// Original moon texture path used by the game.
inline constexpr const char* kMoonTexturePath = "engine\\textures\\sky\\moon.xbm";

/// Phase texture paths (0 = new moon ... 15 = waning crescent).
/// An archive containing these assets must be placed in:
///   Cyberpunk 2077\archive\pc\mod\
/// Each XBM should match the original: 512x512, TCM_QualityColor, TRF_TrueColor,
/// TEXG_Generic_Color, isGamma=1, isStreamable=1, 10 mips.
inline constexpr const char* kPhasePaths[kPhaseCount] = {
    "lunar_phases\\textures\\sky\\moon_phase_0.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_1.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_2.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_3.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_4.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_5.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_6.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_7.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_8.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_9.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_10.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_11.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_12.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_13.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_14.xbm",
    "lunar_phases\\textures\\sky\\moon_phase_15.xbm",
};

/// Currently active phase index (0-15).
/// Written by Redscript via LunarPhases_SetPhase; read by the hook in Hooks.cpp.
extern std::atomic<int32_t> g_currentPhase;

/// FNV1a-hashed, sanitised path of the original moon texture.
/// Initialised in AttachHooks; read by the hook detour in Hooks.cpp.
extern RED4ext::ResourcePath g_moonPath;

/// Pre-loaded tokens for all 16 phase textures.
/// Populated by PreloadPhaseTextures(); read by SwapMoonTexture() in Hooks.cpp.
extern RED4ext::SharedPtr<RED4ext::ResourceToken<>> g_phaseTokens[kPhaseCount];

// ─── Lifetime ────────────────────────────────────────────────────────────────

/// Pre-load all phase textures via ResourceLoader.  Submits all 16 loads at once;
/// prefer PreloadPhaseTexture() + staggering to avoid job-queue flooding on startup.
void PreloadPhaseTextures();

/// Pre-load a single phase texture asynchronously.  Safe to call repeatedly;
/// returns immediately if the token is already held.
void PreloadPhaseTexture(int32_t phase);

/// Release all cached phase tokens.  Call on plugin Unload.
void ReleasePhaseTextures();

/// Store the SDK pointer and handle for logging.  Call before RegisterTypes.
void Init(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle);

/// Register the LunarPhases native class type.  Call via AddRegisterCallback.
void RegisterTypes();

/// Set up the LunarPhases class parent and register its native methods.  Call via AddPostRegisterCallback.
void PostRegisterTypes();

} // namespace LunarPhases

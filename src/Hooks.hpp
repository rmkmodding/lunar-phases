#pragma once

#include <RED4ext/Api/v1/Hooking.hpp>
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

namespace LunarPhases
{

/// Install the ResourceLoader path-redirect hook.  Call during plugin Load.
void AttachHooks(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle);

/// Remove the ResourceLoader path-redirect hook.  Call during plugin Unload.
void DetachHooks(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle);

/// Replace the resource inside the cached moon token with the texture for the
/// given phase.  Safe to call from the game-logic thread at any time after the
/// initial moon load has been captured by the hook.
void SwapMoonTexture(int32_t phase);

/// Discard the captured moon token and saved phase-0 resource so the hook
/// re-acquires a fresh token on the next session load.
void ResetMoonToken();

/// Returns true once the hook has captured a valid moon ResourceToken.
bool IsMoonTokenCaptured();

/// Enable or disable luminosity-patch OnLoaded callback registration inside the hook.
/// Must be called with aRunning=true in OnRunningEnter and false in OnShutdownEnter.
/// Before Running is active, the hook skips OnLoaded registration and synchronous
/// TryRegisterToken calls entirely: engine-init resources are never atmosphere types,
/// and registering callbacks on them floods the job queue during the startup burst.
void SetRunningState(bool aRunning);

} // namespace LunarPhases

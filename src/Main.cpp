#include <RED4ext/RED4ext.hpp>

#include "version.h"
#include "Hooks.hpp"
#include "LuminosityPatch.hpp"
#include "LunarPhases.hpp"

// Tracks whether a texture swap is pending after a new session's moon token is captured.
static bool    s_pendingTextureApply = false;
// Stagger cursor for spreading phase-texture preloads across frames (avoids job-queue flooding).
static int32_t s_preloadCursor       = 0;

// Shutdown OnEnter: the current game session is ending.  Reset the moon token so the
// hook is forced to re-capture a fresh one when the next session's sky loads.
static bool OnShutdownEnter(RED4ext::CGameApplication*)
{
    LunarPhases::SetRunningState(false);
    LunarPhases::ResetMoonToken();
    return true;
}

// Running OnEnter: world has started loading, ResourceLoader is ready.
static bool OnRunningEnter(RED4ext::CGameApplication*)
{
    // Load only the current phase now; stagger the rest in OnRunningUpdate to avoid job-queue flooding.
    LunarPhases::SetRunningState(true);
    LunarPhases::PreloadPhaseTexture(LunarPhases::g_currentPhase.load(std::memory_order_relaxed));
    LunarPhases::LuminosityPatch::ApplyPhase(LunarPhases::g_currentPhase.load(std::memory_order_relaxed));
    // Defer swap to OnUpdate; the sky token isn't captured yet.
    s_pendingTextureApply = true;
    s_preloadCursor       = 0;
    return true;
}

// Running OnUpdate: fires every frame while in Running state.  Used to apply the texture
// swap as soon as the hook confirms the session's moon token is ready, and to stagger
// preloading of the remaining phase textures.  Returns false to remain active across
// future save loads.
static bool OnRunningUpdate(RED4ext::CGameApplication*)
{
    // Stagger remaining phase preloads one per frame to avoid job-queue flooding.
    if (s_preloadCursor < LunarPhases::kPhaseCount)
    {
        LunarPhases::PreloadPhaseTexture(s_preloadCursor);
        ++s_preloadCursor;
    }

    if (s_pendingTextureApply && LunarPhases::IsMoonTokenCaptured())
    {
        LunarPhases::SwapMoonTexture(LunarPhases::g_currentPhase.load(std::memory_order_relaxed));
        s_pendingTextureApply = false;
    }
    return false; // keep alive for future Running re-entries
}

// ─── Plugin entry point ───────────────────────────────────────────────────────

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    switch (aReason)
    {
    case RED4ext::v1::EMainReason::Load:
    {
        // Register the .reds script so RED4ext compiles it after native functions are registered.
        // aSdk->scripts->Add(aHandle, L"scripts\\LunarPhases.reds");

        // Store SDK/handle for logging inside LunarPhases native functions.
        LunarPhases::Init(aSdk, aHandle);

        // Install the resource-path redirect hook.
        LunarPhases::AttachHooks(aSdk, aHandle);

        // Register the LunarPhases native class, matching the SDK example pattern exactly.
        auto* rtti = RED4ext::CRTTISystem::Get();
        rtti->AddRegisterCallback(&LunarPhases::RegisterTypes);
        rtti->AddPostRegisterCallback(&LunarPhases::PostRegisterTypes);

        // Shutdown: reset moon token so the hook re-captures cleanly for the next session.
        RED4ext::v1::GameState shutdownState{&OnShutdownEnter, nullptr, nullptr};
        aSdk->gameStates->Add(aHandle, RED4ext::EGameStateType::Shutdown, &shutdownState);

        // Running: preload phase textures and defer texture swap to OnUpdate.
        RED4ext::v1::GameState runningState{&OnRunningEnter, &OnRunningUpdate, nullptr};
        aSdk->gameStates->Add(aHandle, RED4ext::EGameStateType::Running, &runningState);

        aSdk->logger->Info(aHandle, "LunarPhases plugin loaded.");
        break;
    }
    case RED4ext::v1::EMainReason::Unload:
    {
        LunarPhases::ReleasePhaseTextures();
        LunarPhases::DetachHooks(aSdk, aHandle);
        LunarPhases::LuminosityPatch::ReleaseAll();

        aSdk->logger->Info(aHandle, "LunarPhases plugin unloaded.");
        break;
    }
    }

    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name    = L"LunarPhases";
    aInfo->author  = L"RMK";
    aInfo->version = RED4EXT_V1_SEMVER(VER_MAJOR, VER_MINOR, VER_PATCH);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
    aInfo->sdk     = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}

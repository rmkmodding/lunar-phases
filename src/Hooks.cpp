#include <algorithm>
#include <mutex>
#include <vector>

#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Memory/SharedPtr.hpp>
#include <RED4ext/ResourceLoader.hpp>
#include <RED4ext/SharedSpinLock.hpp>

#include "Hooks.hpp"
#include "LuminosityPatch.hpp"
#include "LunarPhases.hpp"

namespace LunarPhases
{

// ─── Hook state ──────────────────────────────────────────────────────────────

using IssueLoadingRequest_t =
    uintptr_t (*)(RED4ext::ResourceLoader*,
                  RED4ext::SharedPtr<RED4ext::ResourceToken<>>&,
                  const RED4ext::ResourceRequest&);

static IssueLoadingRequest_t                        s_originalLoadByRequest = nullptr;
static void*                                        s_hookTargetByRequest   = nullptr;
static const RED4ext::v1::Sdk*                      s_sdk                   = nullptr;
static RED4ext::v1::PluginHandle                    s_handle;

// All distinct moon ResourceTokens the game is currently holding.  The game can
// hand the sky renderer a fresh ResourceToken on scene transitions (main menu →
// gameplay, or loading a new save) while older tokens may briefly remain the ones
// actively rendered.  To guarantee the live moon updates, SwapMoonTexture()
// rewrites the resource on EVERY tracked token rather than guessing which one the
// renderer reads.
//
// We hold each token by SharedPtr (a strong reference), so a tracked token can
// never dangle.  When a token's strong-ref count drops to 1 it means WE are the
// only remaining holder — the game has released it and it can never be rendered
// again — so it is reaped.  This keeps the set bounded to just the tokens the game
// still references (normally one, occasionally a couple during a transition),
// deterministically and regardless of how many saves the user loads.
static std::vector<RED4ext::SharedPtr<RED4ext::ResourceToken<>>> g_moonTokens;
static std::mutex                                                g_moonTokensMutex;

// Canonical CResource handle for each phase, captured lazily on first swap.
// Stored separately from g_phaseTokens so that writing to a moon token's resource
// (which may alias g_phaseTokens[prev_phase]) never corrupts the source handles.
static RED4ext::Handle<RED4ext::CResource> g_phaseResources[kPhaseCount];

// Drop tokens that only WE still reference (use count <= 1): the game has released
// them, so they are no longer rendered.  Caller must hold g_moonTokensMutex.
static void ReapDeadMoonTokensLocked()
{
    std::erase_if(g_moonTokens,
        [](const auto& t) { return !t.instance || t.GetUseCount() <= 1; });
}

// ─── Hook detour ─────────────────────────────────────────────────────────────

static uintptr_t Hook_IssueLoadingRequest(
    RED4ext::ResourceLoader*                      aThis,
    RED4ext::SharedPtr<RED4ext::ResourceToken<>>& aOut,
    const RED4ext::ResourceRequest&               aRequest)
{
    if (aRequest.path == g_moonPath)
    {
        const int32_t phase = g_currentPhase.load(std::memory_order_relaxed);
        RED4ext::ResourceRequest redirected = aRequest;
        redirected.path = RED4ext::ResourcePath(kPhasePaths[phase]);
        auto result = s_originalLoadByRequest(aThis, aOut, redirected);

        if (aOut.instance)
        {
            // Track every distinct moon token.  Whichever one the renderer ends up
            // reading, SwapMoonTexture() will update it because it rewrites all of them.
            std::lock_guard<std::mutex> guard(g_moonTokensMutex);
            ReapDeadMoonTokensLocked(); // drop tokens the game has since released
            const bool isNew = std::none_of(g_moonTokens.begin(), g_moonTokens.end(),
                [&](const auto& t) { return t.instance == aOut.instance; });
            if (isNew)
            {
                g_moonTokens.push_back(aOut);
                s_sdk->logger->InfoF(s_handle,
                    "[Hook] Moon token captured phase=%d token=0x%p (tracked=%zu)",
                    phase, static_cast<void*>(aOut.instance), g_moonTokens.size());
            }
        }
        else
            s_sdk->logger->Warn(s_handle, "[Hook] Moon token is null after redirect!");

        return result;
    }

    auto result = s_originalLoadByRequest(aThis, aOut, aRequest);

    // Probe every returned token for an AtmosphereAreaSettings to support
    // runtime luminosity scaling.  Already-loaded resources are checked
    // synchronously; pending ones use OnLoaded to check asynchronously.
    if (aOut.instance)
    {
        if (aOut.instance->IsLoaded())
        {
            LuminosityPatch::TryRegisterToken(aOut);
        }
        else
        {
            aOut.instance->OnLoaded(
                [token = aOut](RED4ext::Handle<RED4ext::CResource>&)
                {
                    LuminosityPatch::TryRegisterToken(token);
                });
        }
    }

    return result;
}

// ─── Attach / Detach ─────────────────────────────────────────────────────────

void AttachHooks(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle)
{
    s_sdk    = aSdk;
    s_handle = aHandle;

    g_moonPath = RED4ext::ResourcePath(kMoonTexturePath);

    RED4ext::UniversalRelocFunc<IssueLoadingRequest_t> relocFunc(
        RED4ext::Detail::AddressHashes::ResourceLoader_IssueLoadingRequest);
    s_hookTargetByRequest = reinterpret_cast<void*>(static_cast<IssueLoadingRequest_t>(relocFunc));

    if (!s_hookTargetByRequest)
        aSdk->logger->Error(aHandle, "[Hook] IssueLoadingRequest address lookup returned null.");
    else
    {
        aSdk->hooking->Attach(aHandle, s_hookTargetByRequest,
            reinterpret_cast<void*>(&Hook_IssueLoadingRequest),
            reinterpret_cast<void**>(&s_originalLoadByRequest));
        aSdk->logger->InfoF(aHandle, "[Hook] Attached at 0x%p (moon hash 0x%016llX)",
            s_hookTargetByRequest, static_cast<uint64_t>(g_moonPath));
    }
}

void DetachHooks(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle)
{
    if (s_hookTargetByRequest)
    {
        aSdk->hooking->Detach(aHandle, s_hookTargetByRequest);
        s_hookTargetByRequest   = nullptr;
        s_originalLoadByRequest = nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(g_moonTokensMutex);
        g_moonTokens.clear();
    }
    for (auto& r : g_phaseResources) r.Reset();
}

void ResetMoonToken()
{
    {
        std::lock_guard<std::mutex> guard(g_moonTokensMutex);
        g_moonTokens.clear();
    }
    // g_phaseResources is intentionally kept: the canonical texture handles
    // remain valid across session transitions as long as the tokens are alive.
    if (s_sdk) s_sdk->logger->Info(s_handle, "[Hook] Moon tokens reset — will re-capture on next load.");
}

bool IsMoonTokenCaptured()
{
    std::lock_guard<std::mutex> guard(g_moonTokensMutex);
    return !g_moonTokens.empty();
}

void SwapMoonTexture(int32_t phase)
{
    // Capture the canonical CResource handle for this phase once, from the phase's
    // own preloaded token, before any swap.  g_phaseResources is never mutated
    // through a moon token, so it stays a clean source even though moon tokens may
    // alias g_phaseTokens entries via the ResourceLoader cache.
    if (!g_phaseResources[phase].instance)
    {
        auto& tok = g_phaseTokens[phase];
        if (!tok.instance)
        {
            if (s_sdk) s_sdk->logger->WarnF(s_handle, "[Swap] Phase %d token not preloaded.", phase);
            return;
        }
        if (!tok.instance->IsLoaded())
            tok.instance->Fetch();
        if (!tok.instance->IsLoaded())
        {
            if (s_sdk) s_sdk->logger->WarnF(s_handle, "[Swap] Phase %d texture failed to load.", phase);
            return;
        }
        g_phaseResources[phase] = tok.instance->resource;
    }

    // Rewrite the resource handle on EVERY tracked moon token.  The sky renderer
    // reads token->resource to get the CBitmapTexture*; whichever token it holds,
    // it now points at the requested phase.  Skipping tokens that already match
    // avoids needless churn.
    size_t swapped = 0;
    size_t tracked = 0;
    {
        std::lock_guard<std::mutex> guard(g_moonTokensMutex);
        ReapDeadMoonTokensLocked(); // drop tokens the game has since released
        if (g_moonTokens.empty())
        {
            if (s_sdk) s_sdk->logger->Warn(s_handle, "[Swap] No moon token captured yet.");
            return;
        }
        for (auto& token : g_moonTokens)
        {
            std::lock_guard<RED4ext::SharedSpinLock> tokGuard(token.instance->lock);
            if (token.instance->resource.instance == g_phaseResources[phase].instance)
                continue;
            token.instance->resource = g_phaseResources[phase];
            ++swapped;
        }
        tracked = g_moonTokens.size();
    }

    if (s_sdk)
        s_sdk->logger->InfoF(s_handle,
            "[Swap] phase %d applied to %zu/%zu tracked moon tokens (res=0x%p)",
            phase, swapped, tracked,
            static_cast<void*>(g_phaseResources[phase].instance));
}

} // namespace LunarPhases

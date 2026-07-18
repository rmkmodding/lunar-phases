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

// All distinct moon ResourceTokens the game currently holds.  SwapMoonTexture()
// rewrites every tracked token so the live one (which may change on scene
// transitions) always reflects the current phase.  Tokens with use count <= 1
// are reaped - only we hold them; the game has released them.
static std::vector<RED4ext::SharedPtr<RED4ext::ResourceToken<>>> g_moonTokens;
static std::mutex                                                g_moonTokensMutex;

// Gated to Running state: registering OnLoaded callbacks during the pre-Running
// startup burst (~12 k loads) would flood the job queue and trigger a deadlock assertion.
static std::atomic<bool>    s_runningActive{false};
static std::atomic<int32_t> s_preRunningLoadCount{0}; // resets each Running enter

// Canonical CResource handle for each phase; captured before any swap to prevent
// premature ref-count drops when a moon token aliases g_phaseTokens[i].
static RED4ext::Handle<RED4ext::CResource> g_phaseResources[kPhaseCount];

// Remove tokens the game has released (use count <= 1; caller must hold g_moonTokensMutex).
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

    const bool running = s_runningActive.load(std::memory_order_relaxed);
    if (!running)
        s_preRunningLoadCount.fetch_add(1, std::memory_order_relaxed);

    // Probe for AtmosphereAreaSettings; gated to Running state (see s_runningActive).
    if (aOut.instance && running)
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
    // g_phaseResources is kept across sessions; the handles remain valid as long as tokens are alive.
    if (s_sdk) s_sdk->logger->Info(s_handle, "[Hook] Moon tokens reset - will re-capture on next load.");
}

bool IsMoonTokenCaptured()
{
    std::lock_guard<std::mutex> guard(g_moonTokensMutex);
    return !g_moonTokens.empty();
}

void SetRunningState(bool aRunning)
{
    s_runningActive.store(aRunning, std::memory_order_relaxed);
    if (!s_sdk) return;
    if (aRunning)
    {
        const int32_t count = s_preRunningLoadCount.exchange(0, std::memory_order_relaxed);
        s_sdk->logger->InfoF(s_handle, "[Hook] Running active (%d pre-Running loads).", count);
    }
    else
    {
        s_sdk->logger->Info(s_handle, "[Hook] Shutdown.");
    }
}

void SwapMoonTexture(int32_t phase)
{
    // Eagerly capture g_phaseResources for every phase that is already loaded,
    // before any swap loop overwrites a moon token's resource field.
    //
    // Root cause guarded against: the ResourceLoader cache may return the exact same
    // ResourceToken object for both g_phaseTokens[i] and the captured moon token
    // (they share one token instance because the hook redirected moon.xbm to
    // moon_phase_i.xbm and the loader hit its cache).  Overwriting that token's
    // resource field inside the swap loop therefore also changes
    // g_phaseTokens[i]->resource - dropping the Handle<CResource> ref count for the
    // original phase-i texture to zero and freeing it while ArchiveXL (or the sky
    // renderer) may still hold a raw pointer to it, leading to a dangling-pointer
    // ACCESS_VIOLATION crash (typically surfacing ~30 min later when a GC pass or
    // scene transition walks the freed address).
    //
    // Reading g_phaseTokens[i]->resource NOW, before any swap, captures the
    // correct original CResource into g_phaseResources[i].  Subsequent swaps that
    // overwrite the token's resource field no longer drop the ref count to zero
    // because g_phaseResources[i] always holds one strong reference.
    for (int32_t i = 0; i < kPhaseCount; ++i)
    {
        if (!g_phaseResources[i].instance && g_phaseTokens[i].instance && g_phaseTokens[i].instance->IsLoaded())
            g_phaseResources[i] = g_phaseTokens[i].instance->resource;
    }

    // Fallback: phase texture was not yet loaded when the capture loop ran.
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

    // Rewrite every tracked moon token's resource to the requested phase.
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

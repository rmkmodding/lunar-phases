#include <RED4ext/RED4ext.hpp>
#include <RED4ext/Memory/SharedPtr.hpp>
#include <RED4ext/ResourceLoader.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include "Hooks.hpp"
#include "LuminosityPatch.hpp"
#include "LunarPhases.hpp"

namespace LunarPhases
{

// ─── Global definitions ──────────────────────────────────────────────────────

RED4ext::ResourcePath                        g_moonPath;
RED4ext::SharedPtr<RED4ext::ResourceToken<>> g_phaseTokens[kPhaseCount];
std::atomic<int32_t>                         g_currentPhase{0};
static const RED4ext::v1::Sdk*               s_sdk    = nullptr;
static RED4ext::v1::PluginHandle             s_handle;

// ─── Lifetime ────────────────────────────────────────────────────────────────

void Init(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle)
{
    s_sdk    = aSdk;
    s_handle = aHandle;
    LuminosityPatch::Init(aSdk, aHandle);
}

void PreloadPhaseTextures()
{
    auto* loader = RED4ext::ResourceLoader::Get();
    if (!loader)
        return;

    for (int32_t i = 0; i < kPhaseCount; ++i)
    {
        g_phaseTokens[i] = loader->LoadAsync(RED4ext::ResourcePath(kPhasePaths[i]));
    }
}

void PreloadPhaseTexture(int32_t phase)
{
    if (phase < 0 || phase >= kPhaseCount)
        return;
    auto* loader = RED4ext::ResourceLoader::Get();
    if (!loader)
        return;
    if (!g_phaseTokens[phase].instance)
        g_phaseTokens[phase] = loader->LoadAsync(RED4ext::ResourcePath(kPhasePaths[phase]));
}

void ReleasePhaseTextures()
{
    for (int32_t i = 0; i < kPhaseCount; ++i)
    {
        g_phaseTokens[i].Reset();
    }
}

// ─── Native class ────────────────────────────────────────────────────────────

struct LunarPhasesNative : RED4ext::IScriptable
{
    RED4ext::CClass* GetNativeType();
};

RED4ext::TTypedClass<LunarPhasesNative> s_lunarPhasesClass("LunarPhases");

RED4ext::CClass* LunarPhasesNative::GetNativeType()
{
    return &s_lunarPhasesClass;
}

// ─── Scripting ABI wrappers ──────────────────────────────────────────────────

static void SetPhase(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    int32_t phase = 0;
    RED4ext::GetParameter(aFrame, &phase);
    aFrame->code++;

    if (phase < 0)
        phase = 0;
    if (phase >= kPhaseCount)
        phase = kPhaseCount - 1;
    g_currentPhase.store(phase, std::memory_order_relaxed);
    if (s_sdk)
        s_sdk->logger->InfoF(s_handle, "[LunarPhases] SetPhase(%d) called", phase);

    SwapMoonTexture(phase);
    LuminosityPatch::ApplyPhase(phase);
}

static void GetPhase(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, int32_t* aOut, int64_t)
{
    aFrame->code++;
    if (aOut)
        *aOut = g_currentPhase.load(std::memory_order_relaxed);
}

static void SetMoonColor(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    float r = 1.0f, g = 1.0f, b = 1.0f;
    RED4ext::GetParameter(aFrame, &r);
    RED4ext::GetParameter(aFrame, &g);
    RED4ext::GetParameter(aFrame, &b);
    aFrame->code++;
    LuminosityPatch::SetColorTint(r, g, b);
}

static void SetStarDimming(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    bool enabled = false;
    RED4ext::GetParameter(aFrame, &enabled);
    aFrame->code++;
    LuminosityPatch::SetStarDimming(enabled);
}
static void SetMoonLightScaling(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    bool enabled = true;
    RED4ext::GetParameter(aFrame, &enabled);
    aFrame->code++;
    LuminosityPatch::SetMoonLightScaling(enabled);
}

static void SetStarBrightness(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, void*, int64_t)
{
    float multiplier = 1.0f;
    RED4ext::GetParameter(aFrame, &multiplier);
    aFrame->code++;
    LuminosityPatch::SetStarBrightness(multiplier);
}
// ─── RTTI registration ───────────────────────────────────────────────────────

void RegisterTypes()
{
    RED4ext::CNamePool::Add("LunarPhases");
    s_lunarPhasesClass.flags = {.isNative = true};
    RED4ext::CRTTISystem::Get()->RegisterType(&s_lunarPhasesClass);
}

void PostRegisterTypes()
{
    auto* rtti = RED4ext::CRTTISystem::Get();
    s_lunarPhasesClass.parent = rtti->GetClass("IScriptable");

    RED4ext::CBaseFunction::Flags flags = {.isNative = true};

    auto* setFn = RED4ext::CClassFunction::Create(&s_lunarPhasesClass, "SetPhase", "SetPhase", &SetPhase);
    setFn->flags = flags;
    setFn->AddParam("Int32", "phase");
    s_lunarPhasesClass.RegisterFunction(setFn);

    auto* getFn = RED4ext::CClassFunction::Create(&s_lunarPhasesClass, "GetPhase", "GetPhase", &GetPhase);
    getFn->flags = flags;
    getFn->SetReturnType("Int32");
    s_lunarPhasesClass.RegisterFunction(getFn);

    auto* colorFn = RED4ext::CClassFunction::Create(&s_lunarPhasesClass, "SetMoonColor", "SetMoonColor", &SetMoonColor);
    colorFn->flags = flags;
    colorFn->AddParam("Float", "r");
    colorFn->AddParam("Float", "g");
    colorFn->AddParam("Float", "b");
    s_lunarPhasesClass.RegisterFunction(colorFn);

    auto* starFn = RED4ext::CClassFunction::Create(&s_lunarPhasesClass, "SetStarDimming", "SetStarDimming", &SetStarDimming);
    starFn->flags = flags;
    starFn->AddParam("Bool", "enabled");
    s_lunarPhasesClass.RegisterFunction(starFn);

    auto* lightScaleFn = RED4ext::CClassFunction::Create(&s_lunarPhasesClass, "SetMoonLightScaling", "SetMoonLightScaling", &SetMoonLightScaling);
    lightScaleFn->flags = flags;
    lightScaleFn->AddParam("Bool", "enabled");
    s_lunarPhasesClass.RegisterFunction(lightScaleFn);

    auto* starBrightFn = RED4ext::CClassFunction::Create(&s_lunarPhasesClass, "SetStarBrightness", "SetStarBrightness", &SetStarBrightness);
    starBrightFn->flags = flags;
    starBrightFn->AddParam("Float", "multiplier");
    s_lunarPhasesClass.RegisterFunction(starBrightFn);
}

} // namespace LunarPhases

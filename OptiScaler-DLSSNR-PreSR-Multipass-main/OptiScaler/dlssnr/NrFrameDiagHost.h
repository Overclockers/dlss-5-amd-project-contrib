#pragma once
// Host-side NrFrameDiag entry points that read OptiScaler Config/State.
// Standalone tests include only NrFrameDiag.h (core) via the recording headers.
#include "NrFrameDiag.h"
#include "NrGameWaitDiag.h"
#include <Config.h>
#include <State.h>

namespace DlssNr::FrameDiag
{
inline bool Enabled()
{
    auto* cfg = Config::Instance();
    return cfg && (cfg->NrFrameDiag.value_or_default() || cfg->NrFlickerDiag.value_or_default());
}

inline std::atomic<uint64_t>& PresentSerialAtom()
{
    static std::atomic<uint64_t> serial {0};
    return serial;
}

inline bool GameWaitEnabled()
{
    auto* cfg = Config::Instance();
    return cfg && (cfg->NrFlickerDiag.value_or_default() ||
                   (cfg->NrFrameDiag.value_or_default() && cfg->NrGameWaitDiag.value_or_default()));
}

inline void NotePresent(void* swapchain) noexcept
{
    GameWait::PresentBegin(GameWaitEnabled());
    AutoSpinPresentRing().OnPresent(swapchain);
    if (!Enabled()) return;
    PresentSerialAtom().fetch_add(1, std::memory_order_relaxed);
}

inline void Begin(Counters& c, const char* backend, uint64_t frameId, unsigned w, unsigned h,
                  const char* version, const char* mode = "inline") noexcept
{
    if (!Enabled()) { c = {}; return; }
    c = {};
    c.active = true;
    c.backend = backend ? backend : "nr";
    c.mode = mode && mode[0] ? mode : "inline";
    c.nrFrameId = frameId;
    c.width = w;
    c.height = h;
    c.qpcRecordBegin = Qpc();
    c.presentSerial = PresentSerialAtom().load(std::memory_order_relaxed);
    try { c.swapchain = State::Instance().currentSwapchain; } catch (...) { c.swapchain = nullptr; }
    if (auto* cfg = Config::Instance()) {
        c.fg = cfg->IsFGEnabled() ? 1 : 0;
        c.aco = cfg->MochizukiAcoMode.value_or_default() ? 1u : 0u;
    }
    if (version) std::snprintf(c.version, sizeof c.version, "%s", version);
    GameWait::Snapshot gw;
    if (GameWait::Last(gw)) {
        c.gw = true; c.gwPresent = gw.present; c.gwWaits = gw.waits; c.gwEcls = gw.ecls;
        c.gwPresentMs = gw.presentMs; c.gwPresentWallMs = gw.presentWallMs; c.gwBeginQpc = gw.beginQpc; c.gwEndQpc = gw.endQpc; c.gwToEclMs = gw.toEclMs;
        c.gwToWaitMs = gw.toWaitMs; c.gwWaitMs = gw.waitMs;
    }
}

inline void EmitBypass(const char* backend, uint64_t frameId, unsigned slots, unsigned w, unsigned h,
                       const char* version) noexcept
{
    if (!Enabled()) return;
    Counters c {};
    Begin(c, backend, frameId, w, h, version);
    if (!c.active) return;
    c.mode = "bypass";
    c.outcome = "bypass";
    c.activeSlots = slots;
    c.qpcOutputDone = Qpc();
    Emit(c);
}
} // namespace DlssNr::FrameDiag

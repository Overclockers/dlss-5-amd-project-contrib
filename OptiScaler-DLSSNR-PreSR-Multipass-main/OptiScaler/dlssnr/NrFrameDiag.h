#pragma once
// Default-off per-frame NR diagnostics for PresentMon correlation.
// Host QPC markers only — never WaitForGpu / spin GetCompletedValue for measurement.
// Device-calibrated GPU timestamps remain scratch timeline tooling until promoted.
#include "DiagnosticLog.h"
#include <Config.h>
#include <State.h>
#include <Util.h>
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace DlssNr::FrameDiag
{
inline bool Enabled()
{
    auto* cfg = Config::Instance();
    return cfg && cfg->NrFrameDiag.value_or_default();
}

inline int64_t Qpc()
{
    LARGE_INTEGER v {};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

inline int64_t QpcFrequency()
{
    static const int64_t freq = [] {
        LARGE_INTEGER v {};
        QueryPerformanceFrequency(&v);
        return v.QuadPart ? v.QuadPart : 1;
    }();
    return freq;
}

inline double MsBetween(int64_t begin, int64_t end)
{
    if (!begin || !end || end < begin) return 0.0;
    return 1000.0 * double(end - begin) / double(QpcFrequency());
}

// Filled on InlineRecording::Resources when diag is on; flushed on reclaim / bypass.
struct Counters
{
    bool active = false;
    const char* backend = "nr";
    const char* mode = "inline"; // inline | split | bypass
    const char* outcome = "pending"; // apply|timeout|abort|bypass|reject|fail
    uint64_t nrFrameId = 0;
    uint64_t listId = 0;
    uint64_t generation = 0;
    uint64_t submitSerial = 0;
    uint64_t presentSerial = 0;
    void* swapchain = nullptr;
    unsigned width = 0, height = 0;
    unsigned aco = 0;
    int fg = 0;
    char version[48] {};

    int64_t qpcRecordBegin = 0;
    int64_t qpcAdmit = 0;
    int64_t qpcCaptureSeen = 0;
    int64_t qpcEnqueueWaitBegin = 0;
    int64_t qpcEnqueueWaitEnd = 0;
    int64_t qpcVkEnqueue = 0;      // host observe at EnqueueHip/Between
    int64_t qpcSideReady = 0;      // host observe after NetworkQueued
    int64_t qpcGameWaitExit = 0;   // host observe on reclaim (Complete)
    int64_t qpcOutputDone = 0;

    unsigned capturePolls = 0;
    unsigned captureBackoffUsFinal = 0;
    unsigned waitEnqueueOrderMs = 0;
    unsigned lockWaitMs = 0;
    unsigned recordCpuUs = 0;
    unsigned submitCpuUs = 0;
    unsigned activeSlots = 0;
    unsigned spinCount = 0;
    bool historyReset = false;
};

inline std::atomic<uint64_t>& PresentSerialAtom()
{
    static std::atomic<uint64_t> serial {0};
    return serial;
}

inline void NotePresent(void* /*swapchain*/) noexcept
{
    if (!Enabled()) return;
    PresentSerialAtom().fetch_add(1, std::memory_order_relaxed);
}

inline void Begin(Counters& c, const char* backend, uint64_t frameId, unsigned w, unsigned h,
                  const char* version) noexcept
{
    if (!Enabled()) { c = {}; return; }
    c = {};
    c.active = true;
    c.backend = backend ? backend : "nr";
    c.mode = "inline";
    c.nrFrameId = frameId;
    c.width = w;
    c.height = h;
    c.qpcRecordBegin = Qpc();
    c.presentSerial = PresentSerialAtom().load(std::memory_order_relaxed);
    try { c.swapchain = State::Instance().currentSwapchain; } catch (...) { c.swapchain = nullptr; }
    if (auto* cfg = Config::Instance()) {
        c.fg = cfg->FGEnabled.value_or_default() ? 1 : 0;
        c.aco = cfg->MochizukiAcoMode.value_or_default() ? 1u : 0u;
    }
    if (version) std::snprintf(c.version, sizeof c.version, "%s", version);
}

inline void Emit(const Counters& c) noexcept
{
    if (!c.active) return;
    try {
        char line[1280];
        const int n = std::snprintf(line, sizeof line,
            "NRFRAME schema=1 backend=%s mode=%s outcome=%s "
            "qpc_freq=%lld qpc_record=%lld qpc_admit=%lld qpc_capture_seen=%lld "
            "qpc_enqueue_wait_begin=%lld qpc_enqueue_wait_end=%lld qpc_vk_enqueue=%lld "
            "qpc_side_ready=%lld qpc_game_wait_exit=%lld qpc_output_done=%lld "
            "nr_frame=%llu list=%llu generation=%llu submit_serial=%llu present_serial=%llu swapchain=%p "
            "capture_polls=%u capture_backoff_us=%u wait_enqueue_order_ms=%u lock_wait_ms=%u "
            "record_cpu_us=%u submit_cpu_us=%u active_slots=%u spin=%u history_reset=%u "
            "res=%ux%u aco=%u fg=%d version=%s "
            "cpu_wait_enqueue_ms=%.3f cpu_record_ms=%.3f host_A_capture_to_vk_ms=%.3f host_B_side_ready_ms=%.3f",
            c.backend, c.mode, c.outcome,
            static_cast<long long>(QpcFrequency()),
            static_cast<long long>(c.qpcRecordBegin),
            static_cast<long long>(c.qpcAdmit),
            static_cast<long long>(c.qpcCaptureSeen),
            static_cast<long long>(c.qpcEnqueueWaitBegin),
            static_cast<long long>(c.qpcEnqueueWaitEnd),
            static_cast<long long>(c.qpcVkEnqueue),
            static_cast<long long>(c.qpcSideReady),
            static_cast<long long>(c.qpcGameWaitExit),
            static_cast<long long>(c.qpcOutputDone),
            static_cast<unsigned long long>(c.nrFrameId),
            static_cast<unsigned long long>(c.listId),
            static_cast<unsigned long long>(c.generation),
            static_cast<unsigned long long>(c.submitSerial),
            static_cast<unsigned long long>(c.presentSerial),
            c.swapchain,
            c.capturePolls, c.captureBackoffUsFinal, c.waitEnqueueOrderMs, c.lockWaitMs,
            c.recordCpuUs, c.submitCpuUs, c.activeSlots, c.spinCount, c.historyReset ? 1u : 0u,
            c.width, c.height, c.aco, c.fg, c.version[0] ? c.version : "?",
            double(c.waitEnqueueOrderMs),
            c.recordCpuUs / 1000.0,
            MsBetween(c.qpcCaptureSeen, c.qpcVkEnqueue),
            MsBetween(c.qpcVkEnqueue, c.qpcSideReady));
        if (n <= 0) return;
        const auto path = Util::DllPath().parent_path() / L"NrFrameDiag.log";
        Diagnostics::Append(path, std::string_view(line, size_t(n)), 8ull * 1024 * 1024);
    } catch (...) {}
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

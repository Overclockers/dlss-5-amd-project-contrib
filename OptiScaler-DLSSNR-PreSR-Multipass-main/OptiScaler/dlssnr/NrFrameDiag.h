#pragma once
// Default-off per-frame NR diagnostics for PresentMon correlation.
// Host QPC markers only — never WaitForGpu / spin GetCompletedValue for measurement.
// Device-calibrated GPU timestamps remain scratch timeline tooling until promoted.
// Core is dependency-free so standalone tests can include recording headers;
// Config/State-dependent entry points (Enabled/Begin/NotePresent/EmitBypass) live in NrFrameDiagHost.h.
#include "DiagnosticLog.h"
#include <Windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <memory>
#include <new>
#include <vector>

namespace DlssNr::FrameDiag
{
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
// Directory of the module that contains this code (OptiScaler DLL in product, test EXE standalone).
inline std::filesystem::path ModuleDirectory()
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module))
        module = nullptr;
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    return n ? std::filesystem::path(path).parent_path() : std::filesystem::path {};
}

// GPU timestamps on the game queue (NrFrameDiag only). Inline: capture begin/end, spin exit, select done.
// Split: capture begin/end, post-split resume, outputs done. Resolved in the same list, read after the frame's
// fence completed (reclaim / deferred split emit); converted to QPC with the queue's GetClockCalibration.
enum GpuStamp : unsigned { kGpuCapBegin = 0, kGpuCapEnd, kGpuResume, kGpuDone, kGpuStampCount };
struct GpuStamps
{
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback = nullptr;
    UINT count = 0;
    GpuStamps() = default;
    GpuStamps(const GpuStamps&) = delete;
    GpuStamps& operator=(const GpuStamps&) = delete;
    ~GpuStamps() { if (heap) heap->Release(); if (readback) readback->Release(); }
    bool Init(ID3D12Device* device, UINT n) noexcept
    {
        if (heap || !device || !n) return heap != nullptr;
        D3D12_QUERY_HEAP_DESC qd {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = n;
        if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap)))) { heap = nullptr; return false; }
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 8ull * n; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback)))) {
            heap->Release(); heap = nullptr; readback = nullptr; return false;
        }
        count = n; return true;
    }
    ID3D12Device* device = nullptr; // identity only, for the pool
    // Heap + readback creation is ~0.5 ms and runs under RecordingMutex: recycle retired sets per device.
    static std::shared_ptr<GpuStamps> Acquire(ID3D12Device* d, UINT n) noexcept
    {
        struct Pool { SRWLOCK lock = SRWLOCK_INIT; std::vector<GpuStamps*> free; };
        static Pool* pool = new Pool; // leaked on purpose: no teardown-order hazard
        GpuStamps* g = nullptr;
        AcquireSRWLockExclusive(&pool->lock);
        for (size_t i = 0; i < pool->free.size(); ++i)
            if (pool->free[i]->device == d && pool->free[i]->count == n) { g = pool->free[i]; pool->free.erase(pool->free.begin() + i); break; }
        ReleaseSRWLockExclusive(&pool->lock);
        try {
            if (!g) { g = new GpuStamps; if (!g->Init(d, n)) { delete g; return {}; } g->device = d; }
            return std::shared_ptr<GpuStamps>(g, [](GpuStamps* x) {
                AcquireSRWLockExclusive(&pool->lock);
                const bool keep = pool->free.size() < 32;
                if (keep) { try { pool->free.push_back(x); } catch (...) { ReleaseSRWLockExclusive(&pool->lock); delete x; return; } }
                ReleaseSRWLockExclusive(&pool->lock);
                if (!keep) delete x;
            });
        } catch (...) { delete g; return {}; }
    }
    void Stamp(ID3D12GraphicsCommandList* list, UINT i) const noexcept
    { if (heap && list && i < count) list->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, i); }
    void Resolve(ID3D12GraphicsCommandList* list) const noexcept
    { if (heap && list) list->ResolveQueryData(heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, count, readback, 0); }
    // GetClockCalibration is a kernel round trip (~ms under load) and this runs under RecordingMutex at reclaim:
    // refresh each queue's calibration at most every 500 ms (clock drift over that span is negligible here).
    static bool Calibration(ID3D12CommandQueue* queue, UINT64& freq, UINT64& gpu, UINT64& cpu) noexcept
    {
        struct Entry { ID3D12CommandQueue* queue; UINT64 freq, gpu, cpu; int64_t at; };
        static Entry cache[4] {};
        static SRWLOCK lock = SRWLOCK_INIT;
        const int64_t now = Qpc();
        AcquireSRWLockExclusive(&lock);
        Entry* slot = &cache[0];
        for (auto& e : cache) {
            if (e.queue == queue) { slot = &e; break; }
            if (e.at < slot->at) slot = &e;
        }
        bool ok = slot->queue == queue && slot->freq && now - slot->at < QpcFrequency() / 2;
        if (!ok) {
            Entry e {queue, 0, 0, 0, now};
            ok = SUCCEEDED(queue->GetTimestampFrequency(&e.freq)) && e.freq && SUCCEEDED(queue->GetClockCalibration(&e.gpu, &e.cpu));
            if (ok) *slot = e;
        }
        if (ok) { freq = slot->freq; gpu = slot->gpu; cpu = slot->cpu; }
        ReleaseSRWLockExclusive(&lock);
        return ok;
    }
    // CPU read of an already-completed resolve; no GPU wait. 0 = unavailable.
    void Read(int64_t* qpc, UINT n, ID3D12CommandQueue* queue) const noexcept
    {
        for (UINT i = 0; i < n; ++i) qpc[i] = 0;
        if (!heap || !readback || !queue || n > count) return;
        UINT64 freq = 0, gpu = 0, cpu = 0;
        if (!Calibration(queue, freq, gpu, cpu)) return;
        void* p = nullptr; D3D12_RANGE range {0, size_t(8) * n};
        if (FAILED(readback->Map(0, &range, &p)) || !p) return;
        const auto* t = static_cast<const UINT64*>(p);
        for (UINT i = 0; i < n; ++i)
            if (t[i]) qpc[i] = int64_t(cpu) + int64_t(double(int64_t(t[i] - gpu)) * double(QpcFrequency()) / double(freq));
        D3D12_RANGE none {0, 0}; readback->Unmap(0, &none);
    }
};

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
    const char* captureBackoffMode = "-"; // exp | fixed100 | event (inline only)
    bool inlineReuse = false; // recording came from the NrInlineReuse pool
    unsigned spinPollDelay = 0, spinSlices = 0, watchSlices = 0, watchStartDelay = 0; // inline spin experiments (0 = default)
    const char* handoff = "-"; // inline: cpu (worker discovers capture) | gpu (NrGpuHandoff)
    unsigned waitEnqueueOrderMs = 0;
    unsigned lockWaitMs = 0;
    unsigned recordCpuUs = 0;
    unsigned submitCpuUs = 0;
    unsigned activeSlots = 0;
    unsigned spinCount = 0;
    bool historyReset = false;
    int64_t gpuQpc[kGpuStampCount] {}; // GpuStamp points, QPC; 0 = unavailable
    int64_t gpuSideVkDone = 0;         // inline: side queue resumes after the network (Vulkan done as D3D12 sees it)
    float coreGpuMs = 0;               // Mochizuki Vulkan core GPU ms (last sample; excludes bridge copies)
    // NrGameWaitDiag: last closed game-thread Present interval (gw = 0 when off).
    bool gw = false;
    unsigned long long gwPresent = 0; unsigned gwWaits = 0, gwEcls = 0;
    double gwPresentMs = 0, gwPresentCpuMs = 0, gwToEclMs = -1, gwToWaitMs = -1, gwWaitMs = 0;
};

inline void Emit(const Counters& c) noexcept
{
    if (!c.active) return;
    try {
        char line[2048];
        const int n = std::snprintf(line, sizeof line,
            "NRFRAME schema=1 backend=%s mode=%s outcome=%s "
            "qpc_freq=%lld qpc_record=%lld qpc_admit=%lld qpc_capture_seen=%lld "
            "qpc_enqueue_wait_begin=%lld qpc_enqueue_wait_end=%lld qpc_vk_enqueue=%lld "
            "qpc_side_ready=%lld qpc_game_wait_exit=%lld qpc_output_done=%lld "
            "nr_frame=%llu list=%llu generation=%llu submit_serial=%llu present_serial=%llu swapchain=%p "
            "capture_polls=%u capture_backoff=%s inline_reuse=%u capture_backoff_us=%u wait_enqueue_order_ms=%u lock_wait_ms=%u "
            "record_cpu_us=%u submit_cpu_us=%u active_slots=%u spin=%u history_reset=%u "
            "res=%ux%u aco=%u fg=%d version=%s "
            "cpu_wait_enqueue_ms=%.3f cpu_record_ms=%.3f host_A_capture_to_vk_ms=%.3f host_B_side_ready_ms=%.3f "
            "gpu_capture_ms=%.3f gpu_capture_to_resume_ms=%.3f gpu_core_ms=%.3f gpu_handoff_ms=%.3f "
            "gpu_capture_to_vkdone_ms=%.3f gpu_vkdone_to_resume_ms=%.3f gpu_writeback_ms=%.3f handoff=%s spin_delay=%u spin_slices=%u watch_slices=%u watch_start=%u",
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
            c.capturePolls, c.captureBackoffMode ? c.captureBackoffMode : "-", c.inlineReuse ? 1u : 0u, c.captureBackoffUsFinal, c.waitEnqueueOrderMs, c.lockWaitMs,
            c.recordCpuUs, c.submitCpuUs, c.activeSlots, c.spinCount, c.historyReset ? 1u : 0u,
            c.width, c.height, c.aco, c.fg, c.version[0] ? c.version : "?",
            double(c.waitEnqueueOrderMs),
            c.recordCpuUs / 1000.0,
            MsBetween(c.qpcCaptureSeen, c.qpcVkEnqueue),
            MsBetween(c.qpcVkEnqueue, c.qpcSideReady),
            MsBetween(c.gpuQpc[kGpuCapBegin], c.gpuQpc[kGpuCapEnd]),
            MsBetween(c.gpuQpc[kGpuCapEnd], c.gpuQpc[kGpuResume]),
            double(c.coreGpuMs),
            (c.coreGpuMs > 0 && MsBetween(c.gpuQpc[kGpuCapEnd], c.gpuQpc[kGpuResume]) > 0)
                ? MsBetween(c.gpuQpc[kGpuCapEnd], c.gpuQpc[kGpuResume]) - double(c.coreGpuMs) : 0.0,
            MsBetween(c.gpuQpc[kGpuCapEnd], c.gpuSideVkDone),
            MsBetween(c.gpuSideVkDone, c.gpuQpc[kGpuResume]),
            MsBetween(c.gpuQpc[kGpuResume], c.gpuQpc[kGpuDone]),
            c.handoff ? c.handoff : "-", c.spinPollDelay, c.spinSlices, c.watchSlices, c.watchStartDelay);
        if (n <= 0 || n >= int(sizeof line)) return;
        int len = n;
        if (c.gw) {
            const int m = std::snprintf(line + len, sizeof line - size_t(len),
                " gw_present=%llu gw_present_ms=%.3f gw_present_cpu_ms=%.3f gw_to_ecl_ms=%.3f gw_to_wait_ms=%.3f gw_wait_ms=%.3f gw_waits=%u gw_ecls=%u",
                c.gwPresent, c.gwPresentMs, c.gwPresentCpuMs, c.gwToEclMs, c.gwToWaitMs, c.gwWaitMs, c.gwWaits, c.gwEcls);
            if (m > 0 && len + m < int(sizeof line)) len += m;
        }
        const auto path = ModuleDirectory() / L"NrFrameDiag.log";
        Diagnostics::Append(path, std::string_view(line, size_t(len)), 8ull * 1024 * 1024);
    } catch (...) {}
}

// Read GPU stamps (frame already retired) and write the line on a threadpool thread, off RecordingMutex.
// Owns its stamp sets until done, so a recycled recording cannot overwrite them first.
struct AsyncEmitJob
{
    Counters c;
    std::shared_ptr<GpuStamps> game, side;
    ID3D12CommandQueue* gameQueue = nullptr; ID3D12CommandQueue* sideQueue = nullptr; // AddRef'd
    ~AsyncEmitJob() { if (gameQueue) gameQueue->Release(); if (sideQueue) sideQueue->Release(); }
};
inline void AsyncEmit(Counters c, std::shared_ptr<GpuStamps> game, ID3D12CommandQueue* gameQueue,
                      std::shared_ptr<GpuStamps> side, ID3D12CommandQueue* sideQueue) noexcept
{
    if (!c.active) return;
    AsyncEmitJob* job = nullptr;
    try {
        job = new AsyncEmitJob;
        job->c = c; job->game = std::move(game); job->side = std::move(side);
        job->gameQueue = gameQueue; job->sideQueue = sideQueue;
        if (gameQueue) gameQueue->AddRef();
        if (sideQueue) sideQueue->AddRef();
    } catch (...) { delete job; Emit(c); return; }
    HMODULE self = nullptr; // keep this module loaded until the callback returns
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&AsyncEmit), &self);
    struct Ctx { AsyncEmitJob* job; HMODULE self; };
    auto* ctx = new (std::nothrow) Ctx {job, self};
    const auto run = [](PTP_CALLBACK_INSTANCE instance, void* p) {
        auto* x = static_cast<Ctx*>(p);
        auto* j = x->job;
        if (j->game && j->gameQueue) j->game->Read(j->c.gpuQpc, kGpuStampCount, j->gameQueue);
        if (j->side && j->sideQueue) { int64_t v = 0; j->side->Read(&v, 1, j->sideQueue); j->c.gpuSideVkDone = v; }
        Emit(j->c);
        delete j;
        if (x->self && instance) FreeLibraryWhenCallbackReturns(instance, x->self);
        delete x;
    };
    if (!ctx || !TrySubmitThreadpoolCallback(run, ctx, nullptr)) {
        if (job->game && job->gameQueue) job->game->Read(job->c.gpuQpc, kGpuStampCount, job->gameQueue);
        Emit(job->c); delete job; delete ctx;
        if (self) FreeLibrary(self);
    }
}
} // namespace DlssNr::FrameDiag

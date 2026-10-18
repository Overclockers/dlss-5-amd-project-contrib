#pragma once
#include "LmxxfEvaluateCut.h"
#include "../NrSessionActivity.h"
#include "lmxxf_runtime/LmxxfNrApi.h"
#include "NrInlineRecording.h"
#include "NrInlineAutoSpin.h"
#include "../NrFrameDiag.h"
#include "../NrCoverage.h"
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>
#include <array>
#include <cstdio>
#include <condition_variable>
#include <mutex>
#include <wrl/client.h>
#ifndef LOG_INFO
#define LOG_INFO(...) ((void)0)
#endif
#ifndef LOG_ERROR
#define LOG_ERROR(...) ((void)0)
#endif

namespace DlssNr::Backend::LmxxfRecording
{
struct SessionOwner
{
    struct Failure
    {
        const char* phase = nullptr;
        int32_t result = 0;
        const void* queue = nullptr;
        std::array<char, 256> error {};
    };
    LmxxfNrApi api {};
    void* context = nullptr;
    // NrFrameDiag: backend hook for the runtime's last core GPU ms (Mochizuki); null elsewhere.
    // encodeMs / transferMs: the core's stage pairs (-1 when absent).
    float (*diagCoreGpuMs)(void* context, float* encodeMs, float* transferMs) = nullptr;
    // Split NrFrameDiag lines wait here until their game fence retired, so GPU stamps are readable.
    struct PendingSplitDiag {
        FrameDiag::Counters diag; std::shared_ptr<FrameDiag::GpuStamps> stamps;
        std::shared_ptr<std::atomic<bool>> readable;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue; Microsoft::WRL::ComPtr<ID3D12Fence> fence; UINT64 value = 0;
    };
    std::mutex splitDiagMutex;
    std::deque<PendingSplitDiag> splitDiagPending;
    std::shared_ptr<std::atomic<unsigned>> splitDiagSlots = std::make_shared<std::atomic<unsigned>>(0);
    std::shared_ptr<FrameDiag::GpuStamps> AcquireSplitStamps(ID3D12Device* device) {
        if (splitDiagSlots->load() >= 8) return {}; // skip; retained recipes and pending GPU writes own the budget
        auto stamps = FrameDiag::GpuStamps::Acquire(device, FrameDiag::kGpuStampCount);
        if (!stamps) return {};
        ++*splitDiagSlots;
        try {
            return {stamps.get(), [stamps, slots = splitDiagSlots](FrameDiag::GpuStamps*) mutable {
                stamps.reset(); --*slots;
            }};
        } catch (...) { return {}; } // shared_ptr invokes the supplied deleter if its allocation fails
    }
    void FlushSplitDiag() noexcept {
        try {
            std::lock_guard lock(splitDiagMutex);
            while (!splitDiagPending.empty()) {
                auto& f = splitDiagPending.front();
                const UINT64 done = f.fence ? f.fence->GetCompletedValue() : UINT64_MAX;
                const bool ready = done != UINT64_MAX && done >= f.value;
                if (!ready && done != UINT64_MAX) break;
                if (diagCoreGpuMs) f.diag.coreGpuMs = diagCoreGpuMs(context, &f.diag.encodeGpuMs, &f.diag.transferGpuMs);
                const bool readable = ready && (!f.readable || f.readable->load());
                FrameDiag::AsyncEmit(f.diag, readable ? f.stamps : nullptr, f.queue.Get(), nullptr, nullptr);
                splitDiagPending.pop_front();
            }
        } catch (...) {}
    }
    HMODULE module = nullptr;
    std::atomic<bool> failed { false };
    NrSessionActivity activity;
    std::shared_ptr<Coverage::Counters> coverage;
    // Reserved at Attach, including recordings whose command list is not submitted yet.
    // Default conservatively to one; backends with private recording buffers
    // select their limit. A reservation survives until both queues complete.
    static constexpr size_t kInlineAhead = 4;
    size_t inlineLimit = 1;
    std::vector<std::shared_ptr<InlineRecording::Resources>> inlineInFlight;
    // Started recordings whose completion cannot be proven (m5): kept alive, out of the slot count.
    // Released on device removal; otherwise intentionally leaked at teardown. Inline stops at kInlineAhead.
    std::vector<std::shared_ptr<InlineRecording::Resources>> abandonedInline;
    bool deviceLostLogged = false;
    bool InlineAbandonLimit() const { return abandonedInline.size() >= kInlineAhead; }
    std::deque<void*> inlineEnqueueOrder;
    std::condition_variable_any inlineTurnChanged;
    std::shared_ptr<InlineRecording::Pipelines> inlinePipelines;
    std::vector<std::shared_ptr<InlineRecording::Resources>> inlinePool; // NrInlineReuse only
    uint64_t inlineApplied = 0, inlineTimeouts = 0, inlineAborted = 0, inlineBypassed = 0;
    uint64_t inlineAdmitted = 0, inlineTurnWaits = 0;
    std::atomic<uint64_t> liveRecordings {0};
    InlineAutoSpin::Controller autoSpin {};
    ID3D12CommandQueue* lastInlineGameQueue = nullptr;
    uint64_t lastConsumedPresentSerial = 0;
    double ConsumePresentIntervalMs(uint64_t submitPresentSerial) noexcept {
        auto& ring = FrameDiag::AutoSpinPresentRing();
        FrameDiag::PresentSampleEntry entry {};
        if (submitPresentSerial > 0 && submitPresentSerial + 1 > lastConsumedPresentSerial &&
            ring.TryGet(submitPresentSerial + 1, entry))
        {
            lastConsumedPresentSerial = entry.serial;
            return entry.intervalMs;
        }
        const uint64_t cur = ring.CurrentSerial();
        if (cur > lastConsumedPresentSerial && ring.TryGet(cur, entry))
        {
            lastConsumedPresentSerial = entry.serial;
            return entry.intervalMs;
        }
        return 0.0;
    }
    void ReclaimInlineLocked() {
        void* side = nullptr;
        uint64_t spun = 0;
        for (auto it = inlineInFlight.begin(); it != inlineInFlight.end();) {
            auto& pending = *it;
            // A fresh reservation reports Complete because it has not started.
            if (!pending || !pending->Started()) { ++it; continue; }
            if (!pending->Complete()) {
                if (pending->RetirementUncertain() && !pending->RetryProof() && pending->ProofExhausted()) {
                    abandonedInline.push_back(pending);
                    LOG_ERROR("NR inline: completion of a submitted recording cannot be proven after {} attempts "
                              "(game_signal=0x{:08X} game_proof=0x{:08X} side_proof=0x{:08X}); its D3D12 resources stay "
                              "allocated until device removal or process exit (abandoned={})",
                              InlineRecording::Resources::kProofAttempts, static_cast<unsigned>(pending->GameSignalHr()),
                              static_cast<unsigned>(pending->GameProofHr()), static_cast<unsigned>(pending->SideProofHr()),
                              abandonedInline.size());
                    if (InlineAbandonLimit())
                        LOG_ERROR("NR inline disabled for this session: {} recordings abandoned without completion proof; "
                                  "frames bypass the inline path", abandonedInline.size());
                    activity.Reset(); api.ResetHistory(context);
                    it = inlineInFlight.erase(it);
                    continue;
                }
                if (!pending->Complete()) { ++it; continue; }
            }
            if (pending->DeviceLost() && !deviceLostLogged) {
                deviceLostLogged = true;
                LOG_WARN("NR inline: device removed; releasing inline resources without fence proof");
            }
            const bool succeeded = pending->Succeeded();
            const bool aborted = pending->WasAborted();
            if (coverage) (succeeded ? coverage->inlineApplied : coverage->inlineMissed).fetch_add(1, std::memory_order_relaxed);
            if (!succeeded) {
                // Legacy armed misses reset at reclaim, not before ahead enqueues. This does not certify
                // subsequent history safety; see docs/architecture/nr-capture-proof-deferral.md.
                activity.Reset(); api.ResetHistory(context);
                if (aborted) ++inlineAborted;
                else {
                    ++inlineTimeouts;
                    if (inlineTimeouts <= 5 || inlineTimeouts % 1200 == 0)
                        LOG_WARN("NR inline wait exhausted; original frame selected, history reset (timeouts={})", inlineTimeouts);
                }
            } else {
                ++inlineApplied;
                if (!failed) activity.Succeeded(pending->activityToken);
            }
            if (pending->measureActive && !pending->DeviceLost()) {
                UINT seen = succeeded ? 1u : 0u;
                UINT gate = 1u;
                UINT claimAborted = 0u;
                UINT claimIters = 0u;
                if (pending->GpuArmed()) {
                    pending->ArmedOutcome(seen, gate);
                    pending->ClaimWaitStats(claimAborted, claimIters);
                }
                double gpuWaitMs = -1.0;
                double gpuSpanMs = -1.0;
                pending->HarvestAutoSpinGpuMs(gpuWaitMs, gpuSpanMs);
                InlineAutoSpin::SampleInput sample {};
                sample.epoch = pending->autoSpinEpoch;
                sample.candidate = { static_cast<int>(pending->spinPollDelay),
                                     pending->spinSlices ? static_cast<int>(pending->spinSlices) : 64 };
                sample.presentIntervalMs = ConsumePresentIntervalMs(pending->submitPresentSerial);
                sample.gpuWaitMs = gpuWaitMs;
                sample.gpuFrameSpanMs = gpuSpanMs;
                sample.inlineSubmitted = true;
                sample.captureMissed = !succeeded || (pending->GpuArmed() && (!seen || gate != 1u));
                sample.spinCapped = !succeeded || aborted || (claimAborted != 0u);
                sample.historyReset = !succeeded;
                sample.replayed = pending->submitExecutions != 1u;
                autoSpin.OnSample(sample);
                FrameDiag::AutoSpinPresentRing().SetActive(autoSpin.ShouldMeasure());
            }
            pending->HarvestClaim();
            if (pending->diag.active) {
                pending->diag.qpcGameWaitExit = pending->diag.qpcOutputDone = FrameDiag::Qpc();
                if (diagCoreGpuMs)
                    pending->diag.coreGpuMs = diagCoreGpuMs(context, &pending->diag.encodeGpuMs, &pending->diag.transferGpuMs);
                pending->diag.activeSlots = unsigned(inlineInFlight.size());
                pending->diag.spinCount = aborted ? 0 : pending->LastSpinCount();
                pending->diag.historyReset = !succeeded && !aborted;
                pending->diag.outcome = succeeded ? "apply"
                    : (aborted ? "abort" : "timeout");
                pending->DiagEmitAsync();
                pending->diag = {};
            }
            side = pending->SideQueue();
            spun = aborted ? 0 : pending->LastSpinCount();
            it = inlineInFlight.erase(it);
        }
        if (!abandonedInline.empty() && abandonedInline.front() && abandonedInline.front()->DeviceLost()) {
            LOG_WARN("NR inline: device removed; releasing {} abandoned inline recordings", abandonedInline.size());
            abandonedInline.clear();
        }
        const auto completed = inlineApplied + inlineTimeouts + inlineAborted;
        if (side && (completed <= 3 || completed % 3600 == 0))
            LOG_INFO("NR inline completed={} applied={} timeouts={} aborted={} bypassed={} live_recordings={} in_flight={} side_queue={} last_slice_spins={}",
                     completed, inlineApplied, inlineTimeouts, inlineAborted, inlineBypassed,
                     liveRecordings.load(std::memory_order_relaxed), inlineInFlight.size(), side, spun);
    }
    bool InlineBusy() {
        ReclaimInlineLocked();
        return InlineAbandonLimit() || inlineInFlight.size() >= inlineLimit;
    }
    void InlineBypassed() {
        ++inlineBypassed;
        if (autoSpin.ShouldMeasure()) {
            InlineAutoSpin::SampleInput sample {};
            sample.epoch = autoSpin.CurrentEpoch();
            sample.candidate = autoSpin.CurrentCandidate();
            sample.presentIntervalMs = ConsumePresentIntervalMs(0);
            sample.inlineSubmitted = false;
            autoSpin.OnSample(sample);
            FrameDiag::AutoSpinPresentRing().SetActive(autoSpin.ShouldMeasure());
        }
        if (inlineBypassed > 5 && inlineBypassed % 1200 != 0) return;
        size_t unstarted = 0;
        for (const auto& pending : inlineInFlight)
            if (pending && !pending->Started()) ++unstarted;
        LOG_WARN("NR inline bypass: cap {} in_flight {} unstarted {} bypassed {}",
                 inlineLimit, inlineInFlight.size(), unstarted, inlineBypassed);
    }
    bool AdmitInline(const std::shared_ptr<InlineRecording::Resources>& resources) {
        ReclaimInlineLocked();
        if (!resources) return false;
        if (std::find(inlineInFlight.begin(), inlineInFlight.end(), resources) != inlineInFlight.end()) return true;
        if (InlineAbandonLimit() || inlineInFlight.size() >= inlineLimit) return false;
        inlineInFlight.push_back(resources);
        return true;
    }
    void ForgetUnstarted(const std::shared_ptr<InlineRecording::Resources>& resources) {
        if (!resources || resources->Started()) return;
        auto it = std::find(inlineInFlight.begin(), inlineInFlight.end(), resources);
        if (it != inlineInFlight.end()) inlineInFlight.erase(it);
    }
    void NoteInlineEnqueue(void* job) { if (job) inlineEnqueueOrder.push_back(job); }
    bool InlineEnqueueTurn(void* job) const {
        return !inlineEnqueueOrder.empty() && inlineEnqueueOrder.front() == job;
    }
    bool InlineEnqueueQueued(void* job) const {
        return std::find(inlineEnqueueOrder.begin(), inlineEnqueueOrder.end(), job) != inlineEnqueueOrder.end();
    }
    void PopInlineEnqueue(void* job) {
        auto it = std::find(inlineEnqueueOrder.begin(), inlineEnqueueOrder.end(), job);
        if (it != inlineEnqueueOrder.end()) {
            inlineEnqueueOrder.erase(it);
            inlineTurnChanged.notify_all();
        }
    }
    // Accessed under RecordingMutex. Preserve the first error on the callback
    // thread: GetLastError is thread-local and cannot be queried by the menu.
    Failure failure {};
    void (*onFailure)(const Failure&) noexcept = nullptr;
    void RecordFailure(const char* phase, int32_t result, const void* queue,
                       const char* error = nullptr) noexcept
    {
        activity.Reset();
        if (!failure.phase)
        {
            failure.phase = phase; failure.result = result; failure.queue = queue;
            if (error) std::snprintf(failure.error.data(), failure.error.size(), "%s", error);
            else if (api.GetLastError) api.GetLastError(failure.error.data(), uint32_t(failure.error.size()));
            failure.error.back() = '\0';
            if (onFailure) onFailure(failure);
        }
    }
    ~SessionOwner()
    {
        // GPU work of abandoned recordings may still run: never release it here.
        if (!abandonedInline.empty() && !(abandonedInline.front() && abandonedInline.front()->DeviceLost()))
            for (auto& r : abandonedInline) new std::shared_ptr<InlineRecording::Resources>(std::move(r));
        if (context && api.Destroy(context) != LMXXF_NR_OK)
        {
            OutputDebugStringA("lmxxf: retaining session/module with outstanding recording jobs\n");
            return;
        }
        if (module) FreeLibrary(module);
    }
    static std::shared_ptr<SessionOwner> Create(const LmxxfNrApi& api, void* context, size_t inlineLimit = 1)
    {
        std::shared_ptr<SessionOwner> owner;
        try { owner = std::make_shared<SessionOwner>(); }
        catch (...) { return {}; }
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(api.Destroy), &owner->module)) return {};
        owner->api = api; owner->context = context;
        owner->inlineLimit = std::clamp(inlineLimit, size_t(1), size_t(5));
        return owner;
    }
};

struct Lease;
inline std::vector<std::shared_ptr<Lease>>& Registry()
{
    // Outstanding live recordings are retained at process exit; static teardown
    // must not destroy GPU resources while game lists still own executable code.
    static auto* leases = new std::vector<std::shared_ptr<Lease>>;
    return *leases;
}
inline bool CollectLocked();
inline void ScheduleCollectionLocked() noexcept;

struct Lease final : Submission::RecordingObserver, std::enable_shared_from_this<Lease>
{
    std::shared_ptr<SessionOwner> owner;
    void* job;
    Submission::RecordingIdentity identity;
    bool ready = false, invalidated = false, begun = false;
    std::shared_ptr<InlineRecording::Resources> inlineResources;
    bool inlineEnqueueFinished = false, executionReported = false;
    bool runtimeRetirementUncertain = false;
    uint64_t inlineResetSerial = 0, activeExecutionSerial = 0, inlineBypassSerial = 0;
    Submission::RecordingExecution inlineExecution {};
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> inlineQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> inlineFence;
    uint64_t traceId = 0;
    void (*trace)(uint64_t, const char*, const void*, int32_t) noexcept = nullptr;
    unsigned traceExecutions = 0;
    bool tracing = false;
    void Trace(const char* phase, const void* object, int32_t result = 0) const noexcept
    { if (tracing && trace && traceId) trace(traceId, phase, object, result); }
    bool neural = false, enqueued = false;
    uint64_t activityToken = 0;
    // Split-path diag lives on the lease; inline uses InlineRecording::Resources::diag.
    FrameDiag::Counters diag;
    std::shared_ptr<FrameDiag::GpuStamps> splitStamps; // NrFrameDiag split GPU stamps
    std::shared_ptr<std::atomic<bool>> splitDiagReadable; // lazy: diagnostics off adds no per-lease allocation
    bool splitExecuted = false;
    ID3D12CommandQueue* splitDiagQueue = nullptr; ID3D12Fence* splitDiagFence = nullptr; UINT64 splitDiagValue = 0; // borrowed in FinishExecution
    void EmitSplitDiag(const char* outcome) noexcept
    {
        if (inlineResources || !diag.active) return;
        diag.qpcGameWaitExit = diag.qpcOutputDone = FrameDiag::Qpc();
        diag.outcome = outcome ? outcome : "fail";
        if (splitStamps && splitDiagFence && splitDiagQueue) {
            try {
                if (!splitDiagReadable) splitDiagReadable = std::make_shared<std::atomic<bool>>(true);
                SessionOwner::PendingSplitDiag p; p.diag = diag; p.stamps = splitStamps;
                p.readable = splitDiagReadable;
                p.queue = splitDiagQueue; p.fence = splitDiagFence; p.value = splitDiagValue;
                { std::lock_guard lock(owner->splitDiagMutex); owner->splitDiagPending.push_back(std::move(p)); }
                owner->FlushSplitDiag();
            } catch (...) { FrameDiag::Emit(diag); }
        } else FrameDiag::Emit(diag);
        // A retained closed list can replay its EndQuery/Resolve. Pin until the lease collects.
        splitDiagFence = nullptr; splitDiagQueue = nullptr;
        diag = {};
    }
    Lease(std::shared_ptr<SessionOwner> session, void* token, Submission::RecordingIdentity id)
        : owner(std::move(session)), job(token), identity(id), activityToken(owner->activity.Token()) { ++owner->liveRecordings; }
    ~Lease() override { --owner->liveRecordings; }
    HRESULT BeforeExecute(const Submission::RecordingExecution& e) noexcept override
    {
        if (invalidated || !(e.identity == identity)) {
            owner->RecordFailure("identity", E_UNEXPECTED, e.queue, "invalidated or mismatched recording");
            return E_UNEXPECTED;
        }
        // Never reset flags still used by an earlier execution of this recording.
        if (inlineResources && (begun || (inlineResources->Started() && !inlineResources->Complete()))) return E_PENDING;
        if (!inlineResources) {
            if (splitExecuted && splitDiagReadable) splitDiagReadable->store(false); // never read timestamps a replay may overwrite
            splitExecuted = true;
        }
        tracing = traceId && traceExecutions < 2;
        if (tracing) ++traceExecutions;
        Trace("execute.begin", e.queue);
        enqueued = false;
        runtimeRetirementUncertain = false;
        if (inlineResources) {
            try {
                const bool bypass = !owner->AdmitInline(inlineResources);
                inlineResetSerial = e.serial;
                inlineResources->BeforeExecute(e.queue, bypass);
                if (bypass) {
                    // Preserve the original game batch without entering the runtime
                    // or consuming a network slot. The recorded selector uses original.
                    inlineBypassSerial = e.serial;
                    return S_OK;
                }
            }
            catch (const std::exception& error) {
                owner->RecordFailure("inline reset", E_FAIL, e.queue, error.what()); owner->failed = true; return E_FAIL;
            }
            inlineEnqueueFinished = executionReported = false;
            inlineResources->activityToken = activityToken;
            ++inlineResources->submitExecutions;
            inlineResources->submitPresentSerial = FrameDiag::AutoSpinPresentRing().CurrentSerial();
            owner->lastInlineGameQueue = e.queue;
        }
        const int32_t rc = owner->api.BeginRecordingExecution(owner->context, job, e.queue);
        if (rc != LMXXF_NR_OK) owner->RecordFailure("BeginRecordingExecution", rc, e.queue);
        Trace("execute.admitted", e.queue, rc);
        begun = rc == LMXXF_NR_OK;
        if (begun) {
            activeExecutionSerial = e.serial;
            if (inlineResources) {
                owner->NoteInlineEnqueue(job);
                ++owner->inlineAdmitted;
                if (inlineResources->diag.active) {
                    inlineResources->diag.submitSerial = e.serial;
                    inlineResources->diag.qpcAdmit = FrameDiag::Qpc();
                    inlineResources->diag.activeSlots = unsigned(owner->inlineInFlight.size());
                    if (inlineResources->diag.qpcRecordBegin)
                        inlineResources->diag.recordCpuUs = unsigned(
                            FrameDiag::MsBetween(inlineResources->diag.qpcRecordBegin,
                                                 inlineResources->diag.qpcAdmit) * 1000.0);
                }
                if (owner->inlineAdmitted <= 5 || owner->inlineAdmitted % 3600 == 0)
                    LOG_INFO("NR inline admit serial={} queue={} in_flight={} enqueue_depth={}",
                             static_cast<unsigned long long>(e.serial), static_cast<const void*>(e.queue),
                             owner->inlineInFlight.size(), owner->inlineEnqueueOrder.size());
            } else if (diag.active) {
                diag.submitSerial = e.serial;
                diag.qpcAdmit = FrameDiag::Qpc();
                diag.listId = identity.list;
                diag.generation = identity.generation;
                if (diag.qpcRecordBegin)
                    diag.recordCpuUs = unsigned(
                        FrameDiag::MsBetween(diag.qpcRecordBegin, diag.qpcAdmit) * 1000.0);
            }
        } else if (!inlineResources && diag.active) {
            EmitSplitDiag("fail");
        }
        if (!begun) owner->failed = true;
        return begun ? S_OK : E_FAIL;
    }
    // Host enqueue order, without holding RecordingMutex while sleeping.
    bool WaitInlineTurn() {
        std::unique_lock lock(Submission::RecordingMutex());
        if (!owner) return false;
        const bool changed = owner->inlineTurnChanged.wait_for(lock, std::chrono::seconds(10), [&] {
            return owner->InlineEnqueueTurn(job) || !owner->InlineEnqueueQueued(job);
        });
        return changed && owner->InlineEnqueueTurn(job);
    }
    void FailInlineWorker(const char* error) noexcept {
        std::lock_guard lock(Submission::RecordingMutex());
        owner->RecordFailure("inline worker", E_FAIL, nullptr, error); owner->failed = true;
        try { inlineResources->NetworkQueued(false); } catch (...) {}
        inlineEnqueueFinished = true;
        if (executionReported) FinishInline();
    }
    struct Work { std::shared_ptr<Lease> lease; HMODULE module = nullptr; };
    static void CALLBACK InlineCallback(PTP_CALLBACK_INSTANCE instance, void* context)
    {
        std::unique_ptr<Work> work(static_cast<Work*>(context));
        auto lease = work->lease;
        try {
            // Capture polling submits work to the shared side queue. Only its
            // current enqueue owner may poll; ahead workers would otherwise
            // delay the frame that releases their game-queue consumer waits.
            auto& diag = lease->inlineResources->diag;
            const auto turnStart = GetTickCount64();
            if (diag.active) diag.qpcEnqueueWaitBegin = FrameDiag::Qpc();
            const bool ordered = lease->WaitInlineTurn();
            const auto waitedMs = static_cast<unsigned>(GetTickCount64() - turnStart);
            if (diag.active) { diag.qpcEnqueueWaitEnd = FrameDiag::Qpc(); diag.waitEnqueueOrderMs = waitedMs; }
            // Armed mode reports watch submission, not observed capture. It retains the documented
            // cap/abort input-race risk; CPU capture mode still verifies completion before enqueue.
            const bool captured = ordered && (lease->inlineResources->GpuArmed()
                ? lease->inlineResources->ArmCapture() : lease->inlineResources->WaitCapture());
            std::lock_guard lock(Submission::RecordingMutex());
            if (captured && ordered) {
                if (waitedMs >= 2) {
                    ++lease->owner->inlineTurnWaits;
                    if (lease->owner->inlineTurnWaits <= 5 || lease->owner->inlineTurnWaits % 1200 == 0)
                        LOG_INFO("NR inline enqueue waited {} ms for the previous frame", waitedMs);
                }
                if (diag.active) diag.qpcVkEnqueue = FrameDiag::Qpc();
                Submission::RecordingExecution side {}; side.queue = lease->inlineResources->SideQueue();
                lease->Between(side);
            } else if (!ordered) {
                lease->owner->RecordFailure("inline order", E_FAIL, nullptr, "inline enqueue turn timed out");
                lease->owner->failed = true;

            } else {
                lease->owner->RecordFailure("inline capture", E_FAIL, nullptr, "input capture was not observed; network was not started");
                lease->owner->failed = true;
            }
            lease->inlineResources->NetworkQueued(lease->enqueued);
            lease->inlineEnqueueFinished = true;
            if (lease->executionReported) lease->FinishInline();
        } catch (const std::exception& error) {
            lease->FailInlineWorker(error.what());
        } catch (...) {
            lease->FailInlineWorker("non-standard exception in inline worker");
        }
        const auto module = work->module;
        work.reset(); lease.reset();
        FreeLibraryWhenCallbackReturns(instance, module);
    }
    void ProducerSubmitted(const Submission::RecordingExecution&) noexcept override
    {
        if (!inlineResources || !begun || !ready) return;
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&InlineCallback), &module)) {
            owner->RecordFailure("inline worker module", E_FAIL, nullptr, "host module could not be retained"); owner->failed = true;
            try { inlineResources->NetworkQueued(false); } catch (...) {}
            inlineEnqueueFinished = true; return;
        }
        Work* work = nullptr;
        try { work = new Work {shared_from_this(), module}; } catch (...) {}
        if (!work || !TrySubmitThreadpoolCallback(&InlineCallback, work, nullptr)) {
            delete work; FreeLibrary(module);
            owner->RecordFailure("inline worker start", E_FAIL, nullptr, "threadpool submission failed"); owner->failed = true;
            try { inlineResources->NetworkQueued(false); } catch (...) {}
            inlineEnqueueFinished = true;
        }
    }
    void FinishInline(bool schedule = true) noexcept
    {
        auto facts = inlineExecution;
        facts.queue = inlineQueue.Get(); facts.fence = inlineFence.Get();
        ID3D12Fence* proof = nullptr; UINT64 value = 0;
        if (inlineResources->GameCertificate(proof, value)) {
            // For a submitted unsplit list, status is the tail Signal result. A later same-queue
            // signal replaces that certificate; it does not erase an EnqueueHip/runtime failure.
            facts.fence = proof; facts.fenceValue = value; facts.status = S_OK;
        } else if (!inlineResources->ProofExhausted() && !inlineResources->DeviceLost()) {
            // End is not retryable in either real runtime. Keep Begin and the enqueue turn alive
            // until bounded collection retries obtain the certificate, or exhaust and fail closed.
            if (schedule) ScheduleCollectionLocked();
            return;
        }
        // The consumer was recorded in the same submitted native list as its capture.
        facts.continuationSubmitted = facts.producerSubmitted && ready;
        FinishExecution(facts);
        // Pop after End so the next frame's Enqueue sees the runtime head advance.
        if (inlineResources) owner->PopInlineEnqueue(job);
        inlineQueue.Reset(); inlineFence.Reset();
        if (invalidated) {
            owner->api.InvalidateRecording(owner->context, job);
            if (schedule) ScheduleCollectionLocked();
        }
    }
    void Between(const Submission::RecordingExecution& e) noexcept override
    {
        if (!begun || !ready) return;
        auto& diagnostic = LmxxfCut::Pending();
        diagnostic.betweenHits.fetch_add(1, std::memory_order_relaxed);
        diagnostic.enqueueCalls.fetch_add(1, std::memory_order_relaxed);
        Trace("hip.begin", e.queue);
        if (!inlineResources && diag.active) {
            if (!diag.qpcEnqueueWaitBegin) diag.qpcEnqueueWaitBegin = FrameDiag::Qpc();
            diag.qpcVkEnqueue = FrameDiag::Qpc();
        }
        const int32_t rc = owner->api.EnqueueHip(owner->context, job, e.queue);
        if (!inlineResources && diag.active) diag.qpcSideReady = FrameDiag::Qpc();
        Trace("hip.end", e.queue, rc);
        std::array<char, 256> error {};
        owner->api.GetLastError(error.data(), static_cast<uint32_t>(error.size()));
        enqueued = rc == LMXXF_NR_OK && error[0] == '\0';
        if (owner->coverage) {
            owner->coverage->enqueues.fetch_add(1, std::memory_order_relaxed);
            if (!enqueued) owner->coverage->enqueueFailed.fetch_add(1, std::memory_order_relaxed);
        }
        if (neural && !enqueued)
            owner->activity.Reset();
        if (rc != LMXXF_NR_OK) {
            owner->RecordFailure("EnqueueHip", rc, e.queue, error.data());
            owner->failed = true;
        }
        std::lock_guard lock(diagnostic.mutex);
        diagnostic.lastEnqueueRc.store(rc, std::memory_order_relaxed);
        diagnostic.lastEnqueueError = error;
        diagnostic.lastEnqueueQueue = e.queue;
        diagnostic.lastQueue.store(e.queue, std::memory_order_relaxed);
    }
    void Executed(const Submission::RecordingExecution& e) noexcept override
    {
        if (owner->coverage) {
            owner->coverage->executions.fetch_add(1, std::memory_order_relaxed);
            if (FAILED(e.status)) owner->coverage->executionFailed.fetch_add(1, std::memory_order_relaxed);
        }
        // A rejected overlapping replay must not end the still-running execution.
        if (inlineResources && inlineResetSerial != e.serial) return;
        if (inlineResources && inlineBypassSerial == e.serial) {
            inlineBypassSerial = 0;
            inlineResources->CertifyWithoutNetwork(e.fence, e.fenceValue, e.status, e.producerSubmitted);
            if (e.producerSubmitted) {
                owner->InlineBypassed();
                owner->activity.Reset(); owner->api.ResetHistory(owner->context);
            }
            return;
        }
        if (inlineResources && begun && ready && e.producerSubmitted) {
            inlineExecution = e; inlineQueue = e.queue; inlineFence = e.fence;
            inlineResources->GameSubmitted(e.fence, e.fenceValue, e.status);
            executionReported = true;
            if (inlineEnqueueFinished) FinishInline();
            return;
        }
        if (inlineResources && inlineResetSerial == e.serial && (!ready || !e.producerSubmitted))
            inlineResources->CertifyWithoutNetwork(e.fence, e.fenceValue, e.status, e.producerSubmitted);
        FinishExecution(e);
        if (inlineResources) owner->PopInlineEnqueue(job);
    }
    void FinishExecution(const Submission::RecordingExecution& e) noexcept
    {
        if (!begun) {
            if (FAILED(e.status))
                owner->RecordFailure("ExecuteCommandLists", e.status, e.queue,
                                     "submission rejected before runtime execution; see first failure");
            return;
        }
        const uint32_t flags = (e.producerSubmitted ? LMXXF_NR_SUBMITTED_PRODUCER : 0) |
                               (e.continuationSubmitted ? LMXXF_NR_SUBMITTED_CONSUMER : 0);
        Trace("retire.begin", e.queue, e.status);
        const int32_t rc = owner->api.EndRecordingExecution(owner->context, job, e.queue,
                                                          flags, e.fence, e.fenceValue, e.status);
        if (rc != LMXXF_NR_OK) {
            owner->RecordFailure("EndRecordingExecution", rc, e.queue);
            owner->failed = true;
            runtimeRetirementUncertain = true;
        }
        else if (FAILED(e.status))
            owner->RecordFailure("ExecuteCommandLists", e.status, e.queue, "producer/consumer submission or tail signal failed");
        Trace("retire.end", e.queue, rc);
        splitDiagQueue = e.queue; splitDiagFence = e.fence; splitDiagValue = e.fenceValue;
        tracing = false;
        begun = false;
        if (!inlineResources && neural && ready && enqueued && e.producerSubmitted && e.continuationSubmitted &&
            SUCCEEDED(e.status) && rc == LMXXF_NR_OK && !owner->failed.load(std::memory_order_acquire))
        {
            owner->activity.Succeeded(activityToken);
            if (owner->coverage) owner->coverage->splitAccepted.fetch_add(1, std::memory_order_relaxed);
            EmitSplitDiag("apply");
        }
        else if (!inlineResources && neural)
        {
            owner->activity.Reset();
            EmitSplitDiag((FAILED(e.status) || rc != LMXXF_NR_OK) ? "fail" : "reject");
        }
        else
            EmitSplitDiag((FAILED(e.status) || rc != LMXXF_NR_OK) ? "fail" : "apply");
    }
    void Invalidated(Submission::RecordingIdentity id) noexcept override
    {
        if (invalidated || !(id == identity)) return;
        invalidated = true;
        // Reset/Release before execution relinquishes admission immediately.
        // Started recordings still retain their reservation until both queue proofs finish.
        owner->ForgetUnstarted(inlineResources);
        // A Reset may follow submission before the worker reaches Enqueue. Keep
        // the runtime job executable until its deferred End event has been recorded.
        if (inlineResources && begun) return;
        const int32_t rc = owner->api.InvalidateRecording(owner->context, job);
        if (rc != LMXXF_NR_OK) {
            owner->RecordFailure("InvalidateRecording", rc, nullptr);
            owner->failed = true;
        }
        if (CollectLocked()) ScheduleCollectionLocked();
    }
};

inline bool CollectLocked()
{
    bool pending = false;
    auto& leases = Registry();
    for (auto it = leases.begin(); it != leases.end();)
    {
        auto& lease = *it;
        lease->owner->ReclaimInlineLocked();
        if (lease->begun && lease->inlineResources && lease->executionReported && lease->inlineEnqueueFinished)
            lease->FinishInline(false);
        if (lease->begun && lease->inlineResources && lease->executionReported && lease->inlineEnqueueFinished)
            pending = !lease->inlineResources->ProofExhausted() || pending;
        if (!lease->invalidated) { ++it; continue; }
        if (lease->begun) { pending = true; ++it; continue; }
        if (lease->inlineResources && !lease->inlineResources->Complete()) {
            // ReclaimInlineLocked already ran RetryProof for in-flight recordings.
            // Do not burn a second attempt here. Keep pins; do not rearm a permanent timer
            // while still uncertain; once proven, wait for the proof fence to complete.
            if (!lease->inlineResources->RetirementUncertain()) pending = true;
            ++it; continue;
        }
        const int32_t rc = lease->owner->api.CollectRecording(lease->owner->context, lease->job);
        if (rc == LMXXF_NR_OK || rc == LMXXF_NR_DEVICE_LOST)
        {
            lease->EmitSplitDiag("abort");
            lease->owner->ForgetUnstarted(lease->inlineResources);
            lease->job = nullptr;
            it = leases.erase(it);
        }
        else { if (!lease->runtimeRetirementUncertain) pending = true; ++it; }
    }
    return pending;
}

struct CollectionTimer
{
    PTP_TIMER timer = nullptr;
    HMODULE module = nullptr;
};
inline CollectionTimer& Timer() { static CollectionTimer timer; return timer; }
inline void Arm(PTP_TIMER timer) noexcept
{
    LARGE_INTEGER due {}; due.QuadPart = -1000000; // 100 ms, no GPU/CPU drain.
    FILETIME when {due.LowPart, static_cast<DWORD>(due.HighPart)};
    SetThreadpoolTimer(timer, &when, 0, 0);
}
inline void CALLBACK CollectionCallback(PTP_CALLBACK_INSTANCE instance, void*, PTP_TIMER timer)
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked()) { Arm(timer); return; }
    const auto module = Timer().module;
    Timer() = {};
    CloseThreadpoolTimer(timer);
    FreeLibraryWhenCallbackReturns(instance, module);
}
inline void ScheduleCollectionLocked() noexcept
{
    if (Timer().timer) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&CollectionCallback), &module)) return;
    auto* timer = CreateThreadpoolTimer(&CollectionCallback, nullptr, nullptr);
    if (!timer) { FreeLibrary(module); return; } // Registry still retains every resource.
    Timer() = {timer, module}; Arm(timer);
}
inline void Collect()
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked()) ScheduleCollectionLocked();
}

// Register ownership before any NR commands are appended. A failed later Record
// still leaves its observer attached, so partial private work is tracked safely.
inline std::shared_ptr<Lease> Attach(const std::shared_ptr<SessionOwner>& owner, void* job,
                                     Submission::ILogicalCommandList* logical,
                                     std::shared_ptr<InlineRecording::Resources> inlineResources = {})
{
    // No private commands have been appended yet. An ineligible recording must
    // not acquire an observer or retain a job merely to be rejected after input writes.
    if (!logical || (!inlineResources && logical->IsSplitIneligible()))
    {
        owner->api.InvalidateRecording(owner->context, job);
        owner->api.CollectRecording(owner->context, job);
        return {};
    }
    std::shared_ptr<Lease> lease;
    try
    {
        owner->ReclaimInlineLocked();
        if (inlineResources && owner->inlineInFlight.size() >= owner->inlineLimit) {
            owner->api.InvalidateRecording(owner->context, job);
            owner->api.CollectRecording(owner->context, job);
            return {};
        }
        lease = std::make_shared<Lease>(owner, job, logical->Identity());
        lease->inlineResources = inlineResources;
        if (inlineResources) owner->AdmitInline(inlineResources);
        Registry().push_back(lease);
    }
    catch (...)
    {
        // No command has been appended yet and no observer owns this token.
        owner->ForgetUnstarted(inlineResources);
        owner->api.InvalidateRecording(owner->context, job);
        owner->api.CollectRecording(owner->context, job);
        return {};
    }
    if (FAILED(logical->ObserveRecording(lease)))
    {
        owner->ForgetUnstarted(inlineResources);
        lease->Invalidated(lease->identity);
        return {};
    }
    return lease;
}
} // namespace DlssNr::Backend::LmxxfRecording

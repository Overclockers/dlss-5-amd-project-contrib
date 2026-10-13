#pragma once
#include "LmxxfEvaluateCut.h"
#include "../NrSessionActivity.h"
#include "lmxxf_runtime/LmxxfNrApi.h"
#include "NrInlineRecording.h"
#include <algorithm>
#include <deque>
#include <memory>
#include <vector>
#include <array>
#include <cstdio>
#include <condition_variable>
#ifndef LOG_INFO
#define LOG_INFO(...) ((void)0)
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
    HMODULE module = nullptr;
    std::atomic<bool> failed { false };
    NrSessionActivity activity;
    // Reserved at Attach, including recordings whose command list is not submitted yet.
    // Default conservatively to one; backends with private recording buffers
    // select their limit. A reservation survives until both queues complete.
    static constexpr size_t kInlineAhead = 4;
    size_t inlineLimit = 1;
    std::vector<std::shared_ptr<InlineRecording::Resources>> inlineInFlight;
    std::deque<void*> inlineEnqueueOrder;
    std::condition_variable_any inlineTurnChanged;
    std::shared_ptr<InlineRecording::Pipelines> inlinePipelines;
    uint64_t inlineApplied = 0, inlineTimeouts = 0, inlineAborted = 0, inlineBypassed = 0;
    uint64_t inlineAdmitted = 0, inlineTurnWaits = 0;
    std::atomic<uint64_t> liveRecordings {0};
    void ReclaimInlineLocked() {
        void* side = nullptr;
        uint64_t spun = 0;
        for (auto it = inlineInFlight.begin(); it != inlineInFlight.end();) {
            auto& pending = *it;
            // A fresh reservation reports Complete because it has not started.
            if (!pending || !pending->Started() || !pending->Complete()) { ++it; continue; }
            if (!pending->Succeeded()) {
                activity.Reset(); api.ResetHistory(context);
                if (pending->WasAborted()) ++inlineAborted;
                else {
                    ++inlineTimeouts;
                    if (inlineTimeouts <= 5 || inlineTimeouts % 1200 == 0)
                        LOG_WARN("NR inline wait exhausted; original frame selected, history reset (timeouts={})", inlineTimeouts);
                }
            } else {
                ++inlineApplied;
                if (!failed) activity.Succeeded(pending->activityToken);
            }
            side = pending->SideQueue();
            spun = pending->WasAborted() ? 0 : pending->LastSpinCount();
            it = inlineInFlight.erase(it);
        }
        const auto completed = inlineApplied + inlineTimeouts + inlineAborted;
        if (side && (completed <= 3 || completed % 3600 == 0))
            LOG_INFO("NR inline completed={} applied={} timeouts={} aborted={} bypassed={} live_recordings={} in_flight={} side_queue={} last_slice_spins={}",
                     completed, inlineApplied, inlineTimeouts, inlineAborted, inlineBypassed,
                     liveRecordings.load(std::memory_order_relaxed), inlineInFlight.size(), side, spun);
    }
    bool InlineBusy() {
        ReclaimInlineLocked();
        return inlineInFlight.size() >= inlineLimit;
    }
    void InlineBypassed() {
        ++inlineBypassed;
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
        if (inlineInFlight.size() >= inlineLimit) return false;
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
    uint64_t inlineResetSerial = 0, activeExecutionSerial = 0;
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
    Lease(std::shared_ptr<SessionOwner> session, void* token, Submission::RecordingIdentity id)
        : owner(std::move(session)), job(token), identity(id), activityToken(owner->activity.Token()) { ++owner->liveRecordings; }
    ~Lease() override { --owner->liveRecordings; }
    HRESULT BeforeExecute(const Submission::RecordingExecution& e) noexcept override
    {
        if (invalidated || !(e.identity == identity)) {
            owner->RecordFailure("identity", E_UNEXPECTED, e.queue, "invalidated or mismatched recording");
            return E_UNEXPECTED;
        }
        // Replay must reserve capacity again after an earlier completion was reclaimed.
        // Reject before resetting flags or appending queue work.
        if (inlineResources && inlineResources->Started() && !inlineResources->Complete()) return E_PENDING;
        tracing = traceId && traceExecutions < 2;
        if (tracing) ++traceExecutions;
        Trace("execute.begin", e.queue);
        enqueued = false;
        runtimeRetirementUncertain = false;
        if (inlineResources) {
            try {
                if (!owner->AdmitInline(inlineResources)) return E_PENDING;
                inlineResetSerial = e.serial;
                inlineResources->BeforeExecute(e.queue);
            }
            catch (const std::exception& error) {
                owner->RecordFailure("inline reset", E_FAIL, e.queue, error.what()); owner->failed = true; return E_FAIL;
            }
            inlineEnqueueFinished = executionReported = false;
            inlineResources->activityToken = activityToken;
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
                if (owner->inlineAdmitted <= 5 || owner->inlineAdmitted % 3600 == 0)
                    LOG_INFO("NR inline admit serial={} queue={} in_flight={} enqueue_depth={}",
                             static_cast<unsigned long long>(e.serial), static_cast<const void*>(e.queue),
                             owner->inlineInFlight.size(), owner->inlineEnqueueOrder.size());
            }
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
            const auto turnStart = GetTickCount64();
            const bool ordered = lease->WaitInlineTurn();
            const auto waitedMs = static_cast<unsigned>(GetTickCount64() - turnStart);
            const bool captured = ordered && lease->inlineResources->WaitCapture();
            std::lock_guard lock(Submission::RecordingMutex());
            if (captured && ordered) {
                if (waitedMs >= 2) {
                    ++lease->owner->inlineTurnWaits;
                    if (lease->owner->inlineTurnWaits <= 5 || lease->owner->inlineTurnWaits % 1200 == 0)
                        LOG_INFO("NR inline enqueue waited {} ms for the previous frame", waitedMs);
                }
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
    void FinishInline() noexcept
    {
        auto facts = inlineExecution;
        facts.queue = inlineQueue.Get(); facts.fence = inlineFence.Get();
        // The consumer was recorded in the same submitted native list as its capture.
        facts.continuationSubmitted = facts.producerSubmitted && ready;
        FinishExecution(facts);
        // Pop after End so the next frame's Enqueue sees the runtime head advance.
        if (inlineResources) owner->PopInlineEnqueue(job);
        inlineQueue.Reset(); inlineFence.Reset();
        if (invalidated) {
            owner->api.InvalidateRecording(owner->context, job);
            if (CollectLocked()) ScheduleCollectionLocked();
        }
    }
    void Between(const Submission::RecordingExecution& e) noexcept override
    {
        if (!begun || !ready) return;
        auto& diagnostic = LmxxfCut::Pending();
        diagnostic.betweenHits.fetch_add(1, std::memory_order_relaxed);
        diagnostic.enqueueCalls.fetch_add(1, std::memory_order_relaxed);
        Trace("hip.begin", e.queue);
        const int32_t rc = owner->api.EnqueueHip(owner->context, job, e.queue);
        Trace("hip.end", e.queue, rc);
        std::array<char, 256> error {};
        owner->api.GetLastError(error.data(), static_cast<uint32_t>(error.size()));
        enqueued = rc == LMXXF_NR_OK && error[0] == '\0';
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
        // A rejected overlapping replay must not end the still-running execution.
        if (inlineResources && begun && activeExecutionSerial != e.serial) return;
        if (inlineResources && begun && ready && e.producerSubmitted) {
            inlineExecution = e; inlineQueue = e.queue; inlineFence = e.fence;
            inlineResources->GameSubmitted(e.fence, e.fenceValue, e.status);
            executionReported = true;
            if (inlineEnqueueFinished) FinishInline();
            return;
        }
        if (inlineResources && inlineResetSerial == e.serial && (!ready || !e.producerSubmitted))
            inlineResources->CertifyWithoutNetwork(e.producerSubmitted ? e.fence : nullptr, e.fenceValue, e.status);
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
        tracing = false;
        begun = false;
        if (!inlineResources && neural && ready && enqueued && e.producerSubmitted && e.continuationSubmitted &&
            SUCCEEDED(e.status) && rc == LMXXF_NR_OK && !owner->failed.load(std::memory_order_acquire))
            owner->activity.Succeeded(activityToken);
        else if (!inlineResources && neural)
            owner->activity.Reset();
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
        if (!lease->invalidated) { ++it; continue; }
        if (lease->begun) { pending = true; ++it; continue; }
        if (lease->inlineResources && !lease->inlineResources->Complete()) {
            // Unknown signals cannot gain proof by polling. Keep the pins but
            // stop a permanent 100 ms timer; later explicit collection can retry.
            if (!lease->inlineResources->RetirementUncertain()) pending = true;
            ++it; continue;
        }
        const int32_t rc = lease->owner->api.CollectRecording(lease->owner->context, lease->job);
        if (rc == LMXXF_NR_OK || rc == LMXXF_NR_DEVICE_LOST)
        {
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

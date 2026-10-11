#pragma once
#include "LmxxfEvaluateCut.h"
#include "../NrSessionActivity.h"
#include "lmxxf_runtime/LmxxfNrApi.h"
#include "NrInlineRecording.h"
#include <memory>
#include <vector>
#include <array>
#include <cstdio>
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
    std::shared_ptr<InlineRecording::Resources> pendingInline;
    std::shared_ptr<InlineRecording::Pipelines> inlinePipelines;
    uint64_t inlineActivityToken = 0;
    uint64_t inlineApplied = 0, inlineTimeouts = 0, inlineAborted = 0, inlineBypassed = 0;
    std::atomic<uint64_t> liveRecordings {0};
    void InlineBypassed() { ++inlineBypassed; }
    bool InlineBusy() {
        if (!pendingInline) return false;
        if (!pendingInline->Complete()) return true;
        if (!pendingInline->Succeeded()) {
            activity.Reset(); api.ResetHistory(context);
            if (pendingInline->WasAborted()) ++inlineAborted;
            else {
                ++inlineTimeouts;
                if (inlineTimeouts <= 5 || inlineTimeouts % 120 == 0)
                    LOG_WARN("NR inline wait exhausted; original frame selected, history reset (timeouts={})", inlineTimeouts);
            }
        } else {
            ++inlineApplied;
            if (!failed) activity.Succeeded(inlineActivityToken);
        }
        const auto completed = inlineApplied + inlineTimeouts + inlineAborted;
        if (completed <= 3 || completed % 120 == 0)
            LOG_INFO("NR inline completed={} applied={} timeouts={} aborted={} bypassed={} live_recordings={} side_queue={} last_slice_spins={}",
                     completed, inlineApplied, inlineTimeouts, inlineAborted, inlineBypassed, liveRecordings.load(std::memory_order_relaxed),
                     static_cast<void*>(pendingInline->SideQueue()), pendingInline->WasAborted() ? 0 : pendingInline->LastSpinCount());
        pendingInline.reset();
        return false;
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
    static std::shared_ptr<SessionOwner> Create(const LmxxfNrApi& api, void* context)
    {
        std::shared_ptr<SessionOwner> owner;
        try { owner = std::make_shared<SessionOwner>(); }
        catch (...) { return {}; }
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(api.Destroy), &owner->module)) return {};
        owner->api = api; owner->context = context;
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
        if (inlineResources && owner->InlineBusy()) return E_PENDING;
        tracing = traceId && traceExecutions < 2;
        if (tracing) ++traceExecutions;
        Trace("execute.begin", e.queue);
        enqueued = false;
        if (inlineResources) {
            inlineResetSerial = e.serial;
            try { inlineResources->BeforeExecute(e.queue); }
            catch (const std::exception& error) {
                owner->RecordFailure("inline reset", E_FAIL, e.queue, error.what()); owner->failed = true; return E_FAIL;
            }
            inlineEnqueueFinished = executionReported = false;
            owner->pendingInline = inlineResources;
            owner->inlineActivityToken = activityToken;
        }
        const int32_t rc = owner->api.BeginRecordingExecution(owner->context, job, e.queue);
        if (rc != LMXXF_NR_OK) owner->RecordFailure("BeginRecordingExecution", rc, e.queue);
        Trace("execute.admitted", e.queue, rc);
        begun = rc == LMXXF_NR_OK;
        if (begun) activeExecutionSerial = e.serial;
        if (!begun) owner->failed = true;
        return begun ? S_OK : E_FAIL;
    }
    struct Work { std::shared_ptr<Lease> lease; HMODULE module = nullptr; };
    static void CALLBACK InlineCallback(PTP_CALLBACK_INSTANCE instance, void* context)
    {
        std::unique_ptr<Work> work(static_cast<Work*>(context));
        auto lease = work->lease;
        try {
            const bool captured = lease->inlineResources->WaitCapture();
            std::lock_guard lock(Submission::RecordingMutex());
            if (captured) {
                Submission::RecordingExecution side {}; side.queue = lease->inlineResources->SideQueue();
                lease->Between(side);
            } else {
                lease->owner->RecordFailure("inline capture", E_FAIL, nullptr, "input capture was not observed; network was not started");
                lease->owner->failed = true;
            }
            lease->inlineResources->NetworkQueued(lease->enqueued);
            lease->inlineEnqueueFinished = true;
            if (lease->executionReported) lease->FinishInline();
        } catch (const std::exception& error) {
            std::lock_guard lock(Submission::RecordingMutex());
            lease->owner->RecordFailure("inline worker", E_FAIL, nullptr, error.what()); lease->owner->failed = true;
            try { lease->inlineResources->NetworkQueued(false); } catch (...) {}
            lease->inlineEnqueueFinished = true;
            if (lease->executionReported) lease->FinishInline();
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
        lease->owner->InlineBusy();
        if (!lease->invalidated) { ++it; continue; }
        if (lease->begun || (lease->inlineResources && !lease->inlineResources->Complete())) { pending = true; ++it; continue; }
        const int32_t rc = lease->owner->api.CollectRecording(lease->owner->context, lease->job);
        if (rc == LMXXF_NR_OK || rc == LMXXF_NR_DEVICE_LOST)
        {
            lease->job = nullptr;
            it = leases.erase(it);
        }
        else { pending = true; ++it; }
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
        lease = std::make_shared<Lease>(owner, job, logical->Identity());
        lease->inlineResources = std::move(inlineResources);
        Registry().push_back(lease);
    }
    catch (...)
    {
        // No command has been appended yet and no observer owns this token.
        owner->api.InvalidateRecording(owner->context, job);
        owner->api.CollectRecording(owner->context, job);
        return {};
    }
    if (FAILED(logical->ObserveRecording(lease)))
    {
        lease->Invalidated(lease->identity);
        return {};
    }
    return lease;
}
} // namespace DlssNr::Backend::LmxxfRecording

#include "lmxxf_gpu_test_utils.h"
#include <wrl/client.h>
#include <chrono>
#include <thread>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrApi.h"
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
namespace Own = DlssNr::Backend::LmxxfRecording;
namespace In = DlssNr::Backend::InlineRecording;
namespace Sub = DlssNr::Submission;
static unsigned batchCalls = 0, batchLists = 0;
static void STDMETHODCALLTYPE TraceBatch(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    ++batchCalls; batchLists += n; q->ExecuteCommandLists(n, lists);
}
// --fixed-backoff runs every inline frame with NrCaptureBackoff=1 (fixed 100 us capture polling).
static int gCaptureBackoff = 0;
struct BackoffResources : In::Resources {
    template<class... A> explicit BackoffResources(A&&... a) : In::Resources(std::forward<A>(a)...) { captureBackoff = gCaptureBackoff; }
};
// --reuse routes every recording through the NrInlineReuse pool (AcquireResources).
static bool gReuse = false;
static unsigned gReuseHits = 0;
static std::vector<std::shared_ptr<In::Resources>> gPool;
static std::shared_ptr<In::Resources> MakeResources(ID3D12Device* d, ID3D12Resource* s, ID3D12Resource* n, ID3D12Resource* f,
                                                    UINT w, UINT h, std::shared_ptr<In::Pipelines> cached) {
    if (!gReuse) return std::make_shared<BackoffResources>(d, s, n, f, w, h, std::move(cached));
    std::lock_guard lock(Sub::RecordingMutex());
    auto r = In::AcquireResources(&gPool, d, s, n, f, w, h, std::move(cached));
    if (r->reused) ++gReuseHits;
    r->captureBackoff = gCaptureBackoff; return r;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const bool mochi = argc > 3 && std::strcmp(argv[3], "--mochizuki") == 0;
    bool timeoutMode = false;
    bool large = false;
    bool temporalRequested = false, temporalMode = false;
    bool ahead = false;
    bool aco = false;
    bool threePasses = false;
    bool unorm8 = false; // R8G8B8A8_UNORM colour: the whole inline chain can run on the compute family
    for (int i = 3; i < argc; ++i) timeoutMode |= std::strcmp(argv[i], "--timeout") == 0;
    for (int i = 3; i < argc; ++i) large |= std::strcmp(argv[i], "--large") == 0;
    for (int i = 3; i < argc; ++i) temporalRequested |= std::strcmp(argv[i], "--temporal") == 0;
    for (int i = 3; i < argc; ++i) ahead |= std::strcmp(argv[i], "--ahead") == 0;
    for (int i = 3; i < argc; ++i) aco |= std::strcmp(argv[i], "--aco") == 0;
    for (int i = 3; i < argc; ++i) threePasses |= std::strcmp(argv[i], "--three-passes") == 0;
    for (int i = 3; i < argc; ++i) unorm8 |= std::strcmp(argv[i], "--unorm8") == 0;
    for (int i = 3; i < argc; ++i) if (std::strcmp(argv[i], "--fixed-backoff") == 0) gCaptureBackoff = 1;
    for (int i = 3; i < argc; ++i) if (std::strcmp(argv[i], "--reuse") == 0) gReuse = true;
    Require(!aco || mochi, "ACO fixture requires Mochizuki");
    const UINT waitSlices = mochi ? In::kWaitSlices : In::kLmxxfWaitSlices;
    const UINT waitIterations = mochi ? In::kWaitIterations : In::kLmxxfWaitIterations;
    // The inline output is RGBA16F; an R8G8B8A8_UNORM frame is compared through its 8-bit values.
    auto Hash = [&](ID3D12Device* d, ID3D12CommandQueue* q, ID3D12Resource* t) {
        return unorm8 ? HashTextureUnorm8(d, q, t) : HashTexture(d, q, t);
    };
    Require(!threePasses || mochi, "three-pass fixture requires Mochizuki");
    Ptr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    HMODULE dll = LoadLibraryW(Widen(argv[1]).c_str()); Require(dll != nullptr, "runtime load");
    auto get = reinterpret_cast<int32_t(*)(uint32_t,LmxxfNrApi*)>(GetProcAddress(dll, mochi ? "MochizukiNrGetApi" : "LmxxfNrGetApi"));
    LmxxfNrApi api {}; api.struct_size = sizeof api; Require(get && get(LMXXF_NR_ABI_VERSION, &api) == 0, "package ABI");
    auto ok = [&](int rc, const char* step) {
        if (rc) { char message[256] {}; api.GetLastError(message, sizeof message); std::fprintf(stderr, "%s: %s\n", step, message); }
        Require(rc == 0, step);
    };
    Ptr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Ptr<ID3D12Device> device;
    for (UINT i = 0;; ++i) {
        Ptr<IDXGIAdapter1> adapter; if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d {}; adapter->GetDesc1(&d);
        if (aco && !wcsstr(d.Description, L"9070 XT")) continue;
        if (d.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
    }
    Require(bool(device), "AMD device");
    Ptr<ID3D12CommandQueue> game, other; D3D12_COMMAND_QUEUE_DESC q {};
    Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&game)), "game queue");
    Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&other)), "replay queue");
    const auto assets = Widen(argv[2]);
    LmxxfNrCreateInfo ci {sizeof ci, device.Get(), game.Get(), assets.c_str(),
                         LMXXF_NR_CREATE_FLAG_RECORDING_LEASES | LMXXF_NR_CREATE_FLAG_INLINE_QUEUE};
    void* context = nullptr; ok(api.Create(&ci, &context), "Create");
    if (aco) {
        auto defaults = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll, "MochizukiNrGetControlDefaults"));
        auto set = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll, "MochizukiNrSetControls"));
        MochizukiNrControls controls{sizeof controls}; Require(defaults && set, "ACO controls");
        ok(defaults(&controls), "ACO defaults"); controls.aco_mode = 1; ok(set(context, &controls), "ACO request before device");
    }
    ok(api.PrepareSession(context), "PrepareSession");
    auto owner = Own::SessionOwner::Create(api, context, Own::SessionOwner::kInlineAhead); Require(bool(owner), "module/session owner");
    if (!mochi) { LmxxfNrTimings timing {sizeof timing}; ok(api.GetTimings(context, &timing), "enable physical HIP event timing"); }
    const UINT width = large ? 2560 : (mochi ? 256 : 1280), height = large ? 1440 : (mochi ? 256 : 720);
    D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = unorm8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Ptr<ID3D12Resource> colour;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&colour)), "colour");
    UploadColorPattern(device.Get(), game.Get(), colour.Get());
    Ptr<ID3D12Resource> motion;
    if (mochi && temporalRequested) {
        D3D12_RESOURCE_DESC motionDesc = desc; motionDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &motionDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              nullptr, IID_PPV_ARGS(&motion)), "zero motion");
        Ptr<ID3D12DescriptorHeap> heap; D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "motion heap");
        device->CreateUnorderedAccessView(motion.Get(), nullptr, nullptr, heap->GetCPUDescriptorHandleForHeapStart());
        Ptr<ID3D12CommandAllocator> a; Ptr<ID3D12GraphicsCommandList> l;
        Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&a)), "motion allocator");
        Check(device->CreateCommandList(0, q.Type, a.Get(), nullptr, IID_PPV_ARGS(&l)), "motion list");
        auto* h = heap.Get(); l->SetDescriptorHeaps(1, &h); const float zero[4] {};
        l->ClearUnorderedAccessViewFloat(heap->GetGPUDescriptorHandleForHeapStart(), heap->GetCPUDescriptorHandleForHeapStart(), motion.Get(), zero, 0, nullptr);
        In::Barrier(l.Get(), motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(l->Close(), "motion close"); In::Execute(game.Get(), l.Get()); WaitQueue(device.Get(), game.Get());
    }
    auto mzPrepare = reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(dll, "MochizukiNrPrepareFrame"));
    auto prepare = [&](ID3D12GraphicsCommandList* list, UINT64 tag = 1, ID3D12Resource* source = nullptr, bool inlineFrame = true) {
        if (!source) source = colour.Get();
        LmxxfNrJob job {}; job.struct_size = sizeof job;
        const auto deadline = GetTickCount64() + 180000;
        for (;;) {
            int rc;
            if (mochi) {
                Require(mzPrepare != nullptr, "Mochizuki prepare export");
                MochizukiNrFrameInfo f {}; f.struct_size = sizeof f; f.frame_id = tag;
                f.color = source; f.color_width = width; f.color_height = height;
                f.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                f.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION;
                f.transfer_strength = f.color_strength = f.model_scale = 1; f.passes = threePasses ? 3 : 1;
                if (temporalMode) {
                    f.flags |= MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL; f.motion = motion.Get();
                    f.motion_width = width; f.motion_height = height; f.motion_scale_x = f.motion_scale_y = 1;
                    f.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
                rc = mzPrepare(context, &f, &job);
            } else {
                LmxxfNrFrameInfo f {}; f.struct_size = sizeof f; f.frame_id = tag; f.command_list = list;
                f.color = source; f.color_width = width; f.color_height = height;
                f.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                f.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
                f.inline_slots = inlineFrame ? static_cast<unsigned>(owner->inlineLimit) : 0;
                f.transfer_strength = f.color_strength = f.model_scale = f.paper_white = f.pre_exposure = f.exposure_scale = 1;
                rc = api.PrepareFrame(context, &f, &job);
            }
            if (!rc) {
                if (aco) {
                    auto getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll, "MochizukiNrGetInfo"));
                    MochizukiNrInfo state{sizeof state}; Require(getInfo, "ACO info");
                    ok(getInfo(context, &state), "ACO installed info"); Require(state.aco_effective == 1, "inline fixture must execute ACO");
                }
                return job;
            }
            if (rc != LMXXF_NR_UNAVAILABLE || GetTickCount64() > deadline) ok(rc, "PrepareFrame");
            Sleep(100);
        }
    };
    // Baseline uses the existing split-style producer/network/consumer ordering.
    Ptr<ID3D12CommandAllocator> pa, ca; Ptr<ID3D12GraphicsCommandList> producer, consumer;
    Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&pa)), "producer allocator");
    Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&ca)), "consumer allocator");
    Check(device->CreateCommandList(0, q.Type, pa.Get(), nullptr, IID_PPV_ARGS(&producer)), "producer");
    Check(device->CreateCommandList(0, q.Type, ca.Get(), nullptr, IID_PPV_ARGS(&consumer)), "consumer");
    auto baseline = prepare(producer.Get(), 1, nullptr, false);
    ok(api.RecordInputs(context, baseline.handle, producer.Get()), "baseline inputs"); Check(producer->Close(), "producer close");
    ok(api.RecordOutputs(context, baseline.handle, consumer.Get()), "baseline outputs"); Check(consumer->Close(), "consumer close");
    ok(api.BeginRecordingExecution(context, baseline.handle, game.Get()), "baseline begin");
    In::Execute(game.Get(), producer.Get()); ok(api.EnqueueHip(context, baseline.handle, game.Get()), "baseline enqueue");
    In::Execute(game.Get(), consumer.Get());
    Ptr<ID3D12Fence> tail; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tail)), "tail fence");
    Check(game->Signal(tail.Get(), 1), "baseline signal");
    ok(api.EndRecordingExecution(context, baseline.handle, game.Get(), 3, tail.Get(), 1, S_OK), "baseline end");
    WaitQueue(device.Get(), game.Get());
    const auto expected = Hash(device.Get(), game.Get(), static_cast<ID3D12Resource*>(baseline.private_output));
    const auto originalHash = Hash(device.Get(), game.Get(), colour.Get());
    producer.Reset(); consumer.Reset(); ok(api.InvalidateRecording(context, baseline.handle), "baseline invalidate");
    ok(api.CollectRecording(context, baseline.handle), "baseline collect");
    std::puts("split baseline completed; testing actual inline worker"); std::fflush(stdout);
    Ptr<ID3D12GraphicsCommandList> native; Ptr<Sub::CommandListProxy> proxy;
    Check(pa->Reset(), "inline allocator");
    Check(device->CreateCommandList(0, q.Type, pa.Get(), nullptr, IID_PPV_ARGS(&native)), "inline native");
    Check(Sub::CommandListProxy::Create(device.Get(), pa.Get(), native.Get(), &proxy), "inline proxy");
    Ptr<ID3D12QueryHeap> queries; D3D12_QUERY_HEAP_DESC query {}; query.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; query.Count = 1;
    Check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries)), "query heap");
    proxy->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    auto job = prepare(proxy.Get(), 2);
    auto resources = MakeResources(device.Get(), colour.Get(), static_cast<ID3D12Resource*>(job.private_output), nullptr, width, height, std::shared_ptr<In::Pipelines>{});
    owner->inlinePipelines = resources->PipelineCache();
    std::shared_ptr<Own::Lease> lease;
    {
        std::lock_guard lock(Sub::RecordingMutex());
        ID3D12Resource* sources[] {colour.Get()}; D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        Check(proxy->BeginInlineState(1, sources, states), "state scope");
        lease = Own::Attach(owner, job.handle, proxy.Get(), resources); Require(bool(lease), "inline ownership");
        ok(api.RecordInputs(context, job.handle, proxy.Get()), "inline inputs");
        resources->RecordCapture(proxy.Get()); resources->RecordWait(proxy.Get(), timeoutMode ? 0 : waitSlices, waitIterations);
        ok(api.RecordOutputs(context, job.handle, proxy.Get()), "inline outputs");
        resources->RecordSelect(proxy.Get(), states[0]); proxy->EndInlineState(); lease->ready = true;
        Require(proxy->IsSplitIneligible(), "query remains open after inline NR");
        proxy->EndQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0); Check(proxy->Close(), "query/list close");
    }
    for (unsigned replay = 0; replay < 2; ++replay) {
        auto* queue = replay ? other.Get() : game.Get();
        if (replay == 0) {
            Ptr<ID3D12CommandAllocator> tailAllocator; Ptr<ID3D12GraphicsCommandList> tailList;
            Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&tailAllocator)), "mixed batch allocator");
            Check(device->CreateCommandList(0, q.Type, tailAllocator.Get(), nullptr, IID_PPV_ARGS(&tailList)), "mixed batch tail");
            Check(tailList->Close(), "mixed batch close");
            ID3D12CommandList* lists[] {proxy.Get(), tailList.Get()};
            Sub::Hooks::ExecuteExpanded(queue, 2, lists, nullptr, nullptr, TraceBatch);
            Require(batchCalls == 1 && batchLists == 2, "inline observers preserve original ExecuteCommandLists batch");
            WaitQueue(device.Get(), queue);
        } else { Check(proxy->ExecuteOn(queue), "inline replay"); WaitQueue(device.Get(), queue); }
        const auto deadline = GetTickCount64() + 10000;
        for (;;) {
            bool complete;
            { std::lock_guard lock(Sub::RecordingMutex()); complete = resources->Complete() && !lease->begun; }
            if (complete) break;
            Require(GetTickCount64() < deadline, "inline actual network/worker timeout"); Sleep(1);
        }
        Require(!owner->failed && resources->Succeeded() != timeoutMode, "actual network/frozen timeout decision");
        if (!mochi) {
            LmxxfNrTimings physical {sizeof physical}; ok(api.GetTimings(context, &physical), "physical HIP completion");
            Require(physical.valid && physical.frame_id == 2, "side completion cannot precede actual HIP event completion");
        }
        const auto selected = timeoutMode ? originalHash : expected;
        Require(Hash(device.Get(), queue, resources->OutputTexture()) == selected, "inline output equals selected current-frame source");
        std::printf("%s actual inline replay=%u timeout=%u hash=%016llx PASS\n", mochi ? "Vulkan" : "HIP", replay, timeoutMode, selected);
    }
    // Saturated admission on a completed retained recording must preserve the
    // real mixed game batch, select original, then recover when a slot is free.
    {
        std::vector<std::shared_ptr<In::Resources>> reservations;
        const auto savedLimit = owner->inlineLimit;
        {
            std::lock_guard lock(Sub::RecordingMutex());
            owner->ReclaimInlineLocked();
            for (size_t n = 0; n < savedLimit; ++n) {
                auto held = MakeResources(device.Get(), colour.Get(),
                    static_cast<ID3D12Resource*>(job.private_output), nullptr, width, height, owner->inlinePipelines);
                Require(owner->AdmitInline(held), "reserve capacity for replay bypass"); reservations.push_back(held);
            }
            owner->inlineLimit = 1; // Shrinking must preserve old reservations too.
        }
        Ptr<ID3D12CommandAllocator> ordinaryAllocator; Ptr<ID3D12GraphicsCommandList> ordinary;
        Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&ordinaryAllocator)), "bypass ordinary allocator");
        Check(device->CreateCommandList(0, q.Type, ordinaryAllocator.Get(), nullptr, IID_PPV_ARGS(&ordinary)), "bypass ordinary list");
        Check(ordinary->Close(), "bypass ordinary close");
        ID3D12CommandList* batch[] {ordinary.Get(), proxy.Get()};
        const auto calls = batchCalls, lists = batchLists;
        const auto admitted = owner->inlineAdmitted, bypassed = owner->inlineBypassed;
        Sub::Hooks::ExecuteExpanded(game.Get(), 2, batch, nullptr, nullptr, TraceBatch);
        WaitQueue(device.Get(), game.Get());
        Require(batchCalls == calls + 1 && batchLists == lists + 2, "saturated replay preserves mixed native batch");
        Require(resources->Complete() && !resources->Succeeded() && !lease->begun && !owner->failed,
                "capacity bypass completes without runtime failure");
        Require(owner->inlineAdmitted == admitted && owner->inlineBypassed == bypassed + 1 &&
                owner->inlineInFlight.size() == savedLimit && owner->inlineEnqueueOrder.empty(), "bypass does not admit network work");
        Require(Hash(device.Get(), game.Get(), resources->OutputTexture()) == originalHash, "capacity bypass selects original pixels");
        if (!timeoutMode) Require(resources->LastSpinCount() == 1, "capacity bypass skips long wait");
        {
            std::lock_guard lock(Sub::RecordingMutex());
            for (auto& held : reservations) owner->ForgetUnstarted(held);
            owner->inlineLimit = savedLimit;
        }
        Check(proxy->ExecuteOn(other.Get()), "recover replay after freeing slots"); WaitQueue(device.Get(), other.Get());
        const auto until = GetTickCount64() + 10000;
        for (;;) {
            { std::lock_guard lock(Sub::RecordingMutex()); if (resources->Complete() && !lease->begun) break; }
            Require(GetTickCount64() < until, "recovered worker completion"); Sleep(1);
        }
        Require(!owner->failed && resources->Succeeded() != timeoutMode && owner->inlineAdmitted == admitted + 1,
                "replay recovers normal admission");
        Require(Hash(device.Get(), other.Get(), resources->OutputTexture()) == (timeoutMode ? originalHash : expected),
                "recovered replay equals split baseline");
        std::puts("capacity bypass: mixed batch, original pixels, shrink, bounded slots, recovery vs split PASS");
    }
    // Successful Reset can invalidate a submitted recording before its capture
    // reaches the GPU. The worker must still enqueue the captured job, certify
    // its End event, then invalidate/collect it after BOTH queues finish.
    Ptr<ID3D12Fence> gate; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "reset gate");
    Check(game->Wait(gate.Get(), 1), "block capture");
    Check(proxy->ExecuteOn(game.Get()), "submitted inline before Reset");
    const auto pendingSerial = lease->activeExecutionSerial;
    Require(proxy->ExecuteOn(game.Get()) == E_PENDING && lease->begun &&
            lease->activeExecutionSerial == pendingSerial && !owner->failed,
            "rejected overlapping replay does not mutate the pending worker execution");
    Ptr<ID3D12CommandAllocator> replacement;
    Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&replacement)), "Reset allocator");
    Check(proxy->Reset(replacement.Get(), nullptr), "Reset before capture");
    Require(lease->invalidated && lease->job && lease->begun, "Reset retains asynchronously executing runtime job");
    Check(gate->Signal(1), "unblock capture"); WaitQueue(device.Get(), game.Get());
    const auto deadline = GetTickCount64() + 10000;
    for (;;) {
        bool collected;
        { std::lock_guard lock(Sub::RecordingMutex()); Own::Collect(); collected = lease->job == nullptr; }
        if (collected) break;
        Require(GetTickCount64() < deadline, "inline Reset collection timeout"); Sleep(1);
    }
    Require(!owner->failed && resources->Complete(), "Reset allowed captured work to finish safely");
    Check(proxy->Close(), "replacement close");
    std::puts("inline Reset-before-capture and deferred invalidation/collection PASS");
    proxy.Reset(); native.Reset(); queries.Reset(); Own::Collect();
    Require(lease->job == nullptr, "invalidated inline lease collected after both queues");
    lease.reset(); resources.reset();
    // Fresh recordings, not just closed-list replay: the old implementation
    // created/retained a new runtime producer queue on every iteration.
    const unsigned count = (ahead && !temporalRequested) ? 0 : (large ? 30 : 120);
    temporalMode = mochi && temporalRequested;
    for (unsigned n = 0; n < count; ++n) {
        Ptr<ID3D12CommandAllocator> alloc; Ptr<ID3D12GraphicsCommandList> raw; Ptr<Sub::CommandListProxy> list;
        Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&alloc)), "continuous allocator");
        Check(device->CreateCommandList(0, q.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&raw)), "continuous native");
        Check(Sub::CommandListProxy::Create(device.Get(), alloc.Get(), raw.Get(), &list), "continuous proxy");
        auto next = prepare(list.Get(), 10 + n);
        auto in = MakeResources(device.Get(), colour.Get(), static_cast<ID3D12Resource*>(next.private_output), nullptr,
                                                width, height, owner->inlinePipelines);
        Require(in->SideQueue() == owner->inlinePipelines->sideQueue.Get(), "stable producer queue across new recordings");
        {
            std::lock_guard lock(Sub::RecordingMutex());
            ID3D12Resource* sources[] {colour.Get()}; D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            Check(list->BeginInlineState(1, sources, states), "continuous state");
            auto nextLease = Own::Attach(owner, next.handle, list.Get(), in); Require(bool(nextLease), "continuous lease");
            ok(api.RecordInputs(context, next.handle, list.Get()), "continuous inputs");
            in->RecordCapture(list.Get()); in->RecordWait(list.Get(), timeoutMode ? 0 : waitSlices, waitIterations);
            ok(api.RecordOutputs(context, next.handle, list.Get()), "continuous outputs");
            in->RecordSelect(list.Get(), states[0]); nextLease->ready = true;
            list->EndInlineState(); Check(list->Close(), "continuous close");
        }
        Check(list->ExecuteOn(game.Get()), "continuous execute"); WaitQueue(device.Get(), game.Get());
        const auto until = GetTickCount64() + 10000;
        while (!in->Complete()) { Require(GetTickCount64() < until, "continuous completion"); Sleep(1); }
        Require(in->Succeeded() != timeoutMode, "continuous selected output");
        if (!temporalMode && (n == 0 || n + 1 == count))
            Require(Hash(device.Get(), game.Get(), in->OutputTexture()) == (timeoutMode ? originalHash : expected), "continuous pixels");
        list.Reset(); raw.Reset();
        Own::Collect(); Require(owner->liveRecordings == 0, "completed invalidated recordings do not accumulate");
    }
    if (count)
        std::printf("%u fresh inline recordings at %ux%u: stable queue and bounded leases PASS\n", count, width, height);
    if (threePasses && !timeoutMode) {
        auto getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll, "MochizukiNrGetInfo"));
        MochizukiNrInfo info {}; info.struct_size = sizeof info;
        Require(getInfo && getInfo(context, &info) == 0, "three-pass info");
        Require(info.max_passes >= 3 && info.network_dispatches > 250,
                "three-pass request fell back to fewer network executions");
        std::printf("inline three passes: capacity=%u dispatches=%u PASS\n", info.max_passes, info.network_dispatches);
    }
    if (temporalMode && !timeoutMode) {
        auto getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll, "MochizukiNrGetInfo"));
        MochizukiNrInfo info {}; info.struct_size = sizeof info; Require(getInfo && getInfo(context, &info) == 0, "history info");
        Require(info.history_consumed_pct >= 80, "history survives continuous new inline recordings");
        std::printf("inline temporal history consumed=%u%% PASS\n", info.history_consumed_pct);
    }
    if (ahead && !temporalRequested) {
        const unsigned kPatterns = mochi ? 4 : 5;
        owner->inlineLimit = kPatterns;
        Ptr<ID3D12Resource> patterns[5];
        uint64_t originalHashByPattern[5] {};
        uint64_t neuralHashByPattern[5] {};
        for (unsigned i = 0; i < kPatterns; ++i) {
            Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&patterns[i])), "ahead colour");
            UploadColorPattern(device.Get(), game.Get(), patterns[i].Get(), PatternOptions{i + 1});
            originalHashByPattern[i] = Hash(device.Get(), game.Get(), patterns[i].Get());
            for (unsigned earlier = 0; earlier < i; ++earlier)
                Require(originalHashByPattern[i] != originalHashByPattern[earlier], "ahead patterns are distinguishable");
        }
        auto retire = [&](Ptr<Sub::CommandListProxy>& list, std::shared_ptr<Own::Lease>& recorded,
                           const std::shared_ptr<In::Resources>& frame) {
            const auto until = GetTickCount64() + 20000;
            for (;;) {
                bool done;
                {
                    std::lock_guard lock(Sub::RecordingMutex());
                    done = frame->Complete() && !recorded->begun;
                }
                if (done) break;
                Require(GetTickCount64() < until, "ahead completion");
                Sleep(1);
            }
            Ptr<ID3D12CommandAllocator> replacement;
            Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&replacement)), "ahead reset allocator");
            Check(list->Reset(replacement.Get(), nullptr), "ahead reset");
            Check(list->Close(), "ahead replacement close");
            recorded.reset();
            const auto collectUntil = GetTickCount64() + 10000;
            for (;;) {
                {
                    std::lock_guard lock(Sub::RecordingMutex());
                    Own::Collect();
                }
                if (owner->liveRecordings.load() == 0) break;
                Require(GetTickCount64() < collectUntil, "ahead collection");
                Sleep(1);
            }
        };
        if (!timeoutMode) {
            for (unsigned i = 0; i < kPatterns; ++i) {
                Ptr<ID3D12CommandAllocator> alloc; Ptr<ID3D12GraphicsCommandList> raw; Ptr<Sub::CommandListProxy> list;
                Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&alloc)), "ahead baseline allocator");
                Check(device->CreateCommandList(0, q.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&raw)), "ahead baseline native");
                Check(Sub::CommandListProxy::Create(device.Get(), alloc.Get(), raw.Get(), &list), "ahead baseline proxy");
                auto job = prepare(list.Get(), 1000 + i, patterns[i].Get());
                auto resources = MakeResources(device.Get(), patterns[i].Get(),
                    static_cast<ID3D12Resource*>(job.private_output), nullptr, width, height, owner->inlinePipelines);
                std::shared_ptr<Own::Lease> recorded;
                {
                    std::lock_guard lock(Sub::RecordingMutex());
                    ID3D12Resource* sources[] {patterns[i].Get()};
                    D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
                    Check(list->BeginInlineState(1, sources, states), "ahead baseline state");
                    recorded = Own::Attach(owner, job.handle, list.Get(), resources);
                    Require(bool(recorded), "ahead baseline lease");
                    ok(api.RecordInputs(context, job.handle, list.Get()), "ahead baseline inputs");
                    resources->RecordCapture(list.Get()); resources->RecordWait(list.Get(), waitSlices, waitIterations);
                    ok(api.RecordOutputs(context, job.handle, list.Get()), "ahead baseline outputs");
                    resources->RecordSelect(list.Get(), states[0]); recorded->ready = true;
                    list->EndInlineState(); Check(list->Close(), "ahead baseline close");
                }
                Check(list->ExecuteOn(game.Get()), "ahead baseline execute");
                WaitQueue(device.Get(), game.Get());
                neuralHashByPattern[i] = Hash(device.Get(), game.Get(), resources->OutputTexture());
                Require(neuralHashByPattern[i] != originalHashByPattern[i], "ahead baseline changed the frame");
                retire(list, recorded, resources);
            }
        }
        auto burst = [&](unsigned n) {
            Require(n >= 2 && n <= kPatterns, "ahead depth");
            struct Frame {
                Ptr<ID3D12CommandAllocator> alloc;
                Ptr<ID3D12GraphicsCommandList> raw;
                Ptr<Sub::CommandListProxy> list;
                std::shared_ptr<In::Resources> resources;
                std::shared_ptr<Own::Lease> lease;
            };
            Frame frames[5] {};
            for (unsigned i = 0; i < n; ++i) {
                Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&frames[i].alloc)), "ahead allocator");
                Check(device->CreateCommandList(0, q.Type, frames[i].alloc.Get(), nullptr, IID_PPV_ARGS(&frames[i].raw)), "ahead native");
                Check(Sub::CommandListProxy::Create(device.Get(), frames[i].alloc.Get(), frames[i].raw.Get(), &frames[i].list), "ahead proxy");
                auto job = prepare(frames[i].list.Get(), 2000 + n * 10 + i, patterns[i].Get());
                frames[i].resources = MakeResources(device.Get(), patterns[i].Get(),
                    static_cast<ID3D12Resource*>(job.private_output), nullptr, width, height, owner->inlinePipelines);
                {
                    std::lock_guard lock(Sub::RecordingMutex());
                    ID3D12Resource* sources[] {patterns[i].Get()};
                    D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
                    Check(frames[i].list->BeginInlineState(1, sources, states), "ahead state");
                    frames[i].lease = Own::Attach(owner, job.handle, frames[i].list.Get(), frames[i].resources);
                    Require(bool(frames[i].lease), "ahead lease");
                    ok(api.RecordInputs(context, job.handle, frames[i].list.Get()), "ahead inputs");
                    frames[i].resources->RecordCapture(frames[i].list.Get());
                    frames[i].resources->RecordWait(frames[i].list.Get(), timeoutMode ? 0 : waitSlices, waitIterations);
                    ok(api.RecordOutputs(context, job.handle, frames[i].list.Get()), "ahead outputs");
                    frames[i].resources->RecordSelect(frames[i].list.Get(), states[0]);
                    frames[i].lease->ready = true;
                    frames[i].list->EndInlineState();
                    Check(frames[i].list->Close(), "ahead close");
                }
            }
            if (n == kPatterns) {
                if (mochi) {
                    MochizukiNrFrameInfo extra {}; extra.struct_size = sizeof extra; extra.frame_id = 9000;
                    extra.color = patterns[0].Get(); extra.color_width = width; extra.color_height = height;
                    extra.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    extra.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION;
                    extra.transfer_strength = extra.color_strength = extra.model_scale = 1; extra.passes = threePasses ? 3 : 1;
                    LmxxfNrJob rejected {}; rejected.struct_size = sizeof rejected;
                    const int rc = mzPrepare(context, &extra, &rejected);
                    char message[256] {}; api.GetLastError(message, sizeof message);
                    Require(rc == LMXXF_NR_UNAVAILABLE && std::strstr(message, "inline slots are full"),
                            "fifth ahead recording is rejected before history advances");
                } else {
                    Ptr<ID3D12CommandAllocator> alloc; Ptr<ID3D12GraphicsCommandList> raw; Ptr<Sub::CommandListProxy> list;
                    Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&alloc)), "fifth allocator");
                    Check(device->CreateCommandList(0, q.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&raw)), "fifth native");
                    Check(Sub::CommandListProxy::Create(device.Get(), alloc.Get(), raw.Get(), &list), "fifth proxy");
                    auto job = prepare(list.Get(), 9000, patterns[0].Get());
                    auto resources = MakeResources(device.Get(), patterns[0].Get(),
                        static_cast<ID3D12Resource*>(job.private_output), nullptr, width, height, owner->inlinePipelines);
                    std::shared_ptr<Own::Lease> rejected;
                    {
                        std::lock_guard lock(Sub::RecordingMutex());
                        rejected = Own::Attach(owner, job.handle, list.Get(), resources);
                    }
                    Require(!rejected, "fifth ahead attach is rejected while four reservations are open");
                    list.Reset(); raw.Reset();
                }
            }
            for (unsigned i = 0; i < n; ++i)
                Check(frames[i].list->ExecuteOn(game.Get()), "ahead execute without draining the queue");
            WaitQueue(device.Get(), game.Get());
            for (unsigned i = 0; i < n; ++i) {
                const auto until = GetTickCount64() + 20000;
                for (;;) {
                    bool done;
                    {
                        std::lock_guard lock(Sub::RecordingMutex());
                        done = frames[i].resources->Complete() && !frames[i].lease->begun;
                    }
                    if (done) break;
                    Require(GetTickCount64() < until, "ahead completion");
                    Sleep(1);
                }
                const auto selected = timeoutMode ? originalHashByPattern[i] : neuralHashByPattern[i];
                Require(Hash(device.Get(), game.Get(), frames[i].resources->OutputTexture()) == selected,
                        "ahead output matches that frame's own colour");
            }
            for (unsigned i = 0; i < n; ++i) {
                Ptr<ID3D12CommandAllocator> replacement;
                Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&replacement)), "ahead reset allocator");
                Check(frames[i].list->Reset(replacement.Get(), nullptr), "ahead reset");
                Check(frames[i].list->Close(), "ahead replacement close");
                frames[i].lease.reset();
            }
            const auto collectUntil = GetTickCount64() + 10000;
            for (;;) {
                {
                    std::lock_guard lock(Sub::RecordingMutex());
                    Own::Collect();
                }
                if (owner->liveRecordings.load() == 0) break;
                Require(GetTickCount64() < collectUntil, "ahead collection");
                Sleep(1);
            }
            std::printf("inline ahead %u timeout=%u PASS\n", n, timeoutMode);
        };
        burst(2); burst(3); burst(4); if (!mochi) burst(5);
        // Game order: submit frame 0, then record frame 1 while that execution is still open.
        {
            struct Frame {
                Ptr<ID3D12CommandAllocator> alloc;
                Ptr<ID3D12GraphicsCommandList> raw;
                Ptr<Sub::CommandListProxy> list;
                std::shared_ptr<In::Resources> resources;
                std::shared_ptr<Own::Lease> lease;
            };
            Frame frames[2] {};
            auto recordOne = [&](unsigned i) {
                Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&frames[i].alloc)), "stagger allocator");
                Check(device->CreateCommandList(0, q.Type, frames[i].alloc.Get(), nullptr, IID_PPV_ARGS(&frames[i].raw)), "stagger native");
                Check(Sub::CommandListProxy::Create(device.Get(), frames[i].alloc.Get(), frames[i].raw.Get(), &frames[i].list), "stagger proxy");
                auto job = prepare(frames[i].list.Get(), 3000 + i, patterns[i].Get());
                frames[i].resources = MakeResources(device.Get(), patterns[i].Get(),
                    static_cast<ID3D12Resource*>(job.private_output), nullptr, width, height, owner->inlinePipelines);
                std::lock_guard lock(Sub::RecordingMutex());
                ID3D12Resource* sources[] {patterns[i].Get()};
                D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
                Check(frames[i].list->BeginInlineState(1, sources, states), "stagger state");
                frames[i].lease = Own::Attach(owner, job.handle, frames[i].list.Get(), frames[i].resources);
                Require(bool(frames[i].lease), "stagger lease");
                ok(api.RecordInputs(context, job.handle, frames[i].list.Get()), "stagger inputs");
                frames[i].resources->RecordCapture(frames[i].list.Get());
                frames[i].resources->RecordWait(frames[i].list.Get(), timeoutMode ? 0 : waitSlices, waitIterations);
                ok(api.RecordOutputs(context, job.handle, frames[i].list.Get()), "stagger outputs");
                frames[i].resources->RecordSelect(frames[i].list.Get(), states[0]);
                frames[i].lease->ready = true;
                frames[i].list->EndInlineState();
                Check(frames[i].list->Close(), "stagger close");
            };
            recordOne(0);
            Check(frames[0].list->ExecuteOn(game.Get()), "stagger execute first");
            recordOne(1);
            Check(frames[1].list->ExecuteOn(game.Get()), "stagger execute second");
            WaitQueue(device.Get(), game.Get());
            for (unsigned i = 0; i < 2; ++i) {
                const auto until = GetTickCount64() + 20000;
                for (;;) {
                    bool done;
                    {
                        std::lock_guard lock(Sub::RecordingMutex());
                        done = frames[i].resources->Complete() && !frames[i].lease->begun;
                    }
                    if (done) break;
                    Require(GetTickCount64() < until, "stagger completion");
                    Sleep(1);
                }
                const auto selected = timeoutMode ? originalHashByPattern[i] : neuralHashByPattern[i];
                Require(Hash(device.Get(), game.Get(), frames[i].resources->OutputTexture()) == selected,
                        "stagger output matches that frame's own colour");
            }
            for (unsigned i = 0; i < 2; ++i) {
                Ptr<ID3D12CommandAllocator> replacement;
                Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&replacement)), "stagger reset allocator");
                Check(frames[i].list->Reset(replacement.Get(), nullptr), "stagger reset");
                Check(frames[i].list->Close(), "stagger replacement close");
                frames[i].lease.reset();
            }
            const auto collectUntil = GetTickCount64() + 10000;
            for (;;) {
                {
                    std::lock_guard lock(Sub::RecordingMutex());
                    Own::Collect();
                }
                if (owner->liveRecordings.load() == 0) break;
                Require(GetTickCount64() < collectUntil, "stagger collection");
                Sleep(1);
            }
            std::printf("inline stagger timeout=%u PASS\n", timeoutMode);
        }

    }
    owner.reset();
    if (gReuse) std::printf("inline reuse hits=%u\n", gReuseHits);
    gPool.clear(); FreeLibrary(dll); std::puts("actual inline runtime PASS"); return 0;
}

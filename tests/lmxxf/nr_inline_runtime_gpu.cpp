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
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const bool mochi = argc > 3 && std::strcmp(argv[3], "--mochizuki") == 0;
    bool timeoutMode = false;
    bool large = false;
    bool temporalRequested = false, temporalMode = false;
    for (int i = 3; i < argc; ++i) timeoutMode |= std::strcmp(argv[i], "--timeout") == 0;
    for (int i = 3; i < argc; ++i) large |= std::strcmp(argv[i], "--large") == 0;
    for (int i = 3; i < argc; ++i) temporalRequested |= std::strcmp(argv[i], "--temporal") == 0;
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
        if (d.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
    }
    Require(bool(device), "AMD device");
    Ptr<ID3D12CommandQueue> game, other; D3D12_COMMAND_QUEUE_DESC q {};
    Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&game)), "game queue");
    Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&other)), "replay queue");
    const auto assets = Widen(argv[2]);
    LmxxfNrCreateInfo ci {sizeof ci, device.Get(), game.Get(), assets.c_str(),
                         LMXXF_NR_CREATE_FLAG_RECORDING_LEASES | LMXXF_NR_CREATE_FLAG_INLINE_QUEUE};
    void* context = nullptr; ok(api.Create(&ci, &context), "Create"); ok(api.PrepareSession(context), "PrepareSession");
    auto owner = Own::SessionOwner::Create(api, context); Require(bool(owner), "module/session owner");
    if (!mochi) { LmxxfNrTimings timing {sizeof timing}; ok(api.GetTimings(context, &timing), "enable physical HIP event timing"); }
    const UINT width = large ? 2560 : (mochi ? 256 : 1280), height = large ? 1440 : (mochi ? 256 : 720);
    D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    Ptr<ID3D12Resource> colour;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&colour)), "colour");
    UploadColorPattern(device.Get(), game.Get(), colour.Get());
    Ptr<ID3D12Resource> motion;
    if (mochi && temporalRequested) {
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
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
    auto prepare = [&](ID3D12GraphicsCommandList* list, UINT64 tag = 1) {
        LmxxfNrJob job {}; job.struct_size = sizeof job;
        const auto deadline = GetTickCount64() + 180000;
        for (;;) {
            int rc;
            if (mochi) {
                Require(mzPrepare != nullptr, "Mochizuki prepare export");
                MochizukiNrFrameInfo f {}; f.struct_size = sizeof f; f.frame_id = tag;
                f.color = colour.Get(); f.color_width = width; f.color_height = height;
                f.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                f.flags = LMXXF_NR_FRAME_FLAG_STRENGTH; f.transfer_strength = f.color_strength = f.model_scale = 1; f.passes = 1;
                if (temporalMode) {
                    f.flags |= MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL; f.motion = motion.Get();
                    f.motion_width = width; f.motion_height = height; f.motion_scale_x = f.motion_scale_y = 1;
                    f.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
                rc = mzPrepare(context, &f, &job);
            } else {
                LmxxfNrFrameInfo f {}; f.struct_size = sizeof f; f.frame_id = tag; f.command_list = list;
                f.color = colour.Get(); f.color_width = width; f.color_height = height;
                f.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                f.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
                f.transfer_strength = f.color_strength = f.model_scale = f.paper_white = f.pre_exposure = f.exposure_scale = 1;
                rc = api.PrepareFrame(context, &f, &job);
            }
            if (!rc) return job;
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
    auto baseline = prepare(producer.Get());
    ok(api.RecordInputs(context, baseline.handle, producer.Get()), "baseline inputs"); Check(producer->Close(), "producer close");
    ok(api.RecordOutputs(context, baseline.handle, consumer.Get()), "baseline outputs"); Check(consumer->Close(), "consumer close");
    ok(api.BeginRecordingExecution(context, baseline.handle, game.Get()), "baseline begin");
    In::Execute(game.Get(), producer.Get()); ok(api.EnqueueHip(context, baseline.handle, game.Get()), "baseline enqueue");
    In::Execute(game.Get(), consumer.Get());
    Ptr<ID3D12Fence> tail; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tail)), "tail fence");
    Check(game->Signal(tail.Get(), 1), "baseline signal");
    ok(api.EndRecordingExecution(context, baseline.handle, game.Get(), 3, tail.Get(), 1, S_OK), "baseline end");
    WaitQueue(device.Get(), game.Get());
    const auto expected = HashTexture(device.Get(), game.Get(), static_cast<ID3D12Resource*>(baseline.private_output));
    const auto originalHash = HashTexture(device.Get(), game.Get(), colour.Get());
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
    auto resources = std::make_shared<In::Resources>(device.Get(), colour.Get(), static_cast<ID3D12Resource*>(job.private_output), nullptr, width, height);
    owner->inlinePipelines = resources->PipelineCache();
    std::shared_ptr<Own::Lease> lease;
    {
        std::lock_guard lock(Sub::RecordingMutex());
        ID3D12Resource* sources[] {colour.Get()}; D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        Check(proxy->BeginInlineState(1, sources, states), "state scope");
        lease = Own::Attach(owner, job.handle, proxy.Get(), resources); Require(bool(lease), "inline ownership");
        ok(api.RecordInputs(context, job.handle, proxy.Get()), "inline inputs");
        resources->RecordCapture(proxy.Get()); resources->RecordWait(proxy.Get(), timeoutMode ? 0 : 64);
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
        Require(HashTexture(device.Get(), queue, resources->OutputTexture()) == selected, "inline output equals selected current-frame source");
        std::printf("%s actual inline replay=%u timeout=%u hash=%016llx PASS\n", mochi ? "Vulkan" : "HIP", replay, timeoutMode, selected);
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
    const unsigned count = large ? 30 : 120;
    temporalMode = mochi && temporalRequested;
    for (unsigned n = 0; n < count; ++n) {
        Ptr<ID3D12CommandAllocator> alloc; Ptr<ID3D12GraphicsCommandList> raw; Ptr<Sub::CommandListProxy> list;
        Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&alloc)), "continuous allocator");
        Check(device->CreateCommandList(0, q.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&raw)), "continuous native");
        Check(Sub::CommandListProxy::Create(device.Get(), alloc.Get(), raw.Get(), &list), "continuous proxy");
        auto next = prepare(list.Get(), 10 + n);
        auto in = std::make_shared<In::Resources>(device.Get(), colour.Get(), static_cast<ID3D12Resource*>(next.private_output), nullptr,
                                                width, height, owner->inlinePipelines);
        Require(in->SideQueue() == owner->inlinePipelines->sideQueue.Get(), "stable producer queue across new recordings");
        {
            std::lock_guard lock(Sub::RecordingMutex());
            ID3D12Resource* sources[] {colour.Get()}; D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            Check(list->BeginInlineState(1, sources, states), "continuous state");
            auto nextLease = Own::Attach(owner, next.handle, list.Get(), in); Require(bool(nextLease), "continuous lease");
            ok(api.RecordInputs(context, next.handle, list.Get()), "continuous inputs");
            in->RecordCapture(list.Get()); in->RecordWait(list.Get(), timeoutMode ? 0 : 64);
            ok(api.RecordOutputs(context, next.handle, list.Get()), "continuous outputs");
            in->RecordSelect(list.Get(), states[0]); nextLease->ready = true;
            list->EndInlineState(); Check(list->Close(), "continuous close");
        }
        Check(list->ExecuteOn(game.Get()), "continuous execute"); WaitQueue(device.Get(), game.Get());
        const auto until = GetTickCount64() + 10000;
        while (!in->Complete()) { Require(GetTickCount64() < until, "continuous completion"); Sleep(1); }
        Require(in->Succeeded() != timeoutMode, "continuous selected output");
        if (!temporalMode && (n == 0 || n + 1 == count))
            Require(HashTexture(device.Get(), game.Get(), in->OutputTexture()) == (timeoutMode ? originalHash : expected), "continuous pixels");
        list.Reset(); raw.Reset();
        Own::Collect(); Require(owner->liveRecordings == 0, "completed invalidated recordings do not accumulate");
    }
    std::printf("%u fresh inline recordings at %ux%u: stable queue and bounded leases PASS\n", count, width, height);
    if (temporalMode && !timeoutMode) {
        auto getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll, "MochizukiNrGetInfo"));
        MochizukiNrInfo info {}; info.struct_size = sizeof info; Require(getInfo && getInfo(context, &info) == 0, "history info");
        Require(info.history_consumed_pct >= 80, "history survives continuous new inline recordings");
        std::printf("inline temporal history consumed=%u%% PASS\n", info.history_consumed_pct);
    }
    owner.reset();
    FreeLibrary(dll); std::puts("actual inline runtime PASS"); return 0;
}

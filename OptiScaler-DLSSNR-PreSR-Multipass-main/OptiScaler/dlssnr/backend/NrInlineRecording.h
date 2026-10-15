#pragma once
#include "../submission/CommandListProxy.h"
#include "../NrFrameDiag.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <thread>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include <mutex>
#include <condition_variable>

namespace DlssNr::Backend::InlineRecording
{
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
// Mochizuki's graphics/compute segments regress with longer wait dispatches.
inline constexpr UINT kWaitSlices = 64;
inline constexpr UINT kWaitIterations = 32768;
inline constexpr UINT kCaptureWatchSlices = 4;
// Post-claim capture wait cap: 4x the game-side wait budget (kWaitSlices x kWaitIterations polls).
inline constexpr UINT kClaimWaitSlices = 4 * kWaitSlices;
// Post-claim abort channel: a CPU-mapped word the GPU wait polls every kAbortPollStride iterations.
// A watchdog thread (never the game thread) writes the wait's generation into it once the wait has
// run kClaimAbortMs after the game queue reached the frame; the wait then ends unseen (bypass).
inline constexpr UINT kAbortPollStride = 256;
inline UINT gClaimAbortMs = 100; // tests may lower it
// NrAdaptiveWaitBudget (default off): the post-claim wait's dispatch count follows a measured rate.
// Each armed wait is timed by two GPU timestamps on the side queue; iterations / ms feeds an EMA. The
// cap is kAdaptiveTargetMs at that rate (bounded to [kAdaptiveMinSlices, kClaimWaitSlices]). A wait
// that ends without the capture (cap or abort) halves the slices; kAdaptiveCleanToGrow clean waits
// in a row double them back up to the calibrated cap. Read on retirement only: no CPU wait.
inline constexpr double kAdaptiveTargetMs = 4.0;
inline constexpr UINT kAdaptiveMinSlices = 8;
inline constexpr UINT kAdaptiveCleanToGrow = 64;
struct AdaptiveWaitBudget {
    std::atomic<UINT> slices {kClaimWaitSlices};
    std::atomic<double> itersPerMs {0.0};
    std::atomic<UINT> clean {0}, samples {0}, shrinks {0}, grows {0};
    UINT Cap() const {
        const double rate = itersPerMs.load(std::memory_order_relaxed);
        if (rate <= 0.0) return kClaimWaitSlices; // not calibrated yet: the fixed cap
        const double wanted = kAdaptiveTargetMs * rate / double(kWaitIterations);
        return UINT(std::clamp(std::ceil(wanted), double(kAdaptiveMinSlices), double(kClaimWaitSlices)));
    }
    UINT Slices() const { return std::clamp(slices.load(std::memory_order_relaxed), kAdaptiveMinSlices, kClaimWaitSlices); }
    void Sample(UINT iterations, double ms) {
        if (iterations < kAbortPollStride || ms <= 0.01) return;
        const double rate = double(iterations) / ms, old = itersPerMs.load(std::memory_order_relaxed);
        itersPerMs.store(old > 0.0 ? old * 0.875 + rate * 0.125 : rate, std::memory_order_relaxed);
        samples.fetch_add(1, std::memory_order_relaxed);
    }
    void Timeout() {
        clean.store(0, std::memory_order_relaxed);
        const UINT now = Slices(), next = (std::max)(kAdaptiveMinSlices, now / 2);
        if (next != now) { slices.store(next, std::memory_order_relaxed); Changed(true); }
    }
    void Clean() {
        if (clean.fetch_add(1, std::memory_order_relaxed) + 1 < kAdaptiveCleanToGrow) return;
        clean.store(0, std::memory_order_relaxed);
        const UINT now = Slices(), cap = Cap(), next = (std::min)(cap, (std::max)(now * 2, kAdaptiveMinSlices));
        if (next != now) { slices.store(next, std::memory_order_relaxed); Changed(false); }
    }
    // Counted only (this header stays logger-free for the standalone tests).
    void Changed(bool shrink) { (shrink ? shrinks : grows).fetch_add(1, std::memory_order_relaxed); }
};
inline AdaptiveWaitBudget& AdaptiveBudget() { static AdaptiveWaitBudget budget; return budget; }
struct AbortChannel {
    Ptr<ID3D12Resource> buffer; // custom heap, CPU write-combine, L0; persistently mapped
    volatile LONG* word = nullptr;
    std::atomic<UINT> aborts {0};
};
// Owned thread, started on the first Watch and parked on a condition variable while idle. Stop() joins it
// from the explicit backend Shutdown / process-exit path (never from DllMain: during loader shutdown it only
// signals, since the thread cannot exit while the loader lock is held). The instance is intentionally leaked,
// so no static destructor races a running thread or a join at unload. Watch never waits on the GPU.
class AbortWatchdog {
    struct Entry { std::shared_ptr<AbortChannel> ch; Ptr<ID3D12Fence> arm, side; UINT64 armValue, sideDone; UINT gen, ms;
                   std::chrono::steady_clock::time_point deadline {}; bool started = false; };
    std::mutex m; std::condition_variable cv; std::vector<Entry> entries; std::thread worker; bool stopping = false;
    void Run() {
        std::unique_lock lock(m);
        while (!stopping) {
            if (entries.empty()) { cv.wait(lock, [this] { return stopping || !entries.empty(); }); continue; }
            cv.wait_for(lock, std::chrono::milliseconds(4), [this] { return stopping; });
            if (stopping) break;
            const auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < entries.size();) {
                auto& e = entries[i]; bool drop = false;
                const auto sideDone = e.side->GetCompletedValue();
                if (sideDone == UINT64_MAX || sideDone >= e.sideDone) drop = true; // arm list retired (or device lost)
                else if (!e.started) { if (e.arm->GetCompletedValue() >= e.armValue) { e.started = true; e.deadline = now + std::chrono::milliseconds(e.ms); } }
                else if (now >= e.deadline) {
                    InterlockedExchange(e.ch->word, LONG(e.gen)); MemoryBarrier(); e.ch->aborts.fetch_add(1); drop = true;
                }
                if (drop) { entries[i] = std::move(entries.back()); entries.pop_back(); } else ++i;
            }
        }
        entries.clear(); // release fences/channels on the worker; a pending wait still ends at its cap
    }
    static bool LoaderShutdown() {
        using Fn = BOOLEAN(NTAPI*)();
        static const auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlDllShutdownInProgress")));
        return fn && fn();
    }
public:
    static AbortWatchdog& Get() { static AbortWatchdog* w = new AbortWatchdog; return *w; } // leaked on purpose
    void Watch(std::shared_ptr<AbortChannel> ch, ID3D12Fence* arm, UINT64 armValue, ID3D12Fence* side, UINT64 sideDone, UINT gen, UINT ms) {
        {
            std::lock_guard lock(m);
            if (stopping) return; // shut down: the wait keeps its cap
            entries.push_back({std::move(ch), arm, side, armValue, sideDone, gen, ms});
            if (!worker.joinable()) worker = std::thread([this] { Run(); });
        }
        cv.notify_one();
    }
    bool Running() { std::lock_guard lock(m); return worker.joinable() && !stopping; }
    size_t Pending() { std::lock_guard lock(m); return entries.size(); }
    // Idempotent. Joins unless called on the worker or during loader shutdown (then it only signals and
    // detaches, and stays stopped; the process is ending). After a join, Watch restarts a fresh worker.
    void Stop() {
        std::thread t;
        { std::lock_guard lock(m); if (!worker.joinable()) return; stopping = true; t = std::move(worker); }
        cv.notify_all();
        if (t.get_id() == std::this_thread::get_id() || LoaderShutdown()) { t.detach(); return; } // stays stopped
        t.join();
        std::lock_guard lock(m); stopping = false; entries.clear();
    }
};
// Explicit shutdown hook (backend Shutdown / process exit). Safe to call repeatedly.
inline void StopAbortWatchdog() { AbortWatchdog::Get().Stop(); }
// NrGpuHandoff: after closing the input gate, a game that already claimed it is finishing its
// input copy. The side queue then waits on the GPU for that capture (op 14) before the network
// starts: no budget can release the network while the copy is still in flight. Termination needs
// only game-queue progress: after its claim the game list runs just the input copies and the
// capture store, none of which depend on the side queue.
// lmxxf benefits from fewer dispatch/barrier pairs with the same total work.
// Neither schedule is a portable wall-clock timeout or concurrent-progress guarantee.
inline constexpr UINT kLmxxfWaitSlices = 32;
inline constexpr UINT kLmxxfWaitIterations = 65536;
class StateScope {
    Ptr<Submission::IRecordingResources> state;
    std::string reason = "recording_proxy_unavailable";
public:
    const std::string& Reason() const { return reason; }
    template<class Frame> void TraceFailure(const char* backend, uint64_t frameId,
                                            ID3D12GraphicsCommandList* list, const Frame& frame,
                                            bool motion = true, bool depth = true, bool exposure = true) const {
        LOG_WARN("{} NR input rejected: frame={} list={:p} reason={}",
                 backend, frameId, static_cast<void*>(list), reason);
        Ptr<Submission::IRecordingResources> diagnostic;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&diagnostic)))) return;
        diagnostic->TraceNrInputState(backend, frameId, "colour", frame.colour, frame.colourState);
        if (motion) diagnostic->TraceNrInputState(backend, frameId, "motion", frame.motion, frame.motionState);
        if (depth) diagnostic->TraceNrInputState(backend, frameId, "depth", frame.depth, frame.depthState);
        if (exposure) diagnostic->TraceNrInputState(backend, frameId, "exposure", frame.exposure, frame.exposureState);
    }
    template<class Frame> HRESULT Begin(ID3D12GraphicsCommandList* list, Frame& frame,
                                       bool motion = true, bool depth = true, bool exposure = true,
                                       bool split = false) {
        HRESULT hr = list->QueryInterface(IID_PPV_ARGS(&state));
        if (FAILED(hr)) return hr;
        if (!split && !state->CanRecordInline()) { reason = state->InlineRejectionReason(); state.Reset(); return E_UNEXPECTED; }
        ID3D12Resource* resources[] {frame.colour, motion ? frame.motion : nullptr,
                                   depth ? frame.depth : nullptr, exposure ? frame.exposure : nullptr};
        D3D12_RESOURCE_STATES states[] {frame.colourState, frame.motionState, frame.depthState, frame.exposureState};
        hr = split ? state->BeginSplitState(4, resources, states) : state->BeginInlineState(4, resources, states);
        if (FAILED(hr)) { reason = state->InlineRejectionReason(); state.Reset(); return hr; }
        frame.colourState = states[0]; frame.motionState = states[1]; frame.depthState = states[2]; frame.exposureState = states[3];
        return S_OK;
    }
    ~StateScope() { if (state) state->EndInlineState(); }
};
inline void Check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        char code[16]; std::snprintf(code, sizeof code, "0x%08X", unsigned(hr));
        throw std::runtime_error(std::string(operation) + " HRESULT=" + code);
    }
}
inline void Execute(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list) {
    ID3D12CommandList* native = list;
    Submission::LogicalExecuteScope scope;
    if (Submission::g_rawExecuteCommandLists) Submission::g_rawExecuteCommandLists(queue, 1, &native);
    else queue->ExecuteCommandLists(1, &native);
}
inline void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
inline void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b {}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = resource;
    list->ResourceBarrier(1, &b);
}
inline Ptr<ID3DBlob> Compile(const char* source, const char* entry) {
    Ptr<ID3DBlob> code, error;
    const auto hr = D3DCompile(source, std::strlen(source), "NR inline", nullptr, nullptr, entry,
                             "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (FAILED(hr)) throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "inline shader compile");
    return code;
}

// All polling is between D3D12 queues on the same device. HIP/Vulkan retain their
// existing external-fence handoff on the side queue; no cross-API shader flag is assumed.
// Bounded slices cap shader work; they do not guarantee side-queue scheduling progress.
struct Pipelines {
    Ptr<ID3D12Device> device;
    // Session-owned queue identity; recording-local buffers/fences remain independent.
    Ptr<ID3D12CommandQueue> sideQueue;
    Ptr<ID3D12RootSignature> controlRoot, composeRoot;
    Ptr<ID3D12PipelineState> controlPso, composePso;
};
class Resources
{
    std::shared_ptr<Pipelines> pipelines;
    Ptr<ID3D12Device> device;
    Ptr<ID3D12CommandQueue> side;
    Ptr<ID3D12CommandAllocator> sampleAlloc, doneAlloc;
    Ptr<ID3D12GraphicsCommandList> sampleList, doneList;
    Ptr<ID3D12RootSignature> controlRoot, composeRoot;
    Ptr<ID3D12PipelineState> controlPso, composePso;
    Ptr<ID3D12Resource> control, capture, sample, readback, decision, decisionReadback;
    // NrGpuHandoff input gate predicate (game list): nonzero = the game claimed the gate, run the input copies.
    Ptr<ID3D12Resource> inputGate;
    bool inputGated = false;
    Ptr<ID3D12Fence> sideFence, gameFence;
    // NrGpuHandoff: the game queue signals armFence just before this list runs (BeforeExecute); the side
    // queue waits on it (no spin while the game is still on earlier work), then watches the capture flag.
    Ptr<ID3D12Fence> armFence; UINT64 armValue = 0;
    // Post-claim abort channel (null if the device refuses a CPU-mapped UAV buffer: cap only).
    // claimGen identifies each armed wait; abort writes are generation-tagged, so a late abort for an
    // earlier generation can never cancel a later wait (replay/reuse included).
    std::shared_ptr<AbortChannel> abortChannel; UINT claimGen = 0;
    // NrAdaptiveWaitBudget: timestamps around this frame's post-claim wait (side queue), resolved in the arm list.
    Ptr<ID3D12QueryHeap> claimTimer; Ptr<ID3D12Resource> claimTimes; UINT64 claimTimerHz = 0; bool claimTimed = false;
    Ptr<ID3D12CommandAllocator> armAlloc; Ptr<ID3D12GraphicsCommandList> armList;
    struct KernelHandle {
        HANDLE value = nullptr;
        ~KernelHandle() { if (value) CloseHandle(value); }
    };
    // Unique registrations are never reused for another fence value. Retain
    // even timed-out registrations until this recording's completion proof.
    std::vector<std::unique_ptr<KernelHandle>> captureEvents;
    UINT64 serial = 0, finalValue = 0, gameValue = 0, armSerial = 0;
    std::atomic<bool> scheduled {false};
    bool started = false, uncertain = false;
    std::atomic<bool> sideUncertain {false};
    bool noNetwork = false;
    bool decisionReadable = false;
    struct Output {
        Ptr<ID3D12Resource> original, neural, texture;
        Ptr<ID3D12DescriptorHeap> heap;
        UINT width = 0, height = 0;
    };
    Output output, first;

    Ptr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = type;
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 256; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        Ptr<ID3D12Resource> resource;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&resource)), "inline buffer");
        return resource;
    }
    Ptr<ID3D12RootSignature> Root(const D3D12_ROOT_SIGNATURE_DESC& desc) {
        Ptr<ID3DBlob> blob, error; Ptr<ID3D12RootSignature> root;
        Check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "inline root serialize");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "inline root");
        return root;
    }
    Ptr<ID3D12PipelineState> Pipeline(ID3D12RootSignature* root, const char* source) {
        auto code = Compile(source, "main"); D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = root; desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        Ptr<ID3D12PipelineState> pso;
        Check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)), "inline pipeline"); return pso;
    }
    void Op(ID3D12GraphicsCommandList* list, UINT op, ID3D12Resource* target, UINT iterations = 0) {
        list->SetPipelineState(controlPso.Get()); list->SetComputeRootSignature(controlRoot.Get());
        list->SetComputeRootUnorderedAccessView(0, (op == 1 || op == 3 || op == 6 || op == 7 || op == 8 || op >= 11 ? capture : control)->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
        const bool abortable = op == 14 && abortChannel;
        list->SetComputeRootUnorderedAccessView(3, (abortable ? abortChannel->buffer : sample)->GetGPUVirtualAddress());
        const UINT args[] {op, iterations, op == 4 || op == 7 || op == 13 || op == 14 ? spinPollDelay : 0u, abortable ? claimGen : 0u};
        list->SetComputeRoot32BitConstants(2, 4, args, 0);
        list->Dispatch(1, 1, 1);
    }
    void CreateOutput(Output& out, ID3D12Resource* original, ID3D12Resource* neural, UINT w, UINT h) {
        if (!neural) return;
        const auto source = original->GetDesc(), net = neural->GetDesc();
        if (source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || source.DepthOrArraySize != 1 ||
            source.SampleDesc.Count != 1 || source.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE ||
            net.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || w > source.Width || h > source.Height ||
            w > net.Width || h > net.Height) throw std::runtime_error("inline texture extent/type unsupported");
        switch (source.Format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: break;
        default: throw std::runtime_error("inline source view format unsupported");
        }
        out.original = original; out.neural = neural; out.width = w; out.height = h;
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        // Preserve FP32 input range on timeout instead of overflowing an FP16 fallback.
        desc.Format = source.Format == DXGI_FORMAT_R32G32B32A32_FLOAT ? source.Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out.texture)), "inline output");
        D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out.heap)), "inline descriptors");
        auto handle = out.heap->GetCPUDescriptorHandleForHeapStart();
        const UINT stride = device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {}; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        srv.Format = source.Format; device->CreateShaderResourceView(original, &srv, handle); handle.ptr += stride;
        srv.Format = net.Format; device->CreateShaderResourceView(neural, &srv, handle); handle.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {}; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = desc.Format; device->CreateUnorderedAccessView(out.texture.Get(), nullptr, &uav, handle);
    }
    void Compose(ID3D12GraphicsCommandList* list, Output& out, D3D12_RESOURCE_STATES sourceState) {
        if (!out.texture) return;
        Barrier(list, out.original.Get(), sourceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, out.texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* heap = out.heap.Get(); list->SetDescriptorHeaps(1, &heap);
        list->SetPipelineState(composePso.Get()); list->SetComputeRootSignature(composeRoot.Get());
        auto gpu = out.heap->GetGPUDescriptorHandleForHeapStart();
        list->SetComputeRootDescriptorTable(0, gpu);
        gpu.ptr += 2 * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetComputeRootShaderResourceView(2, decision->GetGPUVirtualAddress());
        const UINT extent[] {out.width, out.height}; list->SetComputeRoot32BitConstants(3, 2, extent, 0);
        list->Dispatch((out.width + 7) / 8, (out.height + 7) / 8, 1);
        Barrier(list, out.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, out.original.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, sourceState);
    }
public:
    Resources(ID3D12Device* dev, ID3D12Resource* source, ID3D12Resource* neural,
              ID3D12Resource* firstPass, UINT width, UINT height, std::shared_ptr<Pipelines> cached = {}) : device(dev) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options {};
        Check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options), "inline options");
        if (!options.TypedUAVLoadAdditionalFormats) throw std::runtime_error("inline typed UAV support unavailable");
        Submission::SuppressProxyWrap suppress;
        D3D12_COMMAND_QUEUE_DESC q {}; q.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (cached && cached->device.Get() == dev && cached->sideQueue)
            side = cached->sideQueue;
        else
            Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&side)), "inline side queue");
        Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&sampleAlloc)), "inline sample allocator");
        Check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&doneAlloc)), "inline completion allocator");
        Check(device->CreateCommandList(0, q.Type, sampleAlloc.Get(), nullptr, IID_PPV_ARGS(&sampleList)), "inline sample list");
        Check(device->CreateCommandList(0, q.Type, doneAlloc.Get(), nullptr, IID_PPV_ARGS(&doneList)), "inline completion list");
        Check(sampleList->Close(), "inline sample close"); Check(doneList->Close(), "inline completion close");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&sideFence)), "inline side fence");
        control = Buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        capture = Buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        sample = Buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        decision = Buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        inputGate = Buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true); inputGate->SetName(L"NR inline input gate");
        {
            D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_CUSTOM;
            hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE; hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 256;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            auto ch = std::make_shared<AbortChannel>(); void* p = nullptr;
            if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&ch->buffer)))
                && SUCCEEDED(ch->buffer->Map(0, nullptr, &p)) && p) {
                ch->word = static_cast<volatile LONG*>(p); *ch->word = 0; ch->buffer->SetName(L"NR inline claim abort");
                abortChannel = std::move(ch);
            }
        }
        control->SetName(L"NR inline atomic control"); capture->SetName(L"NR inline atomic capture");
        sample->SetName(L"NR inline sample"); decision->SetName(L"NR inline decision");
        readback = Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        decisionReadback = Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (cached && cached->device.Get() == dev && cached->sideQueue) {
            pipelines = std::move(cached);
            controlRoot = pipelines->controlRoot; composeRoot = pipelines->composeRoot;
            controlPso = pipelines->controlPso; composePso = pipelines->composePso;
        } else {
        D3D12_ROOT_PARAMETER cp[4] {}; cp[0].ParameterType = cp[1].ParameterType = cp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        cp[0].Descriptor.ShaderRegister = 0; cp[1].Descriptor.ShaderRegister = 1; cp[3].Descriptor.ShaderRegister = 2;
        cp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; cp[2].Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC cr {}; cr.NumParameters = 4; cr.pParameters = cp;
        controlRoot = Root(cr);
        controlPso = Pipeline(controlRoot.Get(), R"(
globallycoherent RWByteAddressBuffer flag : register(u0);
RWByteAddressBuffer target : register(u1);
globallycoherent RWByteAddressBuffer abortWord : register(u2);
cbuffer Args : register(b0) { uint operation; uint iterations; uint pollDelay; uint generation; }
[numthreads(1,1,1)] void main() {
 uint value = 0;
 // Control: 0 pending, 1 network ready, 2 bypass. Capture always starts at 0.
 // Capture +4 is the armed input gate: 0 open, 1 claimed by the game (copy runs), 2 closed by the side queue (copy skipped).
 if (operation == 0) { flag.Store(0, iterations); }
 else if (operation == 6) { flag.Store2(0, uint2(0, 0)); target.Store2(24, uint2(0, 0)); }
 else if (operation == 11) {
  // Game list, before the input copies: claim the gate; the predicate skips the copies once it is closed.
  flag.InterlockedCompareExchange(4, 0, 1, value); DeviceMemoryBarrier(); target.Store2(0, uint2(value == 2 ? 0 : 1, 0));
 }
 else if (operation == 12) { flag.InterlockedCompareExchange(4, 0, 2, value); DeviceMemoryBarrier(); }
 else if (operation == 1 || operation == 2) { flag.InterlockedExchange(0, 1, value); DeviceMemoryBarrier(); }
 else if (operation == 3) { flag.InterlockedCompareExchange(0, 0, 0, value); target.Store(0, value); }
 else if (operation == 14) {
  // Claimed wait (one bounded slice of kClaimWaitSlices): the gate was claimed before op 12 could close
  // it, so the game's input copies are running or about to. Poll for its capture store (issued after
  // those copies). Capped: if every slice expires, op 8 records unseen and op 9 publishes bypass
  // (original frame, history reset); an open/closed gate means nothing is waited for.
  // Abort: every kAbortPollStride iterations, abortWord >= generation (nonzero) ends the wait; target+24
  // records the aborted generation, target+28 accumulates wait iterations (diagnostics).
  uint gate = 0; flag.InterlockedCompareExchange(4, 0, 0, gate);
  flag.InterlockedCompareExchange(0, 0, 0, value);
  if (gate == 1 && value == 0 && target.Load(24) == 0) {
   uint spun = 0;
   [loop] [allow_uav_condition] for (uint i = 0; i < iterations; ++i) {
    ++spun;
    DeviceMemoryBarrier();
    flag.InterlockedCompareExchange(0, 0, 0, value); if (value != 0) break;
    if (generation != 0 && (i % 256u) == 255u && abortWord.Load(0) >= generation) { target.Store(24, generation); break; }
    uint acc = i;
    [loop] for (uint d = 0; d < pollDelay; ++d) acc = acc * 1664525u + 1013904223u;
    if (acc == 0x7fffffffu && pollDelay == 0xffffffffu) target.Store(16, acc);
   }
   uint total; target.InterlockedAdd(28, spun, total);
  }
 }
 else if (operation == 4 || operation == 7 || operation == 13) {
  // 7: side-queue capture watch (NrCaptureBackoff=2); same bounded spin on the capture flag.
  // 13: settle watch, only while the game holds the gate (claimed but not yet captured).
  uint spun = 0, run = iterations;
  if (operation == 13) { flag.InterlockedCompareExchange(4, 0, 0, value); if (value != 1) run = 0; }
  [loop] [allow_uav_condition] for (uint i = 0; i < run; ++i) {
   ++spun;
   DeviceMemoryBarrier();
   flag.InterlockedCompareExchange(0, 0, 0, value); if (value != 0) break;
   // NrSpinPollDelay: dependent ALU between polls, so the spinning wave issues far fewer
   // coherent atomics (L2/fabric traffic) while staying resident; no memory access here.
   uint acc = i;
   [loop] for (uint d = 0; d < pollDelay; ++d) acc = acc * 1664525u + 1013904223u;
   if (acc == 0x7fffffffu && pollDelay == 0xffffffffu) target.Store(16, acc);
  }
  if (operation == 4) target.Store(8, spun);
 } else if (operation == 10) {
  // Watch start delay: dependent ALU only, no memory access until the final (never-taken) store.
  uint acc = iterations;
  [loop] for (uint d = 0; d < iterations; ++d) acc = acc * 1664525u + 1013904223u;
  if (acc == 0x7fffffffu && iterations == 0xffffffffu) target.Store(16, acc);
 } else if (operation == 8) {
  // GPU-armed handoff: record whether the watch saw the capture before the network was released.
  // Seen only if the game claimed the gate (its copies ran) and then published the capture.
  uint gate = 0; flag.InterlockedCompareExchange(4, 0, 0, gate);
  flag.InterlockedCompareExchange(0, 0, 0, value); target.Store(12, value != 0 && gate == 1 ? 1 : 0); target.Store(20, gate); DeviceMemoryBarrier();
 } else if (operation == 9) {
  // GPU-armed handoff done: network output is valid only if the capture was seen first; else bypass (original).
  uint seen = target.Load(12); flag.InterlockedExchange(0, seen != 0 ? 1 : 2, value); DeviceMemoryBarrier();
 } else { flag.InterlockedCompareExchange(0, 0, 0, value); target.Store2(0, uint2(value == 1 ? 1 : 0, 0)); }
})");
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER rp[4] {};
        for (int i = 0; i < 2; ++i) { rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rp[i].DescriptorTable = {1, &ranges[i]}; }
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[2].Descriptor.ShaderRegister = 2;
        rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[3].Constants.Num32BitValues = 2;
        D3D12_ROOT_SIGNATURE_DESC rr {}; rr.NumParameters = 4; rr.pParameters = rp;
        composeRoot = Root(rr);
        composePso = Pipeline(composeRoot.Get(), R"(
Texture2D<float4> original : register(t0); Texture2D<float4> neural : register(t1);
ByteAddressBuffer outcome : register(t2); RWTexture2D<float4> output : register(u0);
cbuffer Size : register(b0) { uint width; uint height; }
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
 if (id.x >= width || id.y >= height) return;
 float4 colour;
 [branch] if (outcome.Load(0) != 0) colour = neural.Load(int3(id.xy, 0));
 else colour = original.Load(int3(id.xy, 0));
 output[id.xy] = colour;
})");
            pipelines = std::make_shared<Pipelines>(); pipelines->device = device; pipelines->sideQueue = side;
            pipelines->controlRoot = controlRoot; pipelines->composeRoot = composeRoot;
            pipelines->controlPso = controlPso; pipelines->composePso = composePso;
        }
        CreateOutput(output, source, neural, width, height); CreateOutput(first, source, firstPass, width, height);
    }
    ID3D12CommandQueue* SideQueue() const { return side.Get(); }
    const std::shared_ptr<Pipelines>& PipelineCache() const { return pipelines; }
    bool WasAborted() const { return noNetwork; }
    ID3D12Resource* OutputTexture() const { return output.texture.Get(); }
    ID3D12Resource* FirstTexture() const { return first.texture.Get(); }
    // NrFrameDiag GPU stamps (game queue + side queue). Created lazily, only when diag is on.
    std::shared_ptr<FrameDiag::GpuStamps> gameStamps, sideStamps;
    Ptr<ID3D12CommandQueue> gameQueue;
    void DiagStamp(ID3D12GraphicsCommandList* list, UINT point) {
        if (!diag.active) return;
        if (!gameStamps) gameStamps = FrameDiag::GpuStamps::Acquire(device.Get(), FrameDiag::kGpuStampCount);
        if (!gameStamps) return;
        gameStamps->Stamp(list, point);
    }
    // At reclaim (Complete): both queues' resolves have retired. Hands the stamp sets to the async emitter
    // (this recording takes fresh pooled ones if it is reused).
    void DiagEmitAsync() {
        if (!diag.active) return;
        FrameDiag::AsyncEmit(diag, std::move(gameStamps), gameQueue.Get(),
                             noNetwork ? nullptr : std::move(sideStamps), side.Get());
        gameStamps.reset(); sideStamps.reset();
    }
    // Fault-injection (tests only): ALU-only game-queue delay dispatches before the input gate.
    UINT testInputDelay = 0;
    // Fault-injection (tests only): ALU-only game-queue delay dispatches after the claim, before the copies.
    UINT testCopyDelay = 0;
    // NrGpuHandoff only: claim the input gate, then predicate the input copies on the claim. If the side
    // watch expired first it closed the gate, so the copies are skipped and the network (whose output
    // is then discarded) never races the game writing its input. Call before RecordInputs.
    void BeginInputs(ID3D12GraphicsCommandList* list) {
        inputGated = false;
        if (!gpuArmed) return;
        for (UINT i = 0; i < testInputDelay; ++i) { Op(list, 10, sample.Get(), 1u << 22); UavBarrier(list, nullptr); }
        Op(list, 11, inputGate.Get()); UavBarrier(list, inputGate.Get());
        // Fault-injection (tests only): delay after the claim, before the input copies.
        for (UINT i = 0; i < testCopyDelay; ++i) { Op(list, 10, sample.Get(), 1u << 22); UavBarrier(list, nullptr); }
        Barrier(list, inputGate.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PREDICATION);
        list->SetPredication(inputGate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        inputGated = true;
    }
    void RecordCapture(ID3D12GraphicsCommandList* list) {
        if (inputGated) {
            list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            Barrier(list, inputGate.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            inputGated = false;
        }
        Op(list, 1, decision.Get()); UavBarrier(list, capture.Get()); DiagStamp(list, FrameDiag::kGpuCapEnd); }
    void RecordWait(ID3D12GraphicsCommandList* list, UINT slices = kWaitSlices, UINT iterations = kWaitIterations) {
        // NrSpinSlices: same total budget (slices x iterations) cut into fewer/more dispatches.
        if (spinSlices && slices == kWaitSlices && iterations == kWaitIterations) {
            slices = spinSlices; iterations = UINT(UINT64(kWaitSlices) * kWaitIterations / spinSlices);
        }
        // Keep the predicate immutable after this point: late completion cannot select unrecorded output.
        for (UINT i = 0; i < slices; ++i) {
            Op(list, 4, decision.Get(), iterations);
            // Dispatch order alone does not serialize independent compute work.
            // The final snapshot must follow the completed wait slices.
            UavBarrier(list, control.Get());
        }
        Op(list, 5, decision.Get()); UavBarrier(list, decision.Get());
        Barrier(list, decision.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PREDICATION);
        list->SetPredication(decision.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        DiagStamp(list, FrameDiag::kGpuResume); // spin exit (EndQuery is not predicated)
    }
    void RecordSelect(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES colourState) {
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        Barrier(list, decision.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Compose(list, output, colourState); Compose(list, first, colourState);
        Barrier(list, decision.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(decisionReadback.Get(), 0, decision.Get(), 0, 16);
        Barrier(list, decision.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        decisionReadable = true;
        DiagStamp(list, FrameDiag::kGpuDone);
        if (diag.active && gameStamps) gameStamps->Resolve(list); // predication already cleared
    }
    bool Started() const { return started; }
    // NrInlineReuse: a retired recording may serve a later frame with identical inputs.
    // BeforeExecute already resets control/capture and continues the side-fence serial.
    bool SameInputs(ID3D12Device* d, ID3D12Resource* s, ID3D12Resource* n, ID3D12Resource* f, UINT w, UINT h) const {
        return device.Get() == d && output.original.Get() == s && output.neural.Get() == n &&
               first.neural.Get() == f && output.width == w && output.height == h;
    }
    void PrepareReuse() {
        captureEvents.clear(); started = false; decisionReadable = false; uncertain = noNetwork = false;
        diag = {}; captureBackoff = 0; gpuArmed = false; spinPollDelay = spinSlices = watchSlices = watchStartDelay = 0; adaptiveBudget = false; activityToken = 0; reused = true;
        inputGated = armedOutcomeReadable = false;
    }
    bool RetirementUncertain() const { return uncertain || sideUncertain; }
    // Set under RecordingMutex from the lease that admitted this recording.
    uint64_t activityToken = 0;
    // Filled when NrFrameDiag is on; RecordingOwner / backends emit on reclaim.
    FrameDiag::Counters diag;
    // Capture discovery policy snapshot for this frame (0 exponential 50..5000 us, 1 fixed 100 us,
    // 2 event: a bounded side-queue spin on the capture flag, then a fence event wakes the worker).
    int captureBackoff = 0;
    // NrGpuHandoff snapshot for this frame: release the network from the side queue when the capture lands.
    bool gpuArmed = false;
    // Spin-cost experiments (default 0 = unchanged): ALU iterations between flag polls (game wait +
    // side watch) and game-wait dispatch count (budget preserved). Snapshotted before recording.
    UINT spinPollDelay = 0, spinSlices = 0;
    // Armed side-watch experiments: watch dispatch count (budget kept) and ALU-only start delay dispatches.
    UINT watchSlices = 0, watchStartDelay = 0;
    // NrAdaptiveWaitBudget snapshot for this frame (default off: the fixed kClaimWaitSlices cap).
    bool adaptiveBudget = false;
    bool reused = false; // came from NrInlineReuse pool
    bool Complete() const {
        if (!started) return true;
        if (RetirementUncertain() || !scheduled.load(std::memory_order_acquire) || !finalValue || !gameFence || !gameValue) return false;
        const auto a = sideFence->GetCompletedValue(), b = gameFence->GetCompletedValue();
        return a != UINT64_MAX && b != UINT64_MAX && a >= finalValue && b >= gameValue;
    }
    bool Succeeded() const {
        if (!Complete() || !decisionReadable || noNetwork) return false;
        void* p = nullptr; D3D12_RANGE range {0, 8};
        if (FAILED(decisionReadback->Map(0, &range, &p)) || !p) return false;
        const bool ok = *static_cast<const UINT64*>(p) != 0;
        D3D12_RANGE none {0, 0}; decisionReadback->Unmap(0, &none); return ok;
    }
    UINT LastSpinCount() const {
        void* p = nullptr; D3D12_RANGE range {0,16};
        if (!Complete() || !decisionReadable || FAILED(decisionReadback->Map(0, &range, &p)) || !p) return 0;
        const auto count = static_cast<const UINT*>(p)[2];
        D3D12_RANGE none {0,0}; decisionReadback->Unmap(0, &none); return count;
    }
    void BeforeExecute(ID3D12CommandQueue* game, bool bypass = false) {
        if (!Complete()) throw std::runtime_error("inline recording still executing");
        HarvestClaim();
        captureEvents.clear();
        gameQueue = game;
        started = true; uncertain = noNetwork = false; sideUncertain = true;
        finalValue = gameValue = 0; gameFence.Reset(); armedOutcomeReadable = false;
        scheduled.store(false, std::memory_order_release);
        Check(sampleAlloc->Reset(), "inline reset allocator"); Check(sampleList->Reset(sampleAlloc.Get(), nullptr), "inline reset list");
        Op(sampleList.Get(), 0, sample.Get(), bypass ? 2 : 0); UavBarrier(sampleList.Get(), control.Get());
        Op(sampleList.Get(), 6, sample.Get()); UavBarrier(sampleList.Get(), capture.Get());
        Check(sampleList->Close(), "inline reset close"); Execute(side.Get(), sampleList.Get());
        const auto reset = ++serial; Check(side->Signal(sideFence.Get(), reset), "inline reset signal");
        sideUncertain = false;
        // No CPU GPU wait under RecordingMutex. Only the producer depends on this clear.
        Check(game->Wait(sideFence.Get(), reset), "inline producer reset wait");
        armValue = 0;
        if (gpuArmed && !bypass) {
            if (!armFence) Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&armFence)), "inline arm fence");
            // Queue-level signal before the producer list: everything earlier on the game queue is done.
            const UINT64 next = armFence->GetCompletedValue() == UINT64_MAX ? 0 : armSerial + 1;
            if (!next) throw std::runtime_error("inline arm fence lost");
            Check(game->Signal(armFence.Get(), next), "inline arm signal");
            armSerial = armValue = next;
        }
    }
    void GameSubmitted(ID3D12Fence* fence, UINT64 value, HRESULT hr) {
        if (FAILED(hr) || !fence || !value || value == UINT64_MAX) { uncertain = true; return; }
        gameFence = fence; gameValue = value;
    }
    void CertifyWithoutNetwork(ID3D12Fence* fence, UINT64 value, HRESULT hr, bool producerSubmitted = false) {
        // Only for a bypass, aborted producer or unarmed partial recording: no worker
        // was started. Retire the side clear and, if submitted, the game commands.
        noNetwork = true; finalValue = serial;
        if (producerSubmitted) GameSubmitted(fence, value, hr);
        else { gameFence = sideFence; gameValue = serial; }
        scheduled.store(true, std::memory_order_release);
    }
    bool WaitCapture() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const bool fixedBackoff = captureBackoff == 1, eventWake = captureBackoff == 2;
        if (diag.active) { diag.capturePolls = 0; diag.captureBackoffUsFinal = 0;
            diag.captureBackoffMode = eventWake ? "event" : fixedBackoff ? "fixed100" : "exp";  diag.handoff = "cpu"; }
        // The event wakes this worker promptly; the fence remains the proof.
        // Reusing one event across values can let a late signal wake a new wait.
        auto waitFence = [&](UINT64 value) {
            const auto done = sideFence->GetCompletedValue();
            if (done == UINT64_MAX) return false;
            if (done >= value) return true;
            auto event = std::make_unique<KernelHandle>();
            event->value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            Check(event->value ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "inline capture event");
            const auto handle = event->value;
            captureEvents.push_back(std::move(event));
            Check(sideFence->SetEventOnCompletion(value, handle), "inline capture event registration");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;
            const auto result = WaitForSingleObject(handle, DWORD(remaining));
            if (result == WAIT_FAILED) Check(HRESULT_FROM_WIN32(GetLastError()), "inline capture event wait");
            const auto finished = sideFence->GetCompletedValue();
            return result == WAIT_OBJECT_0 && finished != UINT64_MAX && finished >= value;
        };
        // The reset list must retire before reusing the sampling allocator.
        const auto reset = serial;
        if (!waitFence(reset)) return false;
        auto timer = std::make_unique<KernelHandle>();
        timer->value = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer->value) timer->value = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        Check(timer->value ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "inline capture backoff timer");
        auto backoff = std::chrono::microseconds(fixedBackoff ? 100 : 50);
        for (unsigned polls = 0; polls < 2048 && std::chrono::steady_clock::now() < until; ++polls) {
            if (diag.active) diag.capturePolls = polls + 1;
            Check(sampleAlloc->Reset(), "inline poll allocator"); Check(sampleList->Reset(sampleAlloc.Get(), nullptr), "inline poll list");
            // Event: the side queue watches the flag (bounded, ~1 ms) so the fence fires at capture,
            // not at the next CPU poll. The game queue is never touched; a miss just re-arms.
            if (eventWake) { for (UINT i = 0; i < kCaptureWatchSlices; ++i) { Op(sampleList.Get(), 7, sample.Get(), kWaitIterations); UavBarrier(sampleList.Get(), capture.Get()); } }
            Op(sampleList.Get(), 3, sample.Get()); UavBarrier(sampleList.Get(), sample.Get());
            Barrier(sampleList.Get(), sample.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            sampleList->CopyBufferRegion(readback.Get(), 0, sample.Get(), 0, 4);
            Barrier(sampleList.Get(), sample.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Check(sampleList->Close(), "inline poll close"); sideUncertain = true; Execute(side.Get(), sampleList.Get());
            const auto done = ++serial; Check(side->Signal(sideFence.Get(), done), "inline poll signal");
            sideUncertain = false;
            if (!waitFence(done)) return false;
            void* p = nullptr; D3D12_RANGE range {0, 4}; Check(readback->Map(0, &range, &p), "inline poll map");
            const UINT captured = *static_cast<const UINT*>(p); D3D12_RANGE none {0, 0}; readback->Unmap(0, &none);
            if (captured) {
                if (diag.active) {
                    diag.qpcCaptureSeen = FrameDiag::Qpc();
                    diag.captureBackoffUsFinal = unsigned(backoff.count());
                }
                return true;
            }
            if (eventWake) continue; // the watch itself paced this round
            // sleep_for(50us) can sleep a whole Windows timer tick. Use a
            // private timer, without changing the process/system timer period.
            LARGE_INTEGER due {}; due.QuadPart = -10 * backoff.count();
            Check(SetWaitableTimer(timer->value, &due, 0, nullptr, nullptr, FALSE) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "inline capture timer arm");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (remaining <= 0 || WaitForSingleObject(timer->value, DWORD(remaining)) != WAIT_OBJECT_0) return false;
            if (!fixedBackoff) backoff = (std::min)(backoff * 2, std::chrono::microseconds(5000));
        }
        return false;
    }
    // NrGpuHandoff replacement for WaitCapture: no CPU wait. Side queue: Wait(arm) -> bounded watch on the
    // capture flag -> record seen/unseen. The caller then enqueues the network behind it on the same queue;
    // NetworkQueued's done op publishes 1 (apply) only if the watch saw the capture, else 2 (original frame).
    bool GpuArmed() const { return armValue != 0; }
    bool armedOutcomeReadable = false;
    // After completion of an armed frame: seen (network output valid) and gate (1 claimed, 2 closed).
    bool ArmedOutcome(UINT& seen, UINT& gate) const {
        if (!Complete() || !armedOutcomeReadable) return false;
        void* p = nullptr; D3D12_RANGE range {0, 32};
        if (FAILED(readback->Map(0, &range, &p)) || !p) return false;
        seen = static_cast<const UINT*>(p)[3]; gate = static_cast<const UINT*>(p)[5];
        D3D12_RANGE none {0, 0}; readback->Unmap(0, &none); return true;
    }
    // After completion of an armed frame: aborted generation (0 = not aborted) and post-claim wait iterations.
    bool ClaimWaitStats(UINT& aborted, UINT& iterations) const {
        if (!Complete() || !armedOutcomeReadable) return false;
        void* p = nullptr; D3D12_RANGE range {0, 32};
        if (FAILED(readback->Map(0, &range, &p)) || !p) return false;
        aborted = static_cast<const UINT*>(p)[6]; iterations = static_cast<const UINT*>(p)[7];
        D3D12_RANGE none {0, 0}; readback->Unmap(0, &none); return true;
    }
    UINT ClaimGeneration() const { return claimGen; }
    // Lean diagnostics: fold a retired armed frame into FrameDiag::Claim() once (no GPU wait).
    bool claimHarvested = true;
    void HarvestClaim() noexcept {
        if (claimHarvested || !Complete() || !armedOutcomeReadable) return;
        void* p = nullptr; D3D12_RANGE range {0, 32};
        if (FAILED(readback->Map(0, &range, &p)) || !p) return;
        const auto* v = static_cast<const UINT*>(p); const UINT seen = v[3], gate = v[5], aborted = v[6], iters = v[7];
        D3D12_RANGE none {0, 0}; readback->Unmap(0, &none); claimHarvested = true;
        auto& t = FrameDiag::Claim();
        if (gate == 1) { t.waits.fetch_add(1); t.iters.fetch_add(iters); }
        if (!seen) t.bypass.fetch_add(1);
        if (aborted) t.aborts.fetch_add(1); else if (!seen && gate == 1) t.caps.fetch_add(1);
        if (claimTimed && gate == 1) {
            claimTimed = false;
            auto& budget = AdaptiveBudget();
            void* q = nullptr; D3D12_RANGE times {0, 16};
            if (SUCCEEDED(claimTimes->Map(0, &times, &q)) && q) {
                const auto* ts = static_cast<const UINT64*>(q);
                if (ts[1] > ts[0]) budget.Sample(iters, double(ts[1] - ts[0]) * 1000.0 / double(claimTimerHz));
                claimTimes->Unmap(0, &none);
            }
            if (aborted || !seen) budget.Timeout(); else budget.Clean();
        }
    }
    ~Resources() { HarvestClaim(); }
    bool AbortAvailable() const { return abortChannel != nullptr; }
    bool ArmCapture() {
        if (!armValue || !armFence) return false;
        if (++claimGen == 0) ++claimGen; // 0 means "no abort"
        if (diag.active) { diag.capturePolls = 0; diag.captureBackoffUsFinal = 0; diag.captureBackoffMode = "gpu"; diag.handoff = "gpu"; }
        if (!armAlloc) {
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&armAlloc)), "inline arm allocator");
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, armAlloc.Get(), nullptr, IID_PPV_ARGS(&armList)), "inline arm list");
        } else {
            // Previous use retired: BeforeExecute requires Complete(), and finalValue follows this list on side.
            Check(armAlloc->Reset(), "inline arm allocator reset"); Check(armList->Reset(armAlloc.Get(), nullptr), "inline arm list reset");
        }
        // NrWatchStartDelay: ALU-only dispatches (no memory traffic) before the watch starts polling.
        // The UAV barrier makes the watch start only after the delay dispatches complete.
        for (UINT i = 0; i < watchStartDelay; ++i) Op(armList.Get(), 10, sample.Get(), kWaitIterations);
        if (watchStartDelay) UavBarrier(armList.Get(), nullptr);
        // NrWatchSlices: same watch budget in shorter dispatches (shorter-resident waves).
        const UINT ws = watchSlices ? watchSlices : kWaitSlices, wi = UINT(UINT64(kWaitSlices) * kWaitIterations / ws);
        for (UINT i = 0; i < ws; ++i) { Op(armList.Get(), 7, sample.Get(), wi); UavBarrier(armList.Get(), capture.Get()); }
        // Close the input gate. A game that has not claimed it skips its input copies, so the network
        // never reads input the game is still writing (its output is discarded: bypass + history reset).
        // A game that claimed it is copying: wait on the GPU until its capture proves the copies done.
        Op(armList.Get(), 12, sample.Get()); UavBarrier(armList.Get(), capture.Get());
        // Bounded claimed wait (always on): kClaimWaitSlices short dispatches, each exits at once after
        // the capture lands. A capture that never lands ends the wait (unseen -> bypass) well under TDR.
        claimTimed = false;
        if (adaptiveBudget && !claimTimer) {
            D3D12_QUERY_HEAP_DESC qd {}; qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2;
            if (SUCCEEDED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&claimTimer))) && SUCCEEDED(side->GetTimestampFrequency(&claimTimerHz)))
                claimTimes = Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
            else claimTimer.Reset();
        }
        const bool timed = adaptiveBudget && claimTimer && claimTimes && claimTimerHz;
        const UINT claimSlices = adaptiveBudget ? AdaptiveBudget().Slices() : kClaimWaitSlices;
        if (timed) armList->EndQuery(claimTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (UINT i = 0; i < claimSlices; ++i) { Op(armList.Get(), 14, sample.Get(), kWaitIterations); UavBarrier(armList.Get(), capture.Get()); }
        if (timed) {
            armList->EndQuery(claimTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            armList->ResolveQueryData(claimTimer.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, claimTimes.Get(), 0);
            claimTimed = true;
        }
        Op(armList.Get(), 8, sample.Get()); UavBarrier(armList.Get(), sample.Get());
        Check(armList->Close(), "inline arm close");
        sideUncertain = true;
        Check(side->Wait(armFence.Get(), armValue), "inline arm wait");
        Execute(side.Get(), armList.Get());
        if (abortChannel) {
            // Retirement marker for the watchdog only; it certifies the arm list, never the game's copies.
            const auto armDone = ++serial; Check(side->Signal(sideFence.Get(), armDone), "inline arm done signal");
            AbortWatchdog::Get().Watch(abortChannel, armFence.Get(), armValue, sideFence.Get(), armDone, claimGen, gClaimAbortMs);
        }
        sideUncertain = false;
        if (diag.active) diag.qpcCaptureSeen = FrameDiag::Qpc(); // armed (not seen): handoff is GPU-side
        return true;
    }
    void NetworkQueued(bool ok) {
        if (diag.active) diag.qpcSideReady = FrameDiag::Qpc();
        // Failure can follow partial runtime submission. Append a fresh side tail,
        // rather than reusing serial (whose Signal might itself have failed).
        // This certifies host D3D12 resources only; CollectRecording still owns
        // HIP/Vulkan completion and may retain its job after an enqueue failure.
        noNetwork = !ok;
        sideUncertain = true;
        if (ok) {
            Check(doneAlloc->Reset(), "inline done allocator"); Check(doneList->Reset(doneAlloc.Get(), nullptr), "inline done list");
            // Runtime Enqueue inserted its network completion Wait on this same side queue.
            if (diag.active && !sideStamps) sideStamps = FrameDiag::GpuStamps::Acquire(device.Get(), 1);
            const bool stamp = diag.active && sideStamps;
            if (stamp) sideStamps->Stamp(doneList.Get(), 0);
            if (armValue) {
                UavBarrier(doneList.Get(), sample.Get()); Op(doneList.Get(), 9, sample.Get()); UavBarrier(doneList.Get(), sample.Get());
                // {seen, gate} for ArmedOutcome (readback is unused by armed frames).
                Barrier(doneList.Get(), sample.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                doneList->CopyBufferRegion(readback.Get(), 0, sample.Get(), 0, 32);
                Barrier(doneList.Get(), sample.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                armedOutcomeReadable = true; claimHarvested = false;
            }
            else Op(doneList.Get(), 2, sample.Get());
            UavBarrier(doneList.Get(), control.Get());
            if (stamp) sideStamps->Resolve(doneList.Get());
            Check(doneList->Close(), "inline done close"); Execute(side.Get(), doneList.Get());
        }
        finalValue = ++serial; Check(side->Signal(sideFence.Get(), finalValue), "inline tail signal");
        sideUncertain = false;
        scheduled.store(true, std::memory_order_release);
    }
};
// Caller holds RecordingMutex. Null pool = always create (default). The pool holds one
// reference; use_count()==1 means the owner, lease and recording have all released it.
inline std::shared_ptr<Resources> AcquireResources(std::vector<std::shared_ptr<Resources>>* pool, ID3D12Device* device,
    ID3D12Resource* source, ID3D12Resource* neural, ID3D12Resource* firstPass, UINT width, UINT height,
    std::shared_ptr<Pipelines> cached)
{
    if (pool) {
        for (auto& r : *pool)
            if (r.use_count() == 1 && r->Complete() && r->SameInputs(device, source, neural, firstPass, width, height)) {
                r->PrepareReuse(); return r;
            }
    }
    auto r = std::make_shared<Resources>(device, source, neural, firstPass, width, height, std::move(cached));
    if (pool) {
        // Drop retired entries for other inputs (resolution/slot changes) before growing.
        pool->erase(std::remove_if(pool->begin(), pool->end(), [](const std::shared_ptr<Resources>& e) {
            return e.use_count() == 1 && e->Complete(); }), pool->end());
        if (pool->size() < 8) pool->push_back(r);
    }
    return r;
}
}

#pragma once
#include "../submission/CommandListProxy.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <atomic>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <thread>
#include <algorithm>
#include <memory>
#include <vector>

namespace DlssNr::Backend::InlineRecording
{
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
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
    Ptr<ID3D12Fence> sideFence, gameFence;
    struct KernelHandle {
        HANDLE value = nullptr;
        ~KernelHandle() { if (value) CloseHandle(value); }
    };
    // Unique registrations are never reused for another fence value. Retain
    // even timed-out registrations until this recording's completion proof.
    std::vector<std::unique_ptr<KernelHandle>> captureEvents;
    UINT64 serial = 0, finalValue = 0, gameValue = 0;
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
        list->SetComputeRootUnorderedAccessView(0, (op == 1 || op == 3 || op == 6 ? capture : control)->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, target->GetGPUVirtualAddress());
        const UINT args[] {op, iterations}; list->SetComputeRoot32BitConstants(2, 2, args, 0);
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
        readback = Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        decisionReadback = Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        if (cached && cached->device.Get() == dev && cached->sideQueue) {
            pipelines = std::move(cached);
            controlRoot = pipelines->controlRoot; composeRoot = pipelines->composeRoot;
            controlPso = pipelines->controlPso; composePso = pipelines->composePso;
        } else {
        D3D12_ROOT_PARAMETER cp[3] {}; cp[0].ParameterType = cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        cp[0].Descriptor.ShaderRegister = 0; cp[1].Descriptor.ShaderRegister = 1;
        cp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; cp[2].Constants.Num32BitValues = 2;
        D3D12_ROOT_SIGNATURE_DESC cr {}; cr.NumParameters = 3; cr.pParameters = cp;
        controlRoot = Root(cr);
        controlPso = Pipeline(controlRoot.Get(), R"(
globallycoherent RWByteAddressBuffer flag : register(u0);
RWByteAddressBuffer target : register(u1);
cbuffer Args : register(b0) { uint operation; uint iterations; }
[numthreads(1,1,1)] void main() {
 uint value = 0;
 if (operation == 0 || operation == 6) { flag.Store(0, 0); }
 else if (operation == 1 || operation == 2) { flag.InterlockedExchange(0, 1, value); DeviceMemoryBarrier(); }
 else if (operation == 3) { flag.InterlockedCompareExchange(0, 0, 0, value); target.Store(0, value); }
 else if (operation == 4) {
  uint spun = 0;
  [loop] [allow_uav_condition] for (uint i = 0; i < iterations; ++i) {
   ++spun;
   DeviceMemoryBarrier();
   flag.InterlockedCompareExchange(0, 0, 0, value); if (value != 0) break;
  }
  target.Store(8, spun);
 } else { flag.InterlockedCompareExchange(0, 0, 0, value); target.Store2(0, uint2(value, 0)); }
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
    void RecordCapture(ID3D12GraphicsCommandList* list) { Op(list, 1, decision.Get()); UavBarrier(list, capture.Get()); }
    void RecordWait(ID3D12GraphicsCommandList* list, UINT slices = 64, UINT iterations = 32768) {
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
    }
    void RecordSelect(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES colourState) {
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        Barrier(list, decision.Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Compose(list, output, colourState); Compose(list, first, colourState);
        Barrier(list, decision.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(decisionReadback.Get(), 0, decision.Get(), 0, 16);
        Barrier(list, decision.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        decisionReadable = true;
    }
    bool Started() const { return started; }
    bool RetirementUncertain() const { return uncertain || sideUncertain; }
    // Set under RecordingMutex from the lease that admitted this recording.
    uint64_t activityToken = 0;
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
    void BeforeExecute(ID3D12CommandQueue* game) {
        if (!Complete()) throw std::runtime_error("inline recording still executing");
        captureEvents.clear();
        started = true; uncertain = noNetwork = false; sideUncertain = true;
        finalValue = gameValue = 0; gameFence.Reset();
        scheduled.store(false, std::memory_order_release);
        Check(sampleAlloc->Reset(), "inline reset allocator"); Check(sampleList->Reset(sampleAlloc.Get(), nullptr), "inline reset list");
        Op(sampleList.Get(), 0, sample.Get()); UavBarrier(sampleList.Get(), control.Get());
        Op(sampleList.Get(), 6, sample.Get()); UavBarrier(sampleList.Get(), capture.Get());
        Check(sampleList->Close(), "inline reset close"); Execute(side.Get(), sampleList.Get());
        const auto reset = ++serial; Check(side->Signal(sideFence.Get(), reset), "inline reset signal");
        sideUncertain = false;
        // No CPU GPU wait under RecordingMutex. Only the producer depends on this clear.
        Check(game->Wait(sideFence.Get(), reset), "inline producer reset wait");
    }
    void GameSubmitted(ID3D12Fence* fence, UINT64 value, HRESULT hr) {
        if (FAILED(hr) || !fence || !value || value == UINT64_MAX) { uncertain = true; return; }
        gameFence = fence; gameValue = value;
    }
    void CertifyWithoutNetwork(ID3D12Fence* fence, UINT64 value, HRESULT hr) {
        // Only for an aborted producer or an unarmed partial recording: no worker
        // was started. Retire the side clear and, if submitted, the game commands.
        noNetwork = true; finalValue = serial;
        if (fence) GameSubmitted(fence, value, hr);
        else { gameFence = sideFence; gameValue = serial; }
        scheduled.store(true, std::memory_order_release);
    }
    bool WaitCapture() {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
        auto backoff = std::chrono::microseconds(50);
        for (unsigned polls = 0; polls < 2048 && std::chrono::steady_clock::now() < until; ++polls) {
            Check(sampleAlloc->Reset(), "inline poll allocator"); Check(sampleList->Reset(sampleAlloc.Get(), nullptr), "inline poll list");
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
            if (captured) return true;
            // sleep_for(50us) can sleep a whole Windows timer tick. Use a
            // private timer, without changing the process/system timer period.
            LARGE_INTEGER due {}; due.QuadPart = -10 * backoff.count();
            Check(SetWaitableTimer(timer->value, &due, 0, nullptr, nullptr, FALSE) ? S_OK : HRESULT_FROM_WIN32(GetLastError()), "inline capture timer arm");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
            if (remaining <= 0 || WaitForSingleObject(timer->value, DWORD(remaining)) != WAIT_OBJECT_0) return false;
            backoff = (std::min)(backoff * 2, std::chrono::microseconds(5000));
        }
        return false;
    }
    void NetworkQueued(bool ok) {
        // Failure can follow partial runtime submission. Append a fresh side tail,
        // rather than reusing serial (whose Signal might itself have failed).
        // This certifies host D3D12 resources only; CollectRecording still owns
        // HIP/Vulkan completion and may retain its job after an enqueue failure.
        noNetwork = !ok;
        sideUncertain = true;
        if (ok) {
            Check(doneAlloc->Reset(), "inline done allocator"); Check(doneList->Reset(doneAlloc.Get(), nullptr), "inline done list");
            // Runtime Enqueue inserted its network completion Wait on this same side queue.
            Op(doneList.Get(), 2, sample.Get()); UavBarrier(doneList.Get(), control.Get());
            Check(doneList->Close(), "inline done close"); Execute(side.Get(), doneList.Get());
        }
        finalValue = ++serial; Check(side->Signal(sideFence.Get(), finalValue), "inline tail signal");
        sideUncertain = false;
        scheduled.store(true, std::memory_order_release);
    }
};
}

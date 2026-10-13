#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/NrInlineRecording.h"

namespace In = DlssNr::Backend::InlineRecording;
namespace Sub = DlssNr::Submission;
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
static void Require(bool b, const char* msg) { if (!b) { std::fprintf(stderr, "FAIL: %s\n", msg); std::exit(1); } }
static void Check(HRESULT hr, const char* msg) { Require(SUCCEEDED(hr), msg); }
struct Gpu {
    Ptr<ID3D12Device> device; Ptr<ID3D12CommandQueue> queue; Ptr<ID3D12Fence> fence;
    Ptr<ID3D12InfoQueue> messages;
    UINT64 serial = 0;
    Gpu(bool hardware, bool debug = false) {
        if (debug) {
            Ptr<ID3D12Debug> layer;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)))) layer->EnableDebugLayer();
            else std::puts("debug layer SKIP: Graphics Tools not installed");
        }
        Ptr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        Ptr<IDXGIAdapter1> adapter;
        if (hardware) {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 desc {}; adapter->GetDesc1(&desc);
                if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == 0x1002 &&
                    SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                    std::printf("hardware vendor=%04x device=%04x\n", desc.VendorId, desc.DeviceId); break;
                }
                adapter.Reset();
            }
            Require(bool(device), "hardware D3D12 adapter required");
        } else {
            Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
            Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        }
        D3D12_COMMAND_QUEUE_DESC q {}; Check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        if (debug) device.As(&messages);
    }
    UINT64 Signal() { const auto v = ++serial; Check(queue->Signal(fence.Get(), v), "signal"); return v; }
    void Wait(UINT64 v) {
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(e != nullptr, "event");
        Check(fence->SetEventOnCompletion(v, e), "event fence");
        Require(WaitForSingleObject(e, 15000) == WAIT_OBJECT_0, "GPU timeout"); CloseHandle(e);
        Require(SUCCEEDED(device->GetDeviceRemovedReason()), "device remains valid");
    }
    Ptr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = type;
        D3D12_RESOURCE_DESC d {}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ptr<ID3D12Resource> r; Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&r)), "buffer"); return r;
    }
    Ptr<ID3D12Resource> Texture(const float* colour) {
        D3D12_RESOURCE_DESC d {}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = d.Height = 8;
        d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        Ptr<ID3D12Resource> r; Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r)), "texture");
        auto upload = Buffer(256 * 8, D3D12_HEAP_TYPE_UPLOAD); void* p = nullptr; D3D12_RANGE none {0,0};
        Check(upload->Map(0, &none, &p), "upload map");
        for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x)
            std::memcpy(static_cast<char*>(p) + y * 256 + x * 16, colour, 16);
        upload->Unmap(0, nullptr);
        Ptr<ID3D12CommandAllocator> a; Ptr<ID3D12GraphicsCommandList> c;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "texture allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&c)), "texture list");
        D3D12_TEXTURE_COPY_LOCATION dst {}; dst.pResource = r.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src {}; src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {d.Format, 8, 8, 1, 256}; c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        In::Barrier(c.Get(), r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(c->Close(), "texture close"); In::Execute(queue.Get(), c.Get()); Wait(Signal()); return r;
    }
    void Pixels(ID3D12Resource* r, const UINT16* expected, const float* expectedFloat = nullptr) {
        auto buffer = Buffer(256 * 8, D3D12_HEAP_TYPE_READBACK);
        Ptr<ID3D12CommandAllocator> a; Ptr<ID3D12GraphicsCommandList> c;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "read allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&c)), "read list");
        In::Barrier(c.Get(), r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src {}; src.pResource = r; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst {}; dst.pResource = buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        const auto format = r->GetDesc().Format;
        dst.PlacedFootprint.Footprint = {format, 8, 8, 1, 256};
        c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        In::Barrier(c.Get(), r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(c->Close(), "read close"); In::Execute(queue.Get(), c.Get()); Wait(Signal());
        void* p = nullptr; D3D12_RANGE range {0, 256 * 8}; Check(buffer->Map(0, &range, &p), "read map");
        for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
            if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                for (UINT channel = 0; channel < 4; ++channel) {
                    const float value = expectedFloat ? expectedFloat[channel] : expected[channel] == 0x3400 ? .25f : expected[channel] == 0x3800 ? .5f : expected[channel] == 0x3a00 ? .75f : 1.f;
                    Require(reinterpret_cast<const float*>(static_cast<const char*>(p) + y * 256)[x * 4 + channel] == value, "FP32 selected pixels");
                }
            } else Require(std::memcmp(static_cast<char*>(p) + y * 256 + x * 8, expected, 8) == 0, "selected pixels");
        }
        D3D12_RANGE none {0,0}; buffer->Unmap(0, &none);
    }
};

static void RunSplitState(Gpu& gpu) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options {};
    Check(gpu.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof options), "options12");
    Require(options.EnhancedBarriersSupported, "split-state regression requires enhanced barriers");
    SetAllowEnhancedBarriers(true);
    const float red[] {.25f, .5f, .75f, 1}, black[] {0, 0, 0, 1};
    const UINT16 redHalf[] {0x3400, 0x3800, 0x3a00, 0x3c00};
    // These helpers submit AND wait on an earlier list, matching the review repro.
    auto source = gpu.Texture(red), other = gpu.Texture(black), output = gpu.Texture(black);
    Ptr<ID3D12CommandAllocator> allocator; Ptr<ID3D12GraphicsCommandList> native;
    Ptr<Sub::CommandListProxy> proxy;
    Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&native)), "list");
    Check(Sub::CommandListProxy::Create(gpu.device.Get(), allocator.Get(), native.Get(), &proxy), "proxy");
    ID3D12Resource* sources[] {source.Get()};
    D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    auto acceptLegacy = [&] {
        Check(proxy->BeginSplitState(1, sources, states), "cross-list legacy source accepted");
        Require(states[0] == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "caller legacy state unchanged");
        proxy->EndInlineState();
    };
    acceptLegacy();
    D3D12_GLOBAL_BARRIER global {};
    global.SyncBefore = global.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    global.AccessBefore = global.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
    D3D12_BARRIER_GROUP group {}; group.Type = D3D12_BARRIER_TYPE_GLOBAL;
    group.NumBarriers = 1; group.pGlobalBarriers = &global;
    proxy->Barrier(1, &group);
    Require(!proxy->IsSplitIneligible(), "global barrier does not forbid split");
    acceptLegacy(); // Previously failed with source_layout_unknown / 0x80070032.
    Check(proxy->BeginInlineState(1, sources, states), "same inherited legacy contract for inline");
    proxy->EndInlineState();
    auto enhanced = [&](ID3D12Resource* resource, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after,
                        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
        D3D12_TEXTURE_BARRIER b {}; b.SyncBefore = b.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        b.AccessBefore = before == D3D12_BARRIER_LAYOUT_COMMON ? D3D12_BARRIER_ACCESS_COMMON : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        b.AccessAfter = after == D3D12_BARRIER_LAYOUT_COMMON ? D3D12_BARRIER_ACCESS_COMMON : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        b.LayoutBefore = before; b.LayoutAfter = after; b.pResource = resource;
        b.Subresources.IndexOrFirstMipLevel = subresource;
        D3D12_BARRIER_GROUP g {}; g.Type = D3D12_BARRIER_TYPE_TEXTURE; g.NumBarriers = 1; g.pTextureBarriers = &b;
        proxy->Barrier(1, &g);
    };
    In::Barrier(proxy.Get(), other.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    enhanced(other.Get(), D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);
    acceptLegacy(); // Unrelated TEXTURE barriers must not invalidate this source either.
    SetAllowEnhancedBarriers(false);
    Require(FAILED(proxy->BeginSplitState(1, sources, states)), "explicit opt-out still rejects enhanced lists");
    SetAllowEnhancedBarriers(true);
    In::Barrier(proxy.Get(), source.Get(), states[0], D3D12_RESOURCE_STATE_COMMON);
    enhanced(source.Get(), D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);

    // A valid, partial transition on a two-mip guide must fail atomically when
    // consumed, even when the preceding colour input has a known enhanced layout.
    D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto desc = source->GetDesc(); desc.MipLevels = 2;
    Ptr<ID3D12Resource> guide;
    Check(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&guide)), "partial guide");
    enhanced(guide.Get(), D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, 0);
    ID3D12Resource* mixed[] {source.Get(), guide.Get()};
    D3D12_RESOURCE_STATES mixedStates[] {states[0], states[0]};
    Require(FAILED(proxy->BeginSplitState(2, mixed, mixedStates)), "consumed partial guide rejected");
    Require(mixedStates[0] == states[0] && mixedStates[1] == states[0], "rejection changes no caller states");
    Require(std::strcmp(proxy->InlineRejectionReason(), "enhanced_layout_partial_or_pending") == 0, "partial rejection reason");
    proxy->EndInlineState(); // Failed Begin must not open a restoring scope.
    Check(proxy->BeginSplitState(1, sources, states), "unused partial guide does not reject colour");
    Require(states[0] == D3D12_RESOURCE_STATE_COMMON, "observed enhanced colour normalized");
    In::Barrier(proxy.Get(), source.Get(), states[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
    In::Barrier(proxy.Get(), output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    proxy->CopyResource(output.Get(), source.Get());
    In::Barrier(proxy.Get(), source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Check(proxy->SplitSegments(), "split with enhanced scope");
    Require(proxy->RecordingNativeList() != native.Get(), "continuation receives subsequent work");
    In::Barrier(proxy.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    proxy->EndInlineState();
    // Exact LayoutBefore verifies restoration on the continuation under the debug layer.
    enhanced(source.Get(), D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_COMMON);
    In::Barrier(proxy.Get(), source.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    enhanced(other.Get(), D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_COMMON);
    Check(proxy->Close(), "split close"); Check(proxy->ExecuteOn(gpu.queue.Get()), "split execute"); gpu.Wait(gpu.Signal());
    gpu.Pixels(source.Get(), redHalf); gpu.Pixels(output.Get(), redHalf);
    Check(proxy->Reset(allocator.Get(), nullptr), "reset clears observed layouts");
    states[0] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; acceptLegacy();
    Check(proxy->Close(), "reset close");
    if (gpu.messages) {
        const auto count = gpu.messages->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0; Check(gpu.messages->GetMessage(i, nullptr, &size), "debug message size");
            std::vector<unsigned char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(gpu.messages->GetMessage(i, message, &size), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "%s\n", message->pDescription); Require(false, "D3D12 debug-layer error");
            }
        }
        std::puts("split-state debug layer: no errors");
    }
    SetAllowEnhancedBarriers(false);
    std::puts("split-state PASS: inherited legacy state, unrelated barriers, partial rejection, enhanced round trip, split pixels, Reset");
}

static void Run(Gpu& gpu, bool timeout, bool enhanced = false, bool fullBudget = false, bool highRange = false) {
    static std::shared_ptr<In::Pipelines> cache;
    const float red[] {highRange ? 100000.f : .25f,.5f,.75f,1}, blue[] {.75f,.25f,.5f,1}, black[] {0,0,0,1};
    const UINT16 redHalf[] {0x3400,0x3800,0x3a00,0x3c00}, blueHalf[] {0x3a00,0x3400,0x3800,0x3c00};
    auto original = gpu.Texture(red), network = gpu.Texture(blue), decoded = gpu.Texture(black);
    auto resources = std::make_shared<In::Resources>(gpu.device.Get(), original.Get(), decoded.Get(), decoded.Get(), 8, 8, cache);
    if (cache) Require(resources->SideQueue() == cache->sideQueue.Get(), "new recordings reuse the session queue");
    cache = resources->PipelineCache();
    Require(resources->OutputTexture()->GetDesc().Format == original->GetDesc().Format, "FP32 fallback preserves source range");
    Ptr<ID3D12CommandAllocator> a; Ptr<ID3D12GraphicsCommandList> raw; Ptr<Sub::CommandListProxy> proxy;
    Check(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "allocator");
    Check(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&raw)), "list");
    Check(Sub::CommandListProxy::Create(gpu.device.Get(), a.Get(), raw.Get(), &proxy), "proxy");
    Ptr<ID3D12GraphicsCommandList7> list7;
    auto enhancedTransition = [&](D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after) {
        D3D12_TEXTURE_BARRIER b {}; b.SyncBefore = b.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        b.AccessBefore = before == D3D12_BARRIER_LAYOUT_COMMON ? D3D12_BARRIER_ACCESS_COMMON : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        b.AccessAfter = after == D3D12_BARRIER_LAYOUT_COMMON ? D3D12_BARRIER_ACCESS_COMMON : D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        b.LayoutBefore = before; b.LayoutAfter = after; b.pResource = original.Get();
        b.Subresources.IndexOrFirstMipLevel = 0;
        b.Subresources.NumMipLevels = b.Subresources.NumArraySlices = b.Subresources.NumPlanes = 1;
        D3D12_BARRIER_GROUP g {}; g.Type = D3D12_BARRIER_TYPE_TEXTURE; g.NumBarriers = 1; g.pTextureBarriers = &b;
        list7->Barrier(1, &g);
    };
    if (enhanced) {
        SetAllowEnhancedBarriers(true); Check(proxy.As(&list7), "enhanced interface");
        // This conversion belongs to the game, before it starts using enhanced layouts.
        In::Barrier(proxy.Get(), original.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        enhancedTransition(D3D12_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE);
        Require(!proxy->CanAppendCompute(), "experimental inline permission does not enable shared effects");
    }
    Ptr<ID3D12QueryHeap> heap; D3D12_QUERY_HEAP_DESC q {}; q.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS; q.Count = 1;
    Check(gpu.device->CreateQueryHeap(&q, IID_PPV_ARGS(&heap)), "query heap");
    proxy->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    Require(proxy->IsSplitIneligible() && proxy->CanRecordInline(), "open query allows inline only");
    ID3D12Resource* sources[] {original.Get()}; D3D12_RESOURCE_STATES states[] {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    Check(proxy->BeginInlineState(1, sources, states), "inline scope");
    Require(states[0] == (enhanced ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), "enhanced source normalized");
    resources->RecordCapture(proxy.Get()); resources->RecordWait(proxy.Get(), timeout && !fullBudget ? 0 : In::kLmxxfWaitSlices, In::kLmxxfWaitIterations);
    In::Barrier(proxy.Get(), network.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    In::Barrier(proxy.Get(), decoded.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    proxy->CopyResource(decoded.Get(), network.Get());
    In::Barrier(proxy.Get(), decoded.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    In::Barrier(proxy.Get(), network.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    resources->RecordSelect(proxy.Get(), states[0]); proxy->EndInlineState();
    Require(proxy->IsSplitIneligible(), "query still open after NR");
    proxy->EndQuery(heap.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    Require(!proxy->IsSplitIneligible(), "original query ends normally");
    Check(proxy->Close(), "close with intact query");
    for (unsigned replay = 0; replay < (enhanced ? 1u : 2u); ++replay) {
        std::atomic<bool> release { !timeout }, captured { false };
        resources->BeforeExecute(gpu.queue.Get());
        const auto started = std::chrono::steady_clock::now();
        std::thread worker([&] {
            Require(resources->WaitCapture(), "capture observed"); captured = true;
            std::printf("capture observed at %.3f ms\n", std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - started).count());
            while (!release) Sleep(1);
            if (!timeout) Sleep(20); // The game wait must allow independently scheduled work to finish.
            resources->NetworkQueued(true);
        });
        In::Execute(gpu.queue.Get(), raw.Get()); const auto tail = gpu.Signal();
        resources->GameSubmitted(gpu.fence.Get(), tail, S_OK); gpu.Wait(tail);
        std::printf("game tail at %.3f ms\n", std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - started).count());
        if (timeout) {
            Require(!resources->Complete(), "game tail alone cannot release late network");
            gpu.Pixels(resources->OutputTexture(), redHalf, red); release = true;
        }
        worker.join();
        const auto until = GetTickCount64() + 10000;
        while (!resources->Complete() && GetTickCount64() < until) Sleep(1);
        Require(resources->Complete() && captured, "both queues complete");
        std::printf("frozen success=%u last slice iterations=%u\n", resources->Succeeded(), resources->LastSpinCount());
        Require(resources->Succeeded() != timeout, "frozen success decision");
        if (fullBudget) Require(resources->LastSpinCount() == In::kLmxxfWaitIterations, "bounded wait actually exhausted its work budget");
        gpu.Pixels(resources->OutputTexture(), timeout ? redHalf : blueHalf, timeout ? red : blue);
        gpu.Pixels(resources->FirstTexture(), timeout ? redHalf : blueHalf, timeout ? red : blue);
    }
    std::printf("inline %s PASS: open query, unchanged native list, current pixels, replay%s\n",
                timeout ? "timeout" : "success", timeout ? ", late completion retention and original-frame selection" : "");
    // Bypass must overwrite the prior neural decision, including first-pass output,
    // without spending the polling budget or starting a network worker.
    resources->BeforeExecute(gpu.queue.Get(), true);
    In::Execute(gpu.queue.Get(), raw.Get()); const auto bypassTail = gpu.Signal();
    resources->CertifyWithoutNetwork(gpu.fence.Get(), bypassTail, S_OK, true); gpu.Wait(bypassTail);
    Require(resources->Complete() && !resources->Succeeded(), "bypass completion and frozen original decision");
    if (!timeout || fullBudget) Require(resources->LastSpinCount() == 1, "bypass skips polling budget");
    gpu.Pixels(resources->OutputTexture(), redHalf, red);
    gpu.Pixels(resources->FirstTexture(), redHalf, red);
    std::puts("inline bypass output and first-pass original pixels PASS");
    if (enhanced) {
        // NR returned to the game's enhanced SRV layout. A game transition using
        // that exact LayoutBefore must remain valid and preserve the selected pixels.
        Check(proxy->Reset(a.Get(), nullptr), "enhanced reset");
        enhancedTransition(D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_COMMON);
        In::Barrier(proxy.Get(), original.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(proxy->Close(), "restored enhanced layout close"); In::Execute(gpu.queue.Get(), raw.Get()); gpu.Wait(gpu.Signal());
        std::puts("enhanced whole-resource normalization/restoration PASS");
        SetAllowEnhancedBarriers(false);
    }
    // A producer rejected before native submission has only the side reset to retire.
    resources->BeforeExecute(gpu.queue.Get());
    resources->CertifyWithoutNetwork(nullptr, 0, E_FAIL);
    const auto cancelledUntil = GetTickCount64() + 5000;
    while (!resources->Complete() && GetTickCount64() < cancelledUntil) Sleep(1);
    Require(resources->Complete() && resources->WasAborted() && !resources->Succeeded(), "aborted producer retires only certified side reset");
}
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::strcmp(argv[1], "--split-state") == 0) {
            Gpu gpu(false, true); RunSplitState(gpu); return 0;
        }
        Gpu gpu(argc > 1 && std::strcmp(argv[1], "--hardware") == 0); Run(gpu, false); Run(gpu, true); Run(gpu, true, false, true);
        Run(gpu, true, false, false, true);
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options {};
        if (SUCCEEDED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof options)) && options.EnhancedBarriersSupported)
            Run(gpu, false, true);
        else std::puts("enhanced barriers SKIP: device capability unavailable");
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return 0;
}

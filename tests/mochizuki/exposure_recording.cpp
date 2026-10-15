#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include "dlssnr/backend/mochizuki_runtime/MochizukiAutoExposure.h"

template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
using Probe = MochizukiAutoExposure::Probe;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); } }
static void Check(HRESULT hr, const char* why) { Require(SUCCEEDED(hr), why); }
int main() {
    Ptr<IDXGIFactory4> factory; Ptr<IDXGIAdapter> warp; Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    Ptr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Ptr<ID3D12Fence> tail, unrelated, gate;
    for (auto* f : {std::addressof(tail), std::addressof(unrelated), std::addressof(gate)})
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(f->GetAddressOf())), "fence");
    UINT64 serial = 0;
    auto drain = [&] {
        Check(queue->Signal(tail.Get(), ++serial), "drain signal");
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "event");
        Check(tail->SetEventOnCompletion(serial, event), "event registration");
        Require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "queue completes"); CloseHandle(event);
    };
    auto newList = [&](Ptr<ID3D12CommandAllocator>& allocator, Ptr<ID3D12GraphicsCommandList>& list) {
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    };
    Ptr<ID3D12Resource> texture[2], upload;
    D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = rd.DepthOrArraySize = rd.MipLevels = rd.SampleDesc.Count = 1; rd.Format = DXGI_FORMAT_R32_FLOAT;
    for (auto& t : texture) Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t)), "texture");
    hp.Type = D3D12_HEAP_TYPE_UPLOAD; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 1024;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "upload");
    void* data = nullptr; Check(upload->Map(0, nullptr, &data), "map");
    *static_cast<float*>(data) = 2.f; *reinterpret_cast<float*>(static_cast<char*>(data) + 512) = 8.f; upload->Unmap(0, nullptr);
    Ptr<ID3D12CommandAllocator> initAllocator; Ptr<ID3D12GraphicsCommandList> init;
    newList(initAllocator, init);
    for (unsigned i = 0; i < 2; ++i) {
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {}; dst.pResource = texture[i].Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 512 * i; src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256};
        init->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        MochizukiAutoExposure::Transition(init.Get(), texture[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    Check(init->Close(), "init close"); ID3D12CommandList* initial[] {init.Get()}; queue->ExecuteCommandLists(1, initial); drain();
    Probe probe; Require(probe.CreateTexel(device.Get()), "texel probe");
    Probe::Ticket tickets[5]; Ptr<ID3D12CommandAllocator> allocators[5]; Ptr<ID3D12GraphicsCommandList> lists[5];
    for (unsigned i = 0; i < 5; ++i) {
        newList(allocators[i], lists[i]);
        const bool accepted = probe.RecordTexel(lists[i].Get(), texture[i % 2].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, tickets[i], i % 2 ? .5f : 3.f);
        Require(accepted == (i < 4), "fifth retained recipe skips measurement");
        Check(lists[i]->Close(), "close"); Check(unrelated->Signal(i + 1), "unrelated advance");
        float value = 0; Require(!probe.Poll(&value), "unrelated fence never completes an unsubmitted measurement");
    }
    auto submit = [&](unsigned i) {
        probe.Begin(tickets[i]); ID3D12CommandList* commands[] {lists[i].Get()}; queue->ExecuteCommandLists(1, commands);
        Check(queue->Signal(tail.Get(), ++serial), "measurement signal"); probe.Submitted(tickets[i], tail.Get(), serial);
    };
    // Submit the second recording first. The first one's readback has never been written.
    submit(1); drain(); float value = 0;
    Require(probe.Poll(&value) && value == 4.f && probe.Measured() == 1, "out-of-order measurement uses its own tail and factor");
    Ptr<ID3D12CommandAllocator> extraAllocator; Ptr<ID3D12GraphicsCommandList> extra; Probe::Ticket extraTicket;
    newList(extraAllocator, extra);
    Require(!probe.RecordTexel(extra.Get(), texture[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, extraTicket), "completed retained recipe still owns its readback");
    Check(extra->Close(), "extra close");
    // Replay stays pinned and cannot be read while the new execution is pending.
    Check(queue->Wait(gate.Get(), 1), "hold replay"); submit(1);
    Require(!probe.Poll(&value), "replay's unfinished resolve is not readable");
    Check(gate->Signal(1), "release replay"); drain();
    Require(probe.Poll(&value) && value == 4.f, "replay reads its actual completion");
    submit(0); drain(); Require(probe.Poll(&value) && value == 6.f, "first recording did not inherit second recording's fence/data");
    const UINT recycled = tickets[1]->slot;
    lists[1].Reset(); allocators[1].Reset(); tickets[1].reset(); // invalidated and all executions retired
    extra.Reset(); extraAllocator.Reset(); newList(extraAllocator, extra);
    Require(probe.RecordTexel(extra.Get(), texture[0].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, extraTicket) && extraTicket->slot == recycled,
            "only invalidated retired recipe releases its slot");
    Check(extra->Close(), "recycled close"); extra.Reset(); extraTicket.reset();
    for (unsigned i = 0; i < 5; ++i) { lists[i].Reset(); tickets[i].reset(); }
    probe.Destroy();
    // Exercise actual meter dispatch/SRV descriptors as well as the texel copy path.
    Require(probe.Create(device.Get()), "meter pipeline");
    for (unsigned i = 0; i < 4; ++i) {
        allocators[i].Reset(); newList(allocators[i], lists[i]);
        Require(probe.Record(lists[i].Get(), texture[i % 2].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, 1, 1, tickets[i]), "meter record");
        Check(lists[i]->Close(), "meter close");
    }
    submit(3); drain();
    const float expected = MochizukiAutoExposure::Target(8.f * .2126f);
    Require(probe.Poll(&value) && std::fabs(value - expected) < 1e-5f && probe.Measured() == 1,
            "retained meter descriptors sample the intended texture out of order");
    for (unsigned i = 0; i < 4; ++i) { lists[i].Reset(); tickets[i].reset(); }
    std::puts("EXPOSURE_RECORDING_OK unsubmitted cap, out-of-order fences, replay lifetime, texel factor, meter SRV");
}

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
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
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
    // Exercise the shipped finite guard directly, before the meter's RGB max clamps negative inputs to zero.
    // This also checks parity with the original Windows intrinsics under the production cs_5_0/flags=0 compiler.
    const std::string meterShader = MochizukiAutoExposure::kMeterShader;
    const size_t guardStart = meterShader.find("const bool ok = ");
    Require(guardStart != std::string::npos, "shipped finite guard exists");
    const size_t guardEnd = meterShader.find(';', guardStart);
    Require(guardEnd != std::string::npos, "shipped finite guard terminates");
    const std::string guardShader =
        "RWByteAddressBuffer Result : register(u0);\n"
        "cbuffer Inputs : register(b0) { uint4 Bits[3]; };\n"
        "[numthreads(1, 1, 1)] void main() {\n"
        "for (uint i = 0u; i < 12u; ++i) {\n"
        "const float l = asfloat(Bits[i / 4u][i % 4u]);\n" +
        meterShader.substr(guardStart, guardEnd - guardStart + 1) +
        "\nResult.Store(i * 4u, ok ? 1u : 0u);\n"
        "Result.Store(48u + i * 4u, (!isnan(l) && !isinf(l)) ? 1u : 0u);\n}}\n";
    const uint32_t samples[] = {
        0x7fc00000u, 0xffc00000u, 0x7f800001u, // positive/negative quiet NaN and signaling NaN
        0x7f800000u, 0xff800000u,             // positive/negative infinity
        0x00000000u, 0x80000000u,             // positive/negative zero
        0x00000001u, 0x80000001u,             // positive/negative denormal
        0x3f800000u, 0xbf800000u, 0x7f7fffffu // normal values, including the largest finite float
    };
    D3D12_ROOT_PARAMETER guardParameters[2] {};
    guardParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    guardParameters[0].Constants.Num32BitValues = 12;
    guardParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC guardSignature {};
    guardSignature.NumParameters = 2; guardSignature.pParameters = guardParameters;
    Ptr<ID3DBlob> serialized, messages;
    Check(D3D12SerializeRootSignature(&guardSignature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages), "finite guard root serialization");
    Ptr<ID3D12RootSignature> guardRoot;
    Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&guardRoot)), "finite guard root");
    std::string error;
    Ptr<ID3DBlob> guardCode; guardCode.Attach(MochizukiAutoExposure::Compile(guardShader.c_str(), "main", &error));
    Require(guardCode != nullptr, error.c_str());
    D3D12_COMPUTE_PIPELINE_STATE_DESC guardPipeline {}; guardPipeline.pRootSignature = guardRoot.Get();
    guardPipeline.CS = {guardCode->GetBufferPointer(), guardCode->GetBufferSize()};
    Ptr<ID3D12PipelineState> guardPso;
    Check(device->CreateComputePipelineState(&guardPipeline, IID_PPV_ARGS(&guardPso)), "finite guard pipeline");
    Ptr<ID3D12Resource> guardOutput, guardReadback;
    hp.Type = D3D12_HEAP_TYPE_DEFAULT; rd.Width = 96; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&guardOutput)), "finite guard output");
    hp.Type = D3D12_HEAP_TYPE_READBACK; rd.Flags = D3D12_RESOURCE_FLAG_NONE;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&guardReadback)), "finite guard readback");
    Ptr<ID3D12CommandAllocator> guardAllocator; Ptr<ID3D12GraphicsCommandList> guardList;
    newList(guardAllocator, guardList);
    guardList->SetComputeRootSignature(guardRoot.Get()); guardList->SetPipelineState(guardPso.Get());
    guardList->SetComputeRoot32BitConstants(0, 12, samples, 0);
    guardList->SetComputeRootUnorderedAccessView(1, guardOutput->GetGPUVirtualAddress());
    guardList->Dispatch(1, 1, 1);
    MochizukiAutoExposure::Transition(guardList.Get(), guardOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    guardList->CopyResource(guardReadback.Get(), guardOutput.Get());
    Check(guardList->Close(), "finite guard close");
    ID3D12CommandList* guardCommands[] {guardList.Get()}; queue->ExecuteCommandLists(1, guardCommands); drain();
    D3D12_RANGE readRange {0, 96}; data = nullptr;
    Check(guardReadback->Map(0, &readRange, &data), "finite guard map");
    const auto* results = static_cast<const uint32_t*>(data);
    for (unsigned i = 0; i < 12; ++i) {
        const uint32_t finite = i >= 5 ? 1u : 0u;
        Require(results[i] == finite, "finite guard rejects NaN/infinities and keeps zero/finite/denormal");
        Require(results[i] == results[12 + i], "finite guard matches original Windows intrinsics");
    }
    D3D12_RANGE noWrite {0, 0}; guardReadback->Unmap(0, &noWrite);

    // Actual meter dispatch: invalid samples must not dilute the usable average; an all-invalid frame keeps history.
    Ptr<ID3D12Resource> mixedTexture, mixedUpload;
    hp.Type = D3D12_HEAP_TYPE_DEFAULT; rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = 4;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = rd.SampleDesc.Count = 1; rd.Format = DXGI_FORMAT_R32_FLOAT;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mixedTexture)), "mixed meter texture");
    hp.Type = D3D12_HEAP_TYPE_UPLOAD; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = 256;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mixedUpload)), "mixed meter upload");
    const float infinity = std::numeric_limits<float>::infinity();
    const float frames[][4] = {{2.f, 8.f, 2.f, 8.f}, {2.f, 8.f, infinity, infinity}, {infinity, infinity, infinity, infinity}};
    for (unsigned frame = 0; frame < 3; ++frame) {
        Check(mixedUpload->Map(0, nullptr, &data), "mixed meter map");
        std::memcpy(data, frames[frame], sizeof(frames[frame])); mixedUpload->Unmap(0, nullptr);
        Ptr<ID3D12CommandAllocator> meterAllocator; Ptr<ID3D12GraphicsCommandList> meterList; Probe::Ticket meterTicket;
        newList(meterAllocator, meterList);
        if (frame) MochizukiAutoExposure::Transition(meterList.Get(), mixedTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {}; dst.pResource = mixedTexture.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = mixedUpload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 4, 1, 1, 256};
        meterList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        MochizukiAutoExposure::Transition(meterList.Get(), mixedTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Require(probe.Record(meterList.Get(), mixedTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, 4, 1, meterTicket), "mixed meter record");
        Check(meterList->Close(), "mixed meter close"); probe.Begin(meterTicket);
        ID3D12CommandList* meterCommands[] {meterList.Get()}; queue->ExecuteCommandLists(1, meterCommands);
        Check(queue->Signal(tail.Get(), ++serial), "mixed meter signal"); probe.Submitted(meterTicket, tail.Get(), serial); drain();
        const float previousExposure = probe.Exposure(); const UINT previousMeasured = probe.Measured();
        const bool measured = probe.Poll(&value);
        if (frame < 2) {
            Require(measured && std::fabs(probe.Mean() - 5.f * .2126f) < 1e-5f, "normal and mixed-invalid meter means are unchanged");
            const float expectedExposure = MochizukiAutoExposure::Blend(previousExposure, MochizukiAutoExposure::Target(5.f * .2126f), true);
            Require(std::fabs(value - expectedExposure) < 1e-5f && probe.Measured() == previousMeasured + 1, "finite meter exposure is unchanged");
        } else {
            Require(!measured && value == previousExposure && probe.Measured() == previousMeasured, "all-invalid meter frame preserves exposure and count");
            Require(std::fabs(probe.Mean() - 5.f * .2126f) < 1e-5f, "all-invalid meter frame preserves last mean");
        }
    }
    std::puts("EXPOSURE_RECORDING_OK unsubmitted cap, out-of-order fences, replay lifetime, texel factor, meter SRV, NaN/Inf finite guard parity, normal/mixed/all-invalid meter");
}

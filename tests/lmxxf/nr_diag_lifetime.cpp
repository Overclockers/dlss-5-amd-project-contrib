#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"
namespace Own = DlssNr::Backend::LmxxfRecording;
namespace Diag = DlssNr::FrameDiag;
using Microsoft::WRL::ComPtr;
static void Require(bool ok, const char* why) { if (!ok) { std::fprintf(stderr, "FAIL %s\n", why); std::exit(1); } }
int main() {
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
    Require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
        SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
    ComPtr<ID3D12Fence> held;
    Require(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&held))), "held fence");
    auto owner = std::make_shared<Own::SessionOwner>();
    auto original = Diag::GpuStamps::Acquire(device.Get(), Diag::kGpuStampCount);
    Require(bool(original), "stamp allocation"); auto* recorded = original.get();
    for (unsigned i = 0; i < 9; ++i) {
        Own::SessionOwner::PendingSplitDiag p; p.fence = held; p.value = 1;
        p.stamps = i == 0 ? std::move(original) : Diag::GpuStamps::Acquire(device.Get(), Diag::kGpuStampCount);
        owner->splitDiagPending.push_back(std::move(p));
    }
    owner->FlushSplitDiag();
    Require(owner->splitDiagPending.size() == 9, "overflow does not retire unfinished GPU writes");
    auto recycled = Diag::GpuStamps::Acquire(device.Get(), Diag::kGpuStampCount);
    Require(recycled.get() != recorded, "unfinished query/readback cannot enter pool");
    std::shared_ptr<Diag::GpuStamps> budget[8];
    for (auto& sample : budget) { sample = owner->AcquireSplitStamps(device.Get()); Require(bool(sample), "eight samples admitted"); }
    Require(!owner->AcquireSplitStamps(device.Get()), "ninth sample skips without waiting");
    auto lease = std::make_shared<Own::Lease>(owner, nullptr, DlssNr::Submission::RecordingIdentity{});
    lease->splitStamps = budget[0]; budget[0].reset();
    lease->diag.active = true; lease->splitDiagFence = held.Get();
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd {};
    Require(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "queue");
    lease->splitDiagQueue = queue.Get(); lease->splitDiagValue = 1;
    lease->EmitSplitDiag("apply");
    Require(bool(lease->splitStamps), "first execution emit does not unpin retained native query addresses");
    // Mark the queued diagnostic inactive so no readback is attempted: this fixture tests ownership only.
    owner->splitDiagPending.back().diag.active = false;
    Require(SUCCEEDED(held->Signal(1)), "release held tail"); owner->FlushSplitDiag();
    Require(owner->splitDiagPending.empty() && !owner->AcquireSplitStamps(device.Get()), "completed recipe still owns sampling budget");
    lease.reset(); Require(bool(owner->AcquireSplitStamps(device.Get())), "invalidated retired recipe can return stamps to pool");
    std::puts("NR_DIAG_LIFETIME_OK unfinished overflow retained, eight-sample cap, completed recipe pin");
}

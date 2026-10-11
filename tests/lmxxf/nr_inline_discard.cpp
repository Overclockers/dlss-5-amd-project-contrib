#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SplitEligibility.h"

template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
namespace Own=DlssNr::Backend::LmxxfRecording;
namespace Sub=DlssNr::Submission;
namespace In=DlssNr::Backend::InlineRecording;
static void Require(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
static void Check(HRESULT hr,const char* why) {Require(SUCCEEDED(hr),why);}
struct Runtime {
    unsigned invalidated=0,collected=0;
    static int32_t Invalidate(void* context,void*) {++static_cast<Runtime*>(context)->invalidated;return LMXXF_NR_OK;}
    static int32_t Collect(void* context,void*) {++static_cast<Runtime*>(context)->collected;return LMXXF_NR_OK;}
    static int32_t Reset(void*) {return LMXXF_NR_OK;}
    static int32_t Destroy(void*) {return LMXXF_NR_OK;}
};

int main() try {
    Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter> warp;Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
    Check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=8;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    Ptr<ID3D12Resource> original,candidate;
    for(auto* texture:{std::addressof(original),std::addressof(candidate)}) Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(texture->GetAddressOf())),"texture");
    Runtime runtime;
    auto owner=std::make_shared<Own::SessionOwner>();owner->context=&runtime;
    owner->api.InvalidateRecording=&Runtime::Invalidate;owner->api.CollectRecording=&Runtime::Collect;owner->api.Destroy=&Runtime::Destroy;
    owner->api.ResetHistory=&Runtime::Reset;
    const auto registry=Own::Registry().size();
    for(unsigned n=0;n<Own::SessionOwner::kInlineAhead*3;++n) {
        Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> native;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&native)),"list");
        Ptr<Sub::CommandListProxy> proxy;Check(Sub::CommandListProxy::Create(device.Get(),allocator.Get(),native.Get(),&proxy),"proxy");
        auto resources=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
        owner->inlinePipelines=resources->PipelineCache();
        std::shared_ptr<Own::Lease> lease;
        {
            std::lock_guard lock(Sub::RecordingMutex());
            Require(!owner->InlineBusy(),"previous discard released inline capacity");
            lease=Own::Attach(owner,reinterpret_cast<void*>(uintptr_t(n+1)),proxy.Get(),resources);
            Require(bool(lease) && owner->inlineInFlight.size()==1,"fresh recording reserves one slot");
            lease->ready=lease->neural=true;
        }
        Check(proxy->Close(),"close");
        if(n%2==0) Check(proxy->Reset(allocator.Get(),nullptr),"discard by Reset");
        else proxy.Reset(); // Real final COM Release, never submitted.
        {
            std::lock_guard lock(Sub::RecordingMutex());
            Require(lease->invalidated && lease->job==nullptr,"discard invalidated and collected job");
            Require(owner->inlineInFlight.empty() && !owner->InlineBusy(),"discard released its reservation");
            Require(Own::Registry().size()==registry,"discard removed registry entry");
        }
        Require(!resources->Started(),"fixture never submitted GPU work");
        proxy.Reset();lease.reset();resources.reset();
        Require(owner->liveRecordings==0,"discard released lease ownership");
    }
    Require(runtime.invalidated==12 && runtime.collected==12,"each discarded job retired exactly once");
    Require(!owner->failed,"discard is not a session failure");
    // The new cleanup must not drop a reservation that already has queue work.
    Ptr<ID3D12CommandQueue> game;D3D12_COMMAND_QUEUE_DESC queueDesc{};
    Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&game)),"queue");
    auto started=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
    started->BeforeExecute(game.Get());
    {
        std::lock_guard lock(Sub::RecordingMutex());owner->AdmitInline(started);owner->ForgetUnstarted(started);
        Require(owner->inlineInFlight.size()==1,"started reservation remains owned");
    }
    started->CertifyWithoutNetwork(nullptr,0,E_FAIL);
    const auto deadline=GetTickCount64()+5000;
    while(!started->Complete()){Require(GetTickCount64()<deadline,"side-reset completion");Sleep(1);}
    {
        std::lock_guard lock(Sub::RecordingMutex());owner->ReclaimInlineLocked();
        Require(owner->inlineInFlight.empty(),"completed started reservation follows normal reclaim");
    }
    std::puts("INLINE_UNSUBMITTED_DISCARD_OK 12 alternating Reset/Release, capacity reusable");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}

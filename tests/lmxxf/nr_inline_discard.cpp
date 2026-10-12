#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <thread>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SplitEligibility.h"

template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
namespace Own=DlssNr::Backend::LmxxfRecording;
namespace Sub=DlssNr::Submission;
namespace In=DlssNr::Backend::InlineRecording;
static void Require(bool ok,const char* why) {if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
static void Check(HRESULT hr,const char* why) {Require(SUCCEEDED(hr),why);}
static unsigned failureReports=0;
static Own::SessionOwner::Failure firstFailure;
static void ReportFailure(const Own::SessionOwner::Failure& failure) noexcept {
    ++failureReports;firstFailure=failure;
}
struct Runtime {
    unsigned invalidated=0,collected=0,enqueueFailures=0,ended=0;
    static int32_t Invalidate(void* context,void*) {++static_cast<Runtime*>(context)->invalidated;return LMXXF_NR_OK;}
    static int32_t Collect(void* context,void*) {++static_cast<Runtime*>(context)->collected;return LMXXF_NR_OK;}
    static int32_t Reset(void*) {return LMXXF_NR_OK;}
    static int32_t Destroy(void*) {return LMXXF_NR_OK;}
    static int32_t Begin(void*,void*,void*) {return LMXXF_NR_OK;}
    static int32_t Enqueue(void* context,void*,void*) {++static_cast<Runtime*>(context)->enqueueFailures;return LMXXF_NR_FAILED;}
    static int32_t Error(char* out,uint32_t size) {
        if(size)std::snprintf(out,size,"injected network enqueue failure");return LMXXF_NR_OK;
    }
    static int32_t End(void* context,void*,void*,uint32_t,void*,uint64_t,int32_t) {++static_cast<Runtime*>(context)->ended;return LMXXF_NR_OK;}
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
    owner->api.BeginRecordingExecution=&Runtime::Begin;owner->api.EnqueueHip=&Runtime::Enqueue;
    owner->api.GetLastError=&Runtime::Error;owner->api.EndRecordingExecution=&Runtime::End;
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
    // Real callback capture/Enqueue failure, real game/side queues, real proxy
    // invalidation and collection. More failures than the admission cap.
    owner->onFailure=&ReportFailure;
    for(unsigned n=0;n<6;++n) {
        Ptr<ID3D12CommandAllocator> allocator;Ptr<ID3D12GraphicsCommandList> native;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"failure allocator");
        Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&native)),"failure list");
        Ptr<Sub::CommandListProxy> proxy;Check(Sub::CommandListProxy::Create(device.Get(),allocator.Get(),native.Get(),&proxy),"failure proxy");
        auto resources=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
        std::shared_ptr<Own::Lease> lease;
        {
            std::lock_guard lock(Sub::RecordingMutex());
            Require(!owner->InlineBusy(),"failed enqueue capacity reusable");
            lease=Own::Attach(owner,reinterpret_cast<void*>(uintptr_t(n+100)),proxy.Get(),resources);
            Require(bool(lease),"admit after failed enqueue");lease->ready=lease->neural=true;
            resources->RecordCapture(proxy.Get());
        }
        Check(proxy->Close(),"close failure capture");
        Check(proxy->ExecuteOn(game.Get()),"submit failure capture");
        proxy.Reset(); // Invalidate while the worker can still be pending.
        const auto until=GetTickCount64()+5000;
        for(;;) {
            bool done=false;
            {std::lock_guard lock(Sub::RecordingMutex());Own::CollectLocked();done=lease->job==nullptr;}
            if(done)break;
            Require(GetTickCount64()<until,"failed worker eventually collected");Sleep(1);
        }
        {
            std::lock_guard lock(Sub::RecordingMutex());
            Require(resources->WasAborted()&&resources->Complete(),"aborted work dual completion");
            Require(owner->inlineInFlight.empty()&&owner->inlineEnqueueOrder.empty(),"failure releases both admission queues");
            Require(Own::Registry().size()==registry,"failed lease collected");
        }
    }
    Require(runtime.enqueueFailures==6&&runtime.ended==6,"six actual callback enqueue failures ended");
    Require(failureReports==1 && std::strcmp(firstFailure.phase,"EnqueueHip")==0 &&
            firstFailure.result==LMXXF_NR_FAILED && firstFailure.queue &&
            std::strcmp(firstFailure.error.data(),"injected network enqueue failure")==0,
            "first worker error reaches reporter exactly once with its original text and queue");
    owner->RecordFailure("EndRecordingExecution",LMXXF_NR_UNAVAILABLE,nullptr,"secondary poisoned session");
    Require(failureReports==1 && owner->failure.result==LMXXF_NR_FAILED &&
            std::strcmp(owner->failure.error.data(),firstFailure.error.data())==0,
            "secondary unavailable error cannot replace first enqueue failure");
    // Side completion never certifies a game fence that has not reached its tail.
    Ptr<ID3D12Fence> delayedGame;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&delayedGame)),"delayed game");
    auto aborted=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
    aborted->BeforeExecute(game.Get());aborted->NetworkQueued(false);aborted->GameSubmitted(delayedGame.Get(),1,S_OK);
    Sleep(20);Require(!aborted->Complete(),"abort retains pending game tail");
    Check(delayedGame->Signal(1),"complete delayed game");
    const auto abortUntil=GetTickCount64()+5000;
    while(!aborted->Complete()){Require(GetTickCount64()<abortUntil,"abort side tail");Sleep(1);}
    // Turn notification wakes a queued worker without millisecond lock polling.
    auto turn=std::make_shared<Own::Lease>(owner,reinterpret_cast<void*>(200),Sub::RecordingIdentity{});
    {std::lock_guard lock(Sub::RecordingMutex());owner->NoteInlineEnqueue(reinterpret_cast<void*>(199));owner->NoteInlineEnqueue(turn->job);}
    std::atomic<bool> woke=false;
    std::thread waiter([&]{woke=turn->WaitInlineTurn();});Sleep(20);Require(!woke,"later enqueue waits its turn");
    {std::lock_guard lock(Sub::RecordingMutex());owner->PopInlineEnqueue(reinterpret_cast<void*>(199));}
    waiter.join();Require(woke,"pop notifies queued worker");
    {std::lock_guard lock(Sub::RecordingMutex());owner->PopInlineEnqueue(turn->job);}
    // Truly unknown game submission is retained, but cannot become actionable
    // merely by rearming a collection timer every 100 ms.
    auto unknown=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
    unknown->BeforeExecute(game.Get());unknown->NetworkQueued(false);unknown->GameSubmitted(delayedGame.Get(),1,E_FAIL);
    auto retained=std::make_shared<Own::Lease>(owner,reinterpret_cast<void*>(201),Sub::RecordingIdentity{});
    retained->inlineResources=unknown;retained->invalidated=true;
    {
        std::lock_guard lock(Sub::RecordingMutex());Own::Registry().push_back(retained);
        Require(!unknown->Complete()&&unknown->RetirementUncertain(),"unknown submission never invents proof");
        Require(!Own::CollectLocked(),"unknown submission does not request permanent timer polling");
        Require(retained->job!=nullptr,"unknown job remains pinned");
    }
    std::puts("INLINE_UNSUBMITTED_DISCARD_OK 12 Reset/Release, 6 callback enqueue failures, dual completion, turn notification");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}

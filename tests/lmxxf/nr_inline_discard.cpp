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
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionHooks.h"
#include "queue_faults.h"

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
    unsigned invalidated=0,collected=0,enqueueFailures=0,ended=0,begins=0,resets=0;
    int32_t endStatus=E_PENDING; void* endFence=nullptr; uint64_t endValue=0;
    static int32_t Invalidate(void* context,void*) {++static_cast<Runtime*>(context)->invalidated;return LMXXF_NR_OK;}
    static int32_t Collect(void* context,void*) {++static_cast<Runtime*>(context)->collected;return LMXXF_NR_OK;}
    static int32_t Reset(void* context) {++static_cast<Runtime*>(context)->resets;return LMXXF_NR_OK;}
    static int32_t Destroy(void*) {return LMXXF_NR_OK;}
    static int32_t Begin(void* context,void*,void*) {++static_cast<Runtime*>(context)->begins;return LMXXF_NR_OK;}
    static int32_t Enqueue(void* context,void*,void*) {++static_cast<Runtime*>(context)->enqueueFailures;return LMXXF_NR_FAILED;}
    static int32_t Error(char* out,uint32_t size) {
        if(size)std::snprintf(out,size,"injected network enqueue failure");return LMXXF_NR_OK;
    }
    static int32_t End(void* context,void*,void*,uint32_t flags,void* fence,uint64_t value,int32_t status) {
        auto& r=*static_cast<Runtime*>(context);++r.ended;r.endStatus=status;r.endFence=fence;r.endValue=value;
        // Match both real runtimes: an End with submitted work and no certificate is terminal, not retryable.
        return flags && (FAILED(status)||!fence||!value) ? LMXXF_NR_UNAVAILABLE : LMXXF_NR_OK;
    }
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
    // Cancel a queued turn before its actual threadpool callback starts. There
    // is deliberately no capture: polling before the turn would spend 10 s on
    // the GPU and misreport this as a capture failure instead of order failure.
    Runtime cancelledRuntime;
    auto cancelledOwner=std::make_shared<Own::SessionOwner>();
    cancelledOwner->api=owner->api;cancelledOwner->context=&cancelledRuntime;
    auto cancelledResources=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),
        nullptr,8,8,owner->inlinePipelines);
    cancelledResources->BeforeExecute(game.Get());
    auto cancelled=std::make_shared<Own::Lease>(cancelledOwner,reinterpret_cast<void*>(299),Sub::RecordingIdentity{});
    cancelled->inlineResources=cancelledResources;cancelled->begun=cancelled->ready=true;
    {
        std::lock_guard lock(Sub::RecordingMutex());
        cancelledOwner->NoteInlineEnqueue(cancelled->job);
        cancelled->ProducerSubmitted({});
        cancelledOwner->PopInlineEnqueue(cancelled->job);
    }
    const auto cancelUntil=GetTickCount64()+3000;
    for(;;) {
        bool done=false;
        {std::lock_guard lock(Sub::RecordingMutex());done=cancelled->inlineEnqueueFinished;}
        if(done)break;
        Require(GetTickCount64()<cancelUntil,"cancelled turn skips capture polling");Sleep(1);
    }
    Require(cancelledRuntime.enqueueFailures==0 && cancelledOwner->failed &&
        std::strcmp(cancelledOwner->failure.phase,"inline order")==0,"cancelled turn reports order, not capture");
    cancelledResources->CertifyWithoutNetwork(nullptr,0,E_FAIL);
    while(!cancelledResources->Complete()) {
        Require(GetTickCount64()<cancelUntil,"cancelled callback side tail completes");Sleep(1);
    }
    std::puts("INLINE_CANCELLED_TURN_OK no capture polling or network enqueue");
    // The shared-buffer backend admits one reservation, including before submit.
    // Completed replays must reacquire a slot for NR, but a full cap must not
    // drop their original game batch. Bypass retains both queue proofs.
    Runtime admissionRuntime;
    auto admission=std::make_shared<Own::SessionOwner>();
    admission->api=owner->api;admission->context=&admissionRuntime;
    auto makeResources=[&] { return std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),
        nullptr,8,8,owner->inlinePipelines); };
    auto first=makeResources(), second=makeResources();
    Require(admission->inlineLimit==1,"shared-buffer backend defaults to one slot");
    Require(admission->AdmitInline(first)&&admission->InlineBusy(),"unsubmitted frame reserves shared buffers");
    Require(!admission->AdmitInline(second),"second capture cannot overwrite reserved shared buffers");
    admission->ForgetUnstarted(first);
    Require(admission->AdmitInline(second),"discard releases single-frame capacity");
    second->BeforeExecute(game.Get());
    Ptr<ID3D12Fence> heldTail;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&heldTail)),"admission game tail");
    second->NetworkQueued(false);second->GameSubmitted(heldTail.Get(),1,S_OK);
    Require(!admission->AdmitInline(first),"timeout side completion cannot release an unfinished game tail");
    Check(heldTail->Signal(1),"admission game complete");
    const auto admissionUntil=GetTickCount64()+5000;
    while(!second->Complete()){Require(GetTickCount64()<admissionUntil,"admission side complete");Sleep(1);}
    Require(admission->AdmitInline(first),"both completions release shared buffers");
    auto replay=std::make_shared<Own::Lease>(admission,reinterpret_cast<void*>(300),Sub::RecordingIdentity{});
    replay->inlineResources=second;
    Sub::RecordingExecution replayExecution{};replayExecution.queue=game.Get();replayExecution.serial=1;
    Ptr<ID3D12CommandAllocator> batchAlloc,ordinaryAlloc;
    Ptr<ID3D12GraphicsCommandList> batchRaw,ordinaryRaw;
    Ptr<Sub::CommandListProxy> batchProxy;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&batchAlloc)),"batch allocator");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&ordinaryAlloc)),"ordinary allocator");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,batchAlloc.Get(),nullptr,IID_PPV_ARGS(&batchRaw)),"batch native");
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,ordinaryAlloc.Get(),nullptr,IID_PPV_ARGS(&ordinaryRaw)),"ordinary native");
    Check(Sub::CommandListProxy::Create(device.Get(),batchAlloc.Get(),batchRaw.Get(),&batchProxy),"batch proxy");
    replay->identity=batchProxy->Identity();replayExecution.identity=replay->identity;
    replay->ready=true;
    Check(batchProxy->ObserveRecording(replay),"observe completed recording");
    second->RecordCapture(batchProxy.Get());second->RecordWait(batchProxy.Get());
    second->RecordSelect(batchProxy.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Check(batchProxy->Close(),"batch close");Check(ordinaryRaw->Close(),"ordinary close");
    Ptr<ID3D12Fence> gate;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"batch gate");
    Check(game->Wait(gate.Get(),1),"hold game batch");
    static unsigned rawCalls=0,rawLists=0;
    ID3D12CommandList* lists[]{ordinaryRaw.Get(),batchProxy.Get()};
    const auto failures=Sub::g_submissionFailures.load();
    Sub::Hooks::ExecuteExpanded(game.Get(),2,lists,nullptr,nullptr,
        [](ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* native){
            ++rawCalls;rawLists+=count;queue->ExecuteCommandLists(count,native);
        });
    Require(rawCalls==1&&rawLists==2&&Sub::g_submissionFailures.load()==failures,"full cap preserves entire original batch");
    Require(!second->Complete()&&!replay->begun&&admission->inlineEnqueueOrder.empty(),"bypass retains game tail without worker");
    Require(admissionRuntime.begins==0&&admissionRuntime.ended==0&&admissionRuntime.enqueueFailures==0,
        "bypass never enters runtime recording execution");
    Require(admission->inlineInFlight.size()==1&&admission->inlineInFlight.front()==first&&admission->inlineBypassed==1,
        "bypass preserves existing reservation and cap");
    Require(batchProxy->ExecuteOn(game.Get())==E_PENDING&&!second->Complete()&&!admission->failed,
        "overlapping bypass replay cannot reset or retire the pending game execution");
    Check(gate->Signal(1),"release game batch");
    const auto bypassUntil=GetTickCount64()+5000;
    while(!second->Complete()){Require(GetTickCount64()<bypassUntil,"bypass dual completion");Sleep(1);}
    Require(!second->Succeeded()&&second->LastSpinCount()==1&&!admission->failed,"bypass exits wait immediately and selects original");
    // A later rejected list can abort the batch after this bypass was prepared.
    replayExecution.serial=100;
    Check(replay->BeforeExecute(replayExecution),"prepare bypass without submit");
    const auto resets=admissionRuntime.resets;
    replayExecution.status=E_FAIL;replay->Executed(replayExecution);
    while(!second->Complete()){Require(GetTickCount64()<bypassUntil,"unsubmitted bypass reset tail");Sleep(1);}
    Require(admissionRuntime.resets==resets,"unsubmitted bypass does not reset history");
    admission->ForgetUnstarted(first);
    replayExecution.serial=101;
    Check(replay->BeforeExecute(replayExecution),"replay after capacity released");
    Require(admission->inlineInFlight.size()==1&&admission->InlineBusy(),"replay reserves capacity again");
    replayExecution.status=E_FAIL;replay->Executed(replayExecution); // No producer: certify the reset only.
    const auto replayUntil=GetTickCount64()+5000;
    while(!second->Complete()){Require(GetTickCount64()<replayUntil,"aborted replay reset completes");Sleep(1);}
    admission->ReclaimInlineLocked();Require(!admission->InlineBusy(),"aborted replay releases capacity");
    Require(admissionRuntime.begins==1&&admissionRuntime.ended==1,"available capacity restores runtime execution");
    // Reset while a bypass batch is pending cannot collect its private buffers.
    Require(admission->AdmitInline(first),"reserve capacity before bypass Reset");
    Check(game->Wait(gate.Get(),2),"hold bypass before Reset");
    Check(batchProxy->ExecuteOn(game.Get()),"bypass before Reset");
    {std::lock_guard lock(Sub::RecordingMutex());Own::Registry().push_back(replay);}
    Ptr<ID3D12CommandAllocator> replacement;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&replacement)),"replacement allocator");
    Check(batchProxy->Reset(replacement.Get(),nullptr),"Reset pending bypass");
    {std::lock_guard lock(Sub::RecordingMutex());Own::CollectLocked();Require(replay->job&&!second->Complete(),"Reset pins bypass buffers until game tail");}
    Check(gate->Signal(2),"release bypass Reset");
    const auto resetUntil=GetTickCount64()+5000;
    for(;;){
        {std::lock_guard lock(Sub::RecordingMutex());Own::CollectLocked();if(!replay->job)break;}
        Require(GetTickCount64()<resetUntil,"bypass Reset collection");Sleep(1);
    }
    Check(batchProxy->Close(),"replacement close");
    admission->ForgetUnstarted(first);
    std::puts("INLINE_REPLAY_BYPASS_OK original batch, bounded slots, no worker, recovery, Reset lifetime");
    admission->inlineLimit=Own::SessionOwner::kInlineAhead;
    std::vector<std::shared_ptr<In::Resources>> privateFrames;
    for(unsigned n=0;n<4;++n) {
        privateFrames.push_back(makeResources());
        Require(admission->AdmitInline(privateFrames.back()),"private buffers retain four-frame admission");
    }
    Require(!admission->AdmitInline(first),"private buffer fifth reservation rejected");
    admission->inlineLimit=1;
    Require(admission->inlineInFlight.size()==4&&!admission->AdmitInline(first),"shrink preserves existing reservations and blocks new work");
    admission->inlineLimit=5;
    Require(admission->AdmitInline(first),"grow admits fifth frame immediately");
    Require(!admission->AdmitInline(second),"five-slot cap");
    admission->ForgetUnstarted(first);
    for(auto& frame:privateFrames)admission->ForgetUnstarted(frame);
    std::puts("INLINE_ADMISSION_OK shared single slot, dual completion, retained replay, private four slots");
    auto drain=[&](ID3D12CommandQueue* q){
        Ptr<ID3D12Fence> f;Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f)),"drain fence");
        Check(q->Signal(f.Get(),1),"drain signal");HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Check(f->SetEventOnCompletion(1,e),"drain event");Require(WaitForSingleObject(e,10000)==WAIT_OBJECT_0,"drain wait");CloseHandle(e);
    };
    // m5: a failed game-queue completion signal is proven by a later signal of our own fence on that queue.
    auto proven=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
    proven->BeforeExecute(game.Get(),true);proven->CertifyWithoutNetwork(nullptr,0,E_FAIL,true);
    auto provenLease=std::make_shared<Own::Lease>(owner,reinterpret_cast<void*>(202),Sub::RecordingIdentity{});
    provenLease->inlineResources=proven;provenLease->invalidated=true;
    drain(game.Get());drain(proven->SideQueue());
    {
        std::lock_guard lock(Sub::RecordingMutex());
        Require(!proven->RetirementUncertain(),"failed game signal proven by a later same-queue signal");
        Require(proven->Complete(),"proof fence completes after the queue drains");
        Own::Registry().push_back(provenLease);Own::CollectLocked();
        Require(provenLease->job==nullptr,"proven recording is collected");
    }
    std::puts("INLINE_GAME_PROOF_OK failed completion signal proven by a later same-queue signal and collected");
    // The proof signal itself may fail at first; a later bounded retry still proves completion.
    auto flaky=new SignalFailQueue(game.Get(),2);Ptr<ID3D12CommandQueue> flakyGame;flakyGame.Attach(flaky);
    auto retried=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
    retried->BeforeExecute(flakyGame.Get(),true);retried->CertifyWithoutNetwork(nullptr,0,E_FAIL,true);
    {
        std::lock_guard lock(Sub::RecordingMutex());
        Require(retried->RetirementUncertain(),"first proof signal failed");
        Require(!retried->RetryProof(),"second proof signal failed");
        Require(retried->RetryProof()&&!retried->RetirementUncertain(),"third signal proves completion");
    }
    drain(game.Get());drain(retried->SideQueue());Require(retried->Complete(),"retried proof completes");
    // Side queue: a failed reset signal is proven by a later side-queue signal once scheduled.
    auto flakySide=new SignalFailQueue(owner->inlinePipelines->sideQueue.Get(),1);
    auto sidePipelines=std::make_shared<In::Pipelines>(*owner->inlinePipelines);sidePipelines->sideQueue.Attach(flakySide);
    auto sideFailed=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,sidePipelines);
    bool threw=false;try{sideFailed->BeforeExecute(game.Get(),true);}catch(const std::exception&){threw=true;}
    Require(threw&&sideFailed->RetirementUncertain(),"side reset signal failure is uncertain");
    {
        std::lock_guard lock(Sub::RecordingMutex());
        Require(!sideFailed->RetryProof(),"side proof waits until the work is scheduled");
        sideFailed->CertifyWithoutNetwork(nullptr,0,E_FAIL,false);
        Require(sideFailed->RetryProof()&&!sideFailed->RetirementUncertain(),"scheduled side work proven by a later side signal");
    }
    drain(owner->inlinePipelines->sideQueue.Get());Require(sideFailed->Complete(),"side proof completes");
    std::puts("INLINE_RETRY_PROOF_OK flaky game and side signals proven by bounded retries");
    // Exercise the lease's actual End path, not just Resources::Complete. End must see the
    // recovered certificate, immediately or after collection retries, and run exactly once.
    for (int failures : {0, 2}) {
        Runtime proofRuntime;
        auto proofOwner=std::make_shared<Own::SessionOwner>();proofOwner->api=owner->api;proofOwner->context=&proofRuntime;
        Ptr<ID3D12CommandQueue> proofQueue;proofQueue.Attach(new SignalFailQueue(game.Get(),failures));
        auto frame=makeResources();frame->BeforeExecute(proofQueue.Get());frame->NetworkQueued(false);
        auto job=std::make_shared<Own::Lease>(proofOwner,reinterpret_cast<void*>(uintptr_t(400+failures)),Sub::RecordingIdentity{});
        job->inlineResources=frame;job->begun=job->ready=job->inlineEnqueueFinished=true;
        job->inlineResetSerial=1;proofOwner->inlineInFlight.push_back(frame);proofOwner->NoteInlineEnqueue(job->job);
        Sub::RecordingExecution e{};e.serial=1;e.queue=proofQueue.Get();e.producerSubmitted=true;e.status=E_FAIL;
        {std::lock_guard lock(Sub::RecordingMutex());Own::Registry().push_back(job);job->Executed(e);}
        if(failures)Require(proofRuntime.ended==0&&job->begun,"End deferred while proof is unavailable");
        const auto until=GetTickCount64()+5000;
        for(;;){
            {std::lock_guard lock(Sub::RecordingMutex());Own::CollectLocked();if(!job->begun)break;}
            Require(GetTickCount64()<until,"deferred End proof recovery");Sleep(1);
        }
        Require(proofRuntime.ended==1&&SUCCEEDED(proofRuntime.endStatus)&&proofRuntime.endFence&&proofRuntime.endValue,
            "real End contract receives recovered successful certificate exactly once");
        Require(!proofOwner->failed&&proofOwner->inlineEnqueueOrder.empty(),"recovered End advances enqueue head without poisoning runtime");
        drain(game.Get());drain(frame->SideQueue());
        {std::lock_guard lock(Sub::RecordingMutex());job->Invalidated(job->identity);Own::CollectLocked();Require(!job->job,"recovered End collects");}
    }
    std::puts("INLINE_RUNTIME_PROOF_OK immediate and deferred End receive recovered fence/status, exactly once");
    // Truly unprovable: every Signal on the game queue fails. Bounded retries, then abandoned
    // outside the slot count; the collection timer is not rearmed forever.
    auto broken=new SignalFailQueue(game.Get());Ptr<ID3D12CommandQueue> brokenGame;brokenGame.Attach(broken);
    auto unknown=std::make_shared<In::Resources>(device.Get(),original.Get(),candidate.Get(),nullptr,8,8,owner->inlinePipelines);
    unknown->BeforeExecute(brokenGame.Get(),true);unknown->CertifyWithoutNetwork(nullptr,0,E_FAIL,true);
    auto retained=std::make_shared<Own::Lease>(owner,reinterpret_cast<void*>(201),Sub::RecordingIdentity{});
    retained->inlineResources=unknown;retained->invalidated=true;
    {
        std::lock_guard lock(Sub::RecordingMutex());Own::Registry().push_back(retained);
        Require(!unknown->Complete()&&unknown->RetirementUncertain(),"unknown submission never invents proof");
        Require(!Own::CollectLocked(),"unknown submission does not request permanent timer polling");
        Require(retained->job!=nullptr,"unknown job remains pinned");
        owner->inlineLimit=5;owner->inlineInFlight.clear();owner->abandonedInline.clear();
        owner->inlineInFlight.push_back(unknown);
        for(unsigned n=0;n<In::Resources::kProofAttempts+1;++n)owner->ReclaimInlineLocked();
        Require(unknown->ProofExhausted(),"proof attempts are bounded");
        Require(owner->inlineInFlight.empty()&&owner->abandonedInline.size()==1,"unprovable recording leaves the slot count");
        Require(!owner->InlineAbandonLimit(),"one abandoned recording keeps inline available");
        for(unsigned n=1;n<Own::SessionOwner::kInlineAhead;++n)owner->abandonedInline.push_back(unknown);
        Require(owner->InlineAbandonLimit()&&owner->InlineBusy(),"inline stops after the abandonment cap");
        Require(!owner->AdmitInline(makeResources()),"no admission after the abandonment cap");
        Require(!unknown->Complete(),"abandoned recording is never released without proof");
        owner->abandonedInline.clear();owner->inlineLimit=1;
    }
    std::puts("INLINE_UNPROVABLE_BOUNDED_OK bounded proof attempts, abandoned outside slots, inline capped");
    std::puts("INLINE_UNSUBMITTED_DISCARD_OK 12 Reset/Release, 6 callback enqueue failures, dual completion, turn notification");
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}

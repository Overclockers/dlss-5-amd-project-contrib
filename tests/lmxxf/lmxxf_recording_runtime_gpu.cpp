#include "lmxxf_gpu_test_utils.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <memory>
#include <vector>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"

template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
static void SetPolicy(const char* key,const char* value)
{
    Require(SetEnvironmentVariableA(key,value)!=FALSE,"process policy environment");
    Require(_putenv_s(key,value)==0,"fixture CRT policy environment");
}
struct RecordedFrame
{
    void* token = nullptr;
    ID3D12Resource* output = nullptr; // owned by runtime lease
    Ptr<ID3D12CommandAllocator> producerAllocator, consumerAllocator;
    Ptr<ID3D12GraphicsCommandList> producer, consumer;
    Ptr<ID3D12Fence> fence;
    UINT64 value = 0;
    UINT activeWidth=0,activeHeight=0,pattern=0;
    bool inlineFrame=false;
};
int main(int argc, char** argv)
{
    const bool drs = argc == 4 && std::strcmp(argv[3], "--drs") == 0;
    const bool hipDiagnostic = argc == 4 && std::strcmp(argv[3], "--hip-passthrough") == 0;
    if (argc != 3 && !hipDiagnostic && !drs) return 2;
    if (drs) {
        for (auto setting : {std::pair{"DLSS5_NETWORK_MODEL","0"}, {"DLSS5_NETWORK_HEIGHT","1080"},
             {"DLSS5_NETWORK_FREE_RES","0"}, {"DLSS5_MULTI_PASS","1"}, {"DLSS5_SKIP_BLOCKS","none"},
             {"DLSS5_VIT_ADAPTIVE","0"}, {"DLSS5_TEMPORAL_MODE","0"}})
            SetPolicy(setting.first, setting.second);
    }
    Ptr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    HMODULE dll = LoadLibraryW(Widen(argv[1]).c_str()); Require(dll != nullptr, "runtime load");
    auto getApi = reinterpret_cast<int32_t(*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(dll, "LmxxfNrGetApi"));
    Require(getApi != nullptr, "runtime export"); LmxxfNrApi api {}; api.struct_size = sizeof api;
    Require(getApi(LMXXF_NR_ABI_VERSION, &api) == LMXXF_NR_OK && api.CollectRecording, "current package table");
    auto getTiming = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrTimingApi*)>(GetProcAddress(dll, "LmxxfNrGetTimingApi"));
    LmxxfNrTimingApi timing {}; timing.struct_size = sizeof timing;
    Require(getTiming && getTiming(NR_TIMING_VERSION, &timing) == LMXXF_NR_OK, "GPU timing extension");
    auto ok = [&](int32_t result, const char* step) {
        if (result != LMXXF_NR_OK) { char error[256] {}; api.GetLastError(error, sizeof error); std::fprintf(stderr, "%s rc=%d: %s\n", step, result, error); }
        Require(result == LMXXF_NR_OK, step);
    };
    Ptr<IDXGIFactory4> factory; Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); SIZE_T best = 0;
    for (UINT i = 0;; ++i) {
        Ptr<IDXGIAdapter1> adapter; if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc {}; adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && desc.DedicatedVideoMemory > best) {
            Ptr<ID3D12Device> candidate;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate)))) {
                device = candidate; best = desc.DedicatedVideoMemory;
            }
        }
    }
    Require(device != nullptr, "AMD device");
    Ptr<ID3D12CommandQueue> queue, other; D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&other)), "other queue");
    const auto modules = Widen(argv[2]);
    auto create = [&] {
        LmxxfNrCreateInfo info {}; info.struct_size = sizeof info; info.device = device.Get(); info.queue = queue.Get();
        info.assets_directory = modules.c_str(); info.flags = LMXXF_NR_CREATE_FLAG_RECORDING_LEASES;
        if(drs) info.flags |= LMXXF_NR_CREATE_FLAG_INLINE_QUEUE;
        void* context = nullptr; ok(api.Create(&info, &context), "create lease session");
        ok(api.PrepareSession(context), "prepare session"); return context;
    };
    void* context = create();
    Require(timing.SetEnabled(context, 1) == LMXXF_NR_OK, "enable GPU telemetry");
    auto makeColour = [&](UINT width, UINT height, UINT pattern = 0) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = hipDiagnostic ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Ptr<ID3D12Resource> colour;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             nullptr, IID_PPV_ARGS(&colour)), "colour");
        UploadColorPattern(device.Get(), queue.Get(), colour.Get(), {pattern});
        return colour;
    };
    uint64_t nextFrame = 0;
    auto record = [&](void* owner, UINT width, UINT height, bool passthrough = false, bool exposure = false,
                      bool hipPassthrough = false, UINT allocWidth = 0, UINT allocHeight = 0,
                      UINT pattern = 0, bool temporal = false, UINT inlineSlots = 0) {
        auto result = std::make_unique<RecordedFrame>();
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result->producerAllocator)), "producer allocator");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result->consumerAllocator)), "consumer allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result->producerAllocator.Get(), nullptr,
                                       IID_PPV_ARGS(&result->producer)), "producer list");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result->consumerAllocator.Get(), nullptr,
                                       IID_PPV_ARGS(&result->consumer)), "consumer list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result->fence)), "tail fence");
        auto colour = makeColour(allocWidth ? allocWidth : width, allocHeight ? allocHeight : height, pattern);
        LmxxfNrFrameInfo frame {}; frame.struct_size = sizeof frame;
        frame.frame_id = ++nextFrame;
        frame.command_list = result->producer.Get(); frame.color = colour.Get(); frame.color_width = width; frame.color_height = height;
        frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.paper_white = frame.pre_exposure = frame.exposure_scale = frame.model_scale = 1;
        frame.temporal_flags = temporal ? LMXXF_NR_TEMPORAL_MODEL_HISTORY : 0;
        frame.inline_slots = inlineSlots;
        result->activeWidth=width;result->activeHeight=height;result->pattern=pattern;
        result->inlineFrame=inlineSlots!=0;
        if (hipDiagnostic) frame.pre_exposure = 2; // Yimo's observed frame contract.
        frame.flags = (passthrough ? LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH : 0) |
                      (exposure ? LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE : 0) |
                      (hipPassthrough ? LMXXF_NR_FRAME_FLAG_HIP_PASSTHROUGH : 0);
        LmxxfNrJob job {}; job.struct_size = sizeof job;
        ok(api.PrepareFrame(owner, &frame, &job), "prepare recording"); result->token = job.handle;
        result->output = static_cast<ID3D12Resource*>(job.private_output);
        ok(api.RecordInputs(owner, job.handle, result->producer.Get()), "record inputs");
        Check(result->producer->Close(), "producer close");
        ok(api.RecordOutputs(owner, job.handle, result->consumer.Get()), "record outputs");
        Check(result->consumer->Close(), "consumer close");
        // Runtime must retain the exact colour allocation after this return.
        return result;
    };
    auto discard = [&](void* owner, std::unique_ptr<RecordedFrame>& frame) {
        const auto token = frame->token;
        frame->producer.Reset(); frame->consumer.Reset(); // recording invalidated before releasing pins
        ok(api.InvalidateRecording(owner, token), "invalidate recording");
        ok(api.CollectRecording(owner, token), "collect completed/unsubmitted recording");
        Require(api.CollectRecording(owner, token) == LMXXF_NR_INVALID_ARGUMENT, "late token rejected"); frame.reset();
    };
    auto abandoned = record(context, 1280, 720); const auto oldToken = abandoned->token; discard(context, abandoned);
    std::vector<std::unique_ptr<RecordedFrame>> frames;
    for (unsigned i = 0; i < (drs ? 0u : 10u); ++i) {
        frames.push_back(record(context, 1280, 720));
        Require(frames.back()->token != oldToken, "discarded token never aliases a new Job");
        Require(api.CollectRecording(context, frames.back()->token) == LMXXF_NR_UNAVAILABLE, "live delayed recording retained");
    }
    if (!drs) Require(api.Destroy(context) == LMXXF_NR_UNAVAILABLE, "Destroy refuses live recordings");
    Ptr<ID3D12CommandQueue> side;
    if(drs){D3D12_COMMAND_QUEUE_DESC sd{};sd.Type=D3D12_COMMAND_LIST_TYPE_COMPUTE;
        Check(device->CreateCommandQueue(&sd,IID_PPV_ARGS(&side)),"DRS inline side queue");}
    auto run = [&](void* owner, RecordedFrame& frame, ID3D12CommandQueue* target) {
        ok(api.BeginRecordingExecution(owner, frame.token, target), "begin execution");
        ID3D12CommandList* producer[] = {frame.producer.Get()}; target->ExecuteCommandLists(1, producer);
        if(frame.inlineFrame){
            Check(target->Signal(frame.fence.Get(),++frame.value),"inline capture proof");
            WaitQueue(device.Get(),target);
            ok(api.EnqueueHip(owner,frame.token,side.Get()),"actual inline HIP enqueue");
            Check(side->Signal(frame.fence.Get(),++frame.value),"inline side proof");
            Check(target->Wait(frame.fence.Get(),frame.value),"inline consumer dependency");
        }else ok(api.EnqueueHip(owner, frame.token, target), "actual HIP enqueue");
        ID3D12CommandList* consumer[] = {frame.consumer.Get()}; target->ExecuteCommandLists(1, consumer);
        const HRESULT signal = target->Signal(frame.fence.Get(), ++frame.value);
        ok(api.EndRecordingExecution(owner, frame.token, target, 3, frame.fence.Get(), frame.value, signal), "end execution");
        WaitQueue(device.Get(), target);
        if (owner == context) { LmxxfNrTimings net {}; net.struct_size = sizeof net; ok(api.GetTimings(owner, &net), "completed network timing"); }
        return HashTexture(device.Get(), target, frame.output, 0,
            drs ? frame.activeWidth : 0, drs ? frame.activeHeight : 0, {frame.pattern});
    };
    if (drs)
    {
        auto counter = [&](void* owner, const char* key) {
            char text[2048] {}; ok(api.GetStatus(owner,text,sizeof text),"DRS status");
            auto p=std::strstr(text,key); Require(p!=nullptr,"DRS counter exists");
            return unsigned(std::strtoul(p+std::strlen(key),nullptr,10));
        };
        Ptr<IDXGIAdapter> adapter; Ptr<IDXGIAdapter3> memory;
        // D3D12 devices do not expose IDXGIDevice: resolve the exact adapter by LUID.
        Check(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"memory adapter");
        Check(adapter.As(&memory),"memory budget interface");
        UINT64 peak=0;
        auto sampleMemory = [&](const char* phase) {
            DXGI_QUERY_VIDEO_MEMORY_INFO m{};
            Check(memory->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&m),"local memory");
            peak=(std::max)(peak,m.CurrentUsage);
            std::printf("DRS memory %s usage=%llu budget=%llu peak=%llu\n",phase,m.CurrentUsage,m.Budget,peak);
        };
        sampleMemory("initial");
        ok(api.Destroy(context),"destroy initial DRS session");
        for (bool smallModel : {false,true}) for (bool native : {false,true})
        {
            SetPolicy("DLSS5_NETWORK_MODEL",smallModel?"1":"0");
            SetPolicy("DLSS5_NETWORK_FREE_RES",native?"1":"0");
            context=create();
            auto old=record(context,1280,720,false,false,false,1280,720,0);
            char modelStatus[2048]{}; ok(api.GetStatus(context,modelStatus,sizeof modelStatus),"effective model");
            Require(std::strstr(modelStatus,smallModel?" model=small ":" model=original ")!=nullptr,"requested model executed, no fallback");
            const auto oldHash=run(context,*old,other.Get());
            const unsigned creates=counter(context,"bridgeCreates=");
            unsigned recreates=counter(context,"recreates=");
            for (unsigned step=0;step<6;++step)
            {
                const UINT width=native?1280u:((step%2)?1280u:1600u);
                const UINT height=native?720u:((step%2)?720u:900u);
                const UINT aw=(step%2)?width:1920u, ah=(step%2)?height:1080u;
                const UINT pattern=step%3;
                auto current=record(context,width,height,false,false,false,aw,ah,pattern);
                Require(counter(context,"bridgeCreates=")==creates,"exact network retained across codec recreation");
                Require(counter(context,"recreates=")==++recreates,"one codec recreation per changed contract");
                Require(api.CollectRecording(context,old->token)==LMXXF_NR_UNAVAILABLE,"old closed recording retained until invalidation");
                const auto actual=run(context,*current,(step%2)?queue.Get():other.Get());
                void* baselineContext=create();
                auto baselineFrame=record(baselineContext,width,height,false,false,false,aw,ah,pattern);
                Require(run(baselineContext,*baselineFrame,queue.Get())==actual,"changed nonzero DRS output equals fresh same-policy network");
                discard(baselineContext,baselineFrame);ok(api.Destroy(baselineContext),"destroy fresh baseline");
                Require(run(context,*old,other.Get())==oldHash,"old immutable recording replays across queues after retention");
                sampleMemory("old-recording-pinned");
                std::printf("DRS %s %s step=%u active=%ux%u alloc=%ux%u bridgeCreates=%u recreates=%u hash=%016llx\n",
                    smallModel?"Small":"Original",native?"native-allocation":"fixed-tier",step,width,height,aw,ah,
                    creates,recreates,actual);
                discard(context,current);
                auto unchanged=record(context,width,height,false,false,false,aw,ah,pattern);
                Require(counter(context,"bridgeCreates=")==creates && counter(context,"recreates=")==recreates,
                        "unchanged active subrect does not repeatedly recreate codecs");
                Require(run(context,*unchanged,queue.Get())==actual,"rotating equal-description input retains output");
                discard(context,unchanged);
            }
            // Native active changes, even with shared processing buckets, replace the network.
            if(native) {
                auto changed=record(context,1280,721);
                Require(counter(context,"bridgeCreates=")==creates+1,"native valid geometry forces replacement");
                run(context,*changed,queue.Get());discard(context,changed);
                Require(run(context,*old,other.Get())==oldHash,"old bridge survives genuine replacement");
                Require(std::strcmp(std::getenv("DLSS5_NETWORK_FREE_RES"),"1")==0,"native user policy never converted to fitted tier");
            }
            discard(context,old);sampleMemory("collected");
            ok(api.Destroy(context),"destroy DRS matrix session");
        }
        // Adaptive mapping changes retain full replacement, never mutate live anchors.
        SetPolicy("DLSS5_NETWORK_MODEL","0");SetPolicy("DLSS5_NETWORK_FREE_RES","0");
        SetPolicy("DLSS5_VIT_ADAPTIVE","1");context=create();
        auto adaptiveOld=record(context,1280,720);run(context,*adaptiveOld,queue.Get());
        const auto before=counter(context,"bridgeCreates=");
        auto adaptiveNew=record(context,1600,900);
        Require(counter(context,"bridgeCreates=")==before+1,"adaptive mapping full replacement");
        run(context,*adaptiveNew,other.Get());discard(context,adaptiveNew);discard(context,adaptiveOld);
        ok(api.Destroy(context),"destroy adaptive session");
        SetPolicy("DLSS5_VIT_ADAPTIVE","0");SetPolicy("DLSS5_TEMPORAL_MODE","2");context=create();
        auto temporalOld=record(context,1280,720,false,false,false,1280,720,0,true);
        const auto temporalHash=run(context,*temporalOld,queue.Get());
        const auto temporalCreates=counter(context,"bridgeCreates=");
        auto temporalAllocation=record(context,1280,720,false,false,false,1920,1080,1,true);
        Require(counter(context,"bridgeCreates=")==temporalCreates,"temporal unchanged mapping allocation retains bridge");
        run(context,*temporalAllocation,other.Get());discard(context,temporalAllocation);
        auto temporalChanged=record(context,1600,900,false,false,false,1600,900,1,true);
        Require(counter(context,"bridgeCreates=")==temporalCreates+1,"unproven temporal mapping replaces bridge");
        run(context,*temporalChanged,queue.Get());discard(context,temporalChanged);
        ok(api.ResetHistory(context),"actual execution reset");
        Require(run(context,*temporalOld,other.Get())==temporalHash,"old temporal recording explicit reset baseline");
        discard(context,temporalOld);ok(api.Destroy(context),"destroy temporal retention session");
        SetPolicy("DLSS5_TEMPORAL_MODE","0");
        context=create();
        auto inlineOld=record(context,1280,720,false,false,false,1280,720,0,false,2);
        const auto inlineCreates=counter(context,"bridgeCreates=");
        auto inlineNew=record(context,1600,900,false,false,false,1920,1080,1,false,2);
        Require(counter(context,"bridgeCreates=")==inlineCreates,"ahead inline mapping retains bridge");
        const auto inlineNewHash=run(context,*inlineNew,queue.Get());
        const auto inlineOldHash=run(context,*inlineOld,other.Get());
        Require(run(context,*inlineNew,other.Get())==inlineNewHash,"inline new recording replay across queues");
        Require(run(context,*inlineOld,queue.Get())==inlineOldHash,"inline old captures survive ahead codec replacement");
        discard(context,inlineNew);discard(context,inlineOld);ok(api.Destroy(context),"destroy inline DRS session");
        context=create();
        auto memoryOld=record(context,1280,720);
        run(context,*memoryOld,queue.Get());sampleMemory("bounded-cycle-start");
        for(unsigned cycle=0;cycle<24;++cycle){
            auto frame=record(context,1280,720,false,false,false,cycle%2?1280:1920,cycle%2?720:1080,cycle%3);
            run(context,*frame,queue.Get());discard(context,frame);
            Require(counter(context,"bridgeCreates=")==1,"bounded codec cycle never adds a bridge generation");
            if(cycle==11||cycle==23)sampleMemory(cycle==11?"bounded-cycle-12-collected":"bounded-cycle-24-collected");
        }
        discard(context,memoryOld);ok(api.Destroy(context),"destroy bounded memory session");
        // A reported failed Signal cannot be repaired by a later unrelated fence.
        // Use a separate session and deliberately retain it to process exit: no
        // completion certificate exists for this fault, even though the test waits.
        void* faultContext=create();
        auto faultOld=record(faultContext,1280,720);
        auto faultNew=record(faultContext,1600,900);
        Require(counter(faultContext,"bridgeCreates=")==1,"failure fixture shares retained bridge");
        ok(api.BeginRecordingExecution(faultContext,faultOld->token,queue.Get()),"fault begin");
        ID3D12CommandList* fp=faultOld->producer.Get();queue->ExecuteCommandLists(1,&fp);
        ok(api.EnqueueHip(faultContext,faultOld->token,queue.Get()),"fault actual HIP");
        ID3D12CommandList* fc=faultOld->consumer.Get();queue->ExecuteCommandLists(1,&fc);
        Require(api.EndRecordingExecution(faultContext,faultOld->token,queue.Get(),3,nullptr,0,E_FAIL)==LMXXF_NR_UNAVAILABLE,
                "failed Signal rejects completion proof");
        WaitQueue(device.Get(),queue.Get());
        faultOld->producer.Reset();faultOld->consumer.Reset();
        ok(api.InvalidateRecording(faultContext,faultOld->token),"invalidate failed-signal recording");
        Require(api.CollectRecording(faultContext,faultOld->token)==LMXXF_NR_UNAVAILABLE,"uncertain old recording retained");
        Require(api.BeginRecordingExecution(faultContext,faultNew->token,other.Get())==LMXXF_NR_UNAVAILABLE,
                "shared retained chain fails closed after uncertain execution");
        Require(api.Destroy(faultContext)==LMXXF_NR_UNAVAILABLE,"fault session cannot free uncertain resources");
        std::puts("DRS failed-Signal completion retention: PASS (isolated session intentionally held to process exit)");
        sampleMemory("final");
        Ptr<ID3D12InfoQueue> messages;
        if(SUCCEEDED(device.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
            SIZE_T bytes=0;messages->GetMessage(i,nullptr,&bytes);std::vector<char> storage(bytes);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(messages->GetMessage(i,m,&bytes),"DRS debug message");
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::fprintf(stderr,"%s\n",m->pDescription);
            Require(m->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"DRS D3D12 debug errors");
        }
        FreeLibrary(dll);std::puts("Exact-network DRS recording execution: PASS");return 0;
    }
    if (hipDiagnostic)
    {
        auto conflictColour = makeColour(1280, 720);
        LmxxfNrFrameInfo conflict {}; conflict.struct_size = sizeof conflict;
        conflict.color = conflictColour.Get(); conflict.color_width = 1280; conflict.color_height = 720;
        conflict.flags = LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH | LMXXF_NR_FRAME_FLAG_HIP_PASSTHROUGH;
        LmxxfNrJob rejected {}; rejected.struct_size = sizeof rejected;
        Require(api.PrepareFrame(context, &conflict, &rejected) == LMXXF_NR_INVALID_ARGUMENT &&
                !rejected.handle && !rejected.private_output, "conflicting diagnostics rejected before publishing a job");
        char conflictError[256] {}; api.GetLastError(conflictError, sizeof conflictError);
        Require(std::strstr(conflictError, "conflicting passthrough") != nullptr, "explicit conflicting-mode error");
        // Retained records must survive resize, intervening codec use and queue changes.
        // Full-texture hashes include every RGB channel and the last pixel: a flat
        // RGBA copy or the Windows HIP row truncation cannot pass this comparison.
        for (auto& frame : frames) discard(context, frame);
        frames.clear();
        std::vector<uint64_t> hashes;
        for (const auto size : {std::pair{1280u,720u}, std::pair{1920u,1080u}, std::pair{1707u,961u}})
        {
            auto codec = record(context, size.first, size.second, true);
            const auto expected = run(context, *codec, queue.Get());
            auto hip = record(context, size.first, size.second, false, false, true);
            Require(run(context, *hip, other.Get()) == expected, "HIP round trip equals codec across entire image");
            run(context, *codec, queue.Get());
            Require(run(context, *hip, queue.Get()) == expected, "HIP round trip replay after codec use on another queue");
            char status[1536] {}; ok(api.GetStatus(context, status, sizeof status), "HIP copy evidence");
            Require(std::strstr(status, "diagnostic=hip-passthrough hip_copy_queued=2") != nullptr,
                    "actual HIP copies recorded, not diagnostic fallback");
            std::printf("HIP passthrough %ux%u: PASS hash=%016llx copies=2\n", size.first, size.second,
                        static_cast<unsigned long long>(expected));
            hashes.push_back(expected); frames.push_back(std::move(hip)); discard(context, codec);
        }
        for (size_t i = 0; i < frames.size(); ++i)
            Require(run(context, *frames[i], (i & 1) ? queue.Get() : other.Get()) == hashes[i],
                    "HIP diagnostic retains old geometry resources");
        auto normal = record(context, 1280, 720);
        const auto normalHash = run(context, *normal, queue.Get());
        Require(normalHash != hashes.front(), "normal network still executes with diagnostic records retained");
        Require(run(context, *frames.front(), other.Get()) == hashes.front(), "HIP copy survives normal network execution");
        Require(run(context, *normal, queue.Get()) == normalHash, "normal network survives diagnostic execution");
        discard(context, normal);
        for (auto& frame : frames) discard(context, frame);
        ok(api.Destroy(context), "destroy HIP diagnostic session");
        Ptr<ID3D12InfoQueue> messages;
        if (SUCCEEDED(device.As(&messages)))
            for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
                SIZE_T bytes = 0; messages->GetMessage(i, nullptr, &bytes); std::vector<char> storage(bytes);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                Check(messages->GetMessage(i, message, &bytes), "diagnostic debug message");
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
                Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "HIP passthrough D3D12 debug errors");
            }
        FreeLibrary(dll);
        std::puts("HIP passthrough recording/resize/replay: PASS");
        return 0;
    }
    LmxxfNrTimings net {}; net.struct_size = sizeof net;
    ok(api.GetTimings(context, &net), "lazy network timing request");
    Require(!net.valid, "first request cannot fabricate a sample");
    char netStatus[1024] {}; ok(api.GetStatus(context, netStatus, sizeof netStatus), "timing availability");
    const auto baseline = run(context, *frames.front(), other.Get());
    run(context, *frames.back(), queue.Get());
    Require(run(context, *frames.front(), other.Get()) == baseline, "old immutable binding survives ten rebinds and replay");
    auto passthrough = record(context, 1280, 720, true);
    run(context, *passthrough, queue.Get());
    Require(run(context, *frames.front(), other.Get()) == baseline, "HIP replay after passthrough shared-resource use");
    auto metered = record(context, 1280, 720, false, true); run(context, *metered, queue.Get());
    auto resized = record(context, 1920, 1080); run(context, *resized, other.Get());
    for (unsigned i = 0; i < 8; ++i) {
        auto next = record(context, 1920, 1080); run(context, *next, other.Get());
        ok(api.GetTimings(context, &net), "rebuilt network timing");
        Require(net.valid && net.frame_id == nextFrame && net.network_ms >= 0.01f, "network reports non-collapsed current frame with PDL on or off");
        std::printf("runtime network frame=%llu ms=%.3f\n", net.frame_id, net.network_ms);
        discard(context, next);
    }
    Require(run(context, *frames.front(), queue.Get()) == baseline, "old bridge/codec recording survives geometry replacement");
    void* newContext = create(); auto fresh = record(newContext, 1280, 720); run(newContext, *fresh, other.Get());
    Require(run(context, *frames.front(), queue.Get()) == baseline, "old recording survives another session");
    discard(newContext, fresh); ok(api.Destroy(newContext), "destroy new session asynchronously");
    discard(context, resized); discard(context, metered); discard(context, passthrough);
    for (auto& frame : frames) discard(context, frame);
    NrTimingSnapshot measured {}; measured.struct_size = sizeof measured;
    ok(timing.GetSnapshot(context, &measured), "read completed GPU timing");
    Require(measured.stages[NR_GPU_NETWORK].samples > 0 && measured.stages[NR_GPU_NETWORK].last_ms > 0,
            "runtime exposes actual HIP durations");
    std::printf("NR GPU samples=%llu median_ms=%.3f last_ms=%.3f\n",
                measured.stages[NR_GPU_NETWORK].samples, measured.stages[NR_GPU_NETWORK].mean_ms,
                measured.stages[NR_GPU_NETWORK].last_ms);
    Require(measured.stages[NR_GPU_ENCODE].samples > 0 && measured.stages[NR_GPU_ENCODE].last_ms > 0 &&
            measured.stages[NR_GPU_DECODE].samples > 0 && measured.stages[NR_GPU_DECODE].last_ms > 0,
            "runtime exposes completed D3D encode and decode durations");
    ok(api.Destroy(context), "destroy old session asynchronously");
    // Exercise the actual host owner + proxy contract, without a backend instance
    // that could keep the old context alive accidentally after NR off/switch.
    namespace Ownership = DlssNr::Backend::LmxxfRecording;
    namespace Submission = DlssNr::Submission;
    struct ProxyFrame {
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12GraphicsCommandList> native;
        Submission::CommandListProxy* proxy = nullptr;
        ID3D12Resource* output = nullptr;
    };
    auto recordProxy = [&](const std::shared_ptr<Ownership::SessionOwner>& owner, bool partial = false) {
        auto result = std::make_unique<ProxyFrame>();
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result->allocator)), "host allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result->allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&result->native)), "host native list");
        Check(Submission::CommandListProxy::Create(device.Get(), result->allocator.Get(), result->native.Get(),
                                                  &result->proxy), "host proxy");
        auto colour = makeColour(1280, 720);
        LmxxfNrFrameInfo frame {}; frame.struct_size = sizeof frame;
        frame.color = colour.Get(); frame.command_list = result->proxy; frame.color_width = 1280; frame.color_height = 720;
        frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.paper_white = frame.pre_exposure = frame.exposure_scale = frame.model_scale = 1;
        LmxxfNrJob job {}; job.struct_size = sizeof job;
        ok(api.PrepareFrame(owner->context, &frame, &job), "host prepare");
        std::lock_guard lifetime(Submission::RecordingMutex());
        auto lease = Ownership::Attach(owner, job.handle, result->proxy); Require(lease != nullptr, "host attach observer");
        ok(api.RecordInputs(owner->context, job.handle, result->proxy), "host inputs");
        if (partial)
        {
            Require(api.RecordOutputs(owner->context, job.handle, nullptr) == LMXXF_NR_INVALID_ARGUMENT,
                    "failed output recording after private input commands");
            owner->failed = true; // Same host response as FinishRecord failure.
        }
        else
        {
            Check(result->proxy->SplitSegments(), "host split");
            ok(api.RecordOutputs(owner->context, job.handle, result->proxy), "host outputs");
            lease->ready = true;
        }
        Check(result->proxy->Close(), "host close");
        result->output = static_cast<ID3D12Resource*>(job.private_output);
        return result;
    };
    auto activeOwner = Ownership::SessionOwner::Create(api, create()); Require(activeOwner != nullptr, "host owner pin");
    std::weak_ptr<Ownership::SessionOwner> oldOwner = activeOwner;
    auto oldProxy = recordProxy(activeOwner);
    activeOwner.reset(); // NR off: only the recording retains the session.
    Require(!oldOwner.expired(), "NR off retains a live unsubmitted recording");
    Check(oldProxy->proxy->ExecuteOn(other.Get()), "old recording after NR off"); WaitQueue(device.Get(), other.Get());
    const auto hostBaseline = HashTexture(device.Get(), other.Get(), oldProxy->output);
    Require(hostBaseline == baseline, "host observer runs HIP for old recording");
    activeOwner = Ownership::SessionOwner::Create(api, create()); auto newProxy = recordProxy(activeOwner);
    Check(newProxy->proxy->ExecuteOn(queue.Get()), "new active session"); WaitQueue(device.Get(), queue.Get());
    Check(oldProxy->proxy->ExecuteOn(other.Get()), "old recording replay after new session"); WaitQueue(device.Get(), other.Get());
    Require(HashTexture(device.Get(), other.Get(), oldProxy->output) == hostBaseline, "off/on preserves old recorded output");
    newProxy->proxy->Release(); newProxy.reset(); activeOwner.reset();
    // Hold the actual queue: Reset invalidates the recording but must retain the
    // session while its producer/HIP/consumer are pending, even without future Record calls.
    Ptr<ID3D12Fence> gate; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "retirement gate");
    Check(other->Wait(gate.Get(), 1), "block old queue");
    Check(oldProxy->proxy->ExecuteOn(other.Get()), "pending old execution");
    Ptr<ID3D12CommandAllocator> replacement;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)), "replacement allocator");
    Check(oldProxy->proxy->Reset(replacement.Get(), nullptr), "Reset while last execution pending");
    Require(!oldOwner.expired(), "Reset cannot release GPU-live lease");
    Check(gate->Signal(1), "unblock old queue"); WaitQueue(device.Get(), other.Get());
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!oldOwner.expired() && GetTickCount64() < deadline) Sleep(10);
    Require(oldOwner.expired(), "background collector releases invalidated owner without active backend");
    Check(oldProxy->proxy->Close(), "close replacement generation"); oldProxy->proxy->Release(); oldProxy.reset();
    // A failed recording still contains input copies on the game list. It must
    // execute and retain its private resources even though HIP was never armed.
    activeOwner = Ownership::SessionOwner::Create(api, create());
    std::weak_ptr<Ownership::SessionOwner> partialOwner = activeOwner;
    auto partial = recordProxy(activeOwner, true);
    activeOwner.reset();
    const auto enqueues = DlssNr::Backend::LmxxfCut::Pending().enqueueCalls.load();
    Check(partial->proxy->ExecuteOn(queue.Get()), "execute partial recording");
    WaitQueue(device.Get(), queue.Get());
    Check(partial->proxy->ExecuteOn(other.Get()), "replay partial recording");
    WaitQueue(device.Get(), other.Get());
    Require(DlssNr::Backend::LmxxfCut::Pending().enqueueCalls.load() == enqueues,
            "partial recording never enqueues HIP");
    partial->proxy->Release(); partial.reset();
    Require(partialOwner.expired(), "partial recording collected after final Release");

    // Invalid queue facts cannot certify the begun execution. A failed Signal
    // must stay unconfirmed even if a later unrelated fence completes.
    void* faultContext = create(); auto fault = record(faultContext, 1280, 720, true);
    ok(api.BeginRecordingExecution(faultContext, fault->token, queue.Get()), "fault begin");
    Require(api.EndRecordingExecution(faultContext, fault->token, other.Get(), 0, nullptr, 0, S_OK)
            == LMXXF_NR_INVALID_ARGUMENT, "wrong actual queue rejected");
    ID3D12CommandList* faultLists[] = {fault->producer.Get(), fault->consumer.Get()};
    queue->ExecuteCommandLists(2, faultLists);
    Require(api.EndRecordingExecution(faultContext, fault->token, queue.Get(), 3, nullptr, 0, E_FAIL)
            == LMXXF_NR_UNAVAILABLE, "failed signal leaves no certificate");
    WaitQueue(device.Get(), queue.Get());
    Require(api.BeginRecordingExecution(faultContext, fault->token, queue.Get()) == LMXXF_NR_UNAVAILABLE,
            "uncertified chain cannot replay");
    fault->producer.Reset(); fault->consumer.Reset();
    ok(api.InvalidateRecording(faultContext, fault->token), "invalidate uncertified work");
    Require(api.CollectRecording(faultContext, fault->token) == LMXXF_NR_UNAVAILABLE,
            "unrelated completion cannot retire failed Signal");
    Require(api.Destroy(faultContext) == LMXXF_NR_UNAVAILABLE, "uncertified session retained until process exit");
    // Deliberately retained faultContext models the production fail-closed path.
    Ptr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info)))
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(i, message, &size), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
            Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "runtime D3D12 debug errors");
        }
    // The DLL must pin its own destructor callbacks when the host drops its module reference.
    FreeLibrary(dll);
    std::printf("runtime recording leases: PASS baseline=%016llx\n", static_cast<unsigned long long>(baseline));
}

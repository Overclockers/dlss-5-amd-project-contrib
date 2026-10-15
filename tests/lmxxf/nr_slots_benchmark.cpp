// Synthetic same-frame replay through the production split/inline host callbacks.
// No game workload: application rate is not a prediction of in-game FPS.
#include "lmxxf_gpu_test_utils.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <chrono>
#include <thread>
#include <deque>
#include <fstream>
#include <algorithm>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrApi.h"
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
namespace Own = DlssNr::Backend::LmxxfRecording;
namespace In = DlssNr::Backend::InlineRecording;
namespace Sub = DlssNr::Submission;
using Clock = std::chrono::steady_clock;
constexpr auto Read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

int main(int argc, char** argv) {
    Require(argc >= 4, "runtime, assets, slots (0=split), optional --verify --history --auto --first --debug --frames=N --interval=N --height=N --wait=N --iterations=N --hot --sweep --wait-sweep --mochizuki --aco --unorm8");
    unsigned slots = unsigned(std::atoi(argv[3])), frames = 120, height = 720;
    unsigned wait = 0, iterations = 0;
    bool explicitWait = false, explicitIterations = false;
    bool verify = false, verifyAhead = false, history = false, autoExposure = false, first = false, debug = false, hot = false, sweep = false;
    bool waitSweep = false, mochi = false, aco = false, unorm8 = false;
    bool gpuHandoff = false; unsigned pollDelay = 0;
    double interval = 0;
    for (int i=4;i<argc;++i) {
        std::string a=argv[i];
        verifyAhead |= a=="--verify-ahead"; verify |= a=="--verify" || a=="--verify-ahead"; history |= a=="--history"; autoExposure |= a=="--auto";
        sweep |= a=="--sweep"; first |= a=="--first"; debug |= a=="--debug"; hot |= a=="--hot";
        waitSweep |= a=="--wait-sweep"; mochi |= a=="--mochizuki"; aco |= a=="--aco";
        unorm8 |= a=="--unorm8";
        gpuHandoff |= a=="--gpu-handoff";
        if(a.find("--poll-delay=")==0) pollDelay=unsigned(std::stoi(a.substr(13)));
        if(a.find("--frames=")==0) frames=unsigned(std::stoi(a.substr(9)));
        if(a.find("--interval=")==0) interval=std::stod(a.substr(11));
        if(a.find("--height=")==0) height=unsigned(std::stoi(a.substr(9)));
        if(a.find("--wait=")==0) {wait=unsigned(std::stoi(a.substr(7)));explicitWait=true;}
        if(a.find("--iterations=")==0) {iterations=unsigned(std::stoi(a.substr(13)));explicitIterations=true;}
    }
    if(!explicitWait)wait=mochi?In::kWaitSlices:In::kLmxxfWaitSlices;
    if(!explicitIterations)iterations=mochi?In::kWaitIterations:In::kLmxxfWaitIterations;
    const unsigned maxSlots = mochi ? 4 : 5;
    Require(slots<=maxSlots && frames && height && iterations && interval>=0,"benchmark arguments");
    Require(!aco || mochi,"ACO requires Mochizuki");
    Require(!gpuHandoff || mochi,"GPU handoff requires Mochizuki");
    Require(!unorm8 || mochi,"UNORM8 comparison is for Mochizuki's whole compute chain");
    Require(!mochi || (!autoExposure && !hot),"Mochizuki does not support this auto-exposure/hot fixture");
    Require(!(hot && (sweep || waitSweep)) && !(sweep && waitSweep),"choose one sweep mode");
    if(debug) { Ptr<ID3D12Debug> d; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&d)),"debug layer"); d->EnableDebugLayer(); }
    Ptr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    Ptr<ID3D12Device> device; Ptr<IDXGIAdapter3> adapter;
    for(UINT i=0;;++i) {
        Ptr<IDXGIAdapter1> a; if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);
        if(d.VendorId==0x1002 && wcsstr(d.Description,L"9070 XT") &&
           SUCCEEDED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) {a.As(&adapter);break;}
    }
    Require(bool(device),"RX 9070 XT");
    Ptr<ID3D12CommandQueue> game; D3D12_COMMAND_QUEUE_DESC q{};Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&game)),"queue");
    HMODULE dll=LoadLibraryW(Widen(argv[1]).c_str());Require(dll!=nullptr,"runtime load");
    auto get=reinterpret_cast<int32_t(*)(uint32_t,LmxxfNrApi*)>(GetProcAddress(dll,mochi?"MochizukiNrGetApi":"LmxxfNrGetApi"));
    LmxxfNrApi api{};api.struct_size=sizeof(api);Require(get&&get(LMXXF_NR_ABI_VERSION,&api)==0,"package ABI");
    auto ok=[&](int rc,const char* step){if(rc){char e[512]{};api.GetLastError(e,sizeof(e));std::fprintf(stderr,"%s: %s\n",step,e);}Require(rc==0,step);};
    auto assets=Widen(argv[2]); LmxxfNrCreateInfo ci{sizeof(ci),device.Get(),game.Get(),assets.c_str(),LMXXF_NR_CREATE_FLAG_RECORDING_LEASES|LMXXF_NR_CREATE_FLAG_INLINE_QUEUE};
    void* context=nullptr;ok(api.Create(&ci,&context),"create");
    if(aco) {
        auto defaults=reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll,"MochizukiNrGetControlDefaults"));
        auto set=reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll,"MochizukiNrSetControls"));
        MochizukiNrControls controls{sizeof controls};Require(defaults&&set,"ACO controls");
        ok(defaults(&controls),"ACO defaults");controls.aco_mode=1;ok(set(context,&controls),"ACO request before device");
    }
    ok(api.PrepareSession(context),"prepare session");
    auto mzPrepare=reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(dll,"MochizukiNrPrepareFrame"));
    auto mzInfo=reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll,"MochizukiNrGetInfo"));
    Require(!mochi || (mzPrepare && mzInfo),"Mochizuki exports");
    auto owner=Own::SessionOwner::Create(api,context,slots?slots:1);Require(bool(owner),"owner");
    const unsigned width=height*16/9;
    auto texture=[&](DXGI_FORMAT format, unsigned w, unsigned h) {
        Ptr<ID3D12Resource> r; D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,Read,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
    };
    const auto colourFormat=unorm8?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT;
    auto colour=texture(colourFormat,width,height);
    Ptr<ID3D12Resource> patterns[3];
    for(unsigned i=0;i<3;++i){patterns[i]=texture(colourFormat,width,height);UploadColorPattern(device.Get(),game.Get(),patterns[i].Get(),PatternOptions{i});}
    auto motion=texture(mochi?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R32G32_FLOAT,width,height),depth=texture(DXGI_FORMAT_R32_FLOAT,width,height);
    // Fixed guides; colour changes every frame, while every recording reads the SAME game allocation.
    for(auto r:{motion.Get(),depth.Get()}) {
        Ptr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"guide heap");device->CreateUnorderedAccessView(r,nullptr,nullptr,heap->GetCPUDescriptorHandleForHeapStart());
        Ptr<ID3D12DescriptorHeap> cpuHeap;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&cpuHeap)),"CPU clear heap");device->CreateUnorderedAccessView(r,nullptr,nullptr,cpuHeap->GetCPUDescriptorHandleForHeapStart());
        Ptr<ID3D12CommandAllocator> a;Ptr<ID3D12GraphicsCommandList> l;
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&a)),"guide alloc");Check(device->CreateCommandList(0,q.Type,a.Get(),nullptr,IID_PPV_ARGS(&l)),"guide list");
        In::Barrier(l.Get(),r,Read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);auto* h=heap.Get();l->SetDescriptorHeaps(1,&h);
        const float z=r==depth.Get()?.5f:0;const float values[]{z,z,z,z};l->ClearUnorderedAccessViewFloat(heap->GetGPUDescriptorHandleForHeapStart(),cpuHeap->GetCPUDescriptorHandleForHeapStart(),r,values,0,nullptr);
        In::Barrier(l.Get(),r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,Read);Check(l->Close(),"guide close");In::Execute(game.Get(),l.Get());WaitQueue(device.Get(),game.Get());
    }
    Ptr<ID3D12Fence> gameTail; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gameTail)),"game tail");
    UINT64 gameValue=0;
    HANDLE completedEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(completedEvent!=nullptr,"completion event");
    struct Frame {
        Ptr<ID3D12CommandAllocator> allocator; Ptr<ID3D12GraphicsCommandList> raw;Ptr<Sub::CommandListProxy> list;
        std::shared_ptr<In::Resources> in;std::shared_ptr<Own::Lease> lease;
        Ptr<ID3D12QueryHeap> query;Ptr<ID3D12Resource> timestamps;
        ID3D12Resource* output=nullptr; ID3D12Resource* first=nullptr;
        Ptr<ID3D12Resource> verifiedOutput, verifiedFirst;
        Clock::time_point submitted; UINT64 tail=0; unsigned id=0; bool measured=false;
    };
    std::deque<std::unique_ptr<Frame>> pending;
    std::vector<double> gpuMs,latencyMs,cpuMs; unsigned applied=0,timedout=0,skipped=0,hotSplit=0,hotInline=0;UINT64 memory=0,frequency=0;
    Check(game->GetTimestampFrequency(&frequency),"timestamp frequency");
    auto memorySample=[&] {DXGI_QUERY_VIDEO_MEMORY_INFO m{};Check(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&m),"memory");memory=(std::max)(memory,m.CurrentUsage);};
    auto collect=[&](bool drain, bool one = false) {
        const auto deadline=GetTickCount64()+30000;
        while(!pending.empty()) {
            auto& f=*pending.front();
            bool complete;
            {std::lock_guard lock(Sub::RecordingMutex());complete=!f.lease->begun && (!f.in || f.in->Complete()) && gameTail->GetCompletedValue()!=UINT64_MAX && gameTail->GetCompletedValue()>=f.tail;}
            if(!complete) {
                if(!drain)return;
                Require(GetTickCount64()<deadline,"worker completion");
                if(gameTail->GetCompletedValue()<f.tail) {
                    Check(gameTail->SetEventOnCompletion(f.tail,completedEvent),"completion event arm");
                    Require(WaitForSingleObject(completedEvent,30000)==WAIT_OBJECT_0,"game completion");
                } else std::this_thread::yield(); // worker/side bookkeeping, no Windows timer-tick penalty
                continue;
            }
            // Split's CPU callback is synchronous; its game fence still must complete before readback/Reset.
            if(wait && f.in && !f.in->Succeeded()) {
                std::lock_guard lock(Sub::RecordingMutex());
                std::fprintf(stderr,"WAIT_FAILURE frame=%u measured=%u spins=%u phase=%s error=%s\n",
                    f.id,f.measured,f.in->LastSpinCount(),owner->failure.phase?owner->failure.phase:"none",owner->failure.error.data());
            }
            if(f.measured) {
                if(!f.in||f.in->Succeeded())++applied;else ++timedout;
                UINT64* data=nullptr;D3D12_RANGE range{0,16};Check(f.timestamps->Map(0,&range,reinterpret_cast<void**>(&data)),"timestamps");
                gpuMs.push_back(double(data[1]-data[0])*1000/double(frequency));D3D12_RANGE none{};f.timestamps->Unmap(0,&none);
                latencyMs.push_back(std::chrono::duration<double,std::milli>(Clock::now()-f.submitted).count());
                if(verify) {
                    Require(!f.in||f.in->Succeeded(),"verification frame applied NR");
                    auto hash=[&](ID3D12Resource* r){return unorm8?HashTextureUnorm8(device.Get(),game.Get(),r):HashTexture(device.Get(),game.Get(),r);};
                    std::printf("HASH frame=%u output=%016llx first=%016llx\n",f.id,
                        hash(f.output),f.first?hash(f.first):0);
                }
            }
            f.list.Reset();f.raw.Reset();
            {std::lock_guard lock(Sub::RecordingMutex());Own::Collect();}
            pending.pop_front();
            if(one)return;
        }
    };
    auto submit=[&](unsigned id,bool measured) {
        collect(false);
        // Unpaced throughput uses backpressure so every mode processes the same
        // number of frames. Paced mode retains the game's actual bypass policy.
        if(!hot && interval==0 && slots) {
            for(;;) {
                bool busy;
                {std::lock_guard lock(Sub::RecordingMutex());busy=owner->InlineBusy();}
                if(!busy)break;
                Require(!pending.empty(),"capacity has an outstanding frame");
                collect(true,true); // sliding window: a free slot admits the next frame immediately
            }
        }
        if(hot && measured) {
            static const unsigned choices[]{5,1,3,2,4,0};slots=choices[(id/6)%6];if(!slots)collect(true);
            std::lock_guard lock(Sub::RecordingMutex());owner->inlineLimit=slots?slots:1;
        }
        {
            std::lock_guard lock(Sub::RecordingMutex());
            owner->ReclaimInlineLocked();
            if((slots && owner->InlineBusy()) || (!slots && !owner->inlineInFlight.empty())) {if(measured)++skipped;return;}
        }
        if(measured){if(slots)++hotInline;else ++hotSplit;}
        auto f=std::make_unique<Frame>();f->id=id;f->measured=measured;
        auto start=Clock::now();
        Check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&f->allocator)),"allocator");
        Check(device->CreateCommandList(0,q.Type,f->allocator.Get(),nullptr,IID_PPV_ARGS(&f->raw)),"native");
        Check(Sub::CommandListProxy::Create(device.Get(),f->allocator.Get(),f->raw.Get(),&f->list),"proxy");
        D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=2;
        Check(device->CreateQueryHeap(&query,IID_PPV_ARGS(&f->query)),"timestamps heap");
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=16;rd.Height=1;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f->timestamps)),"timestamp readback");
        f->list->EndQuery(f->query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        In::Barrier(f->list.Get(),colour.Get(),Read,D3D12_RESOURCE_STATE_COPY_DEST);
        In::Barrier(f->list.Get(),patterns[id%3].Get(),Read,D3D12_RESOURCE_STATE_COPY_SOURCE);
        f->list->CopyResource(colour.Get(),patterns[id%3].Get());
        In::Barrier(f->list.Get(),patterns[id%3].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,Read);
        In::Barrier(f->list.Get(),colour.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);
        LmxxfNrFrameInfo fi{};fi.struct_size=sizeof(fi);fi.frame_id=id+1;fi.command_list=f->list.Get();fi.color=colour.Get();fi.color_width=width;fi.color_height=height;fi.color_state=Read;
        fi.transfer_strength=fi.color_strength=fi.model_scale=fi.paper_white=fi.pre_exposure=fi.exposure_scale=1;
        fi.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|(autoExposure?LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE:0)|(first?LMXXF_NR_FRAME_FLAG_FIRST_PASS:0);
        fi.inline_slots=slots;fi.temporal_flags=history?(LMXXF_NR_TEMPORAL_MODEL_HISTORY|LMXXF_NR_TEMPORAL_INPUTS_VALID):0;
        fi.motion=motion.Get();fi.depth=depth.Get();fi.motion_state=fi.depth_state=Read;fi.motion_width=width;fi.motion_height=height;fi.motion_scale_x=fi.motion_scale_y=1;
        LmxxfNrJob job{};job.struct_size=sizeof(job);
        {
            std::unique_lock lock(Sub::RecordingMutex());
            if(mochi) {
                MochizukiNrFrameInfo mf{};mf.struct_size=sizeof mf;mf.frame_id=fi.frame_id;
                mf.color=colour.Get();mf.color_width=width;mf.color_height=height;mf.color_state=Read;
                mf.transfer_strength=mf.color_strength=mf.model_scale=1;mf.passes=1;
                mf.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|(first?LMXXF_NR_FRAME_FLAG_FIRST_PASS:0)|
                    (slots?MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION:0);
                if(history) {
                    mf.flags|=MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;mf.motion=motion.Get();
                    mf.motion_width=width;mf.motion_height=height;mf.motion_scale_x=mf.motion_scale_y=1;mf.motion_state=Read;
                }
                const auto deadline=GetTickCount64()+180000;
                for(;;) {
                    const auto rc=mzPrepare(context,&mf,&job);
                    if(!rc)break;
                    if(rc!=LMXXF_NR_UNAVAILABLE || GetTickCount64()>=deadline)ok(rc,"Mochizuki prepare frame");
                    // Initial asynchronous network build must not block the worker's host mutex.
                    lock.unlock();Sleep(100);lock.lock();
                }
                if(aco) {MochizukiNrInfo info{sizeof info};ok(mzInfo(context,&info),"ACO info");Require(info.aco_effective==1,"ACO effective");}
            } else ok(api.PrepareFrame(context,&fi,&job),"prepare frame");
            if(slots) {
                f->in=std::make_shared<In::Resources>(device.Get(),colour.Get(),static_cast<ID3D12Resource*>(job.private_output),
                    static_cast<ID3D12Resource*>(job.first_pass_output),width,height,owner->inlinePipelines);
                owner->inlinePipelines=f->in->PipelineCache();
                f->in->gpuArmed=gpuHandoff; f->in->spinPollDelay=pollDelay;
                ID3D12Resource* sources[]{colour.Get(),motion.Get(),depth.Get()};D3D12_RESOURCE_STATES states[]{Read,Read,Read};
                Check(f->list->BeginInlineState(history?3:1,sources,states),"inline scope");
            }
            f->lease=Own::Attach(owner,job.handle,f->list.Get(),f->in);Require(bool(f->lease),"attach");
            f->lease->neural=true;
            if(f->in) f->in->BeginInputs(f->list.Get());
            ok(api.RecordInputs(context,job.handle,f->list.Get()),"inputs");
            if(f->in) {f->in->RecordCapture(f->list.Get());f->in->RecordWait(f->list.Get(),wait,iterations);}
            else Check(DlssNr::Backend::LmxxfCut::TrySplitAtEvaluate(f->list.Get()),"split");
            ok(api.RecordOutputs(context,job.handle,f->list.Get()),"outputs");
            if(f->in) {f->in->RecordSelect(f->list.Get(),Read);f->list->EndInlineState();}
            f->output=f->in?f->in->OutputTexture():static_cast<ID3D12Resource*>(job.private_output);
            f->first=f->in?f->in->FirstTexture():static_cast<ID3D12Resource*>(job.first_pass_output);
            f->lease->ready=true;
        }
        if(verify) {
            auto snapshot=[&](ID3D12Resource*& src,Ptr<ID3D12Resource>& dst){
                if(!src)return;
                auto d=src->GetDesc();dst=texture(d.Format,unsigned(d.Width),d.Height);
                In::Barrier(f->list.Get(),src,Read,D3D12_RESOURCE_STATE_COPY_SOURCE);
                In::Barrier(f->list.Get(),dst.Get(),Read,D3D12_RESOURCE_STATE_COPY_DEST);
                f->list->CopyResource(dst.Get(),src);
                In::Barrier(f->list.Get(),src,D3D12_RESOURCE_STATE_COPY_SOURCE,Read);
                In::Barrier(f->list.Get(),dst.Get(),D3D12_RESOURCE_STATE_COPY_DEST,Read);src=dst.Get();
            };
            snapshot(f->output,f->verifiedOutput);snapshot(f->first,f->verifiedFirst);
        }
        f->list->EndQuery(f->query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        f->list->ResolveQueryData(f->query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,f->timestamps.Get(),0);
        Check(f->list->Close(),"close");f->submitted=Clock::now();Check(f->list->ExecuteOn(game.Get()),"execute");
        if(measured)cpuMs.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
        f->tail=++gameValue; Check(game->Signal(gameTail.Get(),f->tail),"frame tail");
        pending.push_back(std::move(f));memorySample();
        if(verify && !verifyAhead)collect(true);
        // Inline admission itself bounds active work. In paced mode, forcing a
        // drain at five would stall only the five-slot case instead of allowing
        // its normal bypass policy, making coverage incomparable with 1..4.
        else if(!slots && pending.size()>=5)collect(true,true);
    };
    auto segment=[&](unsigned selected,unsigned trial) {
    slots=selected;
    {std::lock_guard lock(Sub::RecordingMutex());owner->inlineLimit=slots?slots:1;}
    for(unsigned i=0;i<12;++i){submit(i,false);collect(true);} // includes compiler/network warm-up
    ok(api.ResetHistory(context),"reset measured sequence");
    gpuMs.clear();latencyMs.clear();cpuMs.clear();applied=timedout=skipped=hotSplit=hotInline=0;memory=0;
    auto begin=Clock::now();
    for(unsigned i=0;i<frames;++i){
        if(interval>0)std::this_thread::sleep_until(begin+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>(i*interval)));
        submit(i,true);
    }
    collect(true);const double wall=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    Require(!owner->failed,"host callbacks succeeded");
    if(hot)Require(hotSplit>0&&hotInline>0,"hot transition actually submitted both split and inline");
    auto percentile=[](std::vector<double> v,double p){if(v.empty())return 0.;std::sort(v.begin(),v.end());return v[size_t((v.size()-1)*p)];};
    auto average=[](const std::vector<double>& v){double sum=0;for(auto x:v)sum+=x;return v.empty()?0.:sum/v.size();};
    std::printf("BENCH backend=%s aco=%u unorm8=%u gpu_handoff=%u poll_delay=%u trial=%u slots=%u history=%u auto=%u first=%u height=%u interval_ms=%.3f wait=%u iterations=%u frames=%u applied=%u timeout=%u skipped=%u wall_ms=%.3f nr_per_s=%.3f gpu_p50_ms=%.3f gpu_p95_ms=%.3f cpu_mean_ms=%.3f observed_completion_p95_ms=%.3f peak_local_mib=%.1f\n",
        mochi?"mochizuki":"lmxxf",aco,unorm8,gpuHandoff,pollDelay,trial,slots,history,autoExposure,first,height,interval,wait,iterations,frames,applied,timedout,skipped,wall,applied*1000./wall,
        percentile(gpuMs,.5),percentile(gpuMs,.95),average(cpuMs),percentile(latencyMs,.95),memory/1048576.);
    std::fflush(stdout);
    };
    if(waitSweep) {
        // Same process/device/network, equal total polling work; reverse the whole
        // sequence to expose run-order bias. Split never emits a polling dispatch.
        struct Variant {unsigned slots,slices,iterations;};
        std::vector<Variant> variants{{0,wait,iterations}};
        for(auto slices:{In::kWaitSlices,In::kLmxxfWaitSlices})for(unsigned i=1;i<=maxSlots;++i)
            variants.push_back({i,slices,2097152u/slices});
        for(unsigned trial=0;trial<2;++trial) {
            for(const auto& v:variants){wait=v.slices;iterations=v.iterations;segment(v.slots,trial);}
            std::reverse(variants.begin(),variants.end());
        }
    } else if(sweep) {
        for(unsigned i=0;i<=maxSlots;++i)segment(i,0);
        for(int i=int(maxSlots);i>=0;--i)segment(unsigned(i),1);
    } else segment(slots,0);
    if(debug) {
        Ptr<ID3D12InfoQueue> info;Check(device.As(&info),"debug info"); unsigned atomicReports=0;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char>b(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data());info->GetMessage(i,m,&size);
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){
                // Existing finite inline handshake intentionally polls two globallycoherent
                // atomic UAVs across queues. The debug layer cannot see this shader protocol.
                const bool handshake=std::strstr(m->pDescription,"Simultaneous-access or Buffer Resource") &&
                    (std::strstr(m->pDescription,"NR inline atomic capture") || std::strstr(m->pDescription,"NR inline atomic control"));
                if(handshake){++atomicReports;continue;}
                std::fprintf(stderr,"D3D12 id=%u: %s\n",unsigned(m->ID),m->pDescription);Require(false,"D3D12 validation");
            }}
        std::printf("DEBUG atomic_handshake_reports=%u other_errors=0\n",atomicReports);
    }
    CloseHandle(completedEvent);owner.reset();FreeLibrary(dll);return 0;
}

#include "runtime_test_helpers.h"
int wmain(int argc,wchar_t**argv) try {
    Require(argc>=3,"runtime and assets required");
    HMODULE dll=LoadLibraryW(argv[1]);Require(dll!=nullptr,"runtime load");
    auto get=reinterpret_cast<PFN_MochizukiNrGetApi>(GetProcAddress(dll,"MochizukiNrGetApi"));
    api.struct_size=sizeof api;Rc(get(LMXXF_NR_ABI_VERSION,&api));
    auto prepare=reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(dll,"MochizukiNrPrepareFrame"));
    auto info=reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll,"MochizukiNrGetInfo"));
    ComPtr<IDXGIFactory4> factory;Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x1002&&SUCCEEDED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;}
    Require(bool(device),"AMD device");ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC desc{};
    Hr(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)));
    LmxxfNrCreateInfo ci{sizeof ci,device.Get(),queue.Get(),argv[2],LMXXF_NR_CREATE_FLAG_RECORDING_LEASES|LMXXF_NR_CREATE_FLAG_INLINE_QUEUE};
    void* context=nullptr;Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));
    const bool refuse=argc>3&&wcscmp(argv[3],L"refuse")==0;
    const bool unorm8=refuse||(argc>3&&wcscmp(argv[3],L"unorm8")==0);
    // r11: R11G11B10_FLOAT with NrInlineComputeConvert; input unpack + network + runtime_pack_r11 write-back = one compute phase.
    const bool r11=argc>3&&wcscmp(argv[3],L"r11")==0;
    // bgra8 / srgb8 / bgra8srgb / rgb10a2: the other NrInlineComputeConvert formats, same one-phase requirement.
    const wchar_t* convertModes[]={L"bgra8",L"srgb8",L"bgra8srgb",L"rgb10a2"};
    const DXGI_FORMAT convertFormats[]={DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R10G10B10A2_UNORM};
    int convertMode=-1;
    for(int i=0;i<4&&argc>3;i++)if(wcscmp(argv[3],convertModes[i])==0)convertMode=i;
    const bool convert=r11||convertMode>=0;
    // motion: dynamic resolution (240x256 in a 256x256 allocation) with 128x128 motion vectors, so they are Rescaled;
    // NrInlineComputeMotion rescales them on compute and the chain stays one compute phase.
    const bool motionMode=argc>3&&wcscmp(argv[3],L"motion")==0;
    // scaled / scaledr11: model scale 0.5 with NrInlineComputeDownscale (RGBA16F packs on compute; R11G11B10 likewise).
    const bool scaledR11=argc>3&&wcscmp(argv[3],L"scaledr11")==0;
    const bool scaled=scaledR11||(argc>3&&wcscmp(argv[3],L"scaled")==0);
    // multi: two layers, no motion. export: two layers with the first-layer export and NrInlineComputeExport, after
    // three split frames (the export is offered from the second served frame on).
    const bool exportMode=argc>3&&wcscmp(argv[3],L"export")==0;
    const bool multi=exportMode||(argc>3&&wcscmp(argv[3],L"multi")==0);
    if(motionMode){
        auto defaults=reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll,"MochizukiNrGetControlDefaults"));
        auto set=reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll,"MochizukiNrSetControls"));
        MochizukiNrControls controls{sizeof controls};Rc(defaults(&controls));controls.drs_mode=1;Rc(set(context,&controls));
    }
    Frame mv(device.Get(),128,128);
    Frame frame(device.Get(),256,256,unorm8?DXGI_FORMAT_R8G8B8A8_UNORM:(r11||scaledR11)?DXGI_FORMAT_R11G11B10_FLOAT:
                convertMode>=0?convertFormats[convertMode]:DXGI_FORMAT_R16G16B16A16_FLOAT);List upload(device.Get());frame.Upload(upload.cmd.Get());if(motionMode)mv.Upload(upload.cmd.Get());Hr(upload.cmd->Close());Submit(queue.Get(),upload);
    ComPtr<ID3D12Fence> tail;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&tail)));
    Hr(queue->Signal(tail.Get(),1));Wait(tail.Get(),1);
    MochizukiNrFrameInfo f{};f.struct_size=sizeof f;f.frame_id=1;f.color=frame.color.Get();f.color_width=f.color_height=256;
    f.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;f.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION|(convert?MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_CONVERT:0u);
    f.transfer_strength=f.color_strength=f.model_scale=1;f.passes=1;
    if(multi)f.passes=2;
    if(exportMode)f.flags|=LMXXF_NR_FRAME_FLAG_FIRST_PASS|MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_EXPORT;
    if(scaled){f.model_scale=.5f;f.flags|=MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_DOWNSCALE;}
    if(motionMode){
        f.color_width=240;f.motion=mv.color.Get();f.motion_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        f.motion_width=f.motion_height=128;f.motion_scale_x=f.motion_scale_y=1;
        f.flags|=MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL|MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_MOTION;
    }
    uint64_t fence=2;
    if(argc>3&&wcscmp(argv[3],L"lifecycle")==0){
        // All compute toggles on, no fault hooks. Every frame must complete, certify and collect while the geometry,
        // the colour format, the motion extent, the model scale, the layer count and the queue (split <-> inline)
        // change under it; two inline frames are prepared and recorded before either executes; ResetHistory and a
        // Destroy/Create (Release) happen mid-run.
        auto defaults=reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll,"MochizukiNrGetControlDefaults"));
        auto set=reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll,"MochizukiNrSetControls"));
        MochizukiNrControls controls{sizeof controls};Rc(defaults(&controls));controls.drs_mode=1;Rc(set(context,&controls));
        Frame r11(device.Get(),256,256,DXGI_FORMAT_R11G11B10_FLOAT);
        {List up(device.Get());r11.Upload(up.cmd.Get());mv.Upload(up.cmd.Get());Hr(up.cmd->Close());Submit(queue.Get(),up);
         Hr(queue->Signal(tail.Get(),++fence));Wait(tail.Get(),fence);}
        const uint32_t all=MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_CONVERT|MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_MOTION|
            MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_DOWNSCALE|MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_EXPORT;
        struct Step{bool r11;UINT width;bool motion;float scale;uint32_t passes;bool inl;};
        const Step steps[]={{false,256,false,1,1,false},{false,256,false,1,1,true},{false,224,true,1,1,true},
            {false,224,true,.5f,1,true},{true,256,false,1,1,true},{true,240,true,1,2,false},{true,240,true,1,2,true},
            {false,256,false,1,2,true},{false,256,false,.5f,1,false},{false,192,true,1,1,true}};
        uint64_t id=1000;
        auto prepareOne=[&](const Step& st,LmxxfNrJob& j,List& in,List& out){
            MochizukiNrFrameInfo fi{};fi.struct_size=sizeof fi;fi.frame_id=++id;
            fi.color=(st.r11?r11:frame).color.Get();fi.color_width=st.width;fi.color_height=256;
            fi.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            fi.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|all|(st.inl?MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION:0u)|
                (st.passes>1?LMXXF_NR_FRAME_FLAG_FIRST_PASS:0u)|(st.motion?MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL:0u);
            fi.transfer_strength=fi.color_strength=1;fi.model_scale=st.scale;fi.passes=st.passes;
            if(st.motion){fi.motion=mv.color.Get();fi.motion_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                fi.motion_width=fi.motion_height=128;fi.motion_scale_x=fi.motion_scale_y=1;}
            const auto until=GetTickCount64()+180000;
            for(;;){auto rc=prepare(context,&fi,&j);if(!rc)break;Require(rc==LMXXF_NR_UNAVAILABLE&&GetTickCount64()<until,"prepare (lifecycle)");Sleep(20);}
            Rc(api.RecordInputs(context,j.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,j.handle,out.cmd.Get()));
            Hr(in.cmd->Close());Hr(out.cmd->Close());
        };
        auto execute=[&](LmxxfNrJob& j,List& in,List& out){
            Rc(api.BeginRecordingExecution(context,j.handle,queue.Get()));Submit(queue.Get(),in);
            Rc(api.EnqueueHip(context,j.handle,queue.Get()));Submit(queue.Get(),out);auto signal=queue->Signal(tail.Get(),++fence);
            Rc(api.EndRecordingExecution(context,j.handle,queue.Get(),3,tail.Get(),fence,signal));Wait(tail.Get(),fence);
            Rc(api.InvalidateRecording(context,j.handle));Rc(api.CollectRecording(context,j.handle));
        };
        for(size_t i=0;i<sizeof steps/sizeof*steps;i++){
            const Step& st=steps[i];
            if(i==4)Rc(api.ResetHistory(context));
            if(i==7){Rc(api.Destroy(context));context=nullptr;Rc(api.Create(&ci,&context));Rc(api.PrepareSession(context));Rc(set(context,&controls));}
            if(st.inl&&i%3==1){
                // Two in flight: both prepared and recorded before the first executes.
                LmxxfNrJob a{sizeof a},b{sizeof b};List ai(device.Get()),ao(device.Get()),bi(device.Get()),bo(device.Get());
                prepareOne(st,a,ai,ao);prepareOne(st,b,bi,bo);execute(a,ai,ao);execute(b,bi,bo);
            } else {
                LmxxfNrJob j{sizeof j};List in(device.Get()),out(device.Get());prepareOne(st,j,in,out);execute(j,in,out);
            }
            MochizukiNrInfo state{sizeof state};Rc(info(context,&state));Require(!state.failed,"lifecycle step failed the session");
            std::printf("LIFECYCLE step %zu ok inline_path=%u %s\n",i,state.inline_path,state.inline_path_why);
        }
        Rc(api.Destroy(context));
        std::printf("INLINE_LIFECYCLE_OK\n");
        return 0;
    }
    for(int i=0;exportMode&&i<3;i++){
        // Split frames (no inline flag, so the phase-failure hook does not apply): complete, certified, collected.
        MochizukiNrFrameInfo sf=f;sf.frame_id=100+i;sf.flags&=~MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION;
        LmxxfNrJob sj{sizeof sj};const auto until=GetTickCount64()+120000;
        for(;;){auto rc=prepare(context,&sf,&sj);if(!rc)break;Require(rc==LMXXF_NR_UNAVAILABLE&&GetTickCount64()<until,"prepare (split)");Sleep(20);}
        List si(device.Get()),so(device.Get());Rc(api.RecordInputs(context,sj.handle,si.cmd.Get()));Rc(api.RecordOutputs(context,sj.handle,so.cmd.Get()));
        Hr(si.cmd->Close());Hr(so.cmd->Close());Rc(api.BeginRecordingExecution(context,sj.handle,queue.Get()));Submit(queue.Get(),si);
        Rc(api.EnqueueHip(context,sj.handle,queue.Get()));Submit(queue.Get(),so);auto signal=queue->Signal(tail.Get(),++fence);
        Rc(api.EndRecordingExecution(context,sj.handle,queue.Get(),3,tail.Get(),fence,signal));Wait(tail.Get(),fence);
        Rc(api.InvalidateRecording(context,sj.handle));Rc(api.CollectRecording(context,sj.handle));
    }
    LmxxfNrJob job{sizeof job};const auto deadline=GetTickCount64()+120000;
    for(;;){auto rc=prepare(context,&f,&job);if(!rc)break;Require(rc==LMXXF_NR_UNAVAILABLE&&GetTickCount64()<deadline,"prepare");Sleep(20);}
    Require(!exportMode || job.first_pass_output!=nullptr,"the first-layer export was not offered");
    List in(device.Get()),out(device.Get());Rc(api.RecordInputs(context,job.handle,in.cmd.Get()));Rc(api.RecordOutputs(context,job.handle,out.cmd.Get()));Hr(in.cmd->Close());Hr(out.cmd->Close());
    Rc(api.BeginRecordingExecution(context,job.handle,queue.Get()));Submit(queue.Get(),in);
    auto rc=api.EnqueueHip(context,job.handle,queue.Get());
    char error[256]{};api.GetLastError(error,sizeof error);
    if(refuse){
        // MZ_TEST_COMPUTE_REFUSE_AT=1: the whole-chain compute recording is refused after
        // the core may have changed its CPU-side state. The frame fails; no passthrough.
        Require(rc!=LMXXF_NR_OK&&strstr(error,"MZ_TEST_COMPUTE_REFUSE_AT")!=nullptr,"refused compute recording was not reported");
        Require(strstr(error,"passed through")==nullptr,"refused compute recording was passed through");
        MochizukiNrInfo state{sizeof state};Rc(info(context,&state));
        Require(state.failed!=0,"refused compute recording left the session running on stale core state");
        std::printf("INLINE_COMPUTE_REFUSAL_FAILED_FRAME %s\n",error);
        return 0; // no Vulkan submit: the context stays pinned until process exit
    }
    Require(rc!=LMXXF_NR_OK && strstr(error,"inline network phase")!=nullptr,"injected partial submission was not reported");
    Require(!unorm8 || strstr(error,"phase 1/1")!=nullptr,"UNORM8 inline frame did not run as one compute phase");
    Require(!r11 || strstr(error,"phase 1/1")!=nullptr,"B10G11R11 inline frame (compute convert) did not run as one compute phase");
    Require(!multi || strstr(error,"phase 1/1")!=nullptr,"two-layer inline frame (with or without compute export) did not run as one compute phase");
    Require(!scaled || strstr(error,"phase 1/1")!=nullptr,"model-scale 0.5 inline frame (compute downscale) did not run as one compute phase");
    Require(!motionMode || strstr(error,"phase 1/1")!=nullptr,"rescaled-motion inline frame (compute motion) did not run as one compute phase");
    Require(convertMode<0 || strstr(error,"phase 1/1")!=nullptr,"compute-convert inline frame did not run as one compute phase");
    ++fence;Hr(queue->Signal(tail.Get(),fence));Wait(tail.Get(),fence);
    api.EndRecordingExecution(context,job.handle,queue.Get(),LMXXF_NR_SUBMITTED_PRODUCER,tail.Get(),fence,S_OK);
    Rc(api.InvalidateRecording(context,job.handle));
    Require(api.CollectRecording(context,job.handle)==LMXXF_NR_UNAVAILABLE,"D3D12 tail alone certified partial Vulkan chain");
    Require(api.Destroy(context)==LMXXF_NR_UNAVAILABLE,"partial chain released resources without Vulkan proof");
    std::printf("INLINE_PHASE_FAILURE_RETAINED %s\n",error);
    // The failed context remains deliberately pinned until process exit.
    return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}

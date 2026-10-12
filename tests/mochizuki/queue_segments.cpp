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
    Frame frame(device.Get(),256,256,unorm8?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT);List upload(device.Get());frame.Upload(upload.cmd.Get());Hr(upload.cmd->Close());Submit(queue.Get(),upload);
    ComPtr<ID3D12Fence> tail;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&tail)));
    Hr(queue->Signal(tail.Get(),1));Wait(tail.Get(),1);
    MochizukiNrFrameInfo f{};f.struct_size=sizeof f;f.frame_id=1;f.color=frame.color.Get();f.color_width=f.color_height=256;
    f.color_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;f.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION;
    f.transfer_strength=f.color_strength=f.model_scale=1;f.passes=1;
    LmxxfNrJob job{sizeof job};const auto deadline=GetTickCount64()+120000;
    for(;;){auto rc=prepare(context,&f,&job);if(!rc)break;Require(rc==LMXXF_NR_UNAVAILABLE&&GetTickCount64()<deadline,"prepare");Sleep(20);}
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
    Hr(queue->Signal(tail.Get(),2));Wait(tail.Get(),2);
    api.EndRecordingExecution(context,job.handle,queue.Get(),LMXXF_NR_SUBMITTED_PRODUCER,tail.Get(),2,S_OK);
    Rc(api.InvalidateRecording(context,job.handle));
    Require(api.CollectRecording(context,job.handle)==LMXXF_NR_UNAVAILABLE,"D3D12 tail alone certified partial Vulkan chain");
    Require(api.Destroy(context)==LMXXF_NR_UNAVAILABLE,"partial chain released resources without Vulkan proof");
    std::printf("INLINE_PHASE_FAILURE_RETAINED %s\n",error);
    // The failed context remains deliberately pinned until process exit.
    return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}

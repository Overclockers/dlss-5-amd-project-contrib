// Temporal/moving-input and retained-recording comparisons adapted from
// Overclockers' PR #5, 60f9202fd6bc0384d1d4615658cee2ddca9b3004.
// Scheduling policy is selected by a separate reference DLL, never the product ABI.
#include "runtime_test_helpers.h"
#include <fstream>

int wmain(int argc, wchar_t** argv) try {
    Require(argc==15, "dll assets output width height format passes frames moving enlargement preprocess scale capacity first");
    const UINT width=UINT(_wtoi(argv[4])), height=UINT(_wtoi(argv[5]));
    const bool fp16=std::wstring(argv[6])==L"fp16";
    Require(fp16 || std::wstring(argv[6])==L"srgb", "format must be fp16 or srgb");
    const UINT passes=UINT(_wtoi(argv[7])), frames=UINT(_wtoi(argv[8]));
    const bool moving=_wtoi(argv[9])!=0, preprocess=_wtoi(argv[11])!=0;
    const UINT enlargement=UINT(_wtoi(argv[10])), capacity=UINT(_wtoi(argv[13]));
    const float scale=float(_wtoi(argv[12]))/100.f;
    const bool firstPass=_wtoi(argv[14])!=0;
    Require(width>=128 && width<=3840 && height>=128 && height<=2160 &&
            passes>=1 && passes<=30 && frames>=3 && frames<=128 && enlargement<=2 &&
            (scale==1.f || scale==.5f) && (capacity==0 || capacity==30), "invalid fixture dimensions or controls");
    HMODULE dll=LoadLibraryW(argv[1]); Require(dll!=nullptr,"runtime load");
    auto get=reinterpret_cast<PFN_MochizukiNrGetApi>(GetProcAddress(dll,"MochizukiNrGetApi"));
    api.struct_size=sizeof api; Require(get!=nullptr,"API export"); Rc(get(LMXXF_NR_ABI_VERSION,&api));
    auto defaults=reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll,"MochizukiNrGetControlDefaults"));
    auto set=reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll,"MochizukiNrSetControls"));
    auto prepare=reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(dll,"MochizukiNrPrepareFrame"));
    auto info=reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll,"MochizukiNrGetInfo"));
    Require(defaults && set && prepare && info,"required exports");
    ComPtr<IDXGIFactory6> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device; DXGI_ADAPTER_DESC1 selected{};
    for(UINT n=0;;++n) {
        ComPtr<IDXGIAdapter1> adapter; if(factory->EnumAdapters1(n,&adapter)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{}; Hr(adapter->GetDesc1(&desc));
        if(desc.VendorId==0x1002 && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) &&
           wcsstr(desc.Description,L"9070 XT") &&
           SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) {selected=desc;break;}
    }
    Require(device!=nullptr,"RX 9070 XT required");
    std::printf("TEMPORAL_ADAPTER vendor=0x%04x device=0x%04x\n",selected.VendorId,selected.DeviceId);
    ComPtr<ID3D12CommandQueue> queues[2]; D3D12_COMMAND_QUEUE_DESC qdesc{};
    for(auto& q:queues) Hr(device->CreateCommandQueue(&qdesc,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); UINT64 value=0;
    LmxxfNrCreateInfo ci{sizeof ci,device.Get(),queues[0].Get(),argv[2],LMXXF_NR_CREATE_FLAG_RECORDING_LEASES};
    void* context=nullptr; Rc(api.Create(&ci,&context));
    MochizukiNrControls controls{sizeof controls}; Rc(defaults(&controls));
    controls.aco_mode=1; controls.compact_transfer=0; controls.enlarge_mode=enlargement;
    controls.max_passes=capacity; controls.preprocess=preprocess; controls.preprocess_exposure=2;
    Rc(set(context,&controls)); Rc(api.PrepareSession(context)); // ACO requested before device creation.
    const auto format=fp16?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    Frame colour(device.Get(),width,height,format), motion(device.Get(),width,height);
    void* ptr=nullptr; Hr(motion.upload->Map(0,nullptr,&ptr));
    std::memset(ptr,0,size_t(motion.footprint.Footprint.RowPitch)*height); motion.upload->Unmap(0,nullptr);
    List initial(device.Get()); motion.Upload(initial.cmd.Get()); colour.Upload(initial.cmd.Get());
    Hr(initial.cmd->Close()); Submit(queues[0].Get(),initial); Hr(queues[0]->Signal(fence.Get(),++value)); Wait(fence.Get(),value);
    const size_t rowBytes=size_t(width)*(fp16?8:4); std::vector<unsigned char> original(rowBytes*height);
    auto copyOriginal=[&] {
        void* p=nullptr; Hr(colour.upload->Map(0,nullptr,&p));
        for(UINT y=0;y<height;++y) memcpy(original.data()+y*rowBytes,static_cast<char*>(p)+y*colour.footprint.Footprint.RowPitch,rowBytes);
        colour.upload->Unmap(0,nullptr);
    };
    copyOriginal();
    auto update=[&](UINT n) {
        void* p=nullptr; Hr(colour.upload->Map(0,nullptr,&p));
        for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
            const float base=.05f+.65f*float((x+3*n)%width)/width+(((x+n)/8+(y+2*n)/8)%2)*.1f;
            auto* row=static_cast<char*>(p)+colour.footprint.Footprint.RowPitch*y;
            if(fp16) {
                auto* pixel=reinterpret_cast<uint16_t*>(row)+4*x;
                for(int c=0;c<3;++c) pixel[c]=DirectX::PackedVector::XMConvertFloatToHalf(base*(1.f-.2f*c));
                pixel[3]=0x3c00;
            } else {
                auto* pixel=reinterpret_cast<uint8_t*>(row)+4*x;
                for(int c=0;c<3;++c) pixel[c]=uint8_t(std::round(base*(1.f-.2f*c)*255));
                pixel[3]=255;
            }
        }
        colour.upload->Unmap(0,nullptr); copyOriginal();
        List change(device.Get()); Barrier(change.cmd.Get(),colour.color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        colour.Upload(change.cmd.Get()); Hr(change.cmd->Close()); Submit(queues[0].Get(),change);
        Hr(queues[0]->Signal(fence.Get(),++value)); Wait(fence.Get(),value);
    };
    struct Recording {LmxxfNrJob job{sizeof job}; List in,out; explicit Recording(ID3D12Device* d):in(d),out(d){}};
    auto execute=[&](Recording& r,UINT queueIndex=0) {
        auto* q=queues[queueIndex].Get(); Rc(api.BeginRecordingExecution(context,r.job.handle,q)); Submit(q,r.in);
        Rc(api.EnqueueHip(context,r.job.handle,q)); Submit(q,r.out);
        const auto hr=q->Signal(fence.Get(),++value); Rc(api.EndRecordingExecution(context,r.job.handle,q,3,fence.Get(),value,hr)); Wait(fence.Get(),value);
        void* p=nullptr; Hr(colour.readback->Map(0,nullptr,&p)); std::vector<unsigned char> bytes(rowBytes*height);
        for(UINT y=0;y<height;++y) memcpy(bytes.data()+y*rowBytes,static_cast<char*>(p)+y*colour.footprint.Footprint.RowPitch,rowBytes);
        colour.readback->Unmap(0,nullptr); return bytes;
    };
    auto release=[&](Recording& r) {Rc(api.InvalidateRecording(context,r.job.handle));Rc(api.CollectRecording(context,r.job.handle));};
    std::ofstream output(argv[3],std::ios::binary|std::ios::trunc); Require(bool(output),"output file");
    std::unique_ptr<Recording> retained; std::vector<unsigned char> first;
    uint32_t lastConsumed=0;
    for(UINT n=0;n<=frames+3;++n) {
        if(n==3) Rc(api.ResetHistory(context));
        if(n==frames+3) {controls.apply_model=0; Rc(set(context,&controls));}
        if(moving) update(n);
        auto r=std::make_unique<Recording>(device.Get());
        MochizukiNrFrameInfo frame{}; frame.struct_size=sizeof frame; frame.frame_id=n+1;
        frame.color=colour.color.Get(); frame.color_width=width; frame.color_height=height;
        frame.motion=motion.color.Get(); frame.motion_width=width; frame.motion_height=height;
        frame.color_state=frame.motion_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.motion_scale_x=frame.motion_scale_y=1; frame.model_scale=scale; frame.passes=passes;
        frame.transfer_strength=frame.color_strength=1;
        frame.flags=LMXXF_NR_FRAME_FLAG_STRENGTH|MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;
        if(firstPass) frame.flags|=LMXXF_NR_FRAME_FLAG_FIRST_PASS;
        const auto deadline=GetTickCount64()+300000;
        for(;;) {
            const int rc=prepare(context,&frame,&r->job);
            if(!rc) {
                MochizukiNrInfo ready{sizeof ready}; Rc(info(context,&ready));
                if(!ready.building && ready.aco_effective) break;
                release(*r);
                if(!ready.building && !ready.aco_effective) throw std::runtime_error(std::string("ACO import required: ")+ready.aco_fallback);
            } else if(rc!=LMXXF_NR_UNAVAILABLE) Rc(rc);
            Require(GetTickCount64()<deadline,"network build timeout"); Sleep(10);
        }
        Rc(api.RecordInputs(context,r->job.handle,r->in.cmd.Get())); Rc(api.RecordOutputs(context,r->job.handle,r->out.cmd.Get()));
        auto* image=static_cast<ID3D12Resource*>(firstPass && controls.apply_model ? r->job.first_pass_output : r->job.private_output);
        Require(image!=nullptr,"requested output image missing"); colour.Read(r->out.cmd.Get(),image);
        Hr(r->in.cmd->Close()); Hr(r->out.cmd->Close()); const auto bytes=execute(*r);
        MochizukiNrInfo sample{sizeof sample}; Rc(info(context,&sample));
        const uint32_t consumed=n>=3?lastConsumed+(n>3?1u:0u):uint32_t(std::lround(sample.history_consumed_pct*(n+1)/100.0));
        if(n!=frames+3 || !firstPass) {
            if(n>=3 && n<64) Require(sample.history_consumed_pct==consumed*100/(n+1),"unexpected temporal reset");
            if(n>=67) Require(sample.history_consumed_pct==100,"continuous history reset after 64-frame window");
        } // Turning apply_model off disables first-pass export and intentionally rebuilds that layout.
        lastConsumed=std::min(consumed,64u);
        if(n==frames+3) Require(bytes==original,"Apply model off changed input");
        else if(n>=3) {
            Require(bytes!=original,"NR reverted to passthrough");
            // All components must be finite; equality of NaN payloads is not acceptance.
            if(fp16) for(size_t p=0;p<bytes.size();p+=2) {
                uint16_t half; memcpy(&half,bytes.data()+p,2);
                Require(std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(half)),"non-finite temporal output");
            }
            output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size())); Require(bool(output),"write temporal output");
            if(n==3) {first=bytes;retained=std::move(r);}
        }
        if(r) release(*r);
    }
    Rc(api.ResetHistory(context)); if(moving) update(3);
    Require(retained && execute(*retained,1)==first,"retained recording changed on replay"); release(*retained); retained.reset();
    output.close(); Rc(api.Destroy(context)); FreeLibrary(dll);
    printf("ACO_TEMPORAL_CASE_OK %ux%u format=%s passes=%u frames=%u moving=%u scale=%.2f capacity=%u replay=passed\n",
           width,height,fp16?"fp16":"srgb",passes,frames,unsigned(moving),scale,capacity);
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}

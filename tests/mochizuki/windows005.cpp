// Windows 0.0.5 modes, real D3D12/Vulkan frames and retained host recordings.
#include "runtime_test_helpers.h"

int wmain(int argc, wchar_t** argv) try {
    Require(argc == 3 || (argc == 4 && !wcscmp(argv[3], L"--no-aco")), "runtime DLL, asset directory and optional --no-aco required");
    const bool aco = argc == 3;
    HMODULE dll = LoadLibraryW(argv[1]); Require(dll != nullptr, "load runtime");
    auto get = reinterpret_cast<PFN_MochizukiNrGetApi>(GetProcAddress(dll, "MochizukiNrGetApi"));
    auto defaults = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll, "MochizukiNrGetControlDefaults"));
    auto set = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll, "MochizukiNrSetControls"));
    auto prepare = reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(dll, "MochizukiNrPrepareFrame"));
    auto getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll, "MochizukiNrGetInfo"));
    Require(get && defaults && set && prepare && getInfo, "required exports");
    api.struct_size = sizeof api; Rc(get(LMXXF_NR_ABI_VERSION, &api));
    reinterpret_cast<void(*)(uint32_t)>(GetProcAddress(dll, "MochizukiNrSetLogging"))(1);
    ComPtr<IDXGIFactory6> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> device;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc {}; adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
    }
    Require(device != nullptr, "AMD GPU required");
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd {}; Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence; Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))); uint64_t serial = 0;
    struct Held { LmxxfNrJob job {sizeof job}; List in, out; Frame* pixels; Held(ID3D12Device* d, Frame* f) : in(d), out(d), pixels(f) {} };
    std::vector<unsigned char> recreatedReference;
    for (auto format : {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        Frame frame(device.Get(), 256, 256, format), motion(device.Get(), 256, 256);
        Frame resized(device.Get(), 384, 384, format);
        void* zero {}; Hr(motion.upload->Map(0, nullptr, &zero));
        memset(zero, 0, size_t(motion.footprint.Footprint.RowPitch) * motion.height); motion.upload->Unmap(0, nullptr);
        List upload(device.Get()); frame.Upload(upload.cmd.Get()); resized.Upload(upload.cmd.Get()); motion.Upload(upload.cmd.Get()); Hr(upload.cmd->Close());
        Submit(queue.Get(), upload); Hr(queue->Signal(fence.Get(), ++serial)); Wait(fence.Get(), serial);
        void* context {};
        LmxxfNrCreateInfo ci {sizeof ci, device.Get(), queue.Get(), argv[2], LMXXF_NR_CREATE_FLAG_RECORDING_LEASES};
        Rc(api.Create(&ci, &context));
        MochizukiNrControls controls {sizeof controls}; Rc(defaults(&controls));
        controls.aco_mode = aco; Rc(set(context, &controls)); Rc(api.PrepareSession(context));
        std::vector<std::unique_ptr<Held>> held;
        uint64_t frameId = 0;
        auto execute = [&](Held& r) {
            auto& frame = *r.pixels;
            Rc(api.BeginRecordingExecution(context, r.job.handle, queue.Get())); Submit(queue.Get(), r.in);
            Rc(api.EnqueueHip(context, r.job.handle, queue.Get())); Submit(queue.Get(), r.out);
            auto hr = queue->Signal(fence.Get(), ++serial);
            Rc(api.EndRecordingExecution(context, r.job.handle, queue.Get(), 3, fence.Get(), serial, hr)); Wait(fence.Get(), serial);
            void* mapped {}; Hr(frame.readback->Map(0, nullptr, &mapped));
            const size_t row = frame.width * (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4);
            std::vector<unsigned char> bytes(row * frame.height);
            for (UINT y = 0; y < frame.height; ++y) memcpy(bytes.data() + row * y, static_cast<char*>(mapped) + y * frame.footprint.Footprint.RowPitch, row);
            frame.readback->Unmap(0, nullptr);
            if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                auto* half = reinterpret_cast<const uint16_t*>(bytes.data());
                for (size_t i = 0; i < bytes.size() / 2; ++i)
                    Require(std::isfinite(DirectX::PackedVector::XMConvertHalfToFloat(half[i])), "non-finite output");
            }
            return bytes;
        };
        std::vector<unsigned char> first;
        // Standard ACO -> INT4 -> RenoDX native model -> RenoDX INT4, then return.
        unsigned scenario = 0;
        for (unsigned mode : {0u, 1u, 2u, 3u, 0u, 0u, 0u, 2u, 2u}) {
            Frame& pixels = scenario == 5 ? resized : frame;
            std::vector<unsigned char> resetReference;
            double wallMs = 0;
            controls.int4_mixed = mode & 1; controls.renodx_enabled = mode >= 2;
            controls.reno_passes = 3; controls.reno_resolution_scale = .5f;
            controls.reno_look_mode = 1; controls.reno_look_stabilize = scenario >= 7 ? 0 : 2;
            controls.reno_nr_enabled = scenario != 7;
            controls.reno_pass_follow_0 = 0; controls.reno_pass_style_0 = 1;
            Rc(set(context, &controls));
            for (unsigned n = 0; n < 4; ++n) {
                if (n == 1) Rc(api.ResetHistory(context));
                if (scenario == 6 && n == 2) Sleep(300); // resident network after a temporal gap
                auto record = std::make_unique<Held>(device.Get(), &pixels);
                MochizukiNrFrameInfo fi {}; fi.struct_size = sizeof fi;
                fi.frame_id = ++frameId;
                fi.color = pixels.color.Get(); fi.color_width = fi.color_height = pixels.width;
                fi.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                fi.motion = motion.color.Get(); fi.motion_width = fi.motion_height = 256;
                fi.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                fi.motion_scale_x = fi.motion_scale_y = 1.f;
                fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;
                if (mode >= 2) fi.flags |= MOCHIZUKI_NR_FRAME_FLAG_AFTER_UPSCALE;
                fi.reset = held.empty(); // later mode/extent changes must reset themselves
                fi.transfer_strength = fi.color_strength = fi.model_scale = 1; fi.passes = 1;
                const auto deadline = GetTickCount64() + 300000;
                for (;;) {
                    int rc = prepare(context, &fi, &record->job);
                    MochizukiNrInfo info {sizeof info}; Rc(getInfo(context, &info));
                    if (!rc) {
                        if (!info.building && info.int4_effective == (aco && controls.int4_mixed) && info.renodx_effective == controls.renodx_enabled) break;
                        Rc(api.InvalidateRecording(context, record->job.handle)); Rc(api.CollectRecording(context, record->job.handle));
                        record->job = {sizeof record->job};
                    } else if (rc != LMXXF_NR_UNAVAILABLE) Rc(rc);
                    if (GetTickCount64() >= deadline) { fprintf(stderr, "%s\n", info.last_error); Require(false, "mode did not become effective"); }
                    Sleep(50);
                }
                Rc(api.RecordInputs(context, record->job.handle, record->in.cmd.Get())); Hr(record->in.cmd->Close());
                Rc(api.RecordOutputs(context, record->job.handle, record->out.cmd.Get()));
                pixels.Read(record->out.cmd.Get(), static_cast<ID3D12Resource*>(record->job.private_output)); Hr(record->out.cmd->Close());
                const auto started = std::chrono::steady_clock::now();
                const auto bytes = execute(*record);
                wallMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                if (held.empty()) {
                    first = bytes;
                    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                        if (recreatedReference.empty()) recreatedReference = bytes;
                        else Require(bytes == recreatedReference, "NR off/on session recreation changed initial output");
                    }
                }
                if (n == 0) resetReference = bytes;
                if ((mode < 2 || scenario == 8) && (n == 1 || (scenario == 6 && n == 2)))
                    Require(bytes == resetReference, "mode/extent/idle history reset differs from explicit reset");
                if (!aco && mode == 1 && n == 0)
                    Require(bytes == first, "INT4 request changed native fallback output");
                if (aco && mode == 1 && n == 1) {
                    double absolute = 0, maximum = 0;
                    const size_t count = bytes.size() / (format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2 : 1);
                    for (size_t i = 0; i < count; ++i) {
                        double delta;
                        if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            uint16_t a, b; memcpy(&a, bytes.data() + i * 2, 2); memcpy(&b, first.data() + i * 2, 2);
                            delta = std::abs(double(DirectX::PackedVector::XMConvertHalfToFloat(a)) - DirectX::PackedVector::XMConvertHalfToFloat(b));
                        } else delta = std::abs(int(bytes[i]) - int(first[i])) / 255.;
                        absolute += delta; maximum = std::max(maximum, delta);
                    }
                    printf("INT4_IMAGE_DELTA format=%u mean_abs_rgba=%.8f max_abs=%.8f (vs default; no quality ground truth)\n", unsigned(format), absolute/count, maximum);
                }
                held.push_back(std::move(record));
            }
            MochizukiNrInfo measured {sizeof measured}; Rc(getInfo(context, &measured));
            Require((measured.network_dispatches == 0) == (scenario == 7), "unexpected NR bypass or dispatch");
            printf("WINDOWS005_MODE_OK format=%u mode=%u extent=%u network_ms=%.4f samples=%llu dispatches=%u fixture_wall_mean_ms=%.4f\n",
                   unsigned(format), mode, pixels.width, measured.gpu_ms_network_last,
                   static_cast<unsigned long long>(measured.gpu_samples), measured.network_dispatches, wallMs / 4); fflush(stdout);
            ++scenario;
        }
        Require(execute(*held.front()) == first, "retained original recording changed after mode switches");
        for (auto& r : held) { Rc(api.InvalidateRecording(context, r->job.handle)); Rc(api.CollectRecording(context, r->job.handle)); }
        Rc(api.Destroy(context));
    }
    FreeLibrary(dll); puts(aco ? "WINDOWS005_GPU_OK" : "WINDOWS005_NATIVE_FALLBACK_OK"); return 0;
} catch (const std::exception& e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; }

// Split frames admitted to the shared colour texture on 2026-10-22 (DRS subrect, bucket, model scale, multipass,
// RenoDX): run one scenario, report the route the runtime chose, the output hash over the subrect and the
// execution time. Run it with and without MZ_TEST_SHARED_EXTENDED=0 and compare: the hashes must match
// (same pixels from either carrier) and the route must differ where the admission was widened.
#include "runtime_test_helpers.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
std::string LogSuffix(const std::filesystem::path& path, uintmax_t offset)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    if (uintmax_t(file.tellg()) < offset) offset = 0;
    file.seekg(std::streamoff(offset));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string LastRoute(const std::string& log, const char* key)
{
    const auto at = log.rfind(key);
    if (at == std::string::npos) return "none";
    const auto start = at + strlen(key);
    return log.substr(start, log.find(" generation", start) - start);
}

void Shift(Frame& frame)
{
    void* data = nullptr;
    Hr(frame.upload->Map(0, nullptr, &data));
    for (unsigned y = 0; y < frame.height; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(static_cast<char*>(data) + y * frame.footprint.Footprint.RowPitch);
        for (unsigned x = 0; x < frame.width; ++x) {
            row[4 * x + 0] = DirectX::PackedVector::XMConvertFloatToHalf(.2f + .4f * y / 256);
            row[4 * x + 1] = DirectX::PackedVector::XMConvertFloatToHalf(.1f + .3f * x / 256);
        }
    }
    frame.upload->Unmap(0, nullptr);
}
}

int wmain(int argc, wchar_t** argv) try
{
    Require(argc >= 4, "runtime assets full|exact-subrect|bucket|scale|passes2|passes3|reno [debug]");
    const std::wstring scenario = argv[3];
    const bool debugLayer = argc > 4 && std::wstring(argv[4]) == L"debug";
    const bool subrect = scenario == L"exact-subrect" || scenario == L"bucket";
    const UINT W = 256, H = 256, w = subrect ? 200 : W, h = subrect ? 180 : H;
    const float scale = scenario == L"scale" ? .5f : 1.f;
    const uint32_t passes = scenario == L"passes2" ? 2 : scenario == L"passes3" ? 3 : 1;
    const bool reno = scenario == L"reno";
    Require(scenario == L"full" || subrect || scale != 1.f || passes != 1 || reno, "unknown scenario");

    ComPtr<ID3D12Debug> debug;
    if (debugLayer) { Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer(); }
    ComPtr<IDXGIFactory4> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device;
    for (UINT n = 0;; ++n) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(n, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
    }
    Require(device != nullptr, "AMD device");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd {};
    Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> tail;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tail)));
    uint64_t value = 0;
    Frame colours[2] = {{device.Get(), W, H}, {device.Get(), W, H}};
    Frame motion[2] = {{device.Get(), W, H, DXGI_FORMAT_R16G16_FLOAT}, {device.Get(), W, H, DXGI_FORMAT_R16G16_FLOAT}};
    Frame result {device.Get(), W, H};
    Shift(colours[1]);
    {
        List upload(device.Get());
        for (auto& f : colours) f.Upload(upload.cmd.Get());
        for (auto& f : motion) f.Upload(upload.cmd.Get());
        Hr(upload.cmd->Close());
        Submit(queue.Get(), upload);
        Hr(queue->Signal(tail.Get(), ++value));
        Wait(tail.Get(), value);
    }

    HMODULE dll = LoadLibraryW(argv[1]);
    Require(dll, "runtime load");
    auto get = reinterpret_cast<PFN_MochizukiNrGetApi>(GetProcAddress(dll, "MochizukiNrGetApi"));
    auto defaults = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(GetProcAddress(dll, "MochizukiNrGetControlDefaults"));
    auto set = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(dll, "MochizukiNrSetControls"));
    auto prepare = reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(dll, "MochizukiNrPrepareFrame"));
    auto info = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(dll, "MochizukiNrGetInfo"));
    auto logging = reinterpret_cast<void(*)(uint32_t)>(GetProcAddress(dll, "MochizukiNrSetLogging"));
    Require(get && defaults && set && prepare && info && logging, "runtime exports");
    api.struct_size = sizeof api;
    Rc(get(LMXXF_NR_ABI_VERSION, &api));
    const auto logPath = std::filesystem::absolute(argv[1]).parent_path() / "mochizuki_nr.log";
    std::error_code logError;
    const auto initialSize = std::filesystem::file_size(logPath, logError);
    const uintmax_t logStart = logError ? 0 : initialSize;
    logging(1);
    const auto root = std::filesystem::absolute(argv[2]).wstring();
    void* context = nullptr;
    LmxxfNrCreateInfo ci {sizeof ci, device.Get(), queue.Get(), root.c_str(), LMXXF_NR_CREATE_FLAG_RECORDING_LEASES};
    Rc(api.Create(&ci, &context));
    MochizukiNrControls controls {sizeof controls};
    Rc(defaults(&controls));
    controls.aco_mode = 0;
    controls.network_model = 1;
    controls.linear_input = 1;
    controls.drs_mode = scenario == L"bucket" ? 1 : 0;
    if (reno) {
        controls.renodx_enabled = 1;
        controls.reno_unlock_passes = 1;
        controls.reno_passes = 1;
        controls.reno_nr_enabled = 1;
    }
    Rc(set(context, &controls));
    Rc(api.PrepareSession(context));

    struct Held {
        LmxxfNrJob job {sizeof job};
        List in, out;
        explicit Held(ID3D12Device* d) : in(d), out(d) {}
    };
    uint64_t frameId = 0;
    auto record = [&](unsigned k) {
        auto held = std::make_unique<Held>(device.Get());
        MochizukiNrFrameInfo frame {sizeof frame};
        frame.frame_id = ++frameId;
        frame.color = colours[k].color.Get();
        frame.color_width = w;
        frame.color_height = h;
        frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.transfer_strength = frame.color_strength = 1;
        frame.model_scale = scale;
        frame.passes = passes;
        frame.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL | MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING;
        frame.motion = motion[k].color.Get();
        frame.motion_width = w;
        frame.motion_height = h;
        frame.motion_scale_x = frame.motion_scale_y = 1;
        frame.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto deadline = GetTickCount64() + 300000;
        for (;;) {
            const auto rc = prepare(context, &frame, &held->job);
            if (!rc) break;
            if (rc != LMXXF_NR_UNAVAILABLE || GetTickCount64() > deadline) Rc(rc);
            Sleep(10);
        }
        Rc(api.RecordInputs(context, held->job.handle, held->in.cmd.Get()));
        Rc(api.RecordOutputs(context, held->job.handle, held->out.cmd.Get()));
        auto* output = static_cast<ID3D12Resource*>(held->job.private_output);
        const auto od = output->GetDesc();
        Require(od.Width == W && od.Height == H && od.Format == result.format, "private output is the allocation");
        result.Read(held->out.cmd.Get(), output);
        Hr(held->in.cmd->Close());
        Hr(held->out.cmd->Close());
        return held;
    };
    auto execute = [&](Held& held) {
        const auto start = std::chrono::steady_clock::now();
        Rc(api.BeginRecordingExecution(context, held.job.handle, queue.Get()));
        Submit(queue.Get(), held.in);
        Rc(api.EnqueueHip(context, held.job.handle, queue.Get()));
        Submit(queue.Get(), held.out);
        Hr(queue->Signal(tail.Get(), ++value));
        Rc(api.EndRecordingExecution(context, held.job.handle, queue.Get(), 3, tail.Get(), value, S_OK));
        Wait(tail.Get(), value);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        Rc(api.InvalidateRecording(context, held.job.handle));
        Rc(api.CollectRecording(context, held.job.handle));
        MochizukiNrInfo actual {sizeof actual};
        Rc(info(context, &actual));
        Require(!actual.failed, "NR execution failed");
        return ms;
    };
    auto hash = [&] {
        void* data = nullptr;
        Hr(result.readback->Map(0, nullptr, &data));
        uint64_t v = 1469598103934665603ull;
        for (unsigned y = 0; y < h; ++y) {
            const auto* row = static_cast<uint8_t*>(data) + y * result.footprint.Footprint.RowPitch;
            for (unsigned x = 0; x < w * 8; ++x) { v ^= row[x]; v *= 1099511628211ull; }
        }
        result.readback->Unmap(0, nullptr);
        return v;
    };
    // Warm up until the network for this scenario is built and served 16 frames in a row.
    for (unsigned n = 0, steady = 0; steady < 16; ++n) {
        Require(n < 2000, "network never settled");
        auto held = record(n % 2);
        execute(*held);
        MochizukiNrInfo t {sizeof t};
        Rc(info(context, &t));
        steady = t.building || (reno && !t.renodx_effective) ? 0 : steady + 1;
    }
    auto reference = [&] {
        Rc(api.ResetHistory(context));
        auto a = record(0);
        execute(*a);
        const auto first = hash();
        auto b = record(1);
        execute(*b);
        return std::pair(first, hash());
    };
    const auto ref = reference();
    std::vector<double> wall;
    for (unsigned n = 0; n < 200; ++n) {
        auto held = record(n % 2);
        wall.push_back(execute(*held));
    }
    MochizukiNrInfo t {sizeof t};
    Rc(info(context, &t));
    const auto again = reference();
    // RenoDX keeps state that ResetHistory does not clear, on either carrier; its hashes are reported, not required.
    const bool repro = again == ref;
    Require(repro || reno, "reference frames not reproducible in one process");
    const auto log = LogSuffix(logPath, logStart);
    std::sort(wall.begin(), wall.end());
    const char* extended = std::getenv("MZ_TEST_SHARED_EXTENDED");
    printf("SHARED_EXTENDED scenario=%ls extended_env=%s subrect=%ux%u scale=%.2f passes=%u reno=%u colour=%s motion=%s "
           "hash0=%016llx hash1=%016llx wall_median_ms=%.3f wall_p90_ms=%.3f core_gpu_median_ms=%.3f max_passes=%u "
           "renodx=%u dispatches=%u debug=%u repro=%u\n",
           scenario.c_str(), extended ? extended : "unset", w, h, scale, passes, unsigned(reno),
           LastRoute(log, "split colour input: ").c_str(), LastRoute(log, "split motion input: ").c_str(),
           ref.first, ref.second, wall[wall.size() / 2], wall[wall.size() * 9 / 10], t.gpu_ms_median, t.max_passes,
           t.renodx_effective, t.network_dispatches, unsigned(debugLayer), unsigned(repro));
    Rc(api.Destroy(context));
    FreeLibrary(dll);
    if (debugLayer) {
        ComPtr<ID3D12InfoQueue> messages;
        Hr(device.As(&messages));
        unsigned errors = 0;
        for (UINT64 n = 0; n < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++n) {
            SIZE_T bytes = 0;
            messages->GetMessage(n, nullptr, &bytes);
            std::vector<char> data(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            Hr(messages->GetMessage(n, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { ++errors; printf("D3D_ERROR %s\n", message->pDescription); }
        }
        printf("D3D_DEBUG_ERRORS %u\n", errors);
        Require(!errors, "D3D debug errors");
    }
    return 0;
}
catch (const std::exception& error) {
    fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}

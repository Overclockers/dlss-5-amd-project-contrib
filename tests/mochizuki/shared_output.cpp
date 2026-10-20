#include "runtime_test_helpers.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <d3d12sdklayers.h>

namespace {
std::string LogSuffix(const std::filesystem::path& path, uintmax_t offset)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    if (uintmax_t(file.tellg()) < offset) offset = 0; // runtime log rotated
    file.seekg(std::streamoff(offset));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void FillAlternate(Frame& frame, bool u8)
{
    void* data = nullptr;
    Hr(frame.upload->Map(0, nullptr, &data));
    for (unsigned y = 0; y < frame.height; ++y) {
        auto* row = static_cast<char*>(data) + y * frame.footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < frame.width; ++x) {
            if (u8) {
                auto* p = reinterpret_cast<uint8_t*>(row) + 4 * x;
                p[0] = uint8_t(40 + y / 2);
                p[1] = uint8_t(30 + x / 2);
                p[2] = 64;
                p[3] = 255;
            } else {
                auto* p = reinterpret_cast<uint16_t*>(row) + 4 * x;
                p[0] = DirectX::PackedVector::XMConvertFloatToHalf(.2f + .4f * y / 256);
                p[1] = DirectX::PackedVector::XMConvertFloatToHalf(.1f + .3f * x / 256);
                p[2] = 0x3400;
                p[3] = 0x3c00;
            }
        }
    }
    frame.upload->Unmap(0, nullptr);
}

void FillMotion(Frame& frame, unsigned alternate)
{
    void* data = nullptr;
    Hr(frame.upload->Map(0, nullptr, &data));
    for (unsigned y = 0; y < frame.height; ++y) {
        auto* row = static_cast<char*>(data) + y * frame.footprint.Footprint.RowPitch;
        for (unsigned x = 0; x < frame.width; ++x) {
            auto* p = reinterpret_cast<uint16_t*>(row) + 2 * x;
            p[0] = DirectX::PackedVector::XMConvertFloatToHalf(alternate ? -.75f : .5f);
            p[1] = DirectX::PackedVector::XMConvertFloatToHalf(alternate ? .25f : -.5f);
        }
    }
    frame.upload->Unmap(0, nullptr);
}
}

int wmain(int argc, wchar_t** argv) try
{
    Require(argc >= 3, "runtime assets [rgba8] [original] [aco]");
    const bool u8 = argc > 3 && std::wstring(argv[3]) == L"rgba8";
    const uint32_t model = argc > 4 && std::wstring(argv[4]) == L"original" ? 0 : 1;
    const bool aco = argc > 5 && std::wstring(argv[5]) == L"aco";
    ComPtr<ID3D12Debug> debug;
    Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device;
    for (UINT n = 0;; ++n) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(n, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && SUCCEEDED(D3D12CreateDevice(
                adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) break;
    }
    Require(device != nullptr, "AMD device");
    ComPtr<ID3D12InfoQueue> messages;
    Hr(device.As(&messages));
    ComPtr<ID3D12CommandQueue> queues[2];
    D3D12_COMMAND_QUEUE_DESC qd {};
    for (auto& queue : queues) Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> tail;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tail)));
    uint64_t value = 0;
    const auto format = u8 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
    Frame colours[2] = {{device.Get(), 256, 256, format}, {device.Get(), 256, 256, format}};
    Frame motion[2] = {{device.Get(), 256, 256, DXGI_FORMAT_R16G16_FLOAT},
                       {device.Get(), 256, 256, DXGI_FORMAT_R16G16_FLOAT}};
    FillAlternate(colours[1], u8);
    for (unsigned k = 0; k < 2; ++k) FillMotion(motion[k], k);
    List upload(device.Get());
    for (auto& frame : colours) frame.Upload(upload.cmd.Get());
    for (auto& frame : motion) frame.Upload(upload.cmd.Get());
    Hr(upload.cmd->Close());
    Submit(queues[0].Get(), upload);
    Hr(queues[0]->Signal(tail.Get(), ++value));
    Wait(tail.Get(), value);

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
    // Use existing production diagnostics, not a new ABI/test route hook. The
    // test uses a DLL under exports and observes only lines appended by this run.
    const auto logPath = std::filesystem::absolute(argv[1]).parent_path() / "mochizuki_nr.log";
    std::error_code logError;
    const auto initialSize = std::filesystem::file_size(logPath, logError);
    const uintmax_t logStart = logError ? 0 : initialSize;
    logging(1);
    const auto root = std::filesystem::absolute(argv[2]).wstring();
    void* context = nullptr;
    LmxxfNrCreateInfo ci {sizeof ci, device.Get(), queues[0].Get(), root.c_str(),
                         LMXXF_NR_CREATE_FLAG_RECORDING_LEASES};
    Rc(api.Create(&ci, &context));
    MochizukiNrControls controls {sizeof controls};
    Rc(defaults(&controls));
    controls.aco_mode = aco;
    controls.network_model = model;
    controls.linear_input = u8 ? 2 : 1;
    Rc(set(context, &controls));
    Rc(api.PrepareSession(context));
    uint64_t frameId = 0, executions = 0;
    struct Held {
        LmxxfNrJob job {sizeof job};
        List in, out;
        explicit Held(ID3D12Device* d) : in(d), out(d) {}
    };
    auto record = [&](unsigned k, bool temporal) {
        auto held = std::make_unique<Held>(device.Get());
        MochizukiNrFrameInfo frame {sizeof frame};
        frame.frame_id = ++frameId;
        frame.color = colours[k].color.Get();
        frame.color_width = frame.color_height = 256;
        frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.transfer_strength = frame.color_strength = frame.model_scale = 1;
        frame.passes = 1;
        frame.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
        if (temporal) {
            frame.motion = motion[k].color.Get();
            frame.motion_width = frame.motion_height = 256;
            frame.motion_scale_x = frame.motion_scale_y = 1;
            frame.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            frame.flags |= MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;
        }
        const auto deadline = GetTickCount64() + 300000;
        for (;;) {
            const auto rc = prepare(context, &frame, &held->job);
            if (!rc) break;
            if (rc != LMXXF_NR_UNAVAILABLE || GetTickCount64() > deadline) Rc(rc);
            Sleep(10);
        }
        Rc(api.RecordInputs(context, held->job.handle, held->in.cmd.Get()));
        Rc(api.RecordOutputs(context, held->job.handle, held->out.cmd.Get()));
        colours[0].Read(held->out.cmd.Get(), static_cast<ID3D12Resource*>(held->job.private_output));
        Hr(held->in.cmd->Close());
        Hr(held->out.cmd->Close());
        return held;
    };
    auto hash = [&] {
        void* data = nullptr;
        Hr(colours[0].readback->Map(0, nullptr, &data));
        uint64_t result = 1469598103934665603ull;
        for (unsigned y = 0; y < 256; ++y) {
            const auto* row = static_cast<uint8_t*>(data) + y * colours[0].footprint.Footprint.RowPitch;
            for (unsigned x = 0; x < 256 * (u8 ? 4 : 8); ++x) {
                result ^= row[x];
                result *= 1099511628211ull;
            }
        }
        colours[0].readback->Unmap(0, nullptr);
        return result;
    };
    auto execute = [&](Held& held, unsigned q) {
        Rc(api.BeginRecordingExecution(context, held.job.handle, queues[q].Get()));
        Submit(queues[q].Get(), held.in);
        Rc(api.EnqueueHip(context, held.job.handle, queues[q].Get()));
        Submit(queues[q].Get(), held.out);
        Hr(queues[q]->Signal(tail.Get(), ++value));
        Rc(api.EndRecordingExecution(context, held.job.handle, queues[q].Get(), 3, tail.Get(), value, S_OK));
        Wait(tail.Get(), value);
        MochizukiNrInfo actual {sizeof actual};
        Rc(info(context, &actual));
        Require(!actual.failed && actual.network_dispatches == (model ? 71u : 123u), "complete NR execution");
        ++executions;
        return hash();
    };
    auto collect = [&](Held& held) {
        Rc(api.InvalidateRecording(context, held.job.handle));
        Rc(api.CollectRecording(context, held.job.handle));
    };
    bool sharedObserved = false;
    for (unsigned n = 0; n < 128; ++n) {
        auto held = record(n % 2, true);
        execute(*held, n % 2);
        collect(*held);
        const auto lines = LogSuffix(logPath, logStart);
        sharedObserved = lines.find("split colour input: shared texture") != std::string::npos;
        if (n >= 15 && sharedObserved) break;
        Sleep(1);
    }
    if (!sharedObserved) {
        Rc(api.Destroy(context));
        FreeLibrary(dll);
        puts("SHARED_OUTPUT_NOT_EXERCISED shared_colour=0; buffer fallback is not a shared-output pass");
        return 2;
    }
    const auto routeLines = LogSuffix(logPath, logStart);
    if (!u8 && std::getenv("MZ_TEST_REJECT_SHARED_STORAGE")) {
        Require(routeLines.find("storage preference unavailable") != std::string::npos,
                "optional storage refusal not observed");
        Require(routeLines.find("usage=0x7") != std::string::npos,
                "sampled shared fallback not observed");
    }
    const bool storageObserved = routeLines.find("usage=0xf") != std::string::npos;
    printf("SHARED_OUTPUT_ROUTE shared_colour=1 storage=%u\n", unsigned(storageObserved));
    if (!u8 && !storageObserved)
        puts("SHARED_STORAGE_NOT_EXERCISED sampled fallback; no storage-preference coverage claim");
    auto first = record(0, true);
    auto second = record(1, true);
    Rc(api.ResetHistory(context));
    const auto a = execute(*first, 0);
    Rc(api.ResetHistory(context));
    const auto b = execute(*second, 1);
    Require(a != b, "changing picture affects output");
    Rc(api.ResetHistory(context));
    Require(execute(*first, 1) == a, "retained shared output cross-queue replay");
    auto buffer = record(1, false);
    execute(*buffer, 0);
    Rc(api.ResetHistory(context));
    Require(execute(*second, 0) == b, "closed shared output survives buffer route switch");
    auto discarded = record(0, true);
    collect(*discarded);
    discarded.reset();
    auto blocked = record(0, true);
    ComPtr<ID3D12Fence> gate;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    Hr(queues[1]->Wait(gate.Get(), 1));
    Rc(api.BeginRecordingExecution(context, blocked->job.handle, queues[1].Get()));
    Submit(queues[1].Get(), blocked->in);
    Rc(api.EnqueueHip(context, blocked->job.handle, queues[1].Get()));
    Submit(queues[1].Get(), blocked->out);
    Hr(queues[1]->Signal(tail.Get(), ++value));
    Rc(api.EndRecordingExecution(context, blocked->job.handle, queues[1].Get(), 3, tail.Get(), value, S_OK));
    Rc(api.InvalidateRecording(context, blocked->job.handle));
    Require(api.CollectRecording(context, blocked->job.handle) == LMXXF_NR_UNAVAILABLE, "pending consumer collected");
    Hr(gate->Signal(1));
    Wait(tail.Get(), value);
    Rc(api.CollectRecording(context, blocked->job.handle));
    ++executions;
    collect(*first);
    collect(*second);
    collect(*buffer);
    first.reset();
    second.reset();
    buffer.reset();
    blocked.reset();
    MochizukiNrInfo actual {sizeof actual};
    Rc(info(context, &actual));
    Require(actual.frames == executions && actual.aco_effective == uint32_t(aco) &&
            actual.network_effective == model, "actual frame/model/compiler count");
    Rc(api.Destroy(context));
    FreeLibrary(dll);
    unsigned errors = 0;
    for (UINT64 n = 0; n < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++n) {
        SIZE_T bytes = 0;
        messages->GetMessage(n, nullptr, &bytes);
        std::vector<char> data(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
        Hr(messages->GetMessage(n, message, &bytes));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++errors;
            printf("D3D_ERROR %s\n", message->pDescription);
        }
    }
    Require(!errors, "D3D debug errors");
    printf("SHARED_OUTPUT_OK frames=%llu dispatches=%llu format=%u model=%u aco=%u "
           "hashA=%016llx hashB=%016llx shared_colour=1 replay=1 cross_queue=1 "
           "route_change=1 pending_tail=1 discarded=1 debug_errors=0\n",
           executions, executions * (model ? 71 : 123), unsigned(format), model, aco, a, b);
    return 0;
}
catch (const std::exception& error) {
    fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}

#pragma once
// Metered auto exposure for the Mochizuki lane.
//
// WHY THIS EXISTS
// The host never calls MochizukiNrSetExposure and Monster Hunter Wilds publishes no exposure texture, so the
// adapter normalises the HDR frame with a white point of 1.0 and highlights blow out. The lmxxf lane measures the
// frame itself for exactly this case (lmxxf_runtime/LmxxfNrRuntime.cpp:1020-1028, "Auto exposure for games that
// give no usable exposure texture ... Without it the codec normalises HDR scene values by an exposure of 1 and
// highlights blow out") and the host supports an encoded-mean fallback target. This
// is the same measurement for this lane: one 16x16 thread group samples a 16x16 grid of the game's colour buffer,
// reduces it to the mean linear luminance and turns that into
//     exposure = 0.2073 / mean          // pow(0.45, 2.2) / (1 - pow(0.45, 2.2)): encoded mean 0.45
// smoothed in the log domain with a 0.25 smoothing factor.
//
// The value feeds the existing "white = controls.white / gameExposure" path in MochizukiNrRuntime.cpp, so no ABI
// field and no third_party change is needed: MochizukiWhitePoint stays the paper-white trim. The meter only runs
// when the host turns it on (MochizukiNrSetAutoExposure, INI MochizukiWhitePointSource 2/3, default off), so a game
// that already looks right keeps exactly the level it had.
//
// Robustness (integration of PR #9): 64x64 samples (4x4 per thread), a two-stage reduction that first takes the
// log-average G and then the arithmetic mean of min(luma, 16 G) - the arithmetic mean the other lanes are calibrated
// on, with specular highlights, the sky and UI sparks clipped so one bright object cannot swing the level - a
// 1/16 EV deadband against shimmer on a static frame, and the exposure clamped to +-6 EV.
//
// Probe also has a texel mode: it copies a game's 1x1 exposure texture to a readback through the same fence-gated
// slots, so a game exposure reaches the CPU-side white point one frame late without any wait.
//
// D3DCompile is loaded from System32 the way amd/AmdPreSr.cpp:953 loads it: the game may already have an older
// d3dcompiler next to it and we must not import an entry point that resolves to that one.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <memory>

#include <d3d12.h>
#include <windows.h>

namespace MochizukiAutoExposure
{
    inline constexpr UINT kGrid = 64;              // samples per axis (16 threads x 4 samples each)
    inline constexpr float kEncodedKey = 0.45f;    // encoded mean the reference runtimes target
    inline constexpr float kKeyExposure = 0.2073f; // pow(kEncodedKey, 2.2) / (1 - pow(kEncodedKey, 2.2))
    inline constexpr float kSmoothing = 0.25f;     // log-domain blend per measurement
    inline constexpr float kMinMean = 1e-4f;
    inline constexpr float kMinExposure = 1.0f / 64.0f; // -6 EV
    inline constexpr float kMaxExposure = 64.0f;        // +6 EV
    inline constexpr float kDeadbandEv = 1.0f / 16.0f;  // smaller changes are shimmer, not a new scene level

    // Mean linear luminance of the sampled region -> exposure that puts the encoded mean on the key.
    inline float Target(float mean)
    {
        const float usable = std::isfinite(mean) && mean > kMinMean ? mean : kMinMean;
        const float target = kKeyExposure / usable;
        if (!std::isfinite(target)) return 1.0f;
        return target < kMinExposure ? kMinExposure : (target > kMaxExposure ? kMaxExposure : target);
    }

    // First measurement is used as-is, later ones are blended in the log domain (no jumps on a bright object).
    inline float Blend(float previous, float target, bool warm)
    {
        if (!warm || !(previous > 0.0f) || !std::isfinite(previous)) return target;
        // Deadband: a static frame whose samples shimmer must not make the level breathe.
        if (std::fabs(std::log2(target / previous)) < kDeadbandEv) return previous;
        return std::exp(previous == target ? std::log(previous)
                                           : std::log(previous) + (std::log(target) - std::log(previous)) * kSmoothing);
    }

    // One 16x16 group, 4x4 samples per thread (a 64x64 grid), groupshared tree reductions into a 1-float buffer.
    // Stage 1 is the log-average G of the usable samples; stage 2 the arithmetic mean of min(luma, 16 G), which is
    // what Target is keyed on. Region is the measured rectangle inside the colour image (Origin, Extent >= 1).
    inline constexpr char kMeterShader[] = R"(
Texture2D<float4> Colour : register(t0);
RWByteAddressBuffer Mean : register(u0);
cbuffer Region : register(b0)
{
    uint2 Origin;
    uint2 Extent;
};
groupshared float Partial[256];
groupshared uint Samples[256];

void Reduce(uint slot)
{
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 128u; step > 0u; step >>= 1u)
    {
        if (slot < step)
        {
            Partial[slot] += Partial[slot + step];
            Samples[slot] += Samples[slot + step];
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(16, 16, 1)]
void main(uint3 thread : SV_GroupThreadID)
{
    const uint slot = thread.y * 16u + thread.x;
    float luma[16];
    float logSum = 0.0f;
    uint usable = 0u;
    [unroll] for (uint i = 0u; i < 16u; ++i)
    {
        const uint2 cell = thread.xy * 4u + uint2(i & 3u, i >> 2u);
        const uint2 offset = min(uint2((float2(cell) + 0.5f) * float2(Extent) / 64.0f), Extent - 1u);
        const float3 colour = max(Colour.Load(int3(int2(Origin + offset), 0)).rgb, 0.0f);
        const float l = dot(colour, float3(0.2126f, 0.7152f, 0.0722f));
        // A non-finite sample must not poison the level: drop it and divide by what was usable.
        const bool ok = (asuint(l) & 0x7f800000u) != 0x7f800000u;
        luma[i] = ok ? l : -1.0f;
        logSum += ok ? log(max(l, 1e-5f)) : 0.0f;
        usable += ok ? 1u : 0u;
    }
    Partial[slot] = logSum;
    Samples[slot] = usable;
    Reduce(slot);
    const uint n = Samples[0];
    const float ceiling = n ? 16.0f * exp(Partial[0] / float(n)) : 0.0f;
    GroupMemoryBarrierWithGroupSync();
    float sum = 0.0f;
    [unroll] for (uint k = 0u; k < 16u; ++k)
        sum += luma[k] >= 0.0f ? min(luma[k], ceiling) : 0.0f;
    Partial[slot] = sum;
    Samples[slot] = usable;
    Reduce(slot);
    // Nothing usable in the frame: store 0, which Poll reads as "no measurement" rather than as a black frame.
    if (slot == 0u) Mean.Store(0u, Samples[0] ? asuint(Partial[0] / float(Samples[0])) : 0u);
}
)";

    // D3DCompile from System32; never the copy the game may have loaded.
    inline HMODULE CompilerModule()
    {
        static HMODULE module = [] {
            wchar_t system[MAX_PATH]{};
            const UINT length = GetSystemDirectoryW(system, MAX_PATH);
            if (length == 0U || length >= MAX_PATH) return static_cast<HMODULE>(nullptr);
            const std::wstring path = std::wstring(system, length) + L"\\d3dcompiler_47.dll";
            return LoadLibraryW(path.c_str());
        }();
        return module;
    }

    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                       LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

    inline CompileFn Compiler()
    {
        static CompileFn compile = [] {
            HMODULE module = CompilerModule();
            return module ? reinterpret_cast<CompileFn>(GetProcAddress(module, "D3DCompile")) : nullptr;
        }();
        return compile;
    }

    // Compiles one entry point; returns nullptr and fills `error` when the compiler or the source is unavailable.
    inline ID3DBlob* Compile(const char* source, const char* entry, std::string* error)
    {
        const CompileFn compile = Compiler();
        if (!compile)
        {
            if (error) *error = "System32 D3DCompile unavailable";
            return nullptr;
        }
        ID3DBlob* code = nullptr;
        ID3DBlob* messages = nullptr;
        const HRESULT hr = compile(source, std::strlen(source), "mochizuki exposure meter", nullptr, nullptr, entry,
                                   "cs_5_0", 0, 0, &code, &messages);
        if (FAILED(hr) || !code)
        {
            if (error)
            {
                *error = "D3DCompile failed";
                if (messages && messages->GetBufferPointer() && messages->GetBufferSize())
                    *error += std::string(": ") + static_cast<const char*>(messages->GetBufferPointer());
            }
            if (messages) messages->Release();
            if (code) code->Release();
            return nullptr;
        }
        if (messages) messages->Release();
        return code;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // GPU side. One 16x16 thread group samples a 16x16 grid of the game's colour image (the same region the input copy
    // uses), reduces it to the mean linear luminance and writes that one float into a buffer of our own. The CPU side
    // never waits: a slot's value is read once the D3D12 fence the runtime was already handed for that frame has
    // passed. A retained native list keeps its slot reserved through all replays, until Collect.

    template <class T>
    inline void ReleaseObject(T*& object)
    {
        if (object)
        {
            object->Release();
            object = nullptr;
        }
    }

    inline void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                           D3D12_RESOURCE_STATES to)
    {
        if (!list || !resource || from == to) return;
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = from;
        barrier.Transition.StateAfter = to;
        list->ResourceBarrier(1, &barrier);
    }

    // A format no SRV may be created from. The game hands over R9G9B9E5; a typeless image must skip the meter rather
    // than take the device down with an illegal view.
    inline bool Typeless(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return true;
        default:
            return false;
        }
    }

    class Probe
    {
    public:
        static constexpr UINT kSlots = 4;   // retained measurement recipes; skip when all four are owned
        static constexpr UINT kBytes = 256; // one float, over-allocated so the copy offsets stay aligned
        struct Measurement { UINT slot; };
        using Ticket = std::shared_ptr<Measurement>;

        Probe() = default;
        Probe(const Probe&) = delete;
        Probe& operator=(const Probe&) = delete;
        ~Probe() { Destroy(); }

        // Meter mode needs its pipeline; texel mode (CreateTexel) only the readbacks.
        bool Ready() const { return texel_ ? device_ != nullptr : pso_ != nullptr; }
        bool Texel() const { return texel_; }
        const std::string& Error() const { return error_; }
        float Exposure() const { return exposure_; }
        float Mean() const { return mean_; }
        UINT Measured() const { return measured_; }
        UINT Skipped() const { return skipped_; }

        // Texel mode: no pipeline, only the fence-gated readbacks; RecordTexel copies a game's 1x1 exposure texel.
        bool CreateTexel(ID3D12Device* device)
        {
            Destroy();
            if (!device)
            {
                error_ = "no D3D12 device";
                return false;
            }
            device_ = device;
            device_->AddRef();
            texel_ = true;
            for (Slot& slot : slots_)
            {
                slot.readback = Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
                if (!slot.readback)
                {
                    Describe("exposure texel readback", nullptr);
                    Destroy();
                    return false;
                }
            }
            return true;
        }

        // Whether `texture` is a game exposure this lane can read: the same contract the lmxxf runtime enforces
        // (a 1x1 R16_FLOAT / R32_FLOAT texture; anything else costs the game its exposure, never the frame).
        static bool UsableExposureTexture(ID3D12Resource* texture)
        {
            if (!texture) return false;
            const D3D12_RESOURCE_DESC d = texture->GetDesc();
            return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == 1 && d.Height == 1 &&
                   d.MipLevels == 1 && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 &&
                   (d.Format == DXGI_FORMAT_R16_FLOAT || d.Format == DXGI_FORMAT_R32_FLOAT);
        }

        // Copies the game's exposure texel into a free readback slot; picked up by Poll once the tail has passed.
        bool RecordTexel(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, D3D12_RESOURCE_STATES state,
                         Ticket& ticket, float factor = 1.f)
        {
            if (!texel_ || !Ready() || !list || ticket || !UsableExposureTexture(texture))
            {
                ++skipped_;
                return false;
            }
            const int free = Free();
            if (free < 0)
            {
                ++skipped_;
                return false;
            }
            Slot& slot = slots_[free];
            ticket = std::make_shared<Measurement>(Measurement{UINT(free)});
            slot.owner = ticket;
            slot.factor = factor;
            const D3D12_RESOURCE_DESC d = texture->GetDesc();
            slot.half = d.Format == DXGI_FORMAT_R16_FLOAT;
            D3D12_TEXTURE_COPY_LOCATION src {};
            src.pResource = texture;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst {};
            dst.pResource = slot.readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint = { d.Format, 1, 1, 1, kBytes };
            Transition(list, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            Transition(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
            return true;
        }

        bool Create(ID3D12Device* device)
        {
            Destroy();
            if (!device)
            {
                error_ = "no D3D12 device";
                return false;
            }
            device_ = device;
            device_->AddRef();

            // t0 SRV of whatever the game hands over, b0 the sampled region, u0 the result. The UAV is a root
            // descriptor, so no second descriptor heap entry is needed beside the colour views.
            D3D12_DESCRIPTOR_RANGE range {};
            range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            range.NumDescriptors = 1;

            D3D12_ROOT_PARAMETER parameters[3] {};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[0].DescriptorTable.NumDescriptorRanges = 1;
            parameters[0].DescriptorTable.pDescriptorRanges = &range;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[1].Constants.Num32BitValues = 4;
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[2].Descriptor.RegisterSpace = 0;
            parameters[2].Descriptor.ShaderRegister = 0;
            parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC signature {};
            signature.NumParameters = 3;
            signature.pParameters = parameters;
            signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ID3DBlob* serialized = nullptr;
            ID3DBlob* messages = nullptr;
            HRESULT hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages);
            if (FAILED(hr) || !serialized)
            {
                Describe("root signature", messages);
                ReleaseObject(messages);
                Destroy();
                return false;
            }
            hr = device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&root_));
            ReleaseObject(serialized);
            if (FAILED(hr))
            {
                Describe("CreateRootSignature", nullptr);
                Destroy();
                return false;
            }

            ID3DBlob* code = Compile(kMeterShader, "main", &error_);
            if (!code)
            {
                Destroy();
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
            pipeline.pRootSignature = root_;
            pipeline.CS.pShaderBytecode = code->GetBufferPointer();
            pipeline.CS.BytecodeLength = code->GetBufferSize();
            hr = device_->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pso_));
            ReleaseObject(code);
            if (FAILED(hr))
            {
                Describe("CreateComputePipelineState", nullptr);
                Destroy();
                return false;
            }

            D3D12_DESCRIPTOR_HEAP_DESC descriptors {};
            descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            descriptors.NumDescriptors = kSlots;
            descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device_->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&heap_))))
            {
                Describe("CreateDescriptorHeap", nullptr);
                Destroy();
                return false;
            }
            increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            for (Slot& slot : slots_)
            {
                slot.value = Buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
                slot.readback = Buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
                if (!slot.value || !slot.readback)
                {
                    Describe("meter buffers", nullptr);
                    Destroy();
                    return false;
                }
            }
            return true;
        }

        void Destroy()
        {
            for (Slot& slot : slots_)
            {
                // A slot's buffers are written by a dispatch in a list the meter does not own, so one whose fence has
                // not passed may still be read by the GPU. Fail closed the way the session's own retire path does:
                // retain what may still be in flight instead of handing the memory back to the device. An unsubmitted
                // recipe also keeps its bindings: a different recording's tail says nothing about its future execution.
                const bool inFlight = !slot.owner.expired();
                const uint64_t completed = slot.fence ? slot.fence->GetCompletedValue() : 0;
                if (inFlight || (slot.bound && completed != UINT64_MAX && completed < slot.tailValue))
                {
                    slot.value = nullptr;
                    slot.readback = nullptr;
                }
                else
                {
                    ReleaseObject(slot.value);
                    ReleaseObject(slot.readback);
                }
                ReleaseObject(slot.fence);
                slot.tailValue = 0;
                slot.bound = false;
            }
            // Destroy is normally reached only after all recipes have collected. If the caller violates
            // that contract, retain the bindings too: a retained native list still references them.
            bool retained = false;
            for (const auto& slot : slots_) retained |= !slot.owner.expired();
            if (retained) { pso_ = nullptr; root_ = nullptr; heap_ = nullptr; }
            else { ReleaseObject(pso_); ReleaseObject(root_); ReleaseObject(heap_); }
            ReleaseObject(device_);
            increment_ = 0;
            warm_ = false;
            texel_ = false;
            mean_ = 0.0f;
            exposure_ = 0.0f;
            measured_ = 0;
            skipped_ = 0;
        }

        // Records the measurement of `colour`, which is in `state` right now, into `list`. False when the frame was
        // skipped: no free slot, a frame already recorded, or an image the meter cannot read.
        bool Record(ID3D12GraphicsCommandList* list, ID3D12Resource* colour, D3D12_RESOURCE_STATES state, UINT width,
                    UINT height, Ticket& ticket)
        {
            if (texel_ || !Ready() || !list || !colour || !width || !height || ticket)
            {
                ++skipped_;
                return false;
            }
            const D3D12_RESOURCE_DESC description = colour->GetDesc();
            if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.SampleDesc.Count != 1 ||
                Typeless(description.Format))
            {
                ++skipped_;
                return false;
            }
            const int free = Free();
            if (free < 0)
            {
                ++skipped_;
                return false;
            }
            Slot& slot = slots_[free];
            ticket = std::make_shared<Measurement>(Measurement{UINT(free)});
            slot.owner = ticket;

            D3D12_SHADER_RESOURCE_VIEW_DESC view {};
            view.Format = description.Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(colour, &view, CpuHandle(UINT(free)));

            // The host records this call inside a private binding scope (ILogicalCommandList::BeginPrivateBindings):
            // our heap, root signature and root arguments reach the native list, which the dispatch needs, but they
            // never enter the snapshot the game's continuation is seeded from. On a list that is not the host's proxy
            // they only affect this list.
            Transition(list, colour, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12DescriptorHeap* heaps[] = { heap_ };
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(root_);
            list->SetPipelineState(pso_);
            D3D12_GPU_DESCRIPTOR_HANDLE table = heap_->GetGPUDescriptorHandleForHeapStart();
            table.ptr += SIZE_T(free) * increment_;
            list->SetComputeRootDescriptorTable(0, table);
            const UINT constants[4] = { 0, 0, width, height };
            list->SetComputeRoot32BitConstants(1, 4, constants, 0);
            list->SetComputeRootUnorderedAccessView(2, slot.value->GetGPUVirtualAddress());
            list->Dispatch(1, 1, 1);
            Transition(list, slot.value, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyBufferRegion(slot.readback, 0, slot.value, 0, UINT(sizeof(float)));
            Transition(list, slot.value, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(list, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state);
            return true;
        }

        // Called for the actual recording at Begin/End, including replays. Its descriptor and buffers
        // remain reserved until Collect destroys the ticket, even after its first execution completes.
        void Begin(const Ticket& ticket)
        {
            if (ticket) Unbind(slots_[ticket->slot]);
        }
        void Submitted(const Ticket& ticket, ID3D12Fence* tail, uint64_t value)
        {
            if (!ticket || !tail || !value) return;
            Slot& slot = slots_[ticket->slot];
            Unbind(slot);
            slot.fence = tail; tail->AddRef(); slot.tailValue = value; slot.bound = true;
        }
        // No inferred previousTail: only this measurement's own submitted execution can make it readable.
        bool Poll(float* exposure)
        {
            bool produced = false;
            for (Slot& slot : slots_)
            {
                if (!slot.bound) continue;
                const uint64_t done = slot.fence ? slot.fence->GetCompletedValue() : 0;
                // A removed device reports UINT64_MAX, which is not a value that has been reached: the readback still
                // holds whatever was copied before, so the measurement is dropped rather than read as exposure.
                if (done == UINT64_MAX || (slot.fence && done < slot.tailValue)) continue;
                float mean = 0.0f;
                // A frame with nothing usable stores 0, and a mean below kMinMean is not evidence of a 1e-4-luminance
                // image either: both are no measurement, so the exposure keeps its history instead of blending toward
                // the clamp (a fade to black would otherwise darken the next frames by up to kMaxExposure).
                if (texel_)
                {
                    // The game's own exposure: taken as is (the game already smooths it), only sanity-checked.
                    if (Read(slot, &mean) && std::isfinite(mean *= slot.factor) && mean > 0.0f && mean < 1e6f)
                    {
                        mean_ = exposure_ = mean;
                        ++measured_;
                        produced = true;
                    }
                }
                else if (Read(slot, &mean) && std::isfinite(mean) && mean > kMinMean)
                {
                    mean_ = mean;
                    exposure_ = Blend(exposure_, Target(mean), warm_);
                    warm_ = true;
                    ++measured_;
                    produced = true;
                }
                Unbind(slot);
            }

            if (exposure) *exposure = exposure_;
            return produced;
        }

    private:
        struct Slot
        {
            ID3D12Resource* value {};
            ID3D12Resource* readback {};
            ID3D12Fence* fence {};
            uint64_t tailValue {};
            bool bound {};
            bool half {}; // texel mode: the readback holds an R16_FLOAT
            float factor = 1.f; // scale / pre-exposure belonging to the recorded texel
            std::weak_ptr<Measurement> owner;
        };

        void Describe(const char* what, ID3DBlob* messages)
        {
            error_ = what ? what : "auto exposure";
            if (messages && messages->GetBufferPointer() && messages->GetBufferSize())
                error_ += std::string(": ") + static_cast<const char*>(messages->GetBufferPointer());
        }

        ID3D12Resource* Buffer(D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool unordered)
        {
            D3D12_HEAP_PROPERTIES properties {};
            properties.Type = heap;
            D3D12_RESOURCE_DESC description {};
            description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            description.Width = kBytes;
            description.Height = 1;
            description.DepthOrArraySize = 1;
            description.MipLevels = 1;
            description.SampleDesc.Count = 1;
            description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            description.Flags = unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            ID3D12Resource* resource = nullptr;
            if (FAILED(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
                                                        IID_PPV_ARGS(&resource))))
                return nullptr;
            return resource;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SIZE_T(index) * increment_;
            return handle;
        }

        int Free()
        {
            for (UINT i = 0; i < kSlots; ++i)
                if (slots_[i].owner.expired()) { Unbind(slots_[i]); return int(i); }
            return -1;
        }

        bool Read(Slot& slot, float* mean)
        {
            if (!slot.readback) return false;
            const D3D12_RANGE range { 0, sizeof(float) };
            void* data = nullptr;
            if (FAILED(slot.readback->Map(0, &range, &data)) || !data) return false;
            if (slot.half)
            {
                // IEEE half -> float, no DirectXMath dependency in this header.
                const uint16_t h = *static_cast<const uint16_t*>(data);
                const uint32_t sign = uint32_t(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
                float v;
                if (exponent == 0) v = std::ldexp(float(mantissa), -24);
                else if (exponent == 31) v = mantissa ? NAN : INFINITY;
                else v = std::ldexp(float(mantissa | 0x400u), int(exponent) - 25);
                *mean = sign ? -v : v;
            }
            else
                *mean = *static_cast<const float*>(data);
            const D3D12_RANGE written { 0, 0 };
            slot.readback->Unmap(0, &written);
            return true;
        }

        void Unbind(Slot& slot)
        {
            ReleaseObject(slot.fence);
            slot.tailValue = 0;
            slot.bound = false;
        }

        ID3D12Device* device_ {};
        ID3D12RootSignature* root_ {};
        ID3D12PipelineState* pso_ {};
        ID3D12DescriptorHeap* heap_ {};
        UINT increment_ {};
        Slot slots_[kSlots] {};
        bool warm_ {};
        bool texel_ {};
        float mean_ {};
        float exposure_ {};
        UINT measured_ {};
        UINT skipped_ {};
        std::string error_;
    };
}

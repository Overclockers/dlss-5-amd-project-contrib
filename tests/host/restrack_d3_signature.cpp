// Host contract test for the ResTrack_Dx12 persistent-binding tracking:
//   the hooked ID3D12GraphicsCommandList / ID3D12Device vtable slots are
//   verified against the REAL D3D12 SDK header (d3d12.h C vtables), and the
//   literal pVTable[N] indices in the shipped ResTrack_dx12.cpp must match them.
//
// d3d12.h is included with CINTERFACE so the real C vtbl structs
// (ID3D12GraphicsCommandListVtbl / ID3D12DeviceVtbl) are available for
// offsetof-based slot assertions and function-pointer-type checks.

#define CINTERFACE
#define COBJMACROS
#include <d3d12.h>

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <limits>
#include <type_traits>
#include <vector>

static_assert(sizeof(void*) == 8, "64-bit target required");

// ---------------------------------------------------------------------------
// Real-vtable slot contract. These are the exact indices production code
// reads in ResTrack_dx12.cpp HookCommandList (pVTable[...]) / H4 fix.
// ---------------------------------------------------------------------------
#define VTABLE_SLOT(vtbl, method) (offsetof(vtbl, method) / sizeof(void*))

static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Release) == 2);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Close) == 9);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Reset) == 10);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, ClearState) == 11, "H4: ClearState is slot 11, not 47");
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced) == 12);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced) == 13);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Dispatch) == 14);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, ExecuteBundle) == 27);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable) == 31);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable) == 32);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets) == 46);
static_assert(VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, ClearDepthStencilView) == 47, "slot 47 is ClearDepthStencilView");


// Extracts the field type from a pointer-to-data-member expression
// (decltype(&Vtbl::Method) is a pointer-to-member, not a function pointer).
template <typename T>
struct MemberType;
template <typename C, typename T>
struct MemberType<T C::*>
{
    using type = T;
};
template <typename PtrToMember>
using MemberTypeT = typename MemberType<PtrToMember>::type;

// ---------------------------------------------------------------------------
// Hook function-pointer type contract. These typedefs mirror the production
// PFN_* declarations from ResTrack_dx12.cpp (hooks/ originals) and must match
// the D3D12 SDK vtable field types exactly, or the detoured originals would be
// called with a wrong calling convention / parameter list.
// ---------------------------------------------------------------------------
using PFN_SetGraphicsRootDescriptorTable_t = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* This, UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
using PFN_SetComputeRootDescriptorTable_t = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* This, UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor);
using PFN_DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* This, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation,
    INT BaseVertexLocation, UINT StartInstanceLocation);
using PFN_OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(
    ID3D12GraphicsCommandList* This, UINT NumRenderTargetDescriptors,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* pDepthStencilDescriptor);
using PFN_Reset_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList* This,
                                                ID3D12CommandAllocator* pAllocator, ID3D12PipelineState* pPipelineState);
using PFN_ClearState_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList* This, ID3D12PipelineState* pPipelineState);
using PFN_Close_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList* This);

static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12GraphicsCommandListVtbl::SetGraphicsRootDescriptorTable)>,
                             PFN_SetGraphicsRootDescriptorTable_t>);
static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12GraphicsCommandListVtbl::SetComputeRootDescriptorTable)>,
                             PFN_SetComputeRootDescriptorTable_t>);
static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12GraphicsCommandListVtbl::DrawIndexedInstanced)>,
                             PFN_DrawIndexedInstanced_t>);
static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12GraphicsCommandListVtbl::OMSetRenderTargets)>,
                             PFN_OMSetRenderTargets_t>);
static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12GraphicsCommandListVtbl::Reset)>, PFN_Reset_t>);
static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12GraphicsCommandListVtbl::ClearState)>, PFN_ClearState_t>);
static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12GraphicsCommandListVtbl::Close)>, PFN_Close_t>);

// ID3D12Device::CreateRootSignature as declared by the SDK. RegisterRootSignature
// consumes the ID3D12RootSignature* produced through this entry point (directly or
// via our hooked device), so the production signature understanding must match.
using PFN_CreateRootSignature_t = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device* This, UINT nodeMask, const void* pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid,
    void** ppvRootSignature);
static_assert(std::is_same_v<MemberTypeT<decltype(&ID3D12DeviceVtbl::CreateRootSignature)>, PFN_CreateRootSignature_t>);

static_assert(VTABLE_SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap) == 14);
static_assert(VTABLE_SLOT(ID3D12DeviceVtbl, CreateShaderResourceView) == 18);
static_assert(VTABLE_SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView) == 19);
static_assert(VTABLE_SLOT(ID3D12DeviceVtbl, CreateRenderTargetView) == 20);
static_assert(VTABLE_SLOT(ID3D12DeviceVtbl, CreateSampler) == 22);
static_assert(VTABLE_SLOT(ID3D12DeviceVtbl, CopyDescriptors) == 23);
static_assert(VTABLE_SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple) == 24);

// ---------------------------------------------------------------------------
// Production slot contract. The static_asserts above only pin the SDK layout;
// this reads the literal pVTable[N] indices in the shipped ResTrack_dx12.cpp and
// requires each one to equal the SDK slot of the method its PFN_* names. A
// regression such as H4 (ClearState hooked through pVTable[47]) fails here.
// ---------------------------------------------------------------------------
static int CheckProductionVtableSlots(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string source = buffer.str();

    const std::map<std::string, size_t> expected = {
        { "Release", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Release) },
        { "Close", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Close) },
        { "Reset", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Reset) },
        { "ClearState", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, ClearState) },
        { "DrawInstanced", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, DrawInstanced) },
        { "DrawIndexedInstanced", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, DrawIndexedInstanced) },
        { "Dispatch", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, Dispatch) },
        { "ExecuteBundle", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, ExecuteBundle) },
        { "SetComputeRootDescriptorTable", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, SetComputeRootDescriptorTable) },
        { "SetGraphicsRootDescriptorTable", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, SetGraphicsRootDescriptorTable) },
        { "OMSetRenderTargets", VTABLE_SLOT(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets) },
        { "ExecuteCommandLists", VTABLE_SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists) },
        { "CreateDescriptorHeap", VTABLE_SLOT(ID3D12DeviceVtbl, CreateDescriptorHeap) },
        { "CreateShaderResourceView", VTABLE_SLOT(ID3D12DeviceVtbl, CreateShaderResourceView) },
        { "CreateUnorderedAccessView", VTABLE_SLOT(ID3D12DeviceVtbl, CreateUnorderedAccessView) },
        { "CreateRenderTargetView", VTABLE_SLOT(ID3D12DeviceVtbl, CreateRenderTargetView) },
        { "CreateSampler", VTABLE_SLOT(ID3D12DeviceVtbl, CreateSampler) },
        { "CopyDescriptors", VTABLE_SLOT(ID3D12DeviceVtbl, CopyDescriptors) },
        { "CopyDescriptorsSimple", VTABLE_SLOT(ID3D12DeviceVtbl, CopyDescriptorsSimple) },
    };

    // Only live code: lines whose first non-blank characters are "//" are skipped.
    const std::regex slot(R"(\(PFN_(\w+)\)\s*pVTable\[(\d+)\])");
    std::map<std::string, bool> seen;
    int failures = 0;
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line))
    {
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line.compare(first, 2, "//") == 0)
            continue;
        std::smatch match;
        if (!std::regex_search(line, match, slot))
            continue;
        const std::string name = match[1].str();
        const size_t index = static_cast<size_t>(std::stoul(match[2].str()));
        const auto entry = expected.find(name);
        if (entry == expected.end())
        {
            std::fprintf(stderr, "unknown hooked slot PFN_%s pVTable[%zu]: add it to the contract\n", name.c_str(), index);
            ++failures;
            continue;
        }
        if (entry->second != index)
        {
            std::fprintf(stderr, "PFN_%s uses pVTable[%zu], SDK slot is %zu\n", name.c_str(), index, entry->second);
            ++failures;
        }
        seen[name] = true;
    }
    for (const char* required : { "Reset", "ClearState", "DrawIndexedInstanced", "SetGraphicsRootDescriptorTable",
                                  "SetComputeRootDescriptorTable", "OMSetRenderTargets" })
    {
        if (!seen.count(required))
        {
            std::fprintf(stderr, "PFN_%s pVTable[...] assignment not found in %s\n", required, path);
            ++failures;
        }
    }
    return failures;
}

int main(int argc, char** argv)
{
    const char* production = argc > 1 ? argv[1]
                                      : "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/resource_tracking/ResTrack_dx12.cpp";
    if (CheckProductionVtableSlots(production) != 0)
        return 1;

    std::puts("RESTRACK_D3_SIGNATURE_OK");
    return 0;
}

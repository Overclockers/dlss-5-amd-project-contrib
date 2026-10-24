#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/CommandListProxy.h"

using Microsoft::WRL::ComPtr;
namespace S = DlssNr::Submission;
constexpr GUID vendorIid = {0x18694f6e, 0x512f, 0x40ab, {0x84, 0xd3, 0x42, 0x64, 0x8a, 0x71, 0x50, 0x28}};

// WARP executes the real list lifecycle without hardware. This outer native layer
// provides the runtime-private face, records its ABI arguments, and a vendor alias.
class NativeList final : public ID3D12GraphicsCommandList, public S::ID3D12CommandListPresent_RS5
{
    ULONG refs = 1;
    ComPtr<ID3D12GraphicsCommandList> original;
public:
    bool supportsPresent = true, failReset = false;
    unsigned blits = 0, presents = 0;
    explicit NativeList(ID3D12GraphicsCommandList* list) : original(list) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == S::IID_CommandListPresentRS5 && supportsPresent)
            *out = static_cast<S::ID3D12CommandListPresent_RS5*>(this);
        else if (iid == IID_IUnknown || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) ||
                 iid == __uuidof(ID3D12CommandList) || iid == __uuidof(ID3D12GraphicsCommandList) || iid == vendorIid)
            *out = static_cast<ID3D12GraphicsCommandList*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) delete this; return n; }
    void STDMETHODCALLTYPE Blt(ID3D12Resource* dst, const RECT* dr, const RECT* ds, UINT sub,
                              ID3D12Resource* src, const RECT* sr, const S::SUBRESOURCE_BLT_MAP* map,
                              UINT count, UINT flags) override
    {
        assert(!dst && !src && dr && dr->left == 11 && ds == dr && sr == dr);
        assert(sub == 3 && !map && count == 5 && flags == 7); ++blits;
    }
    void STDMETHODCALLTYPE Present(ID3D12CommandQueue* queue, const S::D3D12_PRESENTSURFACE* surfaces,
                                  UINT count, ID3D12Resource* resource, const RECT* rect,
                                  UINT flags, UINT interval, UINT presentCount, INT* result,
                                  S::D3D12_PRESENT* present) override
    {
        assert(!queue && !surfaces && count == 2 && !resource && rect && rect->left == 11);
        assert(flags == 4 && interval == 6 && presentCount == 8 && result && !present);
        *result = 42; ++presents;
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID guid, _Inout_ UINT *pDataSize, _Out_writes_bytes_opt_( *pDataSize ) void *pData) override { return original->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID guid, _In_ UINT DataSize, _In_reads_bytes_opt_( DataSize ) const void *pData) override { return original->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID guid, _In_opt_ const IUnknown *pData) override { return original->SetPrivateDataInterface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(_In_z_ LPCWSTR Name) override { return original->SetName(Name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, _COM_Outptr_opt_ void **ppvDevice) override { return original->GetDevice(riid, ppvDevice); }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return original->GetType(); }
    HRESULT STDMETHODCALLTYPE Close() override { return original->Close(); }
    HRESULT STDMETHODCALLTYPE Reset(_In_ ID3D12CommandAllocator *pAllocator, _In_opt_ ID3D12PipelineState *pInitialState) override { return failReset ? E_FAIL : original->Reset(pAllocator, pInitialState); }
    void STDMETHODCALLTYPE ClearState(_In_opt_ ID3D12PipelineState *pPipelineState) override { original->ClearState(pPipelineState); }
    void STDMETHODCALLTYPE DrawInstanced(_In_ UINT VertexCountPerInstance, _In_ UINT InstanceCount, _In_ UINT StartVertexLocation, _In_ UINT StartInstanceLocation) override { original->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(_In_ UINT IndexCountPerInstance, _In_ UINT InstanceCount, _In_ UINT StartIndexLocation, _In_ INT BaseVertexLocation, _In_ UINT StartInstanceLocation) override { original->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE Dispatch(_In_ UINT ThreadGroupCountX, _In_ UINT ThreadGroupCountY, _In_ UINT ThreadGroupCountZ) override { original->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
    void STDMETHODCALLTYPE CopyBufferRegion(_In_ ID3D12Resource *pDstBuffer, UINT64 DstOffset, _In_ ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override { original->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes); }
    void STDMETHODCALLTYPE CopyTextureRegion(_In_ const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, _In_ const D3D12_TEXTURE_COPY_LOCATION *pSrc, _In_opt_ const D3D12_BOX *pSrcBox) override { original->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox); }
    void STDMETHODCALLTYPE CopyResource(_In_ ID3D12Resource *pDstResource, _In_ ID3D12Resource *pSrcResource) override { original->CopyResource(pDstResource, pSrcResource); }
    void STDMETHODCALLTYPE CopyTiles(_In_ ID3D12Resource *pTiledResource, _In_ const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, _In_ const D3D12_TILE_REGION_SIZE *pTileRegionSize, _In_ ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override { original->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags); }
    void STDMETHODCALLTYPE ResolveSubresource(_In_ ID3D12Resource *pDstResource, _In_ UINT DstSubresource, _In_ ID3D12Resource *pSrcResource, _In_ UINT SrcSubresource, _In_ DXGI_FORMAT Format) override { original->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(_In_ D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override { original->IASetPrimitiveTopology(PrimitiveTopology); }
    void STDMETHODCALLTYPE RSSetViewports(_In_range_(0, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) UINT NumViewports, _In_reads_( NumViewports) const D3D12_VIEWPORT *pViewports) override { original->RSSetViewports(NumViewports, pViewports); }
    void STDMETHODCALLTYPE RSSetScissorRects(_In_range_(0, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) UINT NumRects, _In_reads_( NumRects) const D3D12_RECT *pRects) override { original->RSSetScissorRects(NumRects, pRects); }
    void STDMETHODCALLTYPE OMSetBlendFactor(_In_reads_opt_(4) const FLOAT BlendFactor[ 4 ]) override { original->OMSetBlendFactor(BlendFactor); }
    void STDMETHODCALLTYPE OMSetStencilRef(_In_ UINT StencilRef) override { original->OMSetStencilRef(StencilRef); }
    void STDMETHODCALLTYPE SetPipelineState(_In_ ID3D12PipelineState *pPipelineState) override { original->SetPipelineState(pPipelineState); }
    void STDMETHODCALLTYPE ResourceBarrier(_In_ UINT NumBarriers, _In_reads_(NumBarriers) const D3D12_RESOURCE_BARRIER *pBarriers) override { original->ResourceBarrier(NumBarriers, pBarriers); }
    void STDMETHODCALLTYPE ExecuteBundle(_In_ ID3D12GraphicsCommandList *pCommandList) override { original->ExecuteBundle(pCommandList); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(_In_ UINT NumDescriptorHeaps, _In_reads_(NumDescriptorHeaps) ID3D12DescriptorHeap *const *ppDescriptorHeaps) override { original->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps); }
    void STDMETHODCALLTYPE SetComputeRootSignature(_In_opt_ ID3D12RootSignature *pRootSignature) override { original->SetComputeRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(_In_opt_ ID3D12RootSignature *pRootSignature) override { original->SetGraphicsRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { original->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { original->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(_In_ UINT RootParameterIndex, _In_ UINT SrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(_In_ UINT RootParameterIndex, _In_ UINT SrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(_In_ UINT RootParameterIndex, _In_ UINT Num32BitValuesToSet, _In_reads_(Num32BitValuesToSet*sizeof(UINT)) const void *pSrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(_In_ UINT RootParameterIndex, _In_ UINT Num32BitValuesToSet, _In_reads_(Num32BitValuesToSet*sizeof(UINT)) const void *pSrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE IASetIndexBuffer(_In_opt_ const D3D12_INDEX_BUFFER_VIEW *pView) override { original->IASetIndexBuffer(pView); }
    void STDMETHODCALLTYPE IASetVertexBuffers(_In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) const D3D12_VERTEX_BUFFER_VIEW *pViews) override { original->IASetVertexBuffers(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE SOSetTargets(_In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) override { original->SOSetTargets(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE OMSetRenderTargets(_In_ UINT NumRenderTargetDescriptors, _In_opt_ const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, _In_ BOOL RTsSingleHandleToDescriptorRange, _In_opt_ const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor) override { original->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor); }
    void STDMETHODCALLTYPE ClearDepthStencilView(_In_ D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, _In_ D3D12_CLEAR_FLAGS ClearFlags, _In_ FLOAT Depth, _In_ UINT8 Stencil, _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearRenderTargetView(_In_ D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, _In_ const FLOAT ColorRGBA[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(_In_ D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, _In_ D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, _In_ ID3D12Resource *pResource, _In_ const UINT Values[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(_In_ D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, _In_ D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, _In_ ID3D12Resource *pResource, _In_ const FLOAT Values[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE DiscardResource(_In_ ID3D12Resource *pResource, _In_opt_ const D3D12_DISCARD_REGION *pRegion) override { original->DiscardResource(pResource, pRegion); }
    void STDMETHODCALLTYPE BeginQuery(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT Index) override { original->BeginQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE EndQuery(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT Index) override { original->EndQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE ResolveQueryData(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT StartIndex, _In_ UINT NumQueries, _In_ ID3D12Resource *pDestinationBuffer, _In_ UINT64 AlignedDestinationBufferOffset) override { original->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset); }
    void STDMETHODCALLTYPE SetPredication(_In_opt_ ID3D12Resource *pBuffer, _In_ UINT64 AlignedBufferOffset, _In_ D3D12_PREDICATION_OP Operation) override { original->SetPredication(pBuffer, AlignedBufferOffset, Operation); }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, _In_reads_bytes_opt_(Size) const void *pData, UINT Size) override { original->SetMarker(Metadata, pData, Size); }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, _In_reads_bytes_opt_(Size) const void *pData, UINT Size) override { original->BeginEvent(Metadata, pData, Size); }
    void STDMETHODCALLTYPE EndEvent() override { original->EndEvent(); }
    void STDMETHODCALLTYPE ExecuteIndirect(_In_ ID3D12CommandSignature *pCommandSignature, _In_ UINT MaxCommandCount, _In_ ID3D12Resource *pArgumentBuffer, _In_ UINT64 ArgumentBufferOffset, _In_opt_ ID3D12Resource *pCountBuffer, _In_ UINT64 CountBufferOffset) override { original->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset); }
};

int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    assert(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> native;
    assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&native))));
    ComPtr<NativeList> layer; layer.Attach(new NativeList(native.Get()));
    ComPtr<S::CommandListProxy> proxy;
    assert(SUCCEEDED(S::CommandListProxy::Create(device.Get(), allocator.Get(), layer.Get(), &proxy)));
    ComPtr<S::ID3D12CommandListPresent_RS5> present;
    layer->supportsPresent = false;
    assert(proxy->QueryInterface(S::IID_CommandListPresentRS5, &present) == E_NOINTERFACE && !present);
    assert(proxy->CanRecordInline());
    layer->supportsPresent = true;
    assert(SUCCEEDED(proxy->QueryInterface(S::IID_CommandListPresentRS5, &present)));
    assert(present.Get() != static_cast<S::ID3D12CommandListPresent_RS5*>(layer.Get()));
    ComPtr<IUnknown> identity;
    assert(SUCCEEDED(present.As(&identity)));
    assert(identity.Get() == static_cast<ID3D12GraphicsCommandList10*>(proxy.Get()));
    identity.Reset();
    assert(proxy->CanRecordInline() && proxy->CanAppendCompute() && !proxy->IsSplitIneligible());
    RECT rect {11, 0, 0, 0};
    present->Blt(nullptr, &rect, &rect, 3, nullptr, &rect, nullptr, 5, 7);
    assert(layer->blits == 1 && !proxy->CanRecordInline() && !proxy->CanAppendCompute());
    assert(std::strcmp(proxy->InlineRejectionReason(), "private_present_commands") == 0);
    assert(proxy->IsSplitIneligible());
    assert(SUCCEEDED(proxy->Close()));
    layer->failReset = true;
    assert(FAILED(proxy->Reset(allocator.Get(), nullptr)) && !proxy->CanRecordInline());
    layer->failReset = false;
    assert(SUCCEEDED(proxy->Reset(allocator.Get(), nullptr)) && proxy->CanRecordInline());
    INT result = 0;
    // Keep the same interface alive across Reset: calls must still traverse the proxy.
    present->Present(nullptr, nullptr, 2, nullptr, &rect, 4, 6, 8, &result, nullptr);
    assert(layer->presents == 1 && result == 42 && !proxy->CanRecordInline());
    assert(SUCCEEDED(proxy->Close()));
    assert(SUCCEEDED(proxy->Reset(allocator.Get(), nullptr)) && proxy->CanRecordInline());
    // Existing double-wrap defenses remain intact for both creation paths.
    S::CommandListProxy* doubleWrapped = nullptr;
    assert(S::CommandListProxy::Create(device.Get(), allocator.Get(), proxy.Get(), &doubleWrapped) == E_NOINTERFACE);
    assert(S::CommandListProxy::CreateClosed(device.Get(), proxy.Get(), &doubleWrapped) == E_NOINTERFACE);
    // A genuinely unknown successful QI is still lifetime-wide, even after Release/Reset.
    ComPtr<IUnknown> vendor;
    assert(SUCCEEDED(proxy->QueryInterface(vendorIid, &vendor)));
    vendor.Reset();
    assert(!proxy->CanRecordInline() && !proxy->CanAppendCompute() && proxy->IsSplitIneligible());
    assert(std::strcmp(proxy->InlineRejectionReason(), "native_interface_escaped") == 0);
    assert(SUCCEEDED(proxy->Close()));
    assert(SUCCEEDED(proxy->Reset(allocator.Get(), nullptr)) && !proxy->CanRecordInline());
    assert(SUCCEEDED(proxy->Close()));
    std::puts("command list private present: QI identity, Blt/Present ABI, Reset, vendor escape and double-wrap: PASS (WARP)");
}

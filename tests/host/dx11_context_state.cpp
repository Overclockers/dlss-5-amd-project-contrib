#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/with_dx12/dx11_context_state.h"
using Microsoft::WRL::ComPtr;
static void Require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static void Check(HRESULT hr, const char* what) { Require(SUCCEEDED(hr), what); }

struct Lifetime : IUnknown
{
    std::atomic<ULONG> refs {1};
    bool& destroyed;
    explicit Lifetime(bool& value) : destroyed(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs; if (!n) { destroyed = true; delete this; } return n; }
};

static int CheckContextState(D3D_FEATURE_LEVEL level)
{
    try
    {
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                               &device, nullptr, &context), "Create WARP device");
        const auto texture = [&](DXGI_FORMAT format, UINT bind) {
            D3D11_TEXTURE2D_DESC desc {}; desc.Width = desc.Height = 16;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = format; desc.BindFlags = bind;
            ComPtr<ID3D11Texture2D> result; Check(device->CreateTexture2D(&desc, nullptr, &result), "Create texture");
            return result;
        };
        auto colour = texture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        auto target = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
        auto depth = texture(DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL);
        ComPtr<ID3D11ShaderResourceView> srv; Check(device->CreateShaderResourceView(colour.Get(), nullptr, &srv), "Create SRV");
        ComPtr<ID3D11RenderTargetView> rtv; Check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv), "Create RTV");
        ComPtr<ID3D11DepthStencilView> dsv; Check(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv), "Create DSV");
        D3D11_BUFFER_DESC bufferDesc {}; bufferDesc.ByteWidth = 256; bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bufferDesc.StructureByteStride = 16;
        ComPtr<ID3D11Buffer> structured; Check(device->CreateBuffer(&bufferDesc, nullptr, &structured), "Structured buffer");
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc {}; uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 16; uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
        ComPtr<ID3D11UnorderedAccessView> uav;
        Check(device->CreateUnorderedAccessView(structured.Get(), &uavDesc, &uav), "Create append UAV");

        constexpr GUID lifetimeKey {0x7d08d027, 0x8b11, 0x416e, {0xa2,0xd1,0xe3,0x64,0xf9,0x60,0x9e,0xc8}};
        std::array<bool, 4> destroyed {};
        std::array<ID3D11DeviceChild*, 4> views {srv.Get(), uav.Get(), rtv.Get(), dsv.Get()};
        for (size_t i = 0; i < views.size(); ++i) {
            auto* sentinel = new Lifetime(destroyed[i]);
            Check(views[i]->SetPrivateDataInterface(lifetimeKey, sentinel), "Attach lifetime sentinel");
            sentinel->Release();
        }
        ID3D11ShaderResourceView* srvRaw = srv.Get(); context->CSSetShaderResources(3, 1, &srvRaw);
        ID3D11UnorderedAccessView* uavRaw = uav.Get(); UINT counter = 7;
        context->CSSetUnorderedAccessViews(0, 1, &uavRaw, &counter);
        ID3D11RenderTargetView* rtvRaw = rtv.Get(); context->OMSetRenderTargets(1, &rtvRaw, dsv.Get());

        const char source[] = "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {}";
        ComPtr<ID3DBlob> blob; Check(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, nullptr), "Compile shader");
        ComPtr<ID3D11ComputeShader> shader;
        Check(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader), "Create shader");
        context->CSSetShader(shader.Get(), nullptr, 0);
        D3D11_BUFFER_DESC cbDesc {}; cbDesc.ByteWidth = 1024; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> cb; Check(device->CreateBuffer(&cbDesc, nullptr, &cb), "Constant buffer");
        ID3D11Buffer* cbRaw = cb.Get(); ComPtr<ID3D11DeviceContext1> context1; context.As(&context1);
        UINT first = 16, count = 16;
        if (context1) context1->CSSetConstantBuffers1(2, 1, &cbRaw, &first, &count);
        else context->CSSetConstantBuffers(2, 1, &cbRaw);
        srv.Reset(); uav.Reset(); rtv.Reset(); dsv.Reset(); // Context is now the views' sole owner.
        {
            Dx11Bridge::ScopedContextState saved(context.Get());
            context->ClearState();
            for (bool dead : destroyed) Require(!dead, "Snapshot released a view before restoration");
            saved.Restore(); saved.Restore(); // An explicit restore and the destructor must be idempotent.
            ComPtr<ID3D11ShaderResourceView> actualSrv; context->CSGetShaderResources(3, 1, &actualSrv);
            Require(actualSrv.Get() == srvRaw, "SRV identity was not restored");
            ComPtr<ID3D11UnorderedAccessView> actualUav; context->CSGetUnorderedAccessViews(0, 1, &actualUav);
            Require(actualUav.Get() == uavRaw, "UAV identity was not restored");
            ComPtr<ID3D11RenderTargetView> actualRtv; ComPtr<ID3D11DepthStencilView> actualDsv;
            context->OMGetRenderTargets(1, &actualRtv, &actualDsv);
            Require(actualRtv.Get() == views[2] && actualDsv.Get() == views[3], "Output views were not restored");
            ComPtr<ID3D11ComputeShader> actualShader; context->CSGetShader(&actualShader, nullptr, nullptr);
            Require(actualShader.Get() == shader.Get(), "Bridge compute shader leaked into game state");
            if (context1) {
                ComPtr<ID3D11Buffer> actualCb; UINT actualFirst = 0, actualCount = 0;
                context1->CSGetConstantBuffers1(2, 1, &actualCb, &actualFirst, &actualCount);
                Require(actualCb.Get() == cb.Get() && actualFirst == first && actualCount == count, "Constant buffer range was not restored");
            }
        }
        for (bool dead : destroyed) Require(!dead, "Restored context lost a view");
        {
            Dx11Bridge::ScopedContextState saved(context.Get());
            context->ClearState(); // Scope exit must restore even without an explicit Restore call.
        }
        {
            ComPtr<ID3D11RenderTargetView> actualRtv;
            context->OMGetRenderTargets(1, &actualRtv, nullptr);
            Require(actualRtv.Get() == rtvRaw, "Scope-exit restoration lost the render target");
        }
        D3D11_BUFFER_DESC readDesc {}; readDesc.ByteWidth = 4; readDesc.Usage = D3D11_USAGE_STAGING; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> counterRead; Check(device->CreateBuffer(&readDesc, nullptr, &counterRead), "Counter readback");
        context->CopyStructureCount(counterRead.Get(), 0, uavRaw);
        D3D11_MAPPED_SUBRESOURCE mapped {}; Check(context->Map(counterRead.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map counter");
        Require(*static_cast<const UINT*>(mapped.pData) == 7, "Restoration reset an append counter"); context->Unmap(counterRead.Get(), 0);
        context->ClearState(); context->Flush();
        for (bool dead : destroyed) Require(dead, "Snapshot leaked a saved view");
        puts("dx11 context state: PASS (view lifetime, identities, shader, CB ranges, UAV counter, release)");
        return 0;
    }
    catch (const std::exception& e) { printf("dx11 context state: FAIL: %s\n", e.what()); return 1; }
}

int main()
{
    if (CheckContextState(D3D_FEATURE_LEVEL_11_0) != 0) return 1;
    return CheckContextState(D3D_FEATURE_LEVEL_11_1);
}

#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>

namespace Dx11Bridge
{
// Get* bindings acquire references. Keep them until every binding is restored:
// the game is allowed to leave the context as a view's sole owner.
class ScopedContextState
{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
    std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> srvs {};
    std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers {};
    std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constants {};
    std::array<UINT, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constantStarts {}, constantCounts {};
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> uavs {};
    std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ComputeShader* shader = nullptr;
    std::array<ID3D11ClassInstance*, 256> instances {};
    UINT instanceCount = static_cast<UINT>(instances.size());
    UINT uavCount = D3D11_PS_CS_UAV_REGISTER_COUNT;
    bool restored = false;

    template<class T, size_t N> static void Release(std::array<T*, N>& values)
    {
        for (auto* value : values)
            if (value) value->Release();
    }

  public:
    explicit ScopedContextState(ID3D11DeviceContext* source) : context(source)
    {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1)
            uavCount = D3D11_1_UAV_SLOT_COUNT;
        context.As(&context1);
        context->CSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context->CSGetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
        if (context1)
            context1->CSGetConstantBuffers1(0, static_cast<UINT>(constants.size()), constants.data(),
                                           constantStarts.data(), constantCounts.data());
        else
            context->CSGetConstantBuffers(0, static_cast<UINT>(constants.size()), constants.data());
        context->CSGetUnorderedAccessViews(0, uavCount, uavs.data());
        context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
        context->CSGetShader(&shader, instances.data(), &instanceCount);
    }

    ScopedContextState(const ScopedContextState&) = delete;
    ScopedContextState& operator=(const ScopedContextState&) = delete;

    void Restore()
    {
        if (restored) return;
        restored = true;
        // Remove bridge UAVs before restoring SRVs that may alias their resources.
        std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> emptyUavs {};
        context->CSSetUnorderedAccessViews(0, uavCount, emptyUavs.data(), nullptr);
        context->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), dsv);
        context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context->CSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
        if (context1)
            context1->CSSetConstantBuffers1(0, static_cast<UINT>(constants.size()), constants.data(),
                                           constantStarts.data(), constantCounts.data());
        else
            context->CSSetConstantBuffers(0, static_cast<UINT>(constants.size()), constants.data());
        // nullptr preserves append/consume counters.
        context->CSSetUnorderedAccessViews(0, uavCount, uavs.data(), nullptr);
        context->CSSetShader(shader, instances.data(), instanceCount);
    }

    ~ScopedContextState()
    {
        Restore();
        Release(srvs); Release(samplers); Release(constants); Release(uavs); Release(rtvs); Release(instances);
        if (dsv) dsv->Release();
        if (shader) shader->Release();
    }
};
} // namespace Dx11Bridge

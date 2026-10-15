#pragma once
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <memory>
#include <atomic>
#include <thread>
#include "dlssnr/backend/mochizuki_runtime/MochizukiNrApi.h"
using Microsoft::WRL::ComPtr;
void Require(bool yes, const char* text) { if (!yes) throw std::runtime_error(text); }
void Hr(HRESULT hr) { if (FAILED(hr)) { char b[64]; sprintf_s(b, "HRESULT 0x%08X", unsigned(hr)); throw std::runtime_error(b); } }
LmxxfNrApi api {};
void Rc(int rc) { if (rc) { char text[256] {}; api.GetLastError(text, sizeof text); throw std::runtime_error(text); } }
struct List {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    explicit List(ID3D12Device* d) {
        Hr(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        Hr(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)));
    }
};
void Submit(ID3D12CommandQueue* q, List& list) { ID3D12CommandList* lists[] = {list.cmd.Get()}; q->ExecuteCommandLists(1,lists); }
void Wait(ID3D12Fence* fence, UINT64 value) {
    HANDLE ev=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(ev != nullptr,"event");
    Hr(fence->SetEventOnCompletion(value,ev)); DWORD rc=WaitForSingleObject(ev,30000); CloseHandle(ev);
    Require(rc==WAIT_OBJECT_0,"GPU timeout"); Require(fence->GetCompletedValue()!=UINT64_MAX,"device removed");
}
void Barrier(ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v {};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&v);
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* d,UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp {}; hp.Type=heap;
    D3D12_RESOURCE_DESC desc {};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&r)));return r;
}
struct Frame {
    ComPtr<ID3D12Resource> color, upload, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT width, height;
    DXGI_FORMAT format;
    Frame(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT fmt=DXGI_FORMAT_R16G16B16A16_FLOAT):width(w),height(h),format(fmt) {
        D3D12_HEAP_PROPERTIES hp {};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=fmt;
        Hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&color)));
        UINT64 bytes; d->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        upload=Buffer(d,bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        readback=Buffer(d,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        void* data;Hr(upload->Map(0,nullptr,&data));
        for(UINT y=0;y<h;y++)for(UINT x=0;x<w;x++) {
            float base=.05f+.7f*float(x)/w + ((x/8+y/8)%2)*.08f;
            auto* row=static_cast<char*>(data)+footprint.Footprint.RowPitch*y;
            if(fmt==DXGI_FORMAT_R9G9B9E5_SHAREDEXP)
                reinterpret_cast<DirectX::PackedVector::XMFLOAT3SE*>(row)[x]=DirectX::PackedVector::XMFLOAT3SE(base,base*.8f,base*.6f);
            else if(fmt==DXGI_FORMAT_R11G11B10_FLOAT)
                reinterpret_cast<DirectX::PackedVector::XMFLOAT3PK*>(row)[x]=DirectX::PackedVector::XMFLOAT3PK(base,base*.8f,base*.6f);
            else if(fmt==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||fmt==DXGI_FORMAT_R8G8B8A8_UNORM) {
                auto* p=reinterpret_cast<uint8_t*>(row)+x*4;
                for(int k=0;k<3;k++)p[k]=uint8_t(std::round(base*(1.f-.2f*k)*255));p[3]=255;
            } else if(fmt==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||fmt==DXGI_FORMAT_B8G8R8A8_UNORM) {
                auto* p=reinterpret_cast<uint8_t*>(row)+x*4;
                for(int k=0;k<3;k++)p[2-k]=uint8_t(std::round(base*(1.f-.2f*k)*255));p[3]=255;
            } else if(fmt==DXGI_FORMAT_R10G10B10A2_UNORM) {
                uint32_t v=3u<<30;
                for(int k=0;k<3;k++)v|=uint32_t(std::round(base*(1.f-.2f*k)*1023))<<(10*k);
                reinterpret_cast<uint32_t*>(row)[x]=v;
            } else if(fmt==DXGI_FORMAT_R16G16_FLOAT) {
                auto* p=reinterpret_cast<uint16_t*>(row)+x*2; // motion-vector fixtures: small vectors
                p[0]=DirectX::PackedVector::XMConvertFloatToHalf(base*.01f);p[1]=DirectX::PackedVector::XMConvertFloatToHalf(-base*.01f);
            } else {
                auto* p=reinterpret_cast<uint16_t*>(row)+x*4;
                for(int k=0;k<3;k++)p[k]=DirectX::PackedVector::XMConvertFloatToHalf(base*(1.f-.2f*k));p[3]=0x3c00;
            }
        }
        upload->Unmap(0,nullptr);
    }
    void Upload(ID3D12GraphicsCommandList* c) {
        D3D12_TEXTURE_COPY_LOCATION a {color.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
        D3D12_TEXTURE_COPY_LOCATION b {upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};b.PlacedFootprint=footprint;
        c->CopyTextureRegion(&a,0,0,0,&b,nullptr);
        Barrier(c,color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void Read(ID3D12GraphicsCommandList* c,ID3D12Resource* output) {
        Barrier(c,output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a {readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,{}};a.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION b {output,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{}};
        c->CopyTextureRegion(&a,0,0,0,&b,nullptr);
        Barrier(c,output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    void Check(UINT validWidth=0, UINT validHeight=0) {
        if(!validWidth)validWidth=width;if(!validHeight)validHeight=height;
        void* data;Hr(readback->Map(0,nullptr,&data));double sum=0,diff=0;
        for(UINT y=0;y<validHeight;y++)for(UINT x=0;x<validWidth;x++) {
            auto* row=static_cast<char*>(data)+footprint.Footprint.RowPitch*y;
            float v;
            if(format==DXGI_FORMAT_R9G9B9E5_SHAREDEXP)
                v=DirectX::XMVectorGetX(DirectX::PackedVector::XMLoadFloat3SE(reinterpret_cast<DirectX::PackedVector::XMFLOAT3SE*>(row)+x));
            else if(format==DXGI_FORMAT_R11G11B10_FLOAT)
                v=DirectX::XMVectorGetX(DirectX::PackedVector::XMLoadFloat3PK(reinterpret_cast<DirectX::PackedVector::XMFLOAT3PK*>(row)+x));
            else if(format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) v=reinterpret_cast<uint8_t*>(row)[x*4]/255.f;
            else v=DirectX::PackedVector::XMConvertHalfToFloat(reinterpret_cast<uint16_t*>(row)[x*4]);
            Require(std::isfinite(v)&&std::abs(v)<100,"non-finite or excessive output");sum+=v;
            float expected=.05f+.7f*float(x)/width+((x/8+y/8)%2)*.08f;diff+=std::abs(v-expected);
        }
        readback->Unmap(0,nullptr);
        printf("OUTPUT %ux%u format=%u mean=%f mean_abs_correction=%f\n",validWidth,validHeight,unsigned(format),sum/(validWidth*validHeight),diff/(validWidth*validHeight));
        Require(sum/(validWidth*validHeight)>.01,"black output");
        Require(diff/(validWidth*validHeight)>.0001,"network made no observable correction");
    }
};

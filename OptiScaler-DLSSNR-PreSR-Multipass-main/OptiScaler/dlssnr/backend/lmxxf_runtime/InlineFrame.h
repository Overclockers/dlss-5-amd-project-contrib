#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <stdexcept>

namespace LmxxfRuntime
{
// A closed/replayable recording owns its captures and results. Only its side
// lists touch the session's codec/HIP arena, in pre -> HIP -> post queue order.
struct InlineFrame
{
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Capture { Ptr<ID3D12Resource> source, texture; D3D12_RESOURCE_STATES state; };
    std::vector<Capture> captures;
    Ptr<ID3D12Resource> output, first;
    Ptr<ID3D12CommandAllocator> preAllocator, postAllocator;
    Ptr<ID3D12GraphicsCommandList> pre, post;
    Ptr<ID3D12Fence> completion;
    UINT64 value = 0;
    size_t used = 0;

    static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("inline frame D3D12 resource/command failure"); }
    static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r,
                        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    }
    static void Copy(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src,
                     D3D12_RESOURCE_STATES sourceState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        Barrier(list, src, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list, dst, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(dst, src);
        Barrier(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceState);
    }
    static void Texture(ID3D12Device* device, ID3D12Resource* source, Ptr<ID3D12Resource>& target)
    {
        auto desc = source->GetDesc();
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (target) {
            auto old = target->GetDesc();
            if (old.Dimension == desc.Dimension && old.Width == desc.Width && old.Height == desc.Height &&
                old.Format == desc.Format && old.DepthOrArraySize == desc.DepthOrArraySize &&
                old.MipLevels == desc.MipLevels && old.SampleDesc.Count == desc.SampleDesc.Count) return;
        }
        target.Reset();
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&target)));
    }
    void Open(ID3D12Device* device)
    {
        used = 0;
        if (!pre) {
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&preAllocator)));
            Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&postAllocator)));
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, preAllocator.Get(), nullptr, IID_PPV_ARGS(&pre)));
            Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, postAllocator.Get(), nullptr, IID_PPV_ARGS(&post)));
            Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion)));
        } else {
            Check(preAllocator->Reset()); Check(postAllocator->Reset());
            Check(pre->Reset(preAllocator.Get(), nullptr)); Check(post->Reset(postAllocator.Get(), nullptr));
        }
    }
    ID3D12Resource* CaptureInput(ID3D12Device* device, ID3D12Resource* source, D3D12_RESOURCE_STATES state)
    {
        if (!source) return nullptr;
        if (used == captures.size()) captures.emplace_back();
        auto& capture = captures[used++];
        capture.source = source; capture.state = state;
        Texture(device, source, capture.texture);
        return capture.texture.Get();
    }
    void RecordCapture(ID3D12GraphicsCommandList* list)
    {
        for (size_t i = 0; i < used; ++i)
            Copy(list, captures[i].texture.Get(), captures[i].source.Get(), captures[i].state);
    }
    void Close(ID3D12Resource* result, ID3D12Resource* firstResult)
    {
        Copy(post.Get(), output.Get(), result);
        if (firstResult) Copy(post.Get(), first.Get(), firstResult);
        Check(pre->Close()); Check(post->Close());
    }
    void ReleaseSources() { for (auto& c : captures) c.source.Reset(); }
};
}

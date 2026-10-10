#pragma once
#include <d3d12.h>
#include <unordered_map>

namespace DlssNr::Submission
{
// Only uniform, completed whole-resource transitions are admitted for NR interop.
// Partial ranges and pending split barriers remain unsupported, rather than guessing layouts.
class EnhancedBarrierBook
{
public:
    struct State {
        bool texture = false, known = false;
        D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
        D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_NO_ACCESS;
    };
private:
    std::unordered_map<ID3D12Resource*, State> states;
    std::unordered_map<DXGI_FORMAT, UINT8> planeCounts;
    bool WholeTexture(const D3D12_TEXTURE_BARRIER& barrier) {
        const auto& range = barrier.Subresources;
        if (range.NumMipLevels == 0 && range.IndexOrFirstMipLevel == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) return true;
        if (range.IndexOrFirstMipLevel != 0) return false;
        const auto desc = barrier.pResource->GetDesc();
        const UINT arrays = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
        if (range.NumMipLevels == 0) {
            if (desc.MipLevels != 1 || arrays != 1) return false;
        } else if (range.NumMipLevels != desc.MipLevels || range.FirstArraySlice != 0 ||
                   range.NumArraySlices != arrays || range.FirstPlane != 0) return false;
        auto known = planeCounts.find(desc.Format);
        if (known == planeCounts.end()) {
            ID3D12Device* device = nullptr; D3D12_FEATURE_DATA_FORMAT_INFO info {desc.Format, 0};
            if (FAILED(barrier.pResource->GetDevice(IID_PPV_ARGS(&device)))) return false;
            const auto hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof info);
            device->Release();
            if (FAILED(hr) || !info.PlaneCount) return false;
            known = planeCounts.emplace(desc.Format, info.PlaneCount).first;
        }
        return range.NumMipLevels == 0 ? known->second == 1 : range.NumPlanes == known->second;
    }
public:
    void Reset() { states.clear(); planeCounts.clear(); }
    void Forget(ID3D12Resource* resource) { states.erase(resource); }
    const State* Find(ID3D12Resource* resource) const {
        auto it = states.find(resource); return it == states.end() ? nullptr : &it->second;
    }
    void Restore(ID3D12Resource* resource, State state) { states[resource] = state; }
    void Observe(UINT n, const D3D12_BARRIER_GROUP* groups) {
        for (UINT g = 0; groups && g < n; ++g) {
            const auto& group = groups[g];
            if (group.Type == D3D12_BARRIER_TYPE_TEXTURE) {
                for (UINT i = 0; i < group.NumBarriers; ++i) {
                    const auto& b = group.pTextureBarriers[i];
                    if (!b.pResource) continue;
                    const bool whole = WholeTexture(b);
                    states[b.pResource] = {true, whole && b.SyncAfter != D3D12_BARRIER_SYNC_SPLIT,
                                          b.LayoutAfter, b.AccessAfter};
                }
            } else if (group.Type == D3D12_BARRIER_TYPE_BUFFER) {
                for (UINT i = 0; i < group.NumBarriers; ++i) {
                    const auto& b = group.pBufferBarriers[i];
                    if (!b.pResource) continue;
                    const bool whole = b.Offset == 0 && (b.Size == UINT64_MAX || b.Size >= b.pResource->GetDesc().Width);
                    states[b.pResource] = {false, whole && b.SyncAfter != D3D12_BARRIER_SYNC_SPLIT,
                                          D3D12_BARRIER_LAYOUT_UNDEFINED, b.AccessAfter};
                }
            }
        }
    }
};
}

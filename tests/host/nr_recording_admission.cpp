#define NOMINMAX
#include <cassert>
#include <cstdio>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/QueryStateBook.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/EnhancedBarrierBook.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionPolicy.h"
int main() {
    using namespace DlssNr::Submission;
    auto* heap = reinterpret_cast<ID3D12QueryHeap*>(uintptr_t(1));
    QueryStateBook queries;
    assert(queries.Begin(heap, D3D12_QUERY_TYPE_OCCLUSION, 7));
    assert(queries.Begin(heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 3));
    assert(!queries.Begin(heap, D3D12_QUERY_TYPE_OCCLUSION, 7));
    assert(!queries.CanSplit() && queries.OpenCount() == 2);
    unsigned observed = 0;
    queries.ForEachOpen([&](ID3D12QueryHeap* h, D3D12_QUERY_TYPE type, UINT index) {
        assert(h == heap);
        assert((type == D3D12_QUERY_TYPE_OCCLUSION && index == 7) ||
               (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS && index == 3)); ++observed;
    });
    assert(observed == 2 && !queries.CanSplit()); // Diagnostics do not end or restart queries.
    assert(queries.End(heap, D3D12_QUERY_TYPE_TIMESTAMP, 9) && queries.OpenCount() == 2);
    assert(!queries.End(heap, D3D12_QUERY_TYPE_OCCLUSION, 99));
    assert(queries.End(heap, D3D12_QUERY_TYPE_OCCLUSION, 7) && !queries.CanSplit());
    assert(queries.End(heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 3) && queries.CanSplit());
    EnhancedBarrierBook layouts;
    auto* texture = reinterpret_cast<ID3D12Resource*>(uintptr_t(2));
    D3D12_TEXTURE_BARRIER b {}; b.pResource = texture;
    b.LayoutAfter = D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE;
    b.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE; b.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    b.Subresources.IndexOrFirstMipLevel = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_BARRIER_GROUP group {}; group.Type = D3D12_BARRIER_TYPE_TEXTURE; group.NumBarriers = 1; group.pTextureBarriers = &b;
    layouts.Observe(1, &group); assert(layouts.Find(texture)->known);
    const auto whole = *layouts.Find(texture);
    b.Subresources.IndexOrFirstMipLevel = 1; layouts.Observe(1, &group);
    assert(!layouts.Find(texture)->known); // A partial transition must invalidate uniform-layout admission.
    b.Subresources.IndexOrFirstMipLevel = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.SyncAfter = D3D12_BARRIER_SYNC_SPLIT; layouts.Observe(1, &group);
    assert(!layouts.Find(texture)->known);
    layouts.Restore(texture, whole); assert(layouts.Find(texture)->known);
    layouts.Forget(texture); assert(!layouts.Find(texture));
    SetAllowEnhancedBarriers(true); assert(AllowEnhancedBarriers());
    SetAllowEnhancedBarriers(false); assert(!AllowEnhancedBarriers());
    std::puts("NR admission PASS: query diagnostics preserve scopes; partial/pending enhanced layouts fail closed");
}

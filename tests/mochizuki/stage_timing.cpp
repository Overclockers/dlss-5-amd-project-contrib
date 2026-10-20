#include "dlssnr/backend/mochizuki_runtime/MochizukiStageTiming.h"
#include <cassert>
#include <cstdio>
#include <limits>

static uint32_t resets = 0, reads = 0, writes = 0, destroyed = 0;
static VkResult readResult = VK_SUCCESS;
static uint64_t values[MochizukiStageTiming::kQueries] {};
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks*, VkQueryPool* pool) {
    assert(info->queryCount == 8 * (2 + 2 * 30)); *pool = (VkQueryPool)1; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks*) { ++destroyed; }
VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t count) { assert(count == 62); ++resets; }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer, VkPipelineStageFlagBits, VkQueryPool, uint32_t) { ++writes; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t count, size_t, void* data, VkDeviceSize stride, VkQueryResultFlags flags) {
    assert(flags == VK_QUERY_RESULT_64_BIT); assert(stride == sizeof(uint64_t)); ++reads;
    auto* out = static_cast<uint64_t*>(data); for (uint32_t i = 0; i < count; ++i) out[i] = values[i];
    return readResult;
}
}
int main() {
    using Ring = MochizukiStageTiming;
    Ring::Sample sample {};
    uint64_t stamps[Ring::kQueries] {100, 1100, 200, 500, 600, 900};
    assert(Ring::Decode(stamps, 2, UINT64_MAX, 1000, sample));
    assert(sample.total == 1.f && sample.network == .6f);
    assert(Ring::Decode(stamps, 0, UINT64_MAX, 1000, sample) && sample.network == 0);
    stamps[0] = 250; stamps[1] = 14; stamps[2] = 252; stamps[3] = 4;
    assert(Ring::Decode(stamps, 1, 255, 100000, sample) && sample.total == 2.f && sample.network == .8f);
    stamps[3] = 40;
    assert(!Ring::Decode(stamps, 1, 255, 100000, sample));
    assert(!Ring::Decode(stamps, 31, 255, 100000, sample));
    assert(!Ring::Decode(stamps, 0, 255, std::numeric_limits<double>::infinity(), sample));
    uint32_t samples = 0;
    {
        Ring ring; ring.Create((VkDevice)1, 1000, 64, 0);
        assert(ring.Begin((VkCommandBuffer)1, true) == Ring::kUnavailable);
        for (uint32_t i = 0; i < Ring::kSlots; ++i) {
            const auto slot = ring.Begin((VkCommandBuffer)1, false); assert(slot == i);
            if (i == 0) { // All 30 callbacks live in this single unsubmitted frame.
                for (uint32_t p = 0; p < Ring::kPasses; ++p) {
                    ring.BeginNetwork((VkCommandBuffer)1, slot); ring.EndNetwork((VkCommandBuffer)1, slot);
                }
            }
            ring.End((VkCommandBuffer)1, slot); ring.Submitted(slot, i + 1);
        }
        assert(resets == 8 && writes == 76);
        assert(ring.Begin((VkCommandBuffer)1, false) == Ring::kUnavailable && resets == 8);
        ring.Harvest(0, [&](const auto&) { ++samples; }); assert(reads == 0);
        ring.Harvest(UINT64_MAX, [&](const auto&) { ++samples; }); assert(reads == 0);
        readResult = VK_NOT_READY;
        ring.Harvest(8, [&](const auto&) { ++samples; }); assert(samples == 0);
        assert(ring.Begin((VkCommandBuffer)1, false) == Ring::kUnavailable);
        values[0] = 100; values[1] = 1100;
        for (uint32_t p = 0; p < 30; ++p) { values[2 + 2*p] = 110 + 20*p; values[3 + 2*p] = 120 + 20*p; }
        readResult = VK_SUCCESS;
        ring.Harvest(1, [&](const auto& s) { assert(s.total == 1.f && s.network == .3f); ++samples; });
        assert(samples == 1);
        const auto slot = ring.Begin((VkCommandBuffer)1, false); assert(slot == 0);
        ring.CancelUnsubmitted(slot); // A failed recording cannot poison the ring.
        ring.Harvest(8, [&](const auto& s) { assert(s.total == 1.f && s.network == 0.f); ++samples; });
        assert(samples == 8);
        ring.Harvest(8, [&](const auto&) { ++samples; }); assert(samples == 8); // Polling never duplicates samples.
        const auto zero = ring.Begin((VkCommandBuffer)1, false);
        ring.End((VkCommandBuffer)1, zero); ring.Submitted(zero, 9); ring.CancelUnsubmitted(zero);
        ring.Harvest(9, [&](const auto& s) { assert(s.network == 0); ++samples; }); assert(samples == 9);
        const auto old = ring.Begin((VkCommandBuffer)1, false);
        ring.End((VkCommandBuffer)1, old); ring.Submitted(old, 10);
        ring.DiscardSamples();
        ring.Harvest(9, [&](const auto&) { ++samples; }); assert(samples == 9);
        ring.Harvest(10, [&](const auto&) { ++samples; }); assert(samples == 9);
    }
    assert(destroyed == 1);
    std::puts("mochizuki stage timing: PASS (frame ring, 0/30 callbacks, no wait, completion, wrap, lifetime)");
}

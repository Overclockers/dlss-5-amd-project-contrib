#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

namespace mz::transport
{
// Keep the Windows shared-handle workaround for both the probe and real buffers.
// Wine's own-device probe needs only one D3D12 placement-alignment unit.
constexpr uint64_t BufferBytes(uint64_t requested, bool tinyProbe)
{
    const uint64_t aligned = (requested + 65535) & ~uint64_t(65535);
    const uint64_t floor = tinyProbe ? uint64_t(65536) : uint64_t(16) << 20;
    return aligned < floor ? floor : aligned;
}

// Only at the external-import boundary: winevulkan can report HOST_MEMORY for an
// unsupported D3D12_RESOURCE import even when a tiny allocation has ample budget.
// Do not reinterpret ordinary allocations, a proven path's larger allocations,
// or Windows memory-pressure failures.
constexpr bool FixedImportFailure(bool wine, bool tinyProbe, VkResult result)
{
    return wine && result != VK_SUCCESS && result != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
           (result != VK_ERROR_OUT_OF_HOST_MEMORY || tinyProbe) &&
           result != VK_ERROR_DEVICE_LOST && result != VK_TIMEOUT;
}
} // namespace mz::transport

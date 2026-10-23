#pragma once
#include <cstdint>
#include <cstring>
#include <vulkan/vulkan.h>

// Value-only ABI: no driver/application pNext pointers survive vkCreateDevice.
namespace mz::device
{
inline constexpr const char* Extensions[] = {
    VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
    VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME, "VK_KHR_external_semaphore_win32"
};
enum Feature : uint64_t
{
    CooperativeMatrix = 1ull << 0, Float8 = 1ull << 1, Float8CooperativeMatrix = 1ull << 2,
    Storage16 = 1ull << 3, Storage8 = 1ull << 4, Float16 = 1ull << 5, Int8 = 1ull << 6,
    MemoryModel = 1ull << 7, Timeline = 1ull << 8, BufferAddress = 1ull << 9,
    SubgroupSize = 1ull << 10, Synchronization2 = 1ull << 11, WorkgroupLayout = 1ull << 12,
    Workgroup8 = 1ull << 13, Workgroup16 = 1ull << 14
};
struct Enabled
{
    uint32_t struct_size = sizeof(Enabled);
    uint32_t extensions = 0;
    uint64_t features = 0;
};
using Query = int32_t (*)(uint64_t device, Enabled* out);
using SetQuery = void (*)(Query);
inline uint32_t ExtensionBits(uint32_t count, const char* const* names)
{
    uint32_t bits = 0;
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < 4; ++j)
            if (names[i] && !std::strcmp(names[i], Extensions[j])) bits |= 1u << j;
    return bits;
}
inline uint64_t FeatureBits(const void* chain)
{
    uint64_t bits = 0;
    auto add = [&](VkBool32 enabled, Feature bit) { if (enabled == VK_TRUE) bits |= bit; };
    for (auto* p = static_cast<const VkBaseInStructure*>(chain); p; p = p->pNext)
    {
#define MZ_FEATURE_CASE(TYPE, STRUCT, BODY) case TYPE: { const auto& f = *reinterpret_cast<const STRUCT*>(p); BODY; break; }
        switch (p->sType)
        {
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
            VkPhysicalDeviceCooperativeMatrixFeaturesKHR, add(f.cooperativeMatrix, CooperativeMatrix))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT,
            VkPhysicalDeviceShaderFloat8FeaturesEXT, add(f.shaderFloat8, Float8); add(f.shaderFloat8CooperativeMatrix, Float8CooperativeMatrix))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            VkPhysicalDeviceVulkan11Features, add(f.storageBuffer16BitAccess, Storage16))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
            VkPhysicalDevice16BitStorageFeatures, add(f.storageBuffer16BitAccess, Storage16))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            VkPhysicalDeviceVulkan12Features, add(f.storageBuffer8BitAccess, Storage8); add(f.shaderFloat16, Float16);
            add(f.shaderInt8, Int8); add(f.vulkanMemoryModel, MemoryModel); add(f.timelineSemaphore, Timeline);
            add(f.bufferDeviceAddress, BufferAddress))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
            VkPhysicalDevice8BitStorageFeatures, add(f.storageBuffer8BitAccess, Storage8))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
            VkPhysicalDeviceShaderFloat16Int8Features, add(f.shaderFloat16, Float16); add(f.shaderInt8, Int8))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
            VkPhysicalDeviceVulkanMemoryModelFeatures, add(f.vulkanMemoryModel, MemoryModel))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
            VkPhysicalDeviceTimelineSemaphoreFeatures, add(f.timelineSemaphore, Timeline))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
            VkPhysicalDeviceBufferDeviceAddressFeatures, add(f.bufferDeviceAddress, BufferAddress))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            VkPhysicalDeviceVulkan13Features, add(f.subgroupSizeControl, SubgroupSize); add(f.synchronization2, Synchronization2))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,
            VkPhysicalDeviceSubgroupSizeControlFeatures, add(f.subgroupSizeControl, SubgroupSize))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
            VkPhysicalDeviceSynchronization2Features, add(f.synchronization2, Synchronization2))
        MZ_FEATURE_CASE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR,
            VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR, add(f.workgroupMemoryExplicitLayout, WorkgroupLayout);
            add(f.workgroupMemoryExplicitLayout8BitAccess, Workgroup8); add(f.workgroupMemoryExplicitLayout16BitAccess, Workgroup16))
        default: break;
        }
#undef MZ_FEATURE_CASE
    }
    return bits;
}
}

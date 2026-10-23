#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiVulkanRegistry.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace mz::device;
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR layout {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR };
    layout.workgroupMemoryExplicitLayout = layout.workgroupMemoryExplicitLayout8BitAccess =
        layout.workgroupMemoryExplicitLayout16BitAccess = VK_TRUE;
    VkPhysicalDeviceVulkan13Features v13 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &layout };
    v13.subgroupSizeControl = v13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
    v12.storageBuffer8BitAccess = v12.shaderFloat16 = v12.shaderInt8 = v12.vulkanMemoryModel =
        v12.timelineSemaphore = v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan11Features v11 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12 };
    v11.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceShaderFloat8FeaturesEXT fp8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT, &v11 };
    fp8.shaderFloat8 = fp8.shaderFloat8CooperativeMatrix = VK_TRUE;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR, &fp8 };
    coop.cooperativeMatrix = VK_TRUE;
    const uint64_t required = (1ull << 15) - 1;
    assert(FeatureBits(&coop) == required);

    VkPhysicalDeviceSynchronization2Features sync { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, &layout, VK_TRUE };
    VkPhysicalDeviceSubgroupSizeControlFeatures subgroup {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, &sync, VK_TRUE };
    VkPhysicalDeviceBufferDeviceAddressFeatures address {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, &subgroup, VK_TRUE };
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, &address, VK_TRUE };
    VkPhysicalDeviceVulkanMemoryModelFeatures memory {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, &timeline, VK_TRUE };
    VkPhysicalDeviceShaderFloat16Int8Features half {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &memory, VK_TRUE, VK_TRUE };
    VkPhysicalDevice8BitStorageFeatures storage8 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, &half, VK_TRUE };
    VkPhysicalDevice16BitStorageFeatures storage16 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &storage8, VK_TRUE };
    fp8.pNext = &storage16;
    assert(FeatureBits(&coop) == required);
    half.shaderInt8 = VK_FALSE;
    assert(FeatureBits(&coop) == (required & ~Int8));
    half.shaderInt8 = VK_TRUE;

    Registry registry;
    Enabled actual;
    auto device = reinterpret_cast<VkDevice>(uintptr_t(1));
    assert(!registry.Get(1, &actual)); // absent record permits reflection fallback
    VkDeviceCreateInfo ci { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &coop };
    const char* names[] = { Extensions[0], Extensions[1], Extensions[2], Extensions[3] };
    ci.enabledExtensionCount = 4;
    ci.ppEnabledExtensionNames = names;
    registry.Created(device, ci);
    // Neither the application's extension array nor feature structures are retained.
    names[0] = "changed";
    coop.cooperativeMatrix = VK_FALSE;
    assert(registry.Get(1, &actual));
    assert(actual.extensions == 15 && actual.features == required);
    actual.struct_size = 0;
    assert(!registry.Get(1, &actual));
    actual.struct_size = sizeof actual;
    assert(!registry.Get(2, &actual));
    registry.Destroyed(device);
    assert(!registry.Get(1, &actual));
    ci.enabledExtensionCount = 0;
    ci.pNext = nullptr;
    registry.Created(device, ci); // handle reuse must replace all previous enablement
    assert(registry.Get(1, &actual));
    assert(actual.extensions == 0 && actual.features == 0);
    registry.Destroyed(device);
    std::puts("mochizuki_vulkan_device: PASS");
}

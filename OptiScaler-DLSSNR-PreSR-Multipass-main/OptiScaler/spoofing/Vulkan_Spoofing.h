#pragma once

#include "SysUtils.h"

#include <vulkan/vulkan.h>

#include <memory>

#ifdef VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
#endif

class VulkanDeviceFeatureState
{
  public:
    VulkanDeviceFeatureState(VkDeviceCreateInfo* pCreateInfo, PFN_vkGetPhysicalDeviceFeatures2 getFeatures2);
    ~VulkanDeviceFeatureState();

    bool EnableTimelineSemaphores(VkPhysicalDevice physicalDevice);
    void EnableMochizukiFeatures(VkPhysicalDevice physicalDevice);

    VulkanDeviceFeatureState(const VulkanDeviceFeatureState&) = delete;
    VulkanDeviceFeatureState& operator=(const VulkanDeviceFeatureState&) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    friend class VulkanSpoofing;
};

class VulkanSpoofing
{
  private:
  public:
    inline static VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = {};

    static VkResult hkvkCreateDevice(VkPhysicalDevice physicalDevice, VkDeviceCreateInfo* pCreateInfo,
                                     const VkAllocationCallbacks* pAllocator, VkDevice* pDevice,
                                     VulkanDeviceFeatureState* featureState, uint32_t requestedApiVersion);
    static VkResult hkvkCreateInstance(VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                       VkInstance* pInstance);

    static PFN_vkVoidFunction hkvkGetDeviceProcAddr(const PFN_vkVoidFunction orgFunc, const char* pName);
    static PFN_vkVoidFunction hkvkGetInstanceProcAddr(const PFN_vkVoidFunction orgFunc, const char* pName);

    // Driver capability queries must bypass the game-facing extension spoof.
    static PFN_vkEnumerateDeviceExtensionProperties GetOriginalDeviceExtensionEnumerator();

    static void HookForVulkanSpoofing(HMODULE vulkanModule);
    static void HookForVulkanExtensionSpoofing(HMODULE vulkanModule);
    static void HookForVulkanVRAMSpoofing(HMODULE vulkanModule);
};

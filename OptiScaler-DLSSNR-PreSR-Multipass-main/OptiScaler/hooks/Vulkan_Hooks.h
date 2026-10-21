#pragma once
#include "SysUtils.h"

class VulkanHooks
{
  public:
    static PFN_vkCreateSemaphore o_vkCreateSemaphore;
    static PFN_vkSignalSemaphore o_vkSignalSemaphore;
    static PFN_vkAntiLagUpdateAMD o_vkAntiLagUpdateAMD;

    static VkPhysicalDevice GetPhysicalDevice(VkDevice device);
    static VkInstance GetPhysicalDeviceInstance(VkPhysicalDevice physicalDevice);
    static bool GetPhysicalDeviceLuid(VkPhysicalDevice physicalDevice, LUID& luid);

    static void Hook(HMODULE vulkan1);
    static void Unhook();
};

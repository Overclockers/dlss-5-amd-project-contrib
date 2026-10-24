#include <windows.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/DlssNr_VkExtensions.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/include/detours/detours.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace
{
bool nvidia = false;
bool fail = false;
bool incomplete = false;
unsigned spoofCalls = 0;

__declspec(noinline) VkResult VKAPI_CALL DriverExtensions(VkPhysicalDevice, const char*, uint32_t* count,
                                                          VkExtensionProperties* props)
{
    if (fail)
        return VK_ERROR_INITIALIZATION_FAILED;
    const char* names[] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                            "VK_NVX_binary_import", "VK_NVX_image_view_handle" };
    const uint32_t size = nvidia ? 4 : 2;
    if (!props)
    {
        *count = size;
        return VK_SUCCESS;
    }
    const uint32_t copied = (std::min)(*count, size);
    for (uint32_t i = 0; i < copied; ++i)
    {
        props[i] = {};
        strcpy_s(props[i].extensionName, names[i]);
    }
    *count = copied;
    if (incomplete)
    {
        incomplete = false;
        return VK_INCOMPLETE;
    }
    return copied == size ? VK_SUCCESS : VK_INCOMPLETE;
}

PFN_vkEnumerateDeviceExtensionProperties original = DriverExtensions;

VkResult VKAPI_CALL SpoofExtensions(VkPhysicalDevice device, const char* layer, uint32_t* count,
                                    VkExtensionProperties* props)
{
    ++spoofCalls;
    const uint32_t capacity = props ? *count : 0;
    const auto result = original(device, layer, count, props);
    if (result != VK_SUCCESS)
        return result;
    const char* fake[] = { "VK_NVX_binary_import", "VK_NVX_image_view_handle",
                           "VK_NVX_multiview_per_view_attributes", "VK_NV_low_latency",
                           "VK_EXT_buffer_device_address" };
    const uint32_t realCount = *count;
    *count += 5;
    if (!props)
        return VK_SUCCESS;
    if (capacity < *count)
        return VK_INCOMPLETE;
    for (uint32_t i = 0; i < 5; ++i)
    {
        props[realCount + i] = {};
        strcpy_s(props[realCount + i].extensionName, fake[i]);
    }
    return VK_SUCCESS;
}
}

int main()
{
    using namespace DlssNr::VkExt;
    const auto device = reinterpret_cast<VkPhysicalDevice>(uintptr_t(1));
    assert(DetourTransactionBegin() == NO_ERROR);
    assert(DetourUpdateThread(GetCurrentThread()) == NO_ERROR);
    assert(DetourAttach(reinterpret_cast<PVOID*>(&original), SpoofExtensions) == NO_ERROR);
    assert(DetourTransactionCommit() == NO_ERROR);

    // A fresh export lookup sees the patched entry, just as in the BG3 crash.
    const auto gameFacing = SupportedDeviceExtensions(DriverExtensions, device);
    assert(gameFacing.size() == 7 && Contains(gameFacing, "VK_NVX_binary_import"));
    const unsigned before = spoofCalls;
    const auto real = SupportedDeviceExtensions(original, device);
    assert(real.size() == 2 && spoofCalls == before);
    assert(!Contains(real, "VK_NVX_binary_import") && !Contains(real, "VK_NVX_image_view_handle"));

    nvidia = true;
    incomplete = true;
    const auto native = SupportedDeviceExtensions(original, device);
    assert(native.size() == 4 && !incomplete);
    assert(Contains(native, "VK_NVX_binary_import") && Contains(native, "VK_NVX_image_view_handle"));
    fail = true;
    assert(SupportedDeviceExtensions(original, device).empty());
    assert(SupportedDeviceExtensions(nullptr, device).empty());
    assert(SupportedDeviceExtensions(original, VK_NULL_HANDLE).empty());

    assert(DetourTransactionBegin() == NO_ERROR);
    assert(DetourUpdateThread(GetCurrentThread()) == NO_ERROR);
    assert(DetourDetach(reinterpret_cast<PVOID*>(&original), SpoofExtensions) == NO_ERROR);
    assert(DetourTransactionCommit() == NO_ERROR);
    std::puts("Vulkan NR extensions: PASS (spoofed export vs driver trampoline, native NVX, retry, failure)");
}

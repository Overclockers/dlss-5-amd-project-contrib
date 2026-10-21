#include "pch.h"

#include "Vulkan_Hooks.h"

#include <Util.h>
#include <Config.h>
#include <SysUtils.h>

#include <menu/menu_overlay_vk.h>
#include <proxies/KernelBase_Proxy.h>
#include <upscalers/IFeature_Vk.h>

#include <misc/FrameLimit.h>
#include <misc/DeferredRelease.h>
#include "Reflex_Hooks.h"

#include <spoofing/Vulkan_Spoofing.h>

#include <vulkan/vulkan.h>

#include <dlssnr/DlssNr_VkExtensions.h>

#include <detours/detours.h>
#include <misc/IdentifyGpu.h>
#include <misc/VulkanCallScope.h>
#include <unordered_map>

#include "Hook_Utils.h"

// for menu rendering
static VkDevice _device = VK_NULL_HANDLE;
static VkInstance _instance = VK_NULL_HANDLE;
static VkPhysicalDevice _PD = VK_NULL_HANDLE;
static HWND _hwnd = nullptr;

static std::mutex _vkPresentMutex;
static std::mutex _vkApiVersionMutex;
static std::unordered_map<VkInstance, uint32_t> _instanceApiVersions;
static std::unordered_map<VkPhysicalDevice, VkInstance> _physicalDeviceInstances;
static std::unordered_map<VkDevice, VkPhysicalDevice> _devicePhysicalDevices;

PFN_vkCreateDevice o_vkCreateDevice = nullptr;
PFN_vkCreateInstance o_vkCreateInstance = nullptr;
PFN_vkCreateWin32SurfaceKHR o_vkCreateWin32SurfaceKHR = nullptr;
PFN_vkQueuePresentKHR o_QueuePresentKHR = nullptr;
PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR = nullptr;
static PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr = nullptr;
static PFN_vkDestroyInstance o_vkDestroyInstance = nullptr;
static PFN_vkDestroyDevice o_vkDestroyDevice = nullptr;
static PFN_vkEnumeratePhysicalDevices o_vkEnumeratePhysicalDevices = nullptr;
static PFN_vkEnumeratePhysicalDeviceGroups o_vkEnumeratePhysicalDeviceGroups = nullptr;
static PFN_vkEnumeratePhysicalDeviceGroupsKHR o_vkEnumeratePhysicalDeviceGroupsKHR = nullptr;

// Those aren't hooked, just grabbed for use
static PFN_vkGetPhysicalDeviceFeatures2 o_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkCreateSemaphore VulkanHooks::o_vkCreateSemaphore = nullptr;
PFN_vkSignalSemaphore VulkanHooks::o_vkSignalSemaphore = nullptr;
PFN_vkAntiLagUpdateAMD VulkanHooks::o_vkAntiLagUpdateAMD = nullptr;

// Forward declaration
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);

static void HookDevice(VkDevice InDevice)
{
    if (o_CreateSwapchainKHR != nullptr || ScopedSkipVulkanHooks::Active())
        return;

    LOG_FUNC();

    o_QueuePresentKHR = (PFN_vkQueuePresentKHR) (vkGetDeviceProcAddr(InDevice, "vkQueuePresentKHR"));
    o_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR) (vkGetDeviceProcAddr(InDevice, "vkCreateSwapchainKHR"));

    if (o_CreateSwapchainKHR)
    {
        LOG_DEBUG("Hooking VkDevice");

        // Hook
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_QueuePresentKHR != nullptr)
            DetourAttach(&(PVOID&) o_QueuePresentKHR, hkvkQueuePresentKHR);

        if (o_CreateSwapchainKHR != nullptr)
            DetourAttach(&(PVOID&) o_CreateSwapchainKHR, hkvkCreateSwapchainKHR);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook VkDevice, error code: {:X}", detourResult);
            o_QueuePresentKHR = nullptr;
            o_CreateSwapchainKHR = nullptr;
        }
    }
}

VALIDATE_HOOK(hkvkCreateWin32SurfaceKHR, PFN_vkCreateWin32SurfaceKHR)
static VkResult hkvkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                          const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    LOG_FUNC();

    auto result = o_vkCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

    auto procHwnd = Util::GetProcessWindow();
    LOG_DEBUG("procHwnd: {0:X}, swapchain hwnd: {1:X}", (UINT64) procHwnd, (UINT64) pCreateInfo->hwnd);

    if (result == VK_SUCCESS && !ScopedSkipVulkanHooks::Active())
    {
        MenuOverlayVk::DestroyVulkanObjects(false);

        _instance = instance;
        State::Instance().VulkanInstance = instance;
        LOG_DEBUG("_instance captured: {0:X}", (UINT64) _instance);
        _hwnd = pCreateInfo->hwnd;
        LOG_DEBUG("_hwnd captured: {0:X}", (UINT64) _hwnd);
    }

    LOG_FUNC_RESULT(result);

    return result;
}

static void TrackPhysicalDevice(VkInstance instance, VkPhysicalDevice physicalDevice)
{
    if (physicalDevice == VK_NULL_HANDLE || ScopedSkipVulkanHooks::Active() || ScopedCreatingD3DDevice::Active())
        return;

    std::scoped_lock lock(_vkApiVersionMutex);
    _physicalDeviceInstances[physicalDevice] = instance;
}

static uint32_t GetPhysicalDeviceApiVersion(VkPhysicalDevice physicalDevice, VkInstance* instance = nullptr)
{
    if (instance != nullptr)
        *instance = VK_NULL_HANDLE;

    std::scoped_lock lock(_vkApiVersionMutex);

    const auto physicalDeviceIt = _physicalDeviceInstances.find(physicalDevice);
    if (physicalDeviceIt != _physicalDeviceInstances.end())
    {
        if (instance != nullptr)
            *instance = physicalDeviceIt->second;

        const auto instanceIt = _instanceApiVersions.find(physicalDeviceIt->second);
        if (instanceIt != _instanceApiVersions.end())
            return instanceIt->second;
    }

    LOG_WARN("No Vulkan instance API version tracked for VkPhysicalDevice {0:X}; API version is unknown",
             (UINT64) physicalDevice);

    return 0;
}

static void UntrackInstance(VkInstance instance)
{
    std::scoped_lock lock(_vkApiVersionMutex);
    _instanceApiVersions.erase(instance);

    for (auto it = _physicalDeviceInstances.begin(); it != _physicalDeviceInstances.end();)
    {
        if (it->second == instance)
        {
            std::erase_if(_devicePhysicalDevices, [&](const auto& device) { return device.second == it->first; });
            it = _physicalDeviceInstances.erase(it);
        }
        else
            ++it;
    }
}

VALIDATE_HOOK(hkvkEnumeratePhysicalDevices, PFN_vkEnumeratePhysicalDevices)
static VkResult hkvkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount,
                                             VkPhysicalDevice* pPhysicalDevices)
{
    const auto result = o_vkEnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDeviceCount != nullptr &&
        pPhysicalDevices != nullptr)
    {
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i)
            TrackPhysicalDevice(instance, pPhysicalDevices[i]);
    }

    return result;
}

static void TrackPhysicalDeviceGroups(VkInstance instance, uint32_t groupCount,
                                      const VkPhysicalDeviceGroupProperties* groups)
{
    for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex)
    {
        const auto& group = groups[groupIndex];
        for (uint32_t deviceIndex = 0; deviceIndex < group.physicalDeviceCount; ++deviceIndex)
            TrackPhysicalDevice(instance, group.physicalDevices[deviceIndex]);
    }
}

VALIDATE_HOOK(hkvkEnumeratePhysicalDeviceGroups, PFN_vkEnumeratePhysicalDeviceGroups)
static VkResult hkvkEnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                                  VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties)
{
    const auto result =
        o_vkEnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDeviceGroupCount != nullptr &&
        pPhysicalDeviceGroupProperties != nullptr)
        TrackPhysicalDeviceGroups(instance, *pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);

    return result;
}

VALIDATE_HOOK(hkvkEnumeratePhysicalDeviceGroupsKHR, PFN_vkEnumeratePhysicalDeviceGroupsKHR)
static VkResult hkvkEnumeratePhysicalDeviceGroupsKHR(VkInstance instance, uint32_t* pPhysicalDeviceGroupCount,
                                                     VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties)
{
    const auto result =
        o_vkEnumeratePhysicalDeviceGroupsKHR(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);

    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && pPhysicalDeviceGroupCount != nullptr &&
        pPhysicalDeviceGroupProperties != nullptr)
        TrackPhysicalDeviceGroups(instance, *pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);

    return result;
}


VALIDATE_HOOK(hkvkCreateInstance, PFN_vkCreateInstance)
static VkResult hkvkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                   VkInstance* pInstance)
{
    if (ScopedSkipVulkanHooks::Active()) return o_vkCreateInstance(pCreateInfo, pAllocator, pInstance);

    LOG_FUNC();

    VkInstanceCreateInfo localCreateInfo {};
    memcpy(&localCreateInfo, pCreateInfo, sizeof(VkInstanceCreateInfo));

    const uint32_t requestedApiVersion =
        pCreateInfo->pApplicationInfo != nullptr && pCreateInfo->pApplicationInfo->apiVersion != 0
            ? pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;

    VulkanSpoofing::hkvkCreateInstance(&localCreateInfo, pAllocator, pInstance);

    VkResult result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_vkCreateInstance(&localCreateInfo, pAllocator, pInstance);
    }

    if (result == VK_SUCCESS)
    {
        {
            std::scoped_lock lock(_vkApiVersionMutex);
            _instanceApiVersions[*pInstance] = requestedApiVersion;
        }
        State::Instance().VulkanInstance = *pInstance;
        LOG_DEBUG("State::Instance().VulkanInstance captured: {0:X}", (UINT64) State::Instance().VulkanInstance);

#ifdef VULKAN_DEBUG_LAYER
        auto address = vkGetInstanceProcAddr(State::Instance().VulkanInstance, "vkCreateDebugUtilsMessengerEXT");
        auto vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT) address;
        VkDebugUtilsMessengerEXT debugMessenger;
        vkCreateDebugUtilsMessengerEXT(State::Instance().VulkanInstance, &VulkanSpoofing::debugCreateInfo, nullptr,
                                       &debugMessenger);
#endif
    }

    // Disabled to prevent unnecessary object release
    // if (result == VK_SUCCESS && !ScopedSkipVulkanHooks::Active())
    //{
    //     MenuOverlayVk::DestroyVulkanObjects(false);
    // }

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkvkDestroyInstance, PFN_vkDestroyInstance)
static void hkvkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
    UntrackInstance(instance);
    o_vkDestroyInstance(instance, pAllocator);
}

VALIDATE_HOOK(hkvkDestroyDevice, PFN_vkDestroyDevice)
static void hkvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
    {
        std::scoped_lock lock(_vkApiVersionMutex);
        _devicePhysicalDevices.erase(device);
    }
    o_vkDestroyDevice(device, pAllocator);
}

VALIDATE_HOOK(hkvkCreateDevice, PFN_vkCreateDevice)
static VkResult hkvkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    if (ScopedSkipVulkanHooks::Active()) return o_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);

    LOG_FUNC();

    VkDeviceCreateInfo localCreteInfo {};
    memcpy(&localCreteInfo, pCreateInfo, sizeof(VkDeviceCreateInfo));

    VkInstance physicalDeviceInstance = VK_NULL_HANDLE;
    const uint32_t requestedApiVersion = GetPhysicalDeviceApiVersion(physicalDevice, &physicalDeviceInstance);

    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2 = o_vkGetPhysicalDeviceFeatures2;

    if (requestedApiVersion < VK_API_VERSION_1_1)
    {
        getPhysicalDeviceFeatures2 = nullptr;

        if (physicalDeviceInstance != VK_NULL_HANDLE && o_vkGetInstanceProcAddr != nullptr)
        {
            getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                o_vkGetInstanceProcAddr(physicalDeviceInstance, "vkGetPhysicalDeviceFeatures2KHR"));
        }
    }

    // Check support for AntiLag before spoof
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceAntiLagFeaturesAMD antiLagFeatures = {};
    antiLagFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;

    features2.pNext = &antiLagFeatures;

    if (getPhysicalDeviceFeatures2)
    {
        getPhysicalDeviceFeatures2(physicalDevice, &features2);
        State::Instance().vkAntiLagSupported = antiLagFeatures.antiLag != 0;
    }

    VulkanDeviceFeatureState deviceFeatures(&localCreteInfo, getPhysicalDeviceFeatures2);
    VulkanSpoofing::hkvkCreateDevice(physicalDevice, &localCreteInfo, pAllocator, pDevice,
                                     ScopedCreatingD3DDevice::Active() ? nullptr : &deviceFeatures, requestedApiVersion);

    // Neural Rendering on Vulkan without a D3D12 bridge, or the reason it cannot be.
    //
    // The model needs two NVIDIA vendor extensions to load its kernels, a game never asks for them,
    // and a device's extension list cannot be changed after creation. This is the only moment it can
    // be arranged. Reported either way: if the answer is no, the log says so here rather than leaving
    // a create failure three layers down to be explained.
    //
    // Only appended when the feature is switched on, and only what the physical device already
    // offers -- asking for an extension a driver does not have makes vkCreateDevice fail and the game
    // not start.
    DlssNr::VkExt::Merged nrExtensions;

    if (Config::Instance()->DlssNrEnabled.value_or_default())
    {
        const auto supported = DlssNr::VkExt::SupportedDeviceExtensions(
            o_vkGetInstanceProcAddr, State::Instance().VulkanInstance, physicalDevice);

        nrExtensions.names.assign(localCreteInfo.ppEnabledExtensionNames,
                                  localCreteInfo.ppEnabledExtensionNames + localCreteInfo.enabledExtensionCount);

        std::string present, added, missing;

        for (const char* want : DlssNr::VkExt::kDevice)
        {
            const bool already = DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames,
                                                        localCreteInfo.enabledExtensionCount, want);

            if (already)
                present += std::string(present.empty() ? "" : ", ") + want;
            else if (!DlssNr::VkExt::Contains(supported, want))
                missing += std::string(missing.empty() ? "" : ", ") + want;
            else
            {
                nrExtensions.names.push_back(want);
                added += std::string(added.empty() ? "" : ", ") + want;
            }
        }

        LOG_INFO("DLSS-NR Vulkan: device offers {} extensions. game already enabled: [{}]. added here: "
                 "[{}]. NOT AVAILABLE: [{}]",
                 supported.size(), present.empty() ? "none" : present, added.empty() ? "none" : added,
                 missing.empty() ? "none" : missing);

        if (!missing.empty())
            LOG_WARN("DLSS-NR Vulkan: the native path is not possible on this device -- the model's kernels "
                     "cannot be loaded without the extensions listed as NOT AVAILABLE");

        if (!added.empty())
        {
            localCreteInfo.ppEnabledExtensionNames = nrExtensions.names.data();
            localCreteInfo.enabledExtensionCount = (uint32_t) nrExtensions.names.size();
        }
    }

    auto result = o_vkCreateDevice(physicalDevice, &localCreteInfo, pAllocator, pDevice);

    // Private NR devices returned above; creation cannot retire another live device.
    if (result == VK_SUCCESS && !ScopedCreatingD3DDevice::Active())
        DeferredRelease::RegisterOwner((uint64_t) *pDevice);

    if (result == VK_SUCCESS && !ScopedCreatingD3DDevice::Active())
    {
        std::scoped_lock lock(_vkApiVersionMutex);
        _devicePhysicalDevices[*pDevice] = physicalDevice;
    }

    if (Config::Instance()->DlssNrEnabled.value_or_default())
        LOG_INFO("DLSS-NR Vulkan: vkCreateDevice returned {} with {} extensions requested", (int) result,
                 localCreteInfo.enabledExtensionCount);

    if (result == VK_SUCCESS && Config::Instance()->OverlayMenu.value_or_default())
    {
        if (!ScopedSkipVulkanHooks::Active())
        {
            // Disabled to prevent unnecessary object release
            // MenuOverlayVk::DestroyVulkanObjects(false);

            _PD = physicalDevice;
            LOG_DEBUG("_PD captured: {0:X}", (UINT64) _PD);
            _device = *pDevice;
            LOG_DEBUG("_device captured: {0:X}", (UINT64) _device);
            HookDevice(_device);
        }

        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

        VkPhysicalDeviceIDProperties idProps {};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

        VkPhysicalDeviceProperties2 props2 {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idProps;

        vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

        if (idProps.deviceLUIDValid == VK_TRUE)
        {
            auto primaryGpu = IdentifyGpu::getPrimaryGpu();
            auto luid = (PLUID) idProps.deviceLUID;
            if (!IsEqualLUID(*luid, primaryGpu.luid))
                LOG_WARN("VkDevice created with non-primary GPU");
        }
    }

    if (State::Instance().vkAntiLagSupported)
    {
        if (result == VK_SUCCESS && o_vkGetDeviceProcAddr)
        {
            VulkanHooks::o_vkAntiLagUpdateAMD =
                (PFN_vkAntiLagUpdateAMD) o_vkGetDeviceProcAddr(*pDevice, "vkAntiLagUpdateAMD");
        }
        else
        {
            State::Instance().vkAntiLagSupported = false;
            LOG_WARN("Vulkan AntiLag can't be enabled");
        }
    }

#ifdef USE_QUEUE_SUBMIT_2_KHR
    if (result == VK_SUCCESS)
        hkvkGetDeviceProcAddr(*pDevice, "vkQueueSubmit2KHR");
#endif

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkvkQueuePresentKHR, PFN_vkQueuePresentKHR)
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    LOG_FUNC();

    if (auto feature = dynamic_cast<IFeature_Vk*>(State::Instance().currentFeature); feature != nullptr)
    {
        if (auto time = feature->ReadUpscalerTime(nullptr); time.has_value())
        {
            feature->ReadDetailedGpuTimes(nullptr, State::Instance().detailedGpuTimes);
            if (*time < 100.0)
            {
                std::scoped_lock lock(State::Instance().frameTimeMutex);
                State::Instance().upscaleTimes.push_back(*time);
                State::Instance().upscaleTimes.pop_front();
            }
        }
    }

    // ??? TODO: if we are hooking dxvk's vulkan calls then this present call could be either coming from dxvk or from a
    // native vk game
    if (!IdentifyGpu::getPrimaryGpu().usesDxvk)
        State::Instance().swapchainApi = Vulkan;

    // Tick feature to let it know if it's frozen
    if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        currentFeature->TickFrozenCheck();

    VkPresentInfoKHR localPresentInfo {};
    memcpy(&localPresentInfo, pPresentInfo, sizeof(VkPresentInfoKHR));

    // render menu if needed
    if (!MenuOverlayVk::QueuePresent(queue, &localPresentInfo))
    {
        LOG_ERROR("QueuePresent: false!");
        return VK_ERROR_OUT_OF_DATE_KHR;
    }

    ReflexHooks::update(false, true);

    // original call
    ScopedVulkanCreatingSC scopedVulkanCreatingSC {};
    auto result = o_QueuePresentKHR(queue, &localPresentInfo);

    // DXVK's DXGI present already counts this frame.
    if (!IdentifyGpu::getPrimaryGpu().usesDxvk)
        DeferredRelease::Process();

    // Unsure about Vulkan Reflex fps limit and if that could be causing an issue here
    if (!State::Instance().reflexLimitsFps)
        FrameLimit::sleep(false);

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkCreateSwapchainKHR, PFN_vkCreateSwapchainKHR)
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    ScopedVulkanCreatingSC scopedVulkanCreatingSC {};
    VkResult result = VK_SUCCESS;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && *pSwapchain != VK_NULL_HANDLE &&
        !ScopedSkipVulkanHooks::Active())
    {
        State::Instance().screenWidth = static_cast<float>(pCreateInfo->imageExtent.width);
        State::Instance().screenHeight = static_cast<float>(pCreateInfo->imageExtent.height);

        // The same question the DXGI side asks: what does one unit of this buffer mean?
        //
        // EXTENDED_SRGB_LINEAR is scRGB, 1.0 = 80 nits. HDR10_ST2084 is PQ, 1.0 = 10000 nits. Both
        // are absolute, so in either the white point is arithmetic rather than a reading -- which
        // matters most for the games that supply no exposure texture, since nothing else answers for
        // them. Logged, not yet used.
        {
            static VkColorSpaceKHR lastSpace = (VkColorSpaceKHR) -1;

            if (pCreateInfo->imageColorSpace != lastSpace)
            {
                lastSpace = pCreateInfo->imageColorSpace;

                const char* name = "other";
                const char* meaning = "relative -- no scale to be had";

                switch (pCreateInfo->imageColorSpace)
                {
                case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
                    name = "scRGB (extended sRGB, linear)";
                    meaning = "absolute: 1.0 = 80 nits, so 203-nit paper white = 2.5375";
                    break;
                case VK_COLOR_SPACE_HDR10_ST2084_EXT:
                    name = "PQ / ST.2084 (HDR10)";
                    meaning = "absolute: 1.0 = 10000 nits, so 203-nit paper white = 0.0203";
                    break;
                case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
                    name = "sRGB (SDR)";
                    break;
                case VK_COLOR_SPACE_HDR10_HLG_EXT:
                    name = "HLG";
                    break;
                default:
                    break;
                }

                LOG_INFO("DLSS-NR: swapchain colour space {} -- {} ({}), format {}",
                         (int) pCreateInfo->imageColorSpace, name, meaning, (int) pCreateInfo->imageFormat);
            }
        }

        LOG_DEBUG("if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && pSwapchain != "
                  "VK_NULL_HANDLE)");

        _device = device;
        LOG_DEBUG("_device captured: {0:X}", (UINT64) _device);

        MenuOverlayVk::CreateSwapchain(device, _PD, _instance, _hwnd, pCreateInfo, pAllocator, pSwapchain);
    }

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkGetInstanceProcAddr, PFN_vkGetInstanceProcAddr)
PFN_vkVoidFunction hkvkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    auto orgFunc = o_vkGetInstanceProcAddr(instance, pName);

    if (orgFunc == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    auto procName = std::string(pName);

    if (procName == std::string("vkCreateInstance"))
    {
        if (o_vkCreateInstance == nullptr)
            o_vkCreateInstance = (PFN_vkCreateInstance) orgFunc;

        LOG_DEBUG("vkCreateInstance");
        return (PFN_vkVoidFunction) hkvkCreateInstance;
    }
    else if (procName == std::string("vkCreateDevice"))
    {
        if (o_vkCreateDevice == nullptr)
            o_vkCreateDevice = (PFN_vkCreateDevice) orgFunc;

        LOG_DEBUG("vkCreateDevice");
        return (PFN_vkVoidFunction) hkvkCreateDevice;
    }
    if (procName == "vkDestroyInstance")
    {
        if (o_vkDestroyInstance == nullptr)
            o_vkDestroyInstance = (PFN_vkDestroyInstance) orgFunc;
        return (PFN_vkVoidFunction) hkvkDestroyInstance;
    }
    if (procName == "vkEnumeratePhysicalDevices")
    {
        if (o_vkEnumeratePhysicalDevices == nullptr)
            o_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices) orgFunc;
        return (PFN_vkVoidFunction) hkvkEnumeratePhysicalDevices;
    }
    if (procName == "vkEnumeratePhysicalDeviceGroups")
    {
        if (o_vkEnumeratePhysicalDeviceGroups == nullptr)
            o_vkEnumeratePhysicalDeviceGroups = (PFN_vkEnumeratePhysicalDeviceGroups) orgFunc;
        return (PFN_vkVoidFunction) hkvkEnumeratePhysicalDeviceGroups;
    }
    if (procName == "vkEnumeratePhysicalDeviceGroupsKHR")
    {
        if (o_vkEnumeratePhysicalDeviceGroupsKHR == nullptr)
            o_vkEnumeratePhysicalDeviceGroupsKHR = (PFN_vkEnumeratePhysicalDeviceGroupsKHR) orgFunc;
        return (PFN_vkVoidFunction) hkvkEnumeratePhysicalDeviceGroupsKHR;
    }
    if (procName == "vkDestroyDevice")
    {
        if (o_vkDestroyDevice == nullptr)
            o_vkDestroyDevice = (PFN_vkDestroyDevice) orgFunc;
        return (PFN_vkVoidFunction) hkvkDestroyDevice;
    }

    auto result = VulkanSpoofing::hkvkGetInstanceProcAddr(orgFunc, pName);
    if (result != VK_NULL_HANDLE)
        return result;

    return orgFunc;
}

VALIDATE_HOOK(hkvkGetDeviceProcAddr, PFN_vkGetDeviceProcAddr)
PFN_vkVoidFunction hkvkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    auto orgFunc = o_vkGetDeviceProcAddr(device, pName);

    if (orgFunc == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    auto procName = std::string(pName);

    if (procName == std::string("vkCreateInstance"))
    {
        if (o_vkCreateInstance == nullptr)
            o_vkCreateInstance = (PFN_vkCreateInstance) orgFunc;

        LOG_DEBUG("vkCreateInstance");
        return (PFN_vkVoidFunction) hkvkCreateInstance;
    }
    else if (procName == std::string("vkCreateDevice"))
    {
        if (o_vkCreateDevice == nullptr)
            o_vkCreateDevice = (PFN_vkCreateDevice) orgFunc;

        LOG_DEBUG("vkCreateDevice");
        return (PFN_vkVoidFunction) hkvkCreateDevice;
    }
    if (procName == "vkDestroyDevice")
    {
        if (o_vkDestroyDevice == nullptr)
            o_vkDestroyDevice = (PFN_vkDestroyDevice) orgFunc;
        return (PFN_vkVoidFunction) hkvkDestroyDevice;
    }

    auto result = VulkanSpoofing::hkvkGetDeviceProcAddr(orgFunc, pName);
    if (result != VK_NULL_HANDLE)
        return result;

    return orgFunc;
}

VkPhysicalDevice VulkanHooks::GetPhysicalDevice(VkDevice device)
{
    std::scoped_lock lock(_vkApiVersionMutex);
    auto it = _devicePhysicalDevices.find(device);
    return it != _devicePhysicalDevices.end() ? it->second : VK_NULL_HANDLE;
}

VkInstance VulkanHooks::GetPhysicalDeviceInstance(VkPhysicalDevice physicalDevice)
{
    std::scoped_lock lock(_vkApiVersionMutex);
    auto it = _physicalDeviceInstances.find(physicalDevice);
    return it != _physicalDeviceInstances.end() ? it->second : VK_NULL_HANDLE;
}

bool VulkanHooks::GetPhysicalDeviceLuid(VkPhysicalDevice physicalDevice, LUID& luid)
{
    auto instance = GetPhysicalDeviceInstance(physicalDevice);
    if (instance == VK_NULL_HANDLE || o_vkGetInstanceProcAddr == nullptr)
        return false;
    auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        o_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (getProperties == nullptr)
        getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            o_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
    if (getProperties == nullptr)
        return false;
    VkPhysicalDeviceIDProperties identity { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &identity;
    ScopedSkipSpoofingGlobal skipSpoofing;
    getProperties(physicalDevice, &properties);
    if (!identity.deviceLUIDValid)
        return false;
    static_assert(sizeof(luid) == VK_LUID_SIZE);
    memcpy(&luid, identity.deviceLUID, sizeof(luid));
    return true;
}

void VulkanHooks::Hook(HMODULE vulkan1)
{
    if (vulkanModule == nullptr)
        vulkanModule = vulkan1;

    VulkanSpoofing::HookForVulkanSpoofing(vulkan1);
    VulkanSpoofing::HookForVulkanExtensionSpoofing(vulkan1);
    VulkanSpoofing::HookForVulkanVRAMSpoofing(vulkan1);

    if (o_vkCreateDevice != nullptr)
        return;

    FARPROC address = nullptr;

    o_vkCreateDevice = (PFN_vkCreateDevice) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateDevice");
    o_vkCreateInstance = (PFN_vkCreateInstance) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateInstance");
    o_vkDestroyInstance = (PFN_vkDestroyInstance) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkDestroyInstance");
    o_vkDestroyDevice = (PFN_vkDestroyDevice) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkDestroyDevice");
    o_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkEnumeratePhysicalDevices");
    o_vkEnumeratePhysicalDeviceGroups = (PFN_vkEnumeratePhysicalDeviceGroups) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkEnumeratePhysicalDeviceGroups");
    o_vkEnumeratePhysicalDeviceGroupsKHR = (PFN_vkEnumeratePhysicalDeviceGroupsKHR) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkEnumeratePhysicalDeviceGroupsKHR");

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetInstanceProcAddr");
    o_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetDeviceProcAddr");
    o_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateWin32SurfaceKHR");
    o_vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR) address;

    // address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCmdPipelineBarrier");
    // o_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetPhysicalDeviceFeatures2");
    o_vkGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateSemaphore");
    o_vkCreateSemaphore = (PFN_vkCreateSemaphore) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkSignalSemaphore");
    o_vkSignalSemaphore = (PFN_vkSignalSemaphore) address;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_vkCreateDevice != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateDevice, hkvkCreateDevice);

    if (o_vkGetInstanceProcAddr != nullptr)
        DetourAttach(&(PVOID&) o_vkGetInstanceProcAddr, hkvkGetInstanceProcAddr);

    if (o_vkGetDeviceProcAddr != nullptr)
        DetourAttach(&(PVOID&) o_vkGetDeviceProcAddr, hkvkGetDeviceProcAddr);

    if (o_vkCreateInstance != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateInstance, hkvkCreateInstance);
    if (o_vkDestroyInstance != nullptr)
        DetourAttach(&(PVOID&) o_vkDestroyInstance, hkvkDestroyInstance);
    if (o_vkDestroyDevice != nullptr)
        DetourAttach(&(PVOID&) o_vkDestroyDevice, hkvkDestroyDevice);
    if (o_vkEnumeratePhysicalDevices != nullptr)
        DetourAttach(&(PVOID&) o_vkEnumeratePhysicalDevices, hkvkEnumeratePhysicalDevices);
    if (o_vkEnumeratePhysicalDeviceGroups != nullptr)
        DetourAttach(&(PVOID&) o_vkEnumeratePhysicalDeviceGroups, hkvkEnumeratePhysicalDeviceGroups);
    if (o_vkEnumeratePhysicalDeviceGroupsKHR != nullptr)
        DetourAttach(&(PVOID&) o_vkEnumeratePhysicalDeviceGroupsKHR, hkvkEnumeratePhysicalDeviceGroupsKHR);

    if (o_vkCreateWin32SurfaceKHR != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateWin32SurfaceKHR, hkvkCreateWin32SurfaceKHR);

    // if (o_vkCmdPipelineBarrier != nullptr)
    //     DetourAttach(&(PVOID&) o_vkCmdPipelineBarrier, hkvkCmdPipelineBarrier);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to hook Vulkan, error code: {:X}", detourResult);
        o_vkCreateDevice = nullptr;
        o_vkCreateInstance = nullptr;
        o_vkGetInstanceProcAddr = nullptr;
        o_vkGetDeviceProcAddr = nullptr;
        o_vkCreateWin32SurfaceKHR = nullptr;
        // o_vkCmdPipelineBarrier = nullptr;
        o_vkDestroyInstance = nullptr;
        o_vkDestroyDevice = nullptr;
        o_vkEnumeratePhysicalDevices = nullptr;
        o_vkEnumeratePhysicalDeviceGroups = nullptr;
        o_vkEnumeratePhysicalDeviceGroupsKHR = nullptr;
        std::scoped_lock lock(_vkApiVersionMutex);
        _instanceApiVersions.clear();
        _physicalDeviceInstances.clear();
        _devicePhysicalDevices.clear();
    }
}

void VulkanHooks::Unhook()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_QueuePresentKHR != nullptr)
        DetourDetach(&(PVOID&) o_QueuePresentKHR, hkvkQueuePresentKHR);

    if (o_CreateSwapchainKHR != nullptr)
        DetourDetach(&(PVOID&) o_CreateSwapchainKHR, hkvkCreateSwapchainKHR);

    if (o_vkCreateDevice != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateDevice, hkvkCreateDevice);

    if (o_vkCreateInstance != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateInstance, hkvkCreateInstance);
    if (o_vkDestroyInstance != nullptr)
        DetourDetach(&(PVOID&) o_vkDestroyInstance, hkvkDestroyInstance);
    if (o_vkDestroyDevice != nullptr)
        DetourDetach(&(PVOID&) o_vkDestroyDevice, hkvkDestroyDevice);
    if (o_vkEnumeratePhysicalDevices != nullptr)
        DetourDetach(&(PVOID&) o_vkEnumeratePhysicalDevices, hkvkEnumeratePhysicalDevices);
    if (o_vkEnumeratePhysicalDeviceGroups != nullptr)
        DetourDetach(&(PVOID&) o_vkEnumeratePhysicalDeviceGroups, hkvkEnumeratePhysicalDeviceGroups);
    if (o_vkEnumeratePhysicalDeviceGroupsKHR != nullptr)
        DetourDetach(&(PVOID&) o_vkEnumeratePhysicalDeviceGroupsKHR, hkvkEnumeratePhysicalDeviceGroupsKHR);

    if (o_vkCreateWin32SurfaceKHR != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateWin32SurfaceKHR, hkvkCreateWin32SurfaceKHR);

    // if (o_vkCmdPipelineBarrier != nullptr)
    //     DetourDetach(&(PVOID&) o_vkCmdPipelineBarrier, hkvkCmdPipelineBarrier);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Vulkan, error code: {:X}", detourResult);
    }
    else
    {
        o_QueuePresentKHR = nullptr;
        o_CreateSwapchainKHR = nullptr;
        o_vkCreateDevice = nullptr;
        o_vkCreateInstance = nullptr;
        o_vkGetInstanceProcAddr = nullptr;
        o_vkGetDeviceProcAddr = nullptr;
        o_vkCreateWin32SurfaceKHR = nullptr;
        // o_vkCmdPipelineBarrier = nullptr;
        o_vkDestroyInstance = nullptr;
        o_vkDestroyDevice = nullptr;
        o_vkEnumeratePhysicalDevices = nullptr;
        o_vkEnumeratePhysicalDeviceGroups = nullptr;
        o_vkEnumeratePhysicalDeviceGroupsKHR = nullptr;
        std::scoped_lock lock(_vkApiVersionMutex);
        _instanceApiVersions.clear();
        _physicalDeviceInstances.clear();
        _devicePhysicalDevices.clear();
    }
}

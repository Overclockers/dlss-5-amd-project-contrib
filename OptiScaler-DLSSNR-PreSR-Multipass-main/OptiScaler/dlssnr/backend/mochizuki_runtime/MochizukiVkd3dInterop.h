#pragma once
// vkd3d-proton's interop interface (include/vkd3d_device_vkd3d_ext.idl, ID3D12DXVKInteropDevice), declared here
// in its IDL order: the D3D12 device of a game under Proton answers QueryInterface for it, and nothing else does.
// Only the methods of the first version are used: GetVulkanHandles, GetVulkanQueueInfo, GetVulkanResourceInfo,
// LockCommandQueue and UnlockCommandQueue (libs/vkd3d/device_vkd3d_ext.c). LockCommandQueue drains the D3D12
// queue's pending submissions before it takes the vkd3d queue's mutex; UnlockCommandQueue submits vkd3d's own
// timeline step and releases it.
#include <d3d12.h>
#include <vulkan/vulkan.h>

MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd")
ID3D12DXVKInteropDevice : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void** object) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT* extension_count, const char** extensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT* extension_count, const char** extensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** features) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance* vk_instance, VkPhysicalDevice* vk_physical_device,
                                                       VkDevice* vk_device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue* queue, VkQueue* vk_queue,
                                                         UINT32* vk_queue_family) = 0;
    virtual void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                                                        VkImageLayout* vk_layout) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource* resource, UINT64* vk_handle,
                                                            UINT64* buffer_offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue* queue) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue* queue) = 0;
};

// Pinned Linux PE interop, cc27501d: query the current native list, never a
// cached proxy generation. GetVulkanHandle does not begin/end the recording.
MIDL_INTERFACE("77a86b09-2bea-4801-b89a-37648e104af1")
ID3D12GraphicsCommandListExt : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandle(VkCommandBuffer* command) = 0;
    virtual HRESULT STDMETHODCALLTYPE LaunchCubinShader(void* handle, UINT32 x, UINT32 y, UINT32 z,
                                                       const void* params, UINT32 size) = 0;
};

MIDL_INTERFACE("902d8115-59eb-4406-9518-fe00f991ee65")
ID3D12DXVKInteropDevice1 : public ID3D12DXVKInteropDevice
{
    virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo1(ID3D12Resource* resource, UINT64* handle,
                                                           UINT64* offset, VkFormat* format) = 0;
};

namespace mochizuki_vkd3d
{
// Wine's ntdll exports wine_get_version; Windows' never does.
inline bool UnderWine()
{
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

// vkd3d-proton calls winevulkan.dll directly, so the handles it hands over are winevulkan's. This module calls
// Vulkan through vulkan-1.dll. Proton's own vulkan-1.dll only forwards its exports to winevulkan.dll, so the two
// resolve to the same functions. With Proton's nativevulkanloader option (vulkan-1=n, which Steam sets for some
// games) vulkan-1.dll is a Khronos loader that would take winevulkan's handles for its own and crash the first
// call. Returns an empty string when vulkan-1.dll reaches winevulkan.dll, else why the direct path must not be used.
// The Linux route rebinds every entry point to winevulkan.dll instead (c71b0f4, nr_vk_load); here the direct path is
// declined and the own-device route (with its circuit breaker) stays, as before this path existed.
inline const char* EntryPointMismatch()
{
    const HMODULE wine = GetModuleHandleW(L"winevulkan.dll");
    if (!wine)
        return "winevulkan.dll is not loaded";
    // This module's vulkan-1.dll imports are delay-loaded: the DLL may not be in yet. Loading it is what the first
    // call would do anyway (same name, same search).
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    if (!loader)
        loader = LoadLibraryW(L"vulkan-1.dll");
    if (!loader)
        return "vulkan-1.dll cannot be loaded";
    for (const char* name : { "vkQueueSubmit", "vkCreateBuffer", "vkGetDeviceProcAddr" })
    {
        const FARPROC viaLoader = GetProcAddress(loader, name);
        const FARPROC viaWine = GetProcAddress(wine, name);
        if (!viaLoader || viaLoader != viaWine)
            return "vulkan-1.dll is a native Vulkan loader (nativevulkanloader), not Proton's winevulkan forwarder";
    }
    return "";
}

// Holds a D3D12 queue's vkd3d queue (LockCommandQueue) until destroyed.
struct QueueHold
{
    ID3D12DXVKInteropDevice* interop {};
    ID3D12CommandQueue* queue {};
    QueueHold() = default;
    QueueHold(const QueueHold&) = delete;
    QueueHold& operator=(const QueueHold&) = delete;
    ~QueueHold()
    {
        if (interop && queue)
            interop->UnlockCommandQueue(queue);
    }
};
} // namespace mochizuki_vkd3d

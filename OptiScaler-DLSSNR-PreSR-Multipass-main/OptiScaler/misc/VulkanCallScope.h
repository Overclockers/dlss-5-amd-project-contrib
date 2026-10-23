#pragma once

// These describe the current synchronous call stack, not process-wide state.
// A menu/GPU probe must never clear another thread's private runtime protection,
// nor hide a native Vulkan game's concurrent creation. New worker threads must
// establish their own scope if they perform an internal device creation.
class ScopedSkipVulkanHooks
{
    inline static thread_local unsigned depth = 0;

  public:
    ScopedSkipVulkanHooks() noexcept { ++depth; }
    ~ScopedSkipVulkanHooks() { --depth; }
    ScopedSkipVulkanHooks(const ScopedSkipVulkanHooks&) = delete;
    ScopedSkipVulkanHooks& operator=(const ScopedSkipVulkanHooks&) = delete;
    static bool Active() noexcept { return depth != 0; }
};

// Kept distinct: a D3D translation call suppresses Vulkan spoofing, while an
// explicit SkipVulkanHooks scope also suppresses game device/overlay capture.
class ScopedCreatingD3DDevice
{
    inline static thread_local unsigned depth = 0;

  public:
    ScopedCreatingD3DDevice() noexcept { ++depth; }
    ~ScopedCreatingD3DDevice() { --depth; }
    ScopedCreatingD3DDevice(const ScopedCreatingD3DDevice&) = delete;
    ScopedCreatingD3DDevice& operator=(const ScopedCreatingD3DDevice&) = delete;
    static bool Active() noexcept { return depth != 0; }
};

// The game D3D12 hook also uses ScopedCreatingD3DDevice to suppress GPU spoofing.
// Distinguish that translation from private probes without removing its guard.
class ScopedGameD3D12Device
{
    inline static thread_local unsigned depth = 0;
    bool entered;
  public:
    ScopedGameD3D12Device() noexcept
        : entered(!ScopedSkipVulkanHooks::Active() && (!ScopedCreatingD3DDevice::Active() || Active()))
    {
        if (entered) ++depth;
    }
    ~ScopedGameD3D12Device() { if (entered) --depth; }
    ScopedGameD3D12Device(const ScopedGameD3D12Device&) = delete;
    ScopedGameD3D12Device& operator=(const ScopedGameD3D12Device&) = delete;
    static bool Active() noexcept { return depth != 0; }
};

inline bool VulkanInteropGameCreation() noexcept
{
    return !ScopedSkipVulkanHooks::Active() &&
           (!ScopedCreatingD3DDevice::Active() || ScopedGameD3D12Device::Active());
}

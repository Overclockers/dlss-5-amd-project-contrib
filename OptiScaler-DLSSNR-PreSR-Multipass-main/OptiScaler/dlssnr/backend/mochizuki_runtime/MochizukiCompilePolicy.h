#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>

namespace mz::compile
{
// A driver identity, not a user compiler preference: RADV must never compile
// the Windows cooperative-matrix network, even after a failed Linux build.
enum class CompilePolicy
{
    WindowsDefault,
    LinuxSpirvDriverCompile
};

constexpr CompilePolicy PolicyForDriver(uint32_t driverId) noexcept
{
    // VK_DRIVER_ID_MESA_RADV. Keep this header usable without Vulkan or a GPU.
    return driverId == 3 ? CompilePolicy::LinuxSpirvDriverCompile : CompilePolicy::WindowsDefault;
}

constexpr bool Mandatory(CompilePolicy policy) noexcept
{
    return policy == CompilePolicy::LinuxSpirvDriverCompile;
}

inline void GuardWindowsFallback(CompilePolicy policy)
{
    if (Mandatory(policy))
        throw std::runtime_error("RADV requires the Linux SPIR-V network; Windows network fallback is forbidden");
}

// Configure on the thread actually constructing the network. Reset is required
// to be noexcept and runs after success, failed construction, or failed setup.
// Capturing the policy explicitly avoids inheriting another thread's TLS mode.
template<class Reset>
class BackgroundScope
{
public:
    template<class Configure>
    BackgroundScope(CompilePolicy policy, Configure&& configure, Reset reset) : reset_(std::move(reset))
    {
        static_assert(noexcept(std::declval<Reset&>()()), "compiler cleanup must not throw");
        try
        {
            std::forward<Configure>(configure)(policy);
        }
        catch (...)
        {
            reset_();
            throw;
        }
    }

    ~BackgroundScope() noexcept { reset_(); }
    BackgroundScope(const BackgroundScope&) = delete;
    BackgroundScope& operator=(const BackgroundScope&) = delete;

private:
    Reset reset_;
};

template<class Configure, class Reset>
BackgroundScope(CompilePolicy, Configure&&, Reset) -> BackgroundScope<Reset>;
}

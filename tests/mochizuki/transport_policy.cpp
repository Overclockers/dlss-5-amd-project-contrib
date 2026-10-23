// CPU-only coverage of the production own-device probe sizing/failure boundary.
#include "dlssnr/backend/mochizuki_runtime/MochizukiTransportPolicy.h"
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main() try
{
    using namespace mz::transport;
    require(BufferBytes(1, true) == 65536, "Wine probe is not minimal");
    require(BufferBytes(65536, true) == 65536, "Wine probe grew an aligned buffer");
    require(BufferBytes(65537, true) == 131072, "buffer placement alignment lost");
    require(BufferBytes(65536, false) == (uint64_t(16) << 20), "Windows shared-handle floor changed");
    require(BufferBytes((uint64_t(16) << 20) + 1, false) == (uint64_t(16) << 20) + 65536,
            "large Windows buffer alignment changed");
    require(FixedImportFailure(true, true, VK_ERROR_OUT_OF_HOST_MEMORY),
            "Proton unsupported probe allocation became a memory-pressure retry");
    require(!FixedImportFailure(true, false, VK_ERROR_OUT_OF_HOST_MEMORY),
            "a proven path's larger allocation lost its host-memory retry");
    for (bool probe : {false, true})
    {
        require(FixedImportFailure(true, probe, VK_ERROR_INVALID_EXTERNAL_HANDLE), "unsupported Proton handle not latched");
        require(FixedImportFailure(true, probe, VK_ERROR_FEATURE_NOT_PRESENT), "missing Proton import feature not latched");
        for (VkResult result : {VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST, VK_TIMEOUT})
            require(!FixedImportFailure(true, probe, result), "success, pressure or device loss became fixed interop failure");
        for (VkResult result : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                VK_ERROR_INVALID_EXTERNAL_HANDLE, VK_ERROR_FEATURE_NOT_PRESENT})
            require(!FixedImportFailure(false, probe, result), "Windows error/retry classification changed");
    }
    std::puts("TRANSPORT_POLICY_OK");
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}

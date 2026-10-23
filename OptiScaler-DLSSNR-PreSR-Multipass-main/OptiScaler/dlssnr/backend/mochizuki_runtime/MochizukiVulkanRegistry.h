#pragma once
#include "MochizukiVulkanDevice.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mz::device
{
// Host-only registry. A successful create publishes the final enabled contract;
// destroy erases it before the driver can reuse the handle. Runtime gets copies.
class Registry
{
    struct Record
    {
        Enabled enabled;
        std::vector<std::string> extensions;
    };
    std::mutex mutex;
    std::unordered_map<uint64_t, Record> records;
public:
    void Created(VkDevice device, const VkDeviceCreateInfo& info)
    {
        Record record;
        record.enabled.extensions = ExtensionBits(info.enabledExtensionCount, info.ppEnabledExtensionNames);
        record.enabled.features = FeatureBits(info.pNext);
        record.extensions.reserve(info.enabledExtensionCount);
        for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
            record.extensions.emplace_back(info.ppEnabledExtensionNames[i]);
        std::lock_guard lock(mutex);
        records.insert_or_assign(reinterpret_cast<uint64_t>(device), std::move(record));
    }
    void Destroyed(VkDevice device)
    {
        std::lock_guard lock(mutex);
        records.erase(reinterpret_cast<uint64_t>(device));
    }
    int32_t Get(uint64_t device, Enabled* out)
    {
        if (!out || out->struct_size != sizeof(Enabled)) return 0;
        std::lock_guard lock(mutex);
        const auto record = records.find(device);
        if (record == records.end()) return 0;
        *out = record->second.enabled;
        return 1;
    }
};
inline Registry& CreatedDevices()
{
    static Registry registry;
    return registry;
}
inline int32_t QueryCreatedDevice(uint64_t device, Enabled* out)
{
    return CreatedDevices().Get(device, out);
}
}

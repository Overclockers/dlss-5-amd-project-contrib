#include "pch.h"
#include "DeferredRelease.h"

#include <State.h>

#include <mutex>
#include <vector>
#include <unordered_map>
#include <limits>

namespace
{
constexpr uint64_t MinPresents = 8;
constexpr auto MinAge = std::chrono::milliseconds(500);

// Without hooked presents only the age tells when the GPU is done
constexpr auto FallbackAge = std::chrono::seconds(2);

struct Entry
{
    std::function<void()> destroy;
    uint64_t owner = 0;
    uint64_t generation = 0;
    uint64_t present = 0;
    std::chrono::steady_clock::time_point time;
};

std::mutex mutex;
std::vector<Entry> entries;
uint64_t presentCounter = 0;

struct OwnerState
{
    uint64_t generation = 0;
    bool active = false;
};
std::unordered_map<uint64_t, OwnerState> owners;
// Gate only owned callbacks. Device destruction drains a running callback and
// invalidates extracted-but-not-yet-dispatched callbacks before destroying VkDevice.
std::recursive_mutex dispatchMutex;
} // namespace

void DeferredRelease::Enqueue(std::function<void()> destroy, uint64_t owner)
{
    if (!destroy)
        return;

    // The process is going away, the OS cleans up
    if (State::Instance().isShuttingDown)
        return;

    std::lock_guard lock(mutex);
    uint64_t generation = 0;
    if (owner != 0)
    {
        const auto it = owners.find(owner);
        if (it != owners.end())
        {
            if (!it->second.active)
                return;
            generation = it->second.generation;
        }
    }
    entries.push_back({ std::move(destroy), owner, generation, presentCounter, std::chrono::steady_clock::now() });
}

void DeferredRelease::RegisterOwner(uint64_t owner)
{
    if (owner == 0)
        return;
    std::lock_guard dispatchLock(dispatchMutex);
    std::lock_guard lock(mutex);
    auto& state = owners[owner];
    if (state.generation == std::numeric_limits<uint64_t>::max())
    {
        state.active = false;
        LOG_ERROR("Deferred owner generation exhausted");
        return;
    }
    ++state.generation;
    state.active = true;
}

void DeferredRelease::DropOwnersExcept(uint64_t keep)
{
    std::lock_guard dispatchLock(dispatchMutex);
    std::lock_guard lock(mutex);

    const auto dropped =
        std::erase_if(entries, [keep](const Entry& entry) { return entry.owner != 0 && entry.owner != keep; });

    if (dropped > 0)
        LOG_WARN("Dropped {} deferred objects of a previous device", dropped);
}

void DeferredRelease::DropOwner(uint64_t owner)
{
    if (owner == 0)
        return;
    std::lock_guard dispatchLock(dispatchMutex);
    std::lock_guard lock(mutex);
    if (auto it = owners.find(owner); it != owners.end())
        it->second.active = false;
    const auto dropped = std::erase_if(entries, [owner](const Entry& entry) { return entry.owner == owner; });
    if (dropped > 0)
        LOG_WARN("Dropped {} deferred objects of a destroyed device", dropped);
}

static void ReleaseReady(bool present)
{
    if (State::Instance().isShuttingDown)
        return;

    std::vector<Entry> ready;

    {
        std::lock_guard lock(mutex);

        if (present)
            presentCounter++;

        if (entries.empty())
            return;

        const auto now = std::chrono::steady_clock::now();

        std::erase_if(entries,
                      [&](Entry& entry)
                      {
                          const auto age = now - entry.time;
                          const bool presentsPassed = presentCounter - entry.present >= MinPresents && age >= MinAge;

                          // Presents are only counted when the swapchain is hooked
                          if (!presentsPassed && age < FallbackAge)
                              return false;

                          ready.push_back(std::move(entry));
                          return true;
                      });
    }

    for (auto& entry : ready)
    {
        if (entry.owner == 0)
        {
            entry.destroy();
            continue;
        }
        std::lock_guard dispatchLock(dispatchMutex);
        {
            std::lock_guard lock(mutex);
            const auto it = owners.find(entry.owner);
            if (it != owners.end() && (!it->second.active || it->second.generation != entry.generation))
                continue;
        }
        entry.destroy();
    }

    if (!ready.empty())
        LOG_DEBUG("Released {} deferred objects", ready.size());
}

void DeferredRelease::Process() { ReleaseReady(true); }

void DeferredRelease::Collect() { ReleaseReady(false); }

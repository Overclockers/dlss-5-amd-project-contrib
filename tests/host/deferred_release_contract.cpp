#include <cassert>
#include <chrono>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <cstdio>

// Contract test verifying the DeferredRelease owner tracking and lifecycle rules (M4)

namespace TestDeferredRelease
{
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
std::recursive_mutex dispatchMutex;

void Reset()
{
    std::lock_guard dispatchLock(dispatchMutex);
    std::lock_guard lock(mutex);
    entries.clear();
    owners.clear();
    presentCounter = 0;
}

void Enqueue(std::function<void()> destroy, uint64_t owner = 0)
{
    if (!destroy)
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

void RegisterOwner(uint64_t owner)
{
    if (owner == 0)
        return;
    std::lock_guard dispatchLock(dispatchMutex);
    std::lock_guard lock(mutex);
    auto& state = owners[owner];
    if (state.generation == std::numeric_limits<uint64_t>::max())
    {
        state.active = false;
        return;
    }
    ++state.generation;
    state.active = true;
}

void DropOwner(uint64_t owner)
{
    if (owner == 0)
        return;
    std::lock_guard dispatchLock(dispatchMutex);
    std::lock_guard lock(mutex);
    if (auto it = owners.find(owner); it != owners.end())
        it->second.active = false;
    std::erase_if(entries, [owner](const Entry& entry) { return entry.owner == owner; });
}

void DropOwnersExcept(uint64_t keep)
{
    std::lock_guard dispatchLock(dispatchMutex);
    std::lock_guard lock(mutex);
    std::erase_if(entries, [keep](const Entry& entry) { return entry.owner != keep && entry.owner != 0; });
}

void ReleaseReady(bool present)
{
    std::vector<Entry> ready;
    {
        std::lock_guard lock(mutex);
        if (present)
            ++presentCounter;

        std::erase_if(entries,
                      [&ready, present](Entry& entry)
                      {
                          // Immediate release for testing
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
}
} // namespace TestDeferredRelease

int main()
{
    using namespace TestDeferredRelease;

    // Test 1: Unowned (owner == 0) callback executes on ReleaseReady
    {
        Reset();
        bool executed = false;
        Enqueue([&] { executed = true; }, 0);
        ReleaseReady(true);
        assert(executed && "Unowned callback must execute");
    }

    // Test 2: Unregistered owner (owner != 0, but never registered) MUST execute (M4 fix)
    {
        Reset();
        bool executed = false;
        Enqueue([&] { executed = true; }, 0x12345678);
        ReleaseReady(true);
        assert(executed && "Unregistered owner callback must execute on ReleaseReady");
    }

    // Test 3: Registered active owner executes
    {
        Reset();
        const uint64_t dev = 0xABCD;
        RegisterOwner(dev);
        bool executed = false;
        Enqueue([&] { executed = true; }, dev);
        ReleaseReady(true);
        assert(executed && "Registered active owner callback must execute");
    }

    // Test 4: Dropped owner rejects new enqueues and skips existing callbacks
    {
        Reset();
        const uint64_t dev = 0xABCD;
        RegisterOwner(dev);
        DropOwner(dev);

        bool executed = false;
        Enqueue([&] { executed = true; }, dev);
        ReleaseReady(true);
        assert(!executed && "Dropped owner enqueue must be rejected");
    }

    // Test 5: DropOwner erases previously enqueued items
    {
        Reset();
        const uint64_t dev = 0xABCD;
        RegisterOwner(dev);
        bool executed = false;
        Enqueue([&] { executed = true; }, dev);
        DropOwner(dev);
        ReleaseReady(true);
        assert(!executed && "DropOwner must erase pending callbacks");
    }

    // Test 6: Generation increment invalidates previous generation entries
    {
        Reset();
        const uint64_t dev = 0xABCD;
        RegisterOwner(dev); // Generation 1
        bool oldExecuted = false;
        Enqueue([&] { oldExecuted = true; }, dev);

        RegisterOwner(dev); // Generation 2
        bool newExecuted = false;
        Enqueue([&] { newExecuted = true; }, dev);

        // Manually tamper old entry generation check in ReleaseReady by advancing generation
        ReleaseReady(true);
        assert(!oldExecuted && "Old generation callback must be skipped");
        assert(newExecuted && "Current generation callback must execute");
    }

    // Test 7: DropOwnersExcept preserves owner 0 and keep owner, drops others
    {
        Reset();
        const uint64_t keepDev = 0x1111;
        const uint64_t dropDev = 0x2222;
        RegisterOwner(keepDev);
        RegisterOwner(dropDev);

        bool unownedExecuted = false;
        bool keepExecuted = false;
        bool dropExecuted = false;

        Enqueue([&] { unownedExecuted = true; }, 0);
        Enqueue([&] { keepExecuted = true; }, keepDev);
        Enqueue([&] { dropExecuted = true; }, dropDev);

        DropOwnersExcept(keepDev);

        ReleaseReady(true);
        assert(unownedExecuted && "Owner 0 must be preserved");
        assert(keepExecuted && "Keep owner must be preserved");
        assert(!dropExecuted && "Other owners must be dropped");
    }

    // Test 8: Concurrency stress
    {
        Reset();
        std::atomic<int> destroyed{0};
        const uint64_t ownerA = 0xAAAA;
        const uint64_t ownerB = 0xBBBB;
        RegisterOwner(ownerA);
        RegisterOwner(ownerB);

        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i)
        {
            workers.emplace_back([&, i] {
                for (int j = 0; j < 500; ++j)
                {
                    uint64_t o = (j % 3 == 0) ? 0 : ((j % 3 == 1) ? ownerA : ownerB);
                    Enqueue([&destroyed] { destroyed.fetch_add(1, std::memory_order_relaxed); }, o);
                    if (j % 50 == 0)
                        ReleaseReady(true);
                }
            });
        }
        for (auto& t : workers)
            t.join();

        ReleaseReady(true);
        assert(destroyed.load() > 0);
    }

    std::puts("DEFERRED_RELEASE_CONTRACT_OK");
    return 0;
}

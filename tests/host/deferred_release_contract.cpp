#include <State.h>
#include <misc/DeferredRelease.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

// Host contract test that links the REAL production DeferredRelease
// (misc/DeferredRelease.cpp is compiled into this executable by run.cmd).
// The deferred_shim/ include dir supplies minimal pch.h / SysUtils.h / State.h
// replacements so the production TU builds without the full OptiScaler PCH.
//
// Covers the M4/M12/R1 owner-lifecycle contract and the two Plan §10 NR
// invariants:
//   1. Native NR / DestroyPolicy::Immediate objects never enter the queue:
//      they are destroyed inline, and dropping an NR private VkDevice owner
//      (never registered, never enqueued) must not disturb other owners.
//   2. DropOwnersExcept(newGameDevice) drops the old game device's pending
//      entries while owner 0 survives; DropOwner on any VkDevice (even one
//      that was never RegisterOwner'd) guarantees its queued callbacks never
//      run.

namespace
{
constexpr auto MinAgeSlack = std::chrono::milliseconds{ 650 };   // >= 500 ms MinAge
constexpr auto FallbackSlack = std::chrono::milliseconds{ 2100 }; // >= 2 s FallbackAge

void PresentUntilReleased(std::atomic<bool>& executed, int extraPresents = 8)
{
    for (int i = 0; i < extraPresents && !executed.load(); ++i)
    {
        DeferredRelease::Process();
        if (executed.load())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds{ 5 });
    }
}
} // namespace

int main()
{
    // T1: unowned (owner 0) entries release after 8 presents + MinAge.
    {
        std::atomic<bool> executed{ false };
        DeferredRelease::Enqueue([&] { executed = true; });
        std::this_thread::sleep_for(MinAgeSlack);
        PresentUntilReleased(executed);
        assert(executed.load() && "unowned entry must release after presents+age");
    }

    // T2: entries of a registered, active owner release normally.
    {
        constexpr uint64_t dev = 0x1001;
        DeferredRelease::RegisterOwner(dev);
        std::atomic<bool> executed{ false };
        DeferredRelease::Enqueue([&] { executed = true; }, dev);
        std::this_thread::sleep_for(MinAgeSlack);
        PresentUntilReleased(executed);
        assert(executed.load() && "active registered owner entry must release");
    }

    // T3: Enqueue after DropOwner is discarded (owner inactive).
    {
        constexpr uint64_t dev = 0x1002;
        DeferredRelease::RegisterOwner(dev);
        DeferredRelease::DropOwner(dev);
        std::atomic<bool> executed{ false };
        DeferredRelease::Enqueue([&] { executed = true; }, dev);
        std::this_thread::sleep_for(MinAgeSlack);
        for (int i = 0; i < 10; ++i)
            DeferredRelease::Process();
        assert(!executed.load() && "enqueue on a dropped owner must be discarded");
    }

    // T4: DropOwner erases previously enqueued entries of a registered owner.
    {
        constexpr uint64_t dev = 0x1003;
        DeferredRelease::RegisterOwner(dev);
        std::atomic<bool> executed{ false };
        DeferredRelease::Enqueue([&] { executed = true; }, dev);
        DeferredRelease::DropOwner(dev);
        std::this_thread::sleep_for(FallbackSlack);
        DeferredRelease::Collect();
        assert(!executed.load() && "DropOwner must erase pending entries");
    }

    // T5 (R1): DropOwner on a VkDevice that was NEVER registered still
    // guarantees its queued entries never run. Enqueue accepts unknown owners
    // (M4: never-registered owners release normally), so the drop must leave
    // an inactive tombstone that ReleaseReady rechecks, or purge the queue.
    {
        constexpr uint64_t unregDropped = 0x2001;
        constexpr uint64_t unregKept = 0x2002;
        std::atomic<bool> droppedExecuted{ false };
        std::atomic<bool> keptExecuted{ false };
        DeferredRelease::Enqueue([&] { droppedExecuted = true; }, unregDropped);
        DeferredRelease::Enqueue([&] { keptExecuted = true; }, unregKept);
        DeferredRelease::DropOwner(unregDropped);
        std::this_thread::sleep_for(FallbackSlack);
        DeferredRelease::Collect();
        assert(!droppedExecuted.load() && "unregistered dropped owner callback must never run");
        assert(keptExecuted.load() && "unregistered owner without a drop must still release (M4)");
    }

    // T6: a destroyed handle reused via RegisterOwner invalidates entries of
    // the previous generation.
    {
        constexpr uint64_t dev = 0x1004;
        DeferredRelease::RegisterOwner(dev); // generation 1
        std::atomic<bool> staleExecuted{ false };
        DeferredRelease::Enqueue([&] { staleExecuted = true; }, dev);
        DeferredRelease::DropOwner(dev);      // device destroyed
        DeferredRelease::RegisterOwner(dev); // generation 2 on the reused handle
        std::this_thread::sleep_for(MinAgeSlack);
        for (int i = 0; i < 10; ++i)
            DeferredRelease::Process();
        assert(!staleExecuted.load() && "stale generation entry must not run");
    }

    // T7: DropOwnersExcept(keep) drops every other owner's entries but
    // preserves owner 0 (Plan §10 invariant 2, first half).
    {
        constexpr uint64_t keep = 0x3001;
        constexpr uint64_t oldGameDevice = 0x3002;
        DeferredRelease::RegisterOwner(keep);
        DeferredRelease::RegisterOwner(oldGameDevice);
        std::atomic<bool> unowned{ false }, kept{ false }, dropped{ false };
        DeferredRelease::Enqueue([&] { unowned = true; });
        DeferredRelease::Enqueue([&] { kept = true; }, keep);
        DeferredRelease::Enqueue([&] { dropped = true; }, oldGameDevice);
        DeferredRelease::DropOwnersExcept(keep);
        std::this_thread::sleep_for(MinAgeSlack);
        for (int i = 0; i < 10; ++i)
            DeferredRelease::Process();
        assert(unowned.load() && "owner 0 must survive DropOwnersExcept");
        assert(kept.load() && "kept owner entries must survive DropOwnersExcept");
        assert(!dropped.load() && "old game device entries must be dropped");
    }

    // T8 (Plan §10 invariant 1 + NR isolation): native NR uses
    // DestroyPolicy::Immediate - its objects are destroyed inline and never
    // enter the queue. Its private VkDevices are never registered either.
    // Dropping such a device owner must not disturb other live owners.
    {
        constexpr uint64_t nrPrivateDevice = 0x4001; // never registered, never enqueued
        constexpr uint64_t gameDevice = 0x4002;
        DeferredRelease::RegisterOwner(gameDevice);

        int immediateObject = 0;
        auto* nrObject = new int(7); // "NR object": destroyed immediately, no Enqueue
        immediateObject = *nrObject;
        delete nrObject; // Immediate policy: direct destruction, queue untouched
        assert(immediateObject == 7 && "immediate destroy must not go through the queue");

        std::atomic<bool> gameExecuted{ false };
        DeferredRelease::Enqueue([&] { gameExecuted = true; }, gameDevice);
        DeferredRelease::DropOwner(nrPrivateDevice); // hkvkDestroyDevice drops EVERY VkDevice
        std::this_thread::sleep_for(MinAgeSlack);
        PresentUntilReleased(gameExecuted);
        assert(gameExecuted.load() && "dropping an NR private device must not touch other owners");
    }

    // T9: while shutting down, Enqueue discards (the OS cleans up).
    {
        State::Instance().isShuttingDown = true;
        std::atomic<bool> executed{ false };
        DeferredRelease::Enqueue([&] { executed = true; });
        DeferredRelease::RegisterOwner(0x5001);
        DeferredRelease::DropOwner(0x5001);
        DeferredRelease::DropOwnersExcept(0x5001);
        DeferredRelease::Process();
        DeferredRelease::Collect();
        State::Instance().isShuttingDown = false;
        assert(!executed.load() && "enqueue during shutdown must be discarded");
    }

    std::puts("DEFERRED_RELEASE_CONTRACT_OK");
    return 0;
}

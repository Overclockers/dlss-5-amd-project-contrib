#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/input_hook_lifetime.h"

#include <cassert>
#include <cstdio>
#include <thread>

using namespace OptiInput::HookLifetime;
namespace
{
struct Backend
{
    enum Failure { None, BeginFailure, UpdateFailure, AttachFailure, DetachFailure, CommitFailure };
    static inline Failure fail = None;
    static inline int aborts = 0, commits = 0, attaches = 0, detaches = 0;
    static inline void** pending = nullptr;
    static inline void* replacement = nullptr;
    static long Begin() { pending = nullptr; return fail == BeginFailure; }
    static long Update() { return fail == UpdateFailure; }
    static long Attach(void** value, void* hook)
    {
        ++attaches;
        if (fail == AttachFailure) return 1;
        pending = value; replacement = hook; return 0;
    }
    static long Detach(void** value, void* hook)
    {
        ++detaches;
        if (fail == DetachFailure) return 1;
        pending = value; replacement = hook; return 0;
    }
    static long Commit()
    {
        ++commits;
        if (fail == CommitFailure) return 1;
        if (pending) *pending = replacement;
        pending = nullptr;
        return 0;
    }
    static long Abort() { ++aborts; pending = nullptr; return 0; }
};
}
int main()
{
    int original, trampoline;
    for (auto failure : {Backend::BeginFailure, Backend::UpdateFailure, Backend::AttachFailure,
                         Backend::DetachFailure, Backend::CommitFailure, Backend::None})
    {
        Backend::fail = failure;
        void* pointer = &original;
        const auto aborts = Backend::aborts;
        const auto commits = Backend::commits;
        bool installed = false;
        {
            Transaction<Backend> transaction;
            transaction.Attach(&pointer, &trampoline);
            transaction.Detach(&pointer, &trampoline);
            installed = transaction.Commit();
        }
        assert(installed == (failure == Backend::None));
        assert(pointer == (installed ? static_cast<void*>(&trampoline) : static_cast<void*>(&original)));
        assert(Backend::commits - commits == (failure == Backend::CommitFailure || failure == Backend::None));
        assert(Backend::aborts - aborts == (failure != Backend::BeginFailure &&
                                          failure != Backend::CommitFailure && failure != Backend::None));
    }
    Backend::fail = Backend::None;
    void* pointer = &original;
    {
        Transaction<Backend> abandoned;
        abandoned.Attach(&pointer, &trampoline);
    }
    assert(pointer == &original && Backend::pending == nullptr);

    CallbackGate gate;
    {
        CallbackLease callback(gate);
        CloseLease close(gate);
        assert(!close); // callback must never be waited on under the state mutex
    }
    {
        CloseLease close(gate);
        assert(close);
        { CallbackLease reentrant(gate); } // DestroyWindow/Win32 reentry on owner
        CloseLease nested(gate);
        assert(!nested);
    }
    std::atomic<bool> entered {false}, started {false};
    std::thread callback;
    {
        CloseLease close(gate);
        assert(close);
        callback = std::thread([&]
        {
            started.store(true, std::memory_order_release);
            CallbackLease lease(gate);
            entered.store(true, std::memory_order_release);
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        assert(!entered.load(std::memory_order_acquire));
    }
    callback.join();
    assert(entered.load(std::memory_order_acquire));
    { CloseLease close(gate); assert(close); }
    std::puts("input_hook_lifetime: PASS");
}

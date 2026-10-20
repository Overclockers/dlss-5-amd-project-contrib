#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace OptiInput::HookLifetime
{
// Closing never waits for a callback that might itself be waiting for the input
// mutex. The closing thread may reenter a hook synchronously during Win32 cleanup.
class CallbackGate
{
    static constexpr unsigned Closed = 1u << 31;
    std::atomic<unsigned> state {0};
    std::atomic<std::uintptr_t> owner {0};
    static std::uintptr_t ThreadToken()
    {
        static thread_local char token;
        return reinterpret_cast<std::uintptr_t>(&token);
    }

  public:
    void Enter()
    {
        auto value = state.load(std::memory_order_acquire);
        for (;;)
        {
            if ((value & Closed) && owner.load(std::memory_order_acquire) != ThreadToken())
            {
                std::this_thread::yield();
                value = state.load(std::memory_order_acquire);
                continue;
            }
            if (state.compare_exchange_weak(value, value + 1, std::memory_order_acquire))
                return;
        }
    }
    void Leave() { state.fetch_sub(1, std::memory_order_release); }
    bool TryClose()
    {
        unsigned expected = 0;
        if (!state.compare_exchange_strong(expected, Closed, std::memory_order_acq_rel))
            return false;
        owner.store(ThreadToken(), std::memory_order_release);
        return true;
    }
    void Open()
    {
        owner.store(0, std::memory_order_release);
        state.fetch_and(~Closed, std::memory_order_release);
    }
};

class CallbackLease
{
    CallbackGate& gate;
  public:
    explicit CallbackLease(CallbackGate& value) : gate(value) { gate.Enter(); }
    ~CallbackLease() { gate.Leave(); }
    CallbackLease(const CallbackLease&) = delete;
    CallbackLease& operator=(const CallbackLease&) = delete;
};

class CloseLease
{
    CallbackGate& gate;
    bool closed;
  public:
    explicit CloseLease(CallbackGate& value) : gate(value), closed(gate.TryClose()) {}
    ~CloseLease() { if (closed) gate.Open(); }
    explicit operator bool() const { return closed; }
    CloseLease(const CloseLease&) = delete;
    CloseLease& operator=(const CloseLease&) = delete;
};

// Detours does not make a failed queued Attach/Detach safe merely by committing:
// abort on the first error and publish flags only after a successful commit.
template <class Backend> class Transaction
{
    bool active = false;
    bool good = false;
  public:
    Transaction()
    {
        active = Backend::Begin() == 0;
        good = active && Backend::Update() == 0;
    }
    ~Transaction() { if (active) Backend::Abort(); }
    explicit operator bool() const { return good; }
    bool Attach(void** original, void* hook)
    {
        if (good) good = Backend::Attach(original, hook) == 0;
        return good;
    }
    bool Detach(void** original, void* hook)
    {
        if (good) good = Backend::Detach(original, hook) == 0;
        return good;
    }
    bool Commit()
    {
        if (!good)
        {
            if (active) Backend::Abort();
            active = false;
            return false;
        }
        active = false;
        good = Backend::Commit() == 0;
        return good;
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
};
}

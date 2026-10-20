#include <Windows.h>
#include <detours/detours.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/directinput_registry.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/input_hook_lifetime.h"

#include <cassert>
#include <cstdio>

namespace
{
using Function = int(WINAPI*)(int);
using Registry = OptiInput::DirectInputHooks::MethodRegistry<Function, 2>;
Registry hooks;
OptiInput::HookLifetime::CallbackGate gate;
volatile int offsetA = 11, offsetB = 22, offsetC = 33;
__declspec(noinline) int WINAPI TargetA(int value) { return value + offsetA; }
__declspec(noinline) int WINAPI TargetB(int value) { return value + offsetB; }
__declspec(noinline) int WINAPI TargetC(int value) { return value + offsetC; }
Function volatile callA = TargetA, callB = TargetB;
int callsA = 0, callsB = 0;
int WINAPI ProxyA(int value)
{
    OptiInput::HookLifetime::CallbackLease lease(gate);
    ++callsA;
    return hooks.Resolve(0)(value);
}
int WINAPI ProxyB(int value)
{
    OptiInput::HookLifetime::CallbackLease lease(gate);
    ++callsB;
    return hooks.Resolve(1)(value);
}
const std::array<Function, 2> proxies {ProxyA, ProxyB};
struct Backend
{
    static LONG Begin() { return DetourTransactionBegin(); }
    static LONG Update() { return DetourUpdateThread(GetCurrentThread()); }
    static LONG Attach(void** original, void* proxy) { return DetourAttach(original, proxy); }
    static LONG Detach(void** original, void* proxy) { return DetourDetach(original, proxy); }
    static LONG Commit() { return DetourTransactionCommit(); }
    static LONG Abort() { return DetourTransactionAbort(); }
};
void Attach()
{
    OptiInput::HookLifetime::Transaction<Backend> transaction;
    assert(hooks.AttachPending(transaction, 0, proxies));
    assert(hooks.AttachPending(transaction, 1, proxies));
    assert(transaction.Commit());
    hooks.Slots[0].Attached = hooks.Slots[1].Attached = true;
}
void Detach()
{
    OptiInput::HookLifetime::CloseLease close(gate);
    assert(close);
    OptiInput::HookLifetime::Transaction<Backend> transaction;
    hooks.DetachAttached(transaction, proxies);
    assert(transaction.Commit());
    hooks.Detached();
}
}
int main()
{
    assert(hooks.Prepare(TargetA) == 0);
    assert(hooks.Prepare(TargetB) == 1);
    assert(hooks.Prepare(TargetA) == 0); // shared ANSI/Unicode actual target
    assert(hooks.Prepare(TargetC) == hooks.Missing);
    Attach();
    assert(callA(5) == 16 && callB(5) == 27);
    assert(callsA == 1 && callsB == 1);
    {
        // Re-registration of an attached target must not double-detour.
        OptiInput::HookLifetime::Transaction<Backend> duplicate;
        assert(hooks.AttachPending(duplicate, hooks.Prepare(TargetA), proxies));
        assert(duplicate.Commit());
    }
    assert(callA(6) == 17 && callsA == 2);
    // Even if the caller's vtable now points to B, an already-entered A proxy
    // still calls A's original implementation, not B's trampoline.
    Function callerVtable = TargetB;
    assert(callerVtable(7) == 29 && ProxyA(7) == 18);
    Detach();
    const auto before = callsA;
    assert(callA(8) == 19 && callsA == before);
    assert(ProxyA(8) == 19); // late callback after Detours restored the target
    Attach();
    assert(callA(9) == 20 && callB(9) == 31);
    Detach();
    assert(callA(10) == 21 && callB(10) == 32);
    std::puts("directinput_detours: PASS");
}

#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/directinput_registry.h"

#include <cassert>
#include <cstdio>

using namespace OptiInput::DirectInputHooks;
namespace
{
int A(int value) { return value + 11; }
int B(int value) { return value + 22; }
int C(int value) { return value + 33; }
int TrampolineA(int value) { return A(value); }
int TrampolineB(int value) { return B(value); }
}
int main()
{
    using Function = int(*)(int);
    MethodRegistry<Function, 2> hooks;
    assert(hooks.Prepare(nullptr) == hooks.Missing);
    const auto a = hooks.Prepare(A);
    assert(hooks.Prepare(A) == a); // shared A/W target occupies one slot
    const auto b = hooks.Prepare(B);
    assert(a != b && hooks.Prepare(C) == hooks.Missing);
    hooks.Slots[a].Original = TrampolineA;
    hooks.Slots[b].Original = TrampolineB;
    hooks.Slots[a].Attached = hooks.Slots[b].Attached = true;
    assert(hooks.Resolve(a)(3) == 14 && hooks.Resolve(b)(3) == 25);
    // Dispatch identity is the invoked proxy's slot, independent of a caller's
    // current vtable (including direct cross-implementation method calls).
    Function currentVtableTarget = B;
    assert(currentVtableTarget(3) == 25 && hooks.Resolve(a)(3) == 14);
    hooks.Rollback(a, false);
    assert(hooks.Resolve(a)(3) == 14 && hooks.AnyAttached());
    hooks.Detached();
    assert(!hooks.AnyAttached());
    assert(hooks.Resolve(a) == A && hooks.Resolve(b) == B); // late callback
    assert(hooks.Prepare(A) == a && hooks.Prepare(C) == hooks.Missing);
    MethodRegistry<Function, 2> rollback;
    const auto pending = rollback.Prepare(A);
    rollback.Rollback(pending, true);
    assert(rollback.Find(A) == rollback.Missing);
    assert(rollback.Prepare(B) == pending);

    for (bool enabled : {false, true})
        for (bool keyboard : {false, true})
            for (bool mouse : {false, true})
            {
                assert(!ShouldBlock(DeviceKind::Other, enabled, keyboard, mouse));
                assert(ShouldBlock(DeviceKind::Keyboard, enabled, keyboard, mouse) == (enabled && keyboard));
                assert(ShouldBlock(DeviceKind::Mouse, enabled, keyboard, mouse) == (enabled && mouse));
            }
    int wheelAxes[] = {32768, 65535, 14000};
    if (ShouldBlock(DeviceKind::Other, true, true, true))
        for (auto& axis : wheelAxes) axis = 0;
    assert(wheelAxes[0] == 32768 && wheelAxes[1] == 65535 && wheelAxes[2] == 14000);

    DeviceRegistry<2> devices;
    int deviceA, deviceB, deviceC;
    assert(devices.Track(&deviceA, DeviceKind::Keyboard));
    auto slot = devices.Find(&deviceA);
    auto generation = devices.Slots[slot].Generation;
    assert(devices.Track(&deviceA, DeviceKind::Other));
    assert(devices.Slots[slot].Kind == DeviceKind::Keyboard);
    devices.Released(&deviceA, generation); // old final Release, address reused
    assert(devices.Count() == 1);
    assert(devices.Track(&deviceB, DeviceKind::Other));
    assert(!devices.Track(&deviceC, DeviceKind::Mouse));
    devices.MarkDrain();
    assert(devices.Slots[devices.Find(&deviceA)].Drain);
    assert(!devices.Slots[devices.Find(&deviceB)].Drain);
    generation = devices.Slots[slot].Generation;
    devices.Released(&deviceA, generation);
    assert(devices.Find(&deviceA) == devices.Missing && devices.Count() == 1);
    assert(devices.Track(&deviceC, DeviceKind::Mouse));
    devices.Clear();
    assert(devices.Count() == 0 && devices.Track(&deviceA, DeviceKind::Mouse));
    unsigned long queued = 5, count = 3;
    int event = 9;
    int flushes = 0, reads = 0;
    auto read = [&](int* output, unsigned long* requested, unsigned long flags)
    {
        if (!output) { queued = 0; ++flushes; return 0; }
        ++reads;
        *requested = queued;
        *output = 42;
        return flags == 7 ? 17 : 0;
    };
    assert(ReadBuffered(true, false, read, &event, &count, 7, ~0ul) == 0);
    assert(count == 0 && queued == 0 && event == 9 && flushes == 1 && reads == 0);
    queued = 4;
    assert(ReadBuffered(false, true, read, &event, &count, 7, ~0ul) == 17);
    assert(count == 0 && flushes == 2 && reads == 1 && event == 42);
    queued = 6;
    assert(ReadBuffered(false, false, read, &event, &count, 7, ~0ul) == 17);
    assert(count == 6 && queued == 6 && flushes == 2 && reads == 2);
    std::puts("directinput_registry: PASS");
}

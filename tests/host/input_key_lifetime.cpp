#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/input_key_lifetime.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/shortcut_keys.h"

#include <array>
#include <cassert>
#include <iostream>

namespace
{
struct Button
{
    bool Down = false;
    bool Pressed = false;
    bool Released = false;
    OptiInput::KeyPressHistory PressHistory;
    bool BlockedDown = false;
    unsigned long LastMessageTime = 0;
};

struct Modifier
{
    int Aggregate;
    int Left;
    int Right;
};

constexpr Modifier modifiers[] = {
    { VK_CONTROL, VK_LCONTROL, VK_RCONTROL },
    { VK_SHIFT, VK_LSHIFT, VK_RSHIFT },
    { VK_MENU, VK_LMENU, VK_RMENU },
};

// Supply the same per-side metadata as the message/poll producers, then invoke
// the production aggregate transition helper rather than a test implementation.
struct Keys
{
    std::array<Button, 256> Buttons {};
    unsigned long Time = 0;

    void Sync()
    {
        for (const auto& modifier : modifiers)
            OptiInput::SyncAggregateModifierButtonState(
                Buttons[modifier.Aggregate], Buttons[modifier.Left], Buttons[modifier.Right]);
    }

    void Down(int vk, bool blocked = false)
    {
        auto& key = Buttons[vk];
        if (!key.Down)
        {
            key.PressHistory.RecordDown(ShortcutKeys::HeldModifiers([&](int held) { return Buttons[held].Down; }));
            key.Pressed = true;
            key.BlockedDown = blocked;
        }
        key.Down = true;
        key.LastMessageTime = ++Time;
        Sync();
    }

    void Up(int vk, bool message = true)
    {
        auto& key = Buttons[vk];
        if (message)
        {
            key.PressHistory.MessageUp();
            key.BlockedDown = false;
        }
        if (key.Down)
            key.Released = true;
        key.Down = false;
        key.LastMessageTime = ++Time;
        Sync();
    }

    void Poll(int vk, bool down)
    {
        if (!Buttons[vk].PressHistory.AllowPoll(down))
            return;
        if (down)
            Down(vk);
        else if (Buttons[vk].Down)
            Up(vk, false);
    }

    void NextFrame()
    {
        for (auto& key : Buttons)
            key.Pressed = key.Released = false;
    }
};

void TestAggregateTransitions()
{
    for (const auto& modifier : modifiers)
    {
        for (bool rightFirst : { false, true })
        {
            for (bool polling : { false, true })
            {
                Keys keys;
                const int first = rightFirst ? modifier.Right : modifier.Left;
                const int second = rightFirst ? modifier.Left : modifier.Right;
                const auto down = [&](int vk, bool blocked = false) {
                    if (polling) keys.Poll(vk, true);
                    else keys.Down(vk, blocked);
                };
                const auto up = [&](int vk) {
                    if (polling) keys.Poll(vk, false);
                    else keys.Up(vk);
                };
                auto& aggregate = keys.Buttons[modifier.Aggregate];

                // Standalone modifiers must record zero, not unavailable (-1).
                down(first);
                assert(aggregate.Down && aggregate.Pressed && !aggregate.Released);
                assert(aggregate.PressHistory.Modifiers == 0);
                up(first);
                assert(!aggregate.Down && aggregate.Released);
                assert(ShortcutKeys::Matches(modifier.Aggregate, modifier.Aggregate, aggregate.PressHistory.Modifiers));
                keys.NextFrame();
                assert(aggregate.PressHistory.Modifiers == 0);

                // Tab is an independent modifier for every Ctrl/Shift/Alt base.
                // Its release before the second side presses makes metadata
                // replacement visible, regardless of which side was first.
                down(VK_TAB);
                down(first, true);
                assert(aggregate.PressHistory.Modifiers == ShortcutKeys::Tab);
                assert(aggregate.BlockedDown == !polling);
                up(VK_TAB);
                keys.NextFrame();
                down(second);
                assert(aggregate.Down && !aggregate.Pressed && !aggregate.Released);
                assert(aggregate.PressHistory.Modifiers == ShortcutKeys::Tab);
                assert(keys.Buttons[second].PressHistory.Modifiers != aggregate.PressHistory.Modifiers);

                // Ownership stays with the original press even after its side
                // releases and presses again while the opposite side is held.
                up(first);
                assert(aggregate.Down && !aggregate.Released && !aggregate.BlockedDown);
                down(first);
                up(second);
                assert(aggregate.Down && !aggregate.Pressed && !aggregate.Released);
                assert(aggregate.PressHistory.Modifiers == ShortcutKeys::Tab);
                up(first);
                assert(!aggregate.Down && aggregate.Released);
                assert(aggregate.PressHistory.Modifiers == ShortcutKeys::Tab);
                assert(aggregate.LastMessageTime == keys.Buttons[first].LastMessageTime);
                assert(ShortcutKeys::Matches(modifier.Aggregate | ShortcutKeys::Tab,
                                             modifier.Aggregate, aggregate.PressHistory.Modifiers));
                keys.NextFrame();
                assert(aggregate.PressHistory.Modifiers == ShortcutKeys::Tab);

                // Only a new aggregate press may replace original-down data.
                down(second);
                assert(aggregate.Down && aggregate.Pressed && !aggregate.Released);
                assert(aggregate.PressHistory.Modifiers == 0);
                up(second);
            }
        }
    }
}

void TestFastCanonicalChord()
{
    for (int shift : { VK_LSHIFT, VK_RSHIFT })
    {
        for (int ctrl : { VK_LCONTROL, VK_RCONTROL })
        {
            Keys keys;
            ShortcutKeys::Capture capture;
            capture.Begin();
            keys.Down(shift);
            capture.Press(shift, keys.Buttons[shift].PressHistory.Modifiers);
            keys.Down(ctrl);
            capture.Press(ctrl, keys.Buttons[ctrl].PressHistory.Modifiers);
            keys.Up(shift);
            capture.Release(shift, keys.Buttons[shift].PressHistory.Modifiers);
            keys.Up(ctrl);
            capture.Release(ctrl, keys.Buttons[ctrl].PressHistory.Modifiers);
            assert(capture.finished && capture.binding == (VK_CONTROL | ShortcutKeys::Shift));

            // All four messages arrived before an observation. Both edges must
            // remain visible and canonical Ctrl must retain the original Shift.
            const auto& aggregate = keys.Buttons[VK_CONTROL];
            assert(!aggregate.Down && aggregate.Pressed && aggregate.Released);
            assert(aggregate.PressHistory.Modifiers == ShortcutKeys::Shift);
            ShortcutKeys::ReleaseTracker tracker;
            const auto released = tracker.Update(VK_CONTROL, aggregate.Down, aggregate.Pressed,
                                                 aggregate.Released, aggregate.PressHistory.Modifiers);
            assert(released && ShortcutKeys::Matches(*capture.binding, VK_CONTROL, *released));

            keys.NextFrame();
            // Stale asynchronous downs after authoritative message ups cannot
            // rearm either the physical sides or the canonical aggregate.
            keys.Poll(shift, true);
            keys.Poll(ctrl, true);
            assert(!aggregate.Down && !aggregate.Pressed && !aggregate.Released);
            assert(aggregate.PressHistory.Modifiers == ShortcutKeys::Shift);
            keys.Poll(shift, false);
            keys.Poll(ctrl, false);
            keys.Poll(ctrl, true);
            assert(aggregate.Down && aggregate.Pressed && aggregate.PressHistory.Modifiers == 0);
        }
    }
}
} // namespace

int main()
{
    OptiInput::KeyPressHistory key;
    assert(key.Modifiers == -1);
    // Original Ctrl+Home survives modifier-up, frame clearing and capture begun mid-hold.
    constexpr int ctrl = 1 << 16;
    key.RecordDown(ctrl);
    assert(key.Modifiers == ctrl);
    key.MessageUp();
    assert(key.Modifiers == ctrl);
    // WM_KEYUP can precede an asynchronous stale poll-down. It must not rearm a
    // release binding and emit a second action when polling finally observes up.
    assert(!key.AllowPoll(true));
    assert(!key.AllowPoll(true));
    assert(key.AllowPoll(false));
    assert(key.Modifiers == ctrl);
    assert(key.AllowPoll(true));
    key.RecordDown(0);
    assert(key.Modifiers == 0);
    // An actual new message press is authoritative even before polling sees up.
    key.MessageUp();
    key.RecordDown(ctrl);
    assert(key.AllowPoll(true));
    key.Reset();
    assert(key.Modifiers == -1 && !key.AwaitingPhysicalUp);
    TestAggregateTransitions();
    TestFastCanonicalChord();
    std::cout << "input key lifetime: PASS\n";
}

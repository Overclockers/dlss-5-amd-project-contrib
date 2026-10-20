#pragma once

namespace OptiInput
{
// A message release is authoritative until polling observes the physical up.
// Without this barrier an asynchronously stale poll can invent a second press.
struct KeyPressHistory
{
    int Modifiers = -1;
    bool AwaitingPhysicalUp = false;

    void RecordDown(int modifiers)
    {
        Modifiers = modifiers;
        AwaitingPhysicalUp = false;
    }
    void MessageUp() { AwaitingPhysicalUp = true; }
    bool AllowPoll(bool down)
    {
        if (!down)
            AwaitingPhysicalUp = false;
        return !down || !AwaitingPhysicalUp;
    }
    void Reset() { *this = {}; }
};

// Both message and poll producers synchronize after each side transition. The
// first held side owns the aggregate press until both sides are up, even when
// that side releases first. Never replace its metadata with the second side's
// later press or with the currently held modifiers at release.
template <typename Button> void SyncAggregateModifierButtonState(Button& aggregate, const Button& left, const Button& right)
{
    const bool wasDown = aggregate.Down;
    const bool isDown = left.Down || right.Down;

    aggregate.Down = isDown;
    aggregate.BlockedDown = left.BlockedDown || right.BlockedDown;

    if (!wasDown && isDown)
    {
        aggregate.PressHistory.RecordDown((left.Down ? left : right).PressHistory.Modifiers);
        aggregate.Pressed = true;
    }

    if (wasDown && !isDown)
        aggregate.Released = true;

    aggregate.LastMessageTime =
        left.LastMessageTime >= right.LastMessageTime ? left.LastMessageTime : right.LastMessageTime;
}
}

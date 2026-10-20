#pragma once

#include <array>
#include <cstddef>

namespace OptiInput::DirectInputHooks
{
enum class DeviceKind { Other, Keyboard, Mouse };

constexpr bool ShouldBlock(DeviceKind kind, bool enabled, bool keyboard, bool mouse)
{
    // Zero is not a neutral wheel/pedal/joystick axis. Only keyboard/mouse
    // policies own these two classes; unknown devices always pass through.
    return enabled && ((kind == DeviceKind::Keyboard && keyboard) || (kind == DeviceKind::Mouse && mouse));
}

template <class Read, class Data, class Count>
auto ReadBuffered(bool block, bool drain, Read&& read, Data* data, Count* count, unsigned long flags, Count all)
{
    if (block || drain)
    {
        Count flush = all;
        read(nullptr, &flush, 0);
    }
    if (block)
    {
        if (count) *count = 0;
        return decltype(read(data, count, flags)) {0};
    }
    return read(data, count, flags);
}

// Targets have stable slot-specific proxies. Resolve by invoked proxy, not by
// this->vtable: an implementation may invoke another implementation directly.
// Detached records stay reserved so a callback entering late cannot dispatch to
// a different target after teardown/reinitialization.
template <class Function, std::size_t Capacity> class MethodRegistry
{
  public:
    struct Slot
    {
        Function Target = nullptr;
        Function Original = nullptr;
        bool Attached = false;
    };
    std::array<Slot, Capacity> Slots {};
    static constexpr std::size_t Missing = Capacity;

    std::size_t Find(Function target) const
    {
        if (target == nullptr) return Missing;
        for (std::size_t i = 0; i < Capacity; ++i)
            if (Slots[i].Target == target) return i;
        return Missing;
    }
    std::size_t Prepare(Function target)
    {
        auto index = Find(target);
        if (index != Missing || target == nullptr) return index;
        for (std::size_t i = 0; i < Capacity; ++i)
            if (Slots[i].Target == nullptr)
            {
                Slots[i].Target = Slots[i].Original = target;
                return i;
            }
        return Missing;
    }
    Function Resolve(std::size_t index) const
    {
        return index < Capacity ? Slots[index].Original : nullptr;
    }
    bool AnyAttached() const
    {
        for (const auto& slot : Slots) if (slot.Attached) return true;
        return false;
    }
    // Rollback only unpublished entries; existing attached targets survive.
    template <class Transaction> bool AttachPending(Transaction& transaction, std::size_t index,
                                                    const std::array<Function, Capacity>& proxies)
    {
        auto& slot = Slots[index];
        return slot.Attached || transaction.Attach(reinterpret_cast<void**>(&slot.Original),
                                                   reinterpret_cast<void*>(proxies[index]));
    }
    template <class Transaction> void DetachAttached(Transaction& transaction,
                                                    const std::array<Function, Capacity>& proxies)
    {
        for (std::size_t i = 0; i < Capacity; ++i)
            if (Slots[i].Attached)
                transaction.Detach(reinterpret_cast<void**>(&Slots[i].Original),
                                   reinterpret_cast<void*>(proxies[i]));
    }
    void Rollback(std::size_t index, bool newlyReserved)
    {
        if (newlyReserved && index < Capacity && !Slots[index].Attached) Slots[index] = {};
    }
    void Detached()
    {
        for (auto& slot : Slots)
        {
            slot.Attached = false;
            slot.Original = slot.Target;
        }
    }
};

// Generation prevents a final Release from deleting a device recreated at the
// same address while that Release is finishing on another thread.
template <std::size_t Capacity> class DeviceRegistry
{
  public:
    struct Slot
    {
        void* Device = nullptr;
        DeviceKind Kind = DeviceKind::Other;
        std::size_t Generation = 0;
        bool Drain = false;
    };
    std::array<Slot, Capacity> Slots {};
    std::size_t nextGeneration = 0;
    static constexpr std::size_t Missing = Capacity;
    std::size_t Find(void* device) const
    {
        if (device != nullptr)
            for (std::size_t i = 0; i < Capacity; ++i)
                if (Slots[i].Device == device) return i;
        return Missing;
    }
    bool Track(void* device, DeviceKind kind)
    {
        if (device == nullptr) return false;
        auto index = Find(device);
        if (index != Missing)
        {
            if (Slots[index].Kind == DeviceKind::Other) Slots[index].Kind = kind;
            Slots[index].Generation = ++nextGeneration;
            return true;
        }
        for (auto& slot : Slots)
            if (slot.Device == nullptr)
            {
                slot = {device, kind, ++nextGeneration, false};
                return true;
            }
        return false;
    }
    void Released(void* device, std::size_t generation)
    {
        auto index = Find(device);
        if (index != Missing && Slots[index].Generation == generation) Slots[index] = {};
    }
    void MarkDrain()
    {
        for (auto& slot : Slots)
            if (slot.Device && slot.Kind != DeviceKind::Other) slot.Drain = true;
    }
    std::size_t Count() const
    {
        std::size_t count = 0;
        for (const auto& slot : Slots) count += slot.Device != nullptr;
        return count;
    }
    void Clear() { Slots = {}; }
};
}

#pragma once

#include <cstdint>

namespace OptiInput::HookLifetime
{
// Query/restore execute the real USER32 operations in production and allow CPU
// tests to exercise the same state publication on every failure boundary.
template <class Proc, class Query, class Restore>
bool DetachWindowSubclass(bool windowAlive, Proc callback, Proc& original, bool& installed,
                          Query query, Restore restore)
{
    if (original == nullptr)
        return !installed;
    if (windowAlive)
    {
        Proc current = nullptr;
        if (!query(current))
            return false;
        if (current == callback)
        {
            if (!restore(original))
                return false;
        }
        else if (current != original)
            return false; // Another subclass can still call through our callback.
    }
    installed = false;
    original = nullptr;
    return true;
}

// All access is serialized by the input mutex. Posting only wakes the owning
// window thread; it never restores a foreign-thread WndProc or waits for it.
template <class Window> class DeferredWindowRemoval
{
    Window window {};
    std::uintptr_t sequence = 0;
    std::uintptr_t token = 0;
    bool posted = false;

  public:
    template <class Post> void Request(Window target, Post post)
    {
        if (window != target)
            Cancel();
        window = target;
        if (posted)
            return;
        if (++sequence == 0)
            ++sequence;
        token = sequence;
        posted = post(token);
    }

    bool Consume(Window target, std::uintptr_t requestToken)
    {
        if (!posted || window != target || token != requestToken)
            return false;
        posted = false; // A failed restore may be posted again by the next frame.
        return true;
    }

    void Cancel()
    {
        window = Window {};
        posted = false;
        token = 0;
    }
};

template <class Window, class Proc, class Query, class Restore, class Post>
bool RemoveWindowSubclassOnOwner(Window window, bool windowAlive, bool ownerThread,
                                 Proc callback, Proc& original, bool& installed,
                                 DeferredWindowRemoval<Window>& deferred,
                                 Query query, Restore restore, Post post)
{
    if (windowAlive && original != nullptr && !ownerThread)
    {
        deferred.Request(window, post);
        return false;
    }
    const bool removed = DetachWindowSubclass(windowAlive, callback, original, installed, query, restore);
    if (removed)
        deferred.Cancel();
    return removed;
}

// The requested input HWND is separate from the already-published game target.
// Keep it until removal succeeds, even when subsequent frames name that target.
template <class Window> struct PendingInputWindowSwitch
{
    Window Target {};
    bool Requested = false;
    bool UseSubclass = true;
    bool Explicit = false;

    void Request(Window target, bool useSubclass, bool explicitInput)
    {
        Target = target;
        UseSubclass = useSubclass;
        Explicit = explicitInput;
        Requested = true;
    }

    void Cancel() { Requested = false; }

    template <class Remove, class Apply> bool Retry(Remove remove, Apply apply)
    {
        if (!Requested)
            return true;
        if (!remove(Target, UseSubclass))
            return false;
        Requested = false;
        apply(Target, UseSubclass, Explicit);
        return true;
    }
};
}

#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/input_window_lifetime.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/menu/input/input_hook_lifetime.h"
#include <Windows.h>

#include <cassert>
#include <iostream>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace
{
void Original() {}
void Callback() {}
void OtherSubclass() {}

OptiInput::HookLifetime::CallbackGate windowGate;
WNDPROC previousProc = nullptr;
unsigned nestedMessages = 0;
unsigned destroyMessages = 0;

LRESULT CALLBACK WindowOriginal(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_APP + 1)
        return SendMessageW(hwnd, WM_APP + 2, 0, 0);
    if (msg == WM_APP + 2)
    {
        ++nestedMessages;
        return 42;
    }
    if (msg == WM_NCDESTROY)
        ++destroyMessages;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WindowCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    OptiInput::HookLifetime::CallbackLease callback(windowGate);
    return CallWindowProcW(previousProc, hwnd, msg, wParam, lParam);
}

void ExerciseRealWindowReentry()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW cls {};
    cls.lpfnWndProc = WindowOriginal;
    cls.hInstance = instance;
    cls.lpszClassName = L"OptiInputLifetimeCpuTest";
    const ATOM atom = RegisterClassW(&cls);
    assert(atom != 0);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, instance, nullptr);
    assert(hwnd != nullptr);
    previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                             reinterpret_cast<LONG_PTR>(WindowCallback)));
    assert(previousProc == WindowOriginal);
    {
        OptiInput::HookLifetime::CloseLease closed(windowGate);
        assert(closed);
        // Real USER32 sends synchronously reenter the callback on the closing
        // owner thread, including nested dispatch through the original proc.
        assert(SendMessageW(hwnd, WM_APP + 1, 0, 0) == 42);
        assert(nestedMessages == 1);
        assert(DestroyWindow(hwnd));
        assert(destroyMessages == 1);
    }
    assert(UnregisterClassW(cls.lpszClassName, instance));
}

constexpr UINT RemoveMessage = WM_APP + 20;
constexpr UINT InvokeMessage = WM_APP + 21;
constexpr UINT ForwardMessage = WM_APP + 22;

struct SwitchFixture;
LRESULT CALLBACK SwitchCallback(HWND, UINT, WPARAM, LPARAM);

struct SwitchFixture
{
    std::mutex Mutex;
    HWND Input = nullptr;
    HWND Target = nullptr;
    WNDPROC Original = nullptr;
    bool Installed = false;
    bool FailPost = false;
    bool FailRestore = false;
    DWORD RestoreThread = 0;
    std::uintptr_t LastToken = 0;
    OptiInput::HookLifetime::DeferredWindowRemoval<HWND> Deferred;
    OptiInput::HookLifetime::PendingInputWindowSwitch<HWND> Pending;
    std::vector<std::pair<HWND, WNDPROC>> Forwarding;

    WNDPROC Find(HWND hwnd)
    {
        for (auto entry : Forwarding)
            if (entry.first == hwnd)
                return entry.second;
        return nullptr;
    }

    void Install(HWND hwnd)
    {
        Input = hwnd;
        Original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                                               reinterpret_cast<LONG_PTR>(SwitchCallback)));
        assert(Original != nullptr && Original != SwitchCallback);
        Installed = true;
        Deferred.Cancel();
        Forwarding.emplace_back(hwnd, Original);
    }

    bool Remove(bool alive = true)
    {
        return OptiInput::HookLifetime::RemoveWindowSubclassOnOwner(
            Input, alive && IsWindow(Input), GetWindowThreadProcessId(Input, nullptr) == GetCurrentThreadId(),
            WNDPROC(SwitchCallback), Original, Installed, Deferred,
            [&](WNDPROC& current)
            {
                current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(Input, GWLP_WNDPROC));
                return current != nullptr;
            },
            [&](WNDPROC original)
            {
                RestoreThread = GetCurrentThreadId();
                if (FailRestore)
                    return false;
                SetLastError(0);
                const auto previous = SetWindowLongPtrW(Input, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
                return previous != 0 || GetLastError() == 0;
            },
            [&](std::uintptr_t token)
            {
                LastToken = token;
                return !FailPost && PostMessageW(Input, RemoveMessage, token, 0) != FALSE;
            });
    }

    void Retry()
    {
        Pending.Retry(
            [&](HWND target, bool)
            {
                if (Input == target)
                {
                    Deferred.Cancel();
                    return true;
                }
                return Remove();
            },
            [&](HWND target, bool, bool)
            {
                if (Input != target)
                    Install(target);
            });
    }

    void Request(HWND target)
    {
        Target = target; // Deliberately publish target before old removal.
        Pending.Request(target, true, false);
        Retry();
    }
};

LRESULT CALLBACK SwitchOriginal(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == ForwardMessage)
        return 73;
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SwitchCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto& fixture = *reinterpret_cast<SwitchFixture*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    WNDPROC original;
    {
        std::lock_guard lock(fixture.Mutex);
        original = fixture.Find(hwnd);
        if (msg == RemoveMessage)
        {
            if (fixture.Input == hwnd && fixture.Deferred.Consume(hwnd, wParam))
                fixture.Remove();
            return 0;
        }
        if (msg == WM_NCDESTROY && fixture.Input == hwnd)
        {
            assert(fixture.Remove(false));
            fixture.Input = nullptr;
        }
    }
    assert(original != nullptr);
    return CallWindowProcW(original, hwnd, msg, wParam, lParam);
}

// Commands are thread messages, so window subclass restoration cannot consume
// the test's synchronization barriers. No timing sleeps or source checks.
struct WindowThread
{
    std::thread Thread;
    HWND Window = nullptr;
    DWORD Id = 0;

    WindowThread(const wchar_t* className, SwitchFixture& fixture)
    {
        std::promise<void> ready;
        auto created = ready.get_future();
        Thread = std::thread([this, className, &fixture, &ready]
        {
            Id = GetCurrentThreadId();
            Window = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
            assert(Window != nullptr);
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&fixture));
            ready.set_value();
            MSG message {};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                if (message.hwnd == nullptr && message.message == InvokeMessage)
                {
                    std::unique_ptr<std::function<void()>> action(
                        reinterpret_cast<std::function<void()>*>(message.lParam));
                    (*action)();
                }
                else
                    DispatchMessageW(&message);
            }
        });
        assert(created.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }

    void Invoke(std::function<void()> action)
    {
        auto task = std::make_shared<std::packaged_task<void()>>(std::move(action));
        auto finished = task->get_future();
        auto call = std::make_unique<std::function<void()>>([task] { (*task)(); });
        assert(PostThreadMessageW(Id, InvokeMessage, 0, reinterpret_cast<LPARAM>(call.get())));
        call.release();
        assert(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        finished.get();
    }

    ~WindowThread()
    {
        Invoke([&] { if (IsWindow(Window)) assert(DestroyWindow(Window)); });
        assert(PostThreadMessageW(Id, WM_QUIT, 0, 0));
        Thread.join();
    }
};

void ExerciseDeferredTargetSwitch()
{
    WNDCLASSW cls {};
    cls.lpfnWndProc = SwitchOriginal;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"OptiInputTwoThreadSwitchCpuTest";
    assert(RegisterClassW(&cls));
    {
        SwitchFixture fixture;
        WindowThread oldWindow(cls.lpszClassName, fixture);
        WindowThread newWindow(cls.lpszClassName, fixture);
        assert(oldWindow.Id != newWindow.Id && oldWindow.Id != GetCurrentThreadId());
        oldWindow.Invoke([&] { std::lock_guard lock(fixture.Mutex); fixture.Install(oldWindow.Window); });

        // Failed post keeps both the pending request and original forwarding.
        {
            std::lock_guard lock(fixture.Mutex);
            fixture.FailPost = true;
            fixture.Request(newWindow.Window);
            assert(fixture.Target == newWindow.Window && fixture.Input == oldWindow.Window);
            assert(fixture.Pending.Requested && fixture.Original == SwitchOriginal && fixture.Installed);
            fixture.FailPost = false;
            fixture.FailRestore = true;
            fixture.Retry();
        }
        oldWindow.Invoke([] {}); // FIFO barrier after the first owner-thread attempt.
        {
            std::lock_guard lock(fixture.Mutex);
            assert(fixture.RestoreThread == oldWindow.Id);
            assert(fixture.Input == oldWindow.Window && fixture.Original == SwitchOriginal && fixture.Installed);
            fixture.FailRestore = false;
            fixture.Retry(); // Same target; failed restoration must be repostable.
        }
        oldWindow.Invoke([] {});
        {
            std::lock_guard lock(fixture.Mutex);
            assert(fixture.Original == nullptr && !fixture.Installed && fixture.Pending.Requested);
            fixture.Retry(); // No target change is needed to complete the switch.
            assert(fixture.Input == newWindow.Window && !fixture.Pending.Requested && fixture.Installed);
            assert(fixture.Find(oldWindow.Window) == SwitchOriginal);
        }
        oldWindow.Invoke([&]
        {
            // A callback dispatched before detach still forwards to the old root.
            assert(SwitchCallback(oldWindow.Window, ForwardMessage, 0, 0) == 73);
        });

        // Cancel before queued removal is dispatched, then install a later
        // generation on the same HWND. The old token cannot remove that chain.
        newWindow.Invoke([&]
        {
            std::uintptr_t stale;
            {
                std::lock_guard lock(fixture.Mutex);
                fixture.Deferred.Request(newWindow.Window, [&](std::uintptr_t token)
                {
                    stale = token;
                    return PostMessageW(newWindow.Window, RemoveMessage, token, 0) != FALSE;
                });
                fixture.Pending.Request(oldWindow.Window, true, false);
                fixture.Request(newWindow.Window); // Returning to current input cancels removal.
                assert(fixture.Input == newWindow.Window && !fixture.Pending.Requested);
                assert(fixture.Remove());
                fixture.Install(newWindow.Window);
                fixture.Deferred.Request(newWindow.Window, [&](std::uintptr_t token)
                {
                    assert(token != stale); // Cancellation never resets the sequence.
                    return true;
                });
            }
            assert(SwitchCallback(newWindow.Window, RemoveMessage, stale, 0) == 0);
            assert(GetWindowLongPtrW(newWindow.Window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(SwitchCallback));
        });
        newWindow.Invoke([] {}); // Also dispatch the stale posted copy.

        // Destruction on the owner thread completes an otherwise pending switch.
        newWindow.Invoke([&]
        {
            {
                std::lock_guard lock(fixture.Mutex);
                fixture.Pending.Request(oldWindow.Window, true, false);
            }
            assert(DestroyWindow(newWindow.Window));
        });
        {
            std::lock_guard lock(fixture.Mutex);
            assert(fixture.Input == nullptr && fixture.Original == nullptr && !fixture.Installed);
            fixture.Retry();
            assert(fixture.Input == oldWindow.Window && !fixture.Pending.Requested);
        }
        oldWindow.Invoke([&] { assert(SwitchCallback(oldWindow.Window, ForwardMessage, 0, 0) == 73); });
    }
    assert(UnregisterClassW(cls.lpszClassName, cls.hInstance));
}
}

int main()
{
    using Proc = void (*)();
    using OptiInput::HookLifetime::DetachWindowSubclass;
    ExerciseRealWindowReentry();
    ExerciseDeferredTargetSwitch();
    for (bool installed : {false, true})
    {
        Proc original = Original;
        bool active = installed;
        bool restoreCalled = false;
        auto restore = [&](Proc target) { restoreCalled = true; assert(target == Original); return false; };
        // Failed GetWindowLongPtr cannot erase the live forwarding target.
        assert(!DetachWindowSubclass(true, Proc(Callback), original, active,
                                      [](Proc&) { return false; }, restore));
        assert(original == Original && active == installed && !restoreCalled);
        // Failed SetWindowLongPtr preserves exactly the previous ownership state.
        assert(!DetachWindowSubclass(true, Proc(Callback), original, active,
                                      [](Proc& current) { current = Callback; return true; }, restore));
        assert(original == Original && active == installed && restoreCalled);
        // Another subclass can still hold our previous callback in its chain.
        assert(!DetachWindowSubclass(true, Proc(Callback), original, active,
                                      [](Proc& current) { current = OtherSubclass; return true; }, restore));
        assert(original == Original && active == installed);
        assert(DetachWindowSubclass(true, Proc(Callback), original, active,
                                     [](Proc& current) { current = Callback; return true; },
                                     [](Proc target) { return target == Original; }));
        assert(original == nullptr && !active);
    }
    Proc original = Original;
    bool active = true;
    assert(DetachWindowSubclass(false, Proc(Callback), original, active,
                                 [](Proc&) { assert(false); return false; }, [](Proc) { assert(false); return false; }));
    assert(original == nullptr && !active);
    original = Original;
    active = false;
    assert(DetachWindowSubclass(true, Proc(Callback), original, active,
                                 [](Proc& current) { current = Original; return true; }, [](Proc) { assert(false); return false; }));
    std::cout << "input window lifetime: PASS\n";
}

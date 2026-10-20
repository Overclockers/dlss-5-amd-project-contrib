#include "pch.h"
#include "input_system_internal.h"
#include "input_window_lifetime.h"
#include <vector>

namespace OptiInput
{
namespace
{
// Keep forwarding targets beyond detach for a callback already dispatched by USER32.
std::vector<std::pair<HWND, WNDPROC>> wndProcTargets;
UINT DeferredWindowRemovalMessage()
{
    static const UINT message = RegisterWindowMessageW(L"OptiScaler.OptiInput.RemoveWndProc");
    return message;
}
bool QueryWindowIdentity(HWND hwnd, HWND* rootHwnd, DWORD* processId, DWORD* threadId)
{
    if (rootHwnd != nullptr)
        *rootHwnd = nullptr;

    if (processId != nullptr)
        *processId = 0;

    if (threadId != nullptr)
        *threadId = 0;

    if (hwnd == nullptr || !IsWindow(hwnd))
    {
        OPTIINPUT_LOG_VERBOSE("QueryWindowIdentity invalid hwnd:{} isWindow:{}", static_cast<void*>(hwnd),
                              hwnd != nullptr && IsWindow(hwnd) ? 1 : 0);
        return false;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);

    if (root == nullptr || !IsWindow(root))
        return false;

    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);

    if (tid == 0 || pid == 0)
        return false;

    if (rootHwnd != nullptr)
        *rootHwnd = root;

    if (processId != nullptr)
        *processId = pid;

    if (threadId != nullptr)
        *threadId = tid;

    return true;
}

bool IsCurrentProcessWindow(DWORD processId)
{
    if (_state.CurrentProcessId == 0)
        _state.CurrentProcessId = GetCurrentProcessId();

    return processId != 0 && processId == _state.CurrentProcessId;
}

const char* YesNo(bool value) { return value ? "yes" : "no"; }

void LogQueryWindowFailure(const char* label, HWND hwnd)
{
    DWORD processId = 0;
    const DWORD threadId = hwnd != nullptr ? GetWindowThreadProcessId(hwnd, &processId) : 0;

    LOG_WARN("{} rejected hwnd:{} isWindow:{} root:{} pid:{} tid:{} currentPid:{}", label, static_cast<void*>(hwnd),
             hwnd != nullptr && IsWindow(hwnd) ? 1 : 0,
             hwnd != nullptr && IsWindow(hwnd) ? static_cast<void*>(GetAncestor(hwnd, GA_ROOT)) : nullptr, processId,
             threadId, _state.CurrentProcessId);
}

void SetFocusStateLocked(bool focused, const char* reason, HWND foreground, DWORD foregroundProcessId,
                         DWORD foregroundThreadId)
{
    const bool oldFocused = _state.Focused;
    SetInputFocusLocked(focused);

    if (oldFocused != focused)
    {
        LOG_DEBUG(
            "focus changed {} -> {} reason:{} foreground:{} foregroundPid:{} foregroundTid:{} input:{} inputRoot:{} "
            "target:{} targetRoot:{} externalTarget:{}",
            oldFocused ? 1 : 0, focused ? 1 : 0, reason != nullptr ? reason : "?", static_cast<void*>(foreground),
            foregroundProcessId, foregroundThreadId, static_cast<void*>(_state.InputHwnd),
            static_cast<void*>(_state.InputRootHwnd), static_cast<void*>(_state.TargetHwnd),
            static_cast<void*>(_state.TargetRootHwnd), YesNo(_state.ExternalTargetProcess));
    }
}
} // namespace

void SetTargetWindow(HWND hwnd, bool isUwp, bool useWndProcSubclass)
{
    HWND rootHwnd = nullptr;
    DWORD processId = 0;
    DWORD threadId = 0;

    if (!QueryWindowIdentity(hwnd, &rootHwnd, &processId, &threadId))
    {
        LogQueryWindowFailure("SetTargetWindow", hwnd);
        ClearTargetWindowLocked();
        return;
    }

    const bool externalTargetProcess = !IsCurrentProcessWindow(processId);

    if (_state.TargetHwnd == hwnd && _state.IsUwp == isUwp && _state.UseWndProcSubclass == useWndProcSubclass &&
        _state.TargetRootHwnd == rootHwnd && _state.TargetThreadId == threadId && _state.TargetProcessId == processId &&
        _state.ExternalTargetProcess == externalTargetProcess)
    {
        RetryPendingInputWindowLocked();
        return;
    }

    LOG_INFO("target window set hwnd:{} root:{} pid:{} tid:{} currentPid:{} external:{} isUwp:{} useSubclass:{}",
             static_cast<void*>(hwnd), static_cast<void*>(rootHwnd), processId, threadId, _state.CurrentProcessId,
             externalTargetProcess ? 1 : 0, isUwp ? 1 : 0, useWndProcSubclass ? 1 : 0);

    _state.TargetHwnd = hwnd;
    _state.TargetRootHwnd = rootHwnd;
    _state.IsUwp = isUwp;
    _state.UseWndProcSubclass = useWndProcSubclass;
    _state.TargetProcessId = processId;
    _state.TargetThreadId = threadId;
    _state.ExternalTargetProcess = externalTargetProcess;

    // If the caller did not provide a separate input HWND, injected/in-process
    // mode uses the target HWND as the input HWND. In external mode the target
    // HWND belongs to another process, so it cannot be subclassed or used as the
    // message source for OptiInput.
    if (!(_state.PendingInputWindow.Requested ? _state.PendingInputWindow.Explicit : _state.HasExplicitInputHwnd))
    {
        if (externalTargetProcess)
        {
            LOG_WARN(
                "target HWND belongs to another process and no explicit InputHwnd was provided; clearing input window");
            SetInputWindow(nullptr, useWndProcSubclass, false);
        }
        else
        {
            SetInputWindow(hwnd, useWndProcSubclass, false);
        }
    }
}

void SetInputWindow(HWND hwnd, bool useWndProcSubclass, bool explicitInputHwnd)
{
    if (!_state.PendingInputWindow.Requested && hwnd == _state.InputHwnd &&
        useWndProcSubclass == _state.InputUsesWndProcSubclass && explicitInputHwnd == _state.HasExplicitInputHwnd &&
        (hwnd == nullptr || IsWindow(hwnd)))
        return;
    _state.PendingInputWindow.Request(hwnd, useWndProcSubclass, explicitInputHwnd);
    RetryPendingInputWindowLocked();
}

void RetryPendingInputWindowLocked()
{
    if (!_state.PendingInputWindow.Requested)
        return;

    HWND rootHwnd = nullptr;
    DWORD processId = 0;
    DWORD threadId = 0;
    HWND hwnd = _state.PendingInputWindow.Target;
    if (!QueryWindowIdentity(hwnd, &rootHwnd, &processId, &threadId) || !IsCurrentProcessWindow(processId))
    {
        // A pending HWND may disappear before the old window acknowledges.
        hwnd = nullptr;
        rootHwnd = nullptr;
        processId = threadId = 0;
        _state.PendingInputWindow.Target = nullptr;
    }

    _state.PendingInputWindow.Retry(
        [](HWND target, bool useSubclass)
        {
            if (target == _state.InputHwnd && useSubclass == _state.InputUsesWndProcSubclass)
            {
                _state.DeferredWndProcRemoval.Cancel();
                return true;
            }
            return RemoveWindowSubclass();
        },
        [=](HWND, bool useSubclass, bool explicitInputHwnd)
        {
            const bool sameInput = hwnd == _state.InputHwnd && useSubclass == _state.InputUsesWndProcSubclass;
            LOG_INFO("input window set hwnd:{} root:{} pid:{} tid:{} explicitInput:{} useSubclass:{}",
                     static_cast<void*>(hwnd), static_cast<void*>(rootHwnd), processId, threadId,
                     explicitInputHwnd ? 1 : 0, useSubclass ? 1 : 0);

            _state.InputHwnd = hwnd;
            _state.InputRootHwnd = rootHwnd;
            _state.InputProcessId = processId;
            _state.InputThreadId = threadId;
            _state.HasExplicitInputHwnd = explicitInputHwnd;
            _state.InputUsesWndProcSubclass = useSubclass;

            if (useSubclass && hwnd != nullptr && (!sameInput || _state.OriginalWndProc == nullptr))
            {
                const bool subclassed = InstallWindowSubclass(hwnd);
                // UWP wrapper HWND ownership varies; preserve nonfatal failure.
                if (!subclassed && !_state.IsUwp)
                    LOG_WARN("failed to subclass input window hwnd:{} lastError:{}", static_cast<void*>(hwnd),
                             GetLastError());
            }
        });
}

bool TryGetWindowProc(HWND hwnd, WNDPROC* wndProc)
{
    if (wndProc == nullptr)
        return false;

    *wndProc = nullptr;

    if (hwnd == nullptr || !IsWindow(hwnd))
    {
        OPTIINPUT_LOG_VERBOSE("TryGetWindowProc invalid hwnd:{} isWindow:{}", static_cast<void*>(hwnd),
                              hwnd != nullptr && IsWindow(hwnd) ? 1 : 0);
        return false;
    }

    SetLastError(0);

    const LONG_PTR value = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);

    if (value == 0 && GetLastError() != 0)
    {
        LOG_WARN("GetWindowLongPtrW(GWLP_WNDPROC) failed hwnd:{} lastError:{}", static_cast<void*>(hwnd),
                 GetLastError());
        return false;
    }

    *wndProc = reinterpret_cast<WNDPROC>(value);
    return *wndProc != nullptr;
}

void ClearTargetWindowLocked()
{
    if (_state.TargetHwnd != nullptr)
    {
        LOG_WARN("clearing target window hwnd:{} pid:{} tid:{}", static_cast<void*>(_state.TargetHwnd),
                 _state.TargetProcessId, _state.TargetThreadId);
    }

    _state.TargetHwnd = nullptr;
    _state.TargetRootHwnd = nullptr;
    _state.TargetProcessId = 0;
    _state.TargetThreadId = 0;
    _state.ExternalTargetProcess = false;
    SetInputFocusLocked(false);

    if (!(_state.PendingInputWindow.Requested ? _state.PendingInputWindow.Explicit : _state.HasExplicitInputHwnd))
        SetInputWindow(nullptr, _state.UseWndProcSubclass, false);
}

void ClearInputWindowLocked()
{
    SetInputWindow(nullptr, _state.UseWndProcSubclass, false);
}

void OnInputWindowDestroyedLocked(HWND hwnd)
{
    if (hwnd != _state.InputHwnd)
        return;
    _state.DeferredWndProcRemoval.Cancel();
    if (_state.InputHwnd != nullptr)
    {
        LOG_WARN("clearing input window hwnd:{} pid:{} tid:{} subclassed:{}", static_cast<void*>(_state.InputHwnd),
                 _state.InputProcessId, _state.InputThreadId, _state.WndProcSubclassed ? 1 : 0);
    }

    _state.InputHwnd = nullptr;
    _state.InputRootHwnd = nullptr;
    _state.InputProcessId = 0;
    _state.InputThreadId = 0;
    _state.HasExplicitInputHwnd = false;

    HookLifetime::DetachWindowSubclass(false, static_cast<WNDPROC>(OptiInputWndProc),
                                       _state.OriginalWndProc, _state.WndProcSubclassed,
                                       [](WNDPROC&) { return false; }, [](WNDPROC) { return false; });
}

bool ValidateTargetWindowLocked()
{
    if (_state.TargetHwnd == nullptr)
        return false;

    HWND rootHwnd = nullptr;
    DWORD processId = 0;
    DWORD threadId = 0;

    if (!QueryWindowIdentity(_state.TargetHwnd, &rootHwnd, &processId, &threadId))
    {
        LogQueryWindowFailure("ValidateTargetWindow", _state.TargetHwnd);
        ClearTargetWindowLocked();
        return false;
    }

    const bool newExternalTargetProcess = !IsCurrentProcessWindow(processId);
    if (_state.TargetRootHwnd != rootHwnd || _state.TargetProcessId != processId || _state.TargetThreadId != threadId ||
        _state.ExternalTargetProcess != newExternalTargetProcess)
    {
        LOG_DEBUG("target identity changed hwnd:{} root {} -> {} pid {} -> {} tid {} -> {} external {} -> {}",
                  static_cast<void*>(_state.TargetHwnd), static_cast<void*>(_state.TargetRootHwnd),
                  static_cast<void*>(rootHwnd), _state.TargetProcessId, processId, _state.TargetThreadId, threadId,
                  _state.ExternalTargetProcess ? 1 : 0, newExternalTargetProcess ? 1 : 0);
    }

    _state.TargetRootHwnd = rootHwnd;
    _state.TargetProcessId = processId;
    _state.TargetThreadId = threadId;
    _state.ExternalTargetProcess = newExternalTargetProcess;

    if (_state.ExternalTargetProcess && !_state.HasExplicitInputHwnd && _state.InputHwnd != nullptr)
        SetInputWindow(nullptr, _state.UseWndProcSubclass, false);

    return true;
}

bool ValidateInputWindowLocked()
{
    if (_state.InputHwnd == nullptr)
        return false;

    HWND rootHwnd = nullptr;
    DWORD processId = 0;
    DWORD threadId = 0;

    if (!QueryWindowIdentity(_state.InputHwnd, &rootHwnd, &processId, &threadId))
    {
        LogQueryWindowFailure("ValidateInputWindow", _state.InputHwnd);
        if (!IsWindow(_state.InputHwnd))
            OnInputWindowDestroyedLocked(_state.InputHwnd);
        return false;
    }

    if (!IsCurrentProcessWindow(processId))
    {
        LOG_WARN("ValidateInputWindow found foreign input HWND input:{} inputPid:{} currentPid:{}",
                 static_cast<void*>(_state.InputHwnd), processId, _state.CurrentProcessId);
        ClearInputWindowLocked();
        return false;
    }

    if (_state.InputRootHwnd != rootHwnd || _state.InputProcessId != processId || _state.InputThreadId != threadId)
    {
        LOG_DEBUG("input identity changed hwnd:{} root {} -> {} pid {} -> {} tid {} -> {}",
                  static_cast<void*>(_state.InputHwnd), static_cast<void*>(_state.InputRootHwnd),
                  static_cast<void*>(rootHwnd), _state.InputProcessId, processId, _state.InputThreadId, threadId);
    }

    _state.InputRootHwnd = rootHwnd;
    _state.InputProcessId = processId;
    _state.InputThreadId = threadId;

    return true;
}

bool InstallWindowSubclass(HWND hwnd)
{
    if (_state.OriginalWndProc != nullptr && _state.InputHwnd != hwnd)
        return false;
    if (!RetainInputCallbackModule(reinterpret_cast<const void*>(&OptiInputWndProc)))
        return false;
    WNDPROC currentWndProc = nullptr;

    if (!TryGetWindowProc(hwnd, &currentWndProc))
    {
        LOG_WARN("InstallWindowSubclass failed to query current WndProc hwnd:{}", static_cast<void*>(hwnd));
        // A failed query does not prove that USER32 stopped referencing us.
        return false;
    }

    if (currentWndProc == OptiInputWndProc)
    {
        _state.WndProcSubclassed = _state.OriginalWndProc != nullptr;
        if (_state.WndProcSubclassed)
            _state.DeferredWndProcRemoval.Cancel();
        LOG_DEBUG("InstallWindowSubclass found existing OptiInput WndProc hwnd:{} original:{} subclassed:{}",
                  static_cast<void*>(hwnd), reinterpret_cast<std::uintptr_t>(_state.OriginalWndProc),
                  _state.WndProcSubclassed ? 1 : 0);
        return _state.WndProcSubclassed;
    }
    if (_state.OriginalWndProc != nullptr && currentWndProc != _state.OriginalWndProc)
        return false; // Reinstalling above another subclass would create a cycle.

    SetLastError(0);

    LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(OptiInputWndProc));

    if (previous == 0 && GetLastError() != 0)
    {
        LOG_WARN("SetWindowLongPtrW(GWLP_WNDPROC) failed hwnd:{} currentWndProc:{} lastError:{}",
                 static_cast<void*>(hwnd), reinterpret_cast<std::uintptr_t>(currentWndProc), GetLastError());
        return false;
    }

    _state.OriginalWndProc = reinterpret_cast<WNDPROC>(previous);
    _state.DeferredWndProcRemoval.Cancel(); // A queued token must not detach a later installation.
    auto target = std::find_if(wndProcTargets.begin(), wndProcTargets.end(),
                               [hwnd](const auto& entry) { return entry.first == hwnd; });
    if (target == wndProcTargets.end())
        wndProcTargets.emplace_back(hwnd, _state.OriginalWndProc);
    else
        target->second = _state.OriginalWndProc;
    _state.WndProcSubclassed = true;

    LOG_INFO("subclass installed hwnd:{} previousWndProc:{} optiWndProc:{}", static_cast<void*>(hwnd),
             reinterpret_cast<std::uintptr_t>(_state.OriginalWndProc),
             reinterpret_cast<std::uintptr_t>(OptiInputWndProc));

    return true;
}

void ValidateWindowSubclassLocked()
{
    if (_state.PendingInputWindow.Requested || !_state.InputUsesWndProcSubclass || !_state.WndProcSubclassed)
        return;

    if (!ValidateInputWindowLocked())
        return;

    WNDPROC currentWndProc = nullptr;

    if (!TryGetWindowProc(_state.InputHwnd, &currentWndProc))
    {
        LOG_WARN("subclass validation failed to query WndProc input:{}", static_cast<void*>(_state.InputHwnd));
        // Preserve the forwarding target until detachment can be proven.
        return;
    }

    if (currentWndProc == OptiInputWndProc)
        return;

    const WNDPROC previousOriginalWndProc = _state.OriginalWndProc;
    const bool restoredToOriginalWndProc = currentWndProc == previousOriginalWndProc;

    LOG_WARN("subclass lost input:{} currentWndProc:{} previousOriginal:{} restoredToOriginal:{}",
             static_cast<void*>(_state.InputHwnd), reinterpret_cast<std::uintptr_t>(currentWndProc),
             reinterpret_cast<std::uintptr_t>(previousOriginalWndProc), restoredToOriginalWndProc ? 1 : 0);

    _state.WndProcSubclassed = false;

    // If another component installed a new WndProc on top of ours, it may have
    // captured OptiInputWndProc as its previous WndProc. In that chain, our
    // OptiInputWndProc can still be called even though GWLP_WNDPROC no longer
    // points at us. Keep the original WndProc that we wrapped so our stale
    // callback forwards to the real previous proc instead of forwarding back
    // into the new top-level WndProc and creating recursion:
    //
    //   OtherWndProc -> OptiInputWndProc -> OtherWndProc -> ...
    //
    // Only replace/refresh OriginalWndProc through InstallWindowSubclass(),
    // where SetWindowLongPtrW returns the actual previous proc for the new
    // chain.
    _state.OriginalWndProc = previousOriginalWndProc;

    // Safe auto-reinstall case: something restored the input window directly
    // back to the WndProc we originally wrapped. If another component installed
    // a new WndProc on top of ours, do not reinstall here; with raw
    // GWLP_WNDPROC subclassing, that can create a recursive Opti -> other ->
    // Opti chain.
    if (restoredToOriginalWndProc)
    {
        LOG_INFO("attempting safe subclass reinstall input:{}", static_cast<void*>(_state.InputHwnd));
        InstallWindowSubclass(_state.InputHwnd);
    }
    else
    {
        LOG_WARN("subclass lost to another WndProc; preserving previous original to avoid recursive WndProc chain "
                 "input:{} current:{} preservedOriginal:{}",
                 static_cast<void*>(_state.InputHwnd), reinterpret_cast<std::uintptr_t>(currentWndProc),
                 reinterpret_cast<std::uintptr_t>(_state.OriginalWndProc));
    }
}

bool IsDeferredWindowRemovalMessage(UINT msg)
{
    const UINT message = DeferredWindowRemovalMessage();
    return message != 0 && msg == message;
}

bool RemoveWindowSubclass()
{
    if (_state.InputHwnd == nullptr && _state.OriginalWndProc != nullptr)
        return false;
    const HWND hwnd = _state.InputHwnd;
    return HookLifetime::RemoveWindowSubclassOnOwner(
        hwnd, IsWindow(hwnd) != FALSE, GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId(),
        static_cast<WNDPROC>(OptiInputWndProc), _state.OriginalWndProc, _state.WndProcSubclassed,
        _state.DeferredWndProcRemoval,
        [](WNDPROC& current) { return TryGetWindowProc(_state.InputHwnd, &current); },
        [](WNDPROC original)
        {
            // This operation is reached only on the owning window thread.
            SetLastError(0);
            const LONG_PTR previous = SetWindowLongPtrW(_state.InputHwnd, GWLP_WNDPROC,
                                                       reinterpret_cast<LONG_PTR>(original));
            return previous != 0 || GetLastError() == 0;
        },
        [hwnd](std::uintptr_t token)
        {
            const UINT message = DeferredWindowRemovalMessage();
            return message != 0 && PostMessageW(hwnd, message, static_cast<WPARAM>(token), 0) != FALSE;
        });
}

bool HandleDeferredWindowRemovalLocked(HWND hwnd, UINT msg, WPARAM token)
{
    const UINT message = DeferredWindowRemovalMessage();
    if (message == 0 || msg != message)
        return false;
    if (hwnd == _state.InputHwnd && _state.DeferredWndProcRemoval.Consume(hwnd, token))
        RemoveWindowSubclass();
    return true;
}

WNDPROC GetOriginalInputWndProcLocked(HWND hwnd)
{
    const auto target = std::find_if(wndProcTargets.begin(), wndProcTargets.end(),
                                    [hwnd](const auto& entry) { return entry.first == hwnd; });
    return target != wndProcTargets.end() ? target->second : nullptr;
}

bool IsInputWindow(HWND hwnd)
{
    if (hwnd == nullptr)
        return false;

    if (hwnd == _state.InputHwnd)
        return true;

    HWND root = GetAncestor(hwnd, GA_ROOT);

    if (root != nullptr && root == _state.InputRootHwnd)
        return true;

    return false;
}

bool IsTargetWindow(HWND hwnd)
{
    // Historical name retained for the message-processing code. The only HWNDs
    // that should be consumed by OptiInput are input HWNDs in the current
    // process, not a foreign-process game TargetHwnd.
    return IsInputWindow(hwnd);
}

void UpdateFocusState(HWND targetHwnd)
{
    HWND foreground = GetForegroundWindow();

    DWORD foregroundProcessId = 0;
    DWORD foregroundThreadId = 0;

    if (foreground != nullptr)
        foregroundThreadId = GetWindowThreadProcessId(foreground, &foregroundProcessId);

    if (foreground == nullptr)
    {
        SetFocusStateLocked(false, "no foreground", foreground, foregroundProcessId, foregroundThreadId);
        return;
    }

    HWND foregroundRoot = GetAncestor(foreground, GA_ROOT);

    if (foregroundRoot != nullptr && _state.InputRootHwnd != nullptr && foregroundRoot == _state.InputRootHwnd)
    {
        SetFocusStateLocked(true, "foreground matches input root", foreground, foregroundProcessId, foregroundThreadId);
        return;
    }

    HWND targetRoot = targetHwnd != nullptr ? GetAncestor(targetHwnd, GA_ROOT) : _state.TargetRootHwnd;

    if (foregroundRoot != nullptr && targetRoot != nullptr && foregroundRoot == targetRoot)
    {
        SetFocusStateLocked(true, "foreground matches target root", foreground, foregroundProcessId,
                            foregroundThreadId);
        return;
    }

    if (_state.IsUwp)
    {
        const bool focused = foregroundProcessId != 0 && foregroundProcessId == _state.TargetProcessId;
        SetFocusStateLocked(focused, focused ? "uwp foreground pid matches target" : "uwp foreground pid mismatch",
                            foreground, foregroundProcessId, foregroundThreadId);
        return;
    }

    SetFocusStateLocked(false, "foreground does not match input/target", foreground, foregroundProcessId,
                        foregroundThreadId);
}

} // namespace OptiInput

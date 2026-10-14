#pragma once
// NrGameWaitDiag (default off, needs NrFrameDiag): game-thread timing between Presents.
// Timestamps only: Present return -> first ExecuteCommandLists, and fence waits
// (ID3D12Fence::SetEventOnCompletion on a not-yet-completed value, then the
// WaitForSingleObject(Ex) on that event). Adds no sync waits; hooks stay installed
// (pass-through) after the first enable.
// Limits: WaitForMultipleObjects and other wait APIs are not sampled; zero-timeout
// probes are not counted. presentWallMs is wall time inside Present (includes
// blocking, frame limiting and descheduling), not CPU execution time.
#include "NrFrameDiag.h"
#include <detours/detours.h>
#include <tlhelp32.h>
#include <mutex>
#include <vector>

namespace DlssNr::GameWait
{
struct Snapshot
{
    uint64_t present = 0;          // game Present count when this interval closed
    int64_t beginQpc = 0, endQpc = 0; // the interval: previous Present begin -> this Present begin
    double presentMs = 0;          // Present begin -> next Present begin
    double presentWallMs = 0;      // wall time inside the previous Present (not CPU time)
    double toEclMs = -1;           // Present return -> first submit on the game thread
    double toWaitMs = -1;          // Present return -> first pending fence wait
    double waitMs = 0;             // total time blocked on pending game fences (sampled waits only)
    unsigned waits = 0, ecls = 0;
};

// Install states: 0 not installed, 1 installing, 2 installed, 3 failed (retry later).
struct StateData
{
    std::atomic<DWORD> tid {0};
    std::atomic<int> install {0};
    std::atomic<unsigned> attempts {0};
    std::atomic<bool> loggedFailure {false};
    std::atomic<uint32_t> generation {0}; // bumped on each off->on; clears thread-local pending events
    std::mutex mu;
    int64_t presentBegin = 0, prevPresentBegin = 0, presentEnd = 0, firstEcl = 0, firstWait = 0, waitQpc = 0;
    unsigned waits = 0, ecls = 0;
    uint64_t presents = 0;
    Snapshot last;
    bool wasOn = false;
};
inline StateData& S() { static StateData s; return s; }
inline std::atomic<bool>& On() { static std::atomic<bool> on {false}; return on; }
inline bool GameThread() { return On().load(std::memory_order_relaxed) && GetCurrentThreadId() == S().tid.load(std::memory_order_relaxed); }

// Events registered for not-yet-completed fence values on the game thread. Several can be
// pending; an entry is dropped only when its wait returns signalled (a timed-out poll keeps it).
struct Pending { HANDLE events[8] {}; unsigned count = 0; uint32_t generation = 0; };
inline thread_local Pending t_pending;
inline Pending& PendingSet()
{
    const auto g = S().generation.load(std::memory_order_relaxed);
    if (t_pending.generation != g) { t_pending = {}; t_pending.generation = g; }
    return t_pending;
}
inline int FindPending(HANDLE h)
{
    auto& p = PendingSet();
    for (unsigned i = 0; i < p.count; ++i) if (p.events[i] == h) return int(i);
    return -1;
}
inline void AddPending(HANDLE h)
{
    auto& p = PendingSet();
    if (FindPending(h) >= 0) return;
    if (p.count == 8) { for (unsigned i = 1; i < 8; ++i) p.events[i - 1] = p.events[i]; p.count = 7; } // drop oldest
    p.events[p.count++] = h;
}
inline void DropPending(int i)
{
    auto& p = PendingSet();
    for (unsigned j = unsigned(i) + 1; j < p.count; ++j) p.events[j - 1] = p.events[j];
    --p.count;
}

inline void NoteWait(int64_t begin, int64_t end)
{
    auto& s = S(); std::lock_guard lock(s.mu);
    if (!s.firstWait) s.firstWait = begin;
    s.waitQpc += end - begin; ++s.waits;
}

using SetEventFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
using WaitFn = DWORD(WINAPI*)(HANDLE, DWORD);
using WaitExFn = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
inline SetEventFn o_SetEvent = nullptr;
inline WaitFn o_Wait = nullptr;
inline WaitExFn o_WaitEx = nullptr;

inline HRESULT STDMETHODCALLTYPE hkSetEvent(ID3D12Fence* f, UINT64 v, HANDLE e)
{
    if (!GameThread() || f->GetCompletedValue() >= v) return o_SetEvent(f, v, e);
    if (e) { const HRESULT hr = o_SetEvent(f, v, e); if (SUCCEEDED(hr)) AddPending(e); return hr; }
    const int64_t b = FrameDiag::Qpc(); const HRESULT hr = o_SetEvent(f, v, e); NoteWait(b, FrameDiag::Qpc()); return hr;
}
template<class Call> inline DWORD TimedWait(HANDLE h, DWORD ms, Call call)
{
    // Zero-timeout probes pass through untimed and keep the event pending for the real wait.
    if (!h || ms == 0 || !GameThread()) return call();
    const int i = FindPending(h);
    if (i < 0) return call();
    const int64_t b = FrameDiag::Qpc(); const DWORD r = call(); NoteWait(b, FrameDiag::Qpc());
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED || r == WAIT_FAILED) {
        const int j = FindPending(h); if (j >= 0) DropPending(j);
    }
    return r;
}
inline DWORD WINAPI hkWait(HANDLE h, DWORD ms) { return TimedWait(h, ms, [&] { return o_Wait(h, ms); }); }
inline DWORD WINAPI hkWaitEx(HANDLE h, DWORD ms, BOOL a) { return TimedWait(h, ms, [&] { return o_WaitEx(h, ms, a); }); }

// Detours must update every other thread of the process (Microsoft: DetourUpdateThread on the
// GetCurrentThread() pseudo-handle does nothing), so a thread inside a patched prologue is
// moved off it. Handles are collected before the transaction so nothing allocates while
// other threads are suspended. Installed state is published only after a successful commit.
inline bool Install(ID3D12CommandQueue* q)
{
    auto& s = S();
    int expected = s.install.load(std::memory_order_acquire);
    if (expected == 2) return true;
    if (expected == 1) return false;
    if (expected == 3 && s.attempts.load(std::memory_order_relaxed) >= 3) return false;
    if (!s.install.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return expected == 2;
    s.attempts.fetch_add(1, std::memory_order_relaxed);
    const char* failure = nullptr; LONG code = NO_ERROR;
    std::vector<HANDLE> threads;
    SetEventFn setEvent = nullptr; WaitFn wait = nullptr; WaitExFn waitEx = nullptr;
    do {
        ID3D12Device* dev = nullptr; ID3D12Fence* fence = nullptr;
        if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) { failure = "GetDevice"; break; }
        const HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        dev->Release();
        if (FAILED(hr)) { failure = "CreateFence"; code = hr; break; }
        setEvent = reinterpret_cast<SetEventFn>((*reinterpret_cast<void***>(fence))[9]); // ID3D12Fence::SetEventOnCompletion
        fence->Release();
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        wait = k32 ? reinterpret_cast<WaitFn>(GetProcAddress(k32, "WaitForSingleObject")) : nullptr;
        waitEx = k32 ? reinterpret_cast<WaitExFn>(GetProcAddress(k32, "WaitForSingleObjectEx")) : nullptr;
        if (!setEvent || !wait || !waitEx) { failure = "resolve targets"; break; }
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) { failure = "thread snapshot"; code = LONG(GetLastError()); break; }
        threads.reserve(256);
        THREADENTRY32 te {}; te.dwSize = sizeof te;
        const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID))
                threads.push_back(t); // a thread that exits meanwhile is harmless
        }
        CloseHandle(snap);
        o_SetEvent = setEvent; o_Wait = wait; o_WaitEx = waitEx; // originals until commit swaps in trampolines
        if ((code = DetourTransactionBegin()) != NO_ERROR) { failure = "DetourTransactionBegin"; break; }
        for (HANDLE t : threads) DetourUpdateThread(t); // suspends; resumed by commit/abort
        if ((code = DetourAttach(reinterpret_cast<PVOID*>(&o_SetEvent), hkSetEvent)) != NO_ERROR ||
            (code = DetourAttach(reinterpret_cast<PVOID*>(&o_Wait), hkWait)) != NO_ERROR ||
            (code = DetourAttach(reinterpret_cast<PVOID*>(&o_WaitEx), hkWaitEx)) != NO_ERROR) {
            DetourTransactionAbort(); failure = "DetourAttach"; break;
        }
        if ((code = DetourTransactionCommit()) != NO_ERROR) { failure = "DetourTransactionCommit"; break; }
    } while (false);
    for (HANDLE t : threads) CloseHandle(t);
    if (!failure) { s.install.store(2, std::memory_order_release); return true; }
    o_SetEvent = setEvent; o_Wait = wait; o_WaitEx = waitEx; // abort/failed commit leaves the targets unpatched
    s.install.store(3, std::memory_order_release);
    if (!s.loggedFailure.exchange(true)) {
#ifdef LOG_WARN
        LOG_WARN("NrGameWaitDiag: hook install failed at {} (code {}); gw_* fence waits not sampled, will retry", failure, code);
#else
        (void)code;
#endif
    }
    return false;
}

// Present entry/exit on the presenting (game) thread. enabled = NrFrameDiag && NrGameWaitDiag.
inline void PresentBegin(bool enabled) noexcept
{
    On().store(enabled, std::memory_order_relaxed);
    auto& s = S();
    if (!enabled) { s.wasOn = false; return; }
    s.tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const int64_t now = FrameDiag::Qpc();
    std::lock_guard lock(s.mu);
    if (!s.wasOn) {
        // off -> on: start a fresh interval; no stale snapshot or pending events from the last session.
        s.wasOn = true; s.generation.fetch_add(1, std::memory_order_relaxed);
        s.presentEnd = s.firstEcl = s.firstWait = s.waitQpc = 0; s.waits = s.ecls = 0; s.last = {};
        s.prevPresentBegin = now;
        return;
    }
    if (s.presentEnd) {
        Snapshot n; n.present = ++s.presents;
        n.beginQpc = s.prevPresentBegin; n.endQpc = now;
        n.presentMs = FrameDiag::MsBetween(s.prevPresentBegin, now);
        n.presentWallMs = FrameDiag::MsBetween(s.prevPresentBegin, s.presentEnd);
        n.toEclMs = s.firstEcl ? FrameDiag::MsBetween(s.presentEnd, s.firstEcl) : -1;
        n.toWaitMs = s.firstWait ? FrameDiag::MsBetween(s.presentEnd, s.firstWait) : -1;
        n.waitMs = s.waitQpc * 1000.0 / double(FrameDiag::QpcFrequency());
        n.waits = s.waits; n.ecls = s.ecls;
        s.last = n;
    }
    s.prevPresentBegin = now;
}
inline void PresentEnd() noexcept
{
    if (!On().load(std::memory_order_relaxed)) return;
    auto& s = S(); std::lock_guard lock(s.mu);
    s.presentEnd = FrameDiag::Qpc();
    s.firstEcl = s.firstWait = s.waitQpc = 0; s.waits = s.ecls = 0;
}
inline void OnExecute(ID3D12CommandQueue* q) noexcept
{
    if (!GameThread()) return;
    if (S().install.load(std::memory_order_acquire) != 2) { try { Install(q); } catch (...) {} }
    auto& s = S(); std::lock_guard lock(s.mu);
    if (!s.firstEcl) s.firstEcl = FrameDiag::Qpc();
    ++s.ecls;
}
inline bool Last(Snapshot& out) noexcept
{
    if (!On().load(std::memory_order_relaxed)) return false;
    auto& s = S(); std::lock_guard lock(s.mu);
    if (!s.last.present) return false;
    out = s.last; return true;
}
} // namespace DlssNr::GameWait
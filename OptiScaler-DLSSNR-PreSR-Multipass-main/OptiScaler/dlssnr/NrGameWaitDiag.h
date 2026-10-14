#pragma once
// NrGameWaitDiag (default off, needs NrFrameDiag): game-thread timing between Presents.
// Timestamps only: Present return -> first ExecuteCommandLists, and fence waits
// (ID3D12Fence::SetEventOnCompletion on a not-yet-completed value, then the
// WaitForSingleObject(Ex) on that event). No sync waits, no behavior change.
// Hooks install lazily the first time the option is on and stay pass-through.
#include "NrFrameDiag.h"
#include <detours/detours.h>
#include <mutex>

namespace DlssNr::GameWait
{
struct Snapshot
{
    uint64_t present = 0;          // game Present count when this interval closed
    double presentMs = 0;          // Present begin -> next Present begin
    double presentCpuMs = 0;       // time inside Present
    double toEclMs = -1;           // Present return -> first submit on the game thread
    double toWaitMs = -1;          // Present return -> first pending fence wait
    double waitMs = 0;             // total time blocked on pending game fences
    unsigned waits = 0, ecls = 0;
};

struct StateData
{
    std::atomic<DWORD> tid {0};
    std::atomic<bool> hooked {false};
    std::mutex mu;
    int64_t presentBegin = 0, prevPresentBegin = 0, presentEnd = 0, firstEcl = 0, firstWait = 0, waitQpc = 0;
    unsigned waits = 0, ecls = 0;
    uint64_t presents = 0;
    Snapshot last;
};
inline StateData& S() { static StateData s; return s; }
inline std::atomic<bool>& On() { static std::atomic<bool> on {false}; return on; }
inline bool GameThread() { return On().load(std::memory_order_relaxed) && GetCurrentThreadId() == S().tid.load(std::memory_order_relaxed); }

inline thread_local HANDLE t_pendingEvent = nullptr;

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
    if (e) { t_pendingEvent = e; return o_SetEvent(f, v, e); }
    const int64_t b = FrameDiag::Qpc(); const HRESULT hr = o_SetEvent(f, v, e); NoteWait(b, FrameDiag::Qpc()); return hr;
}
inline DWORD WINAPI hkWait(HANDLE h, DWORD ms)
{
    if (!h || h != t_pendingEvent || !GameThread()) return o_Wait(h, ms);
    t_pendingEvent = nullptr;
    const int64_t b = FrameDiag::Qpc(); const DWORD r = o_Wait(h, ms); NoteWait(b, FrameDiag::Qpc()); return r;
}
inline DWORD WINAPI hkWaitEx(HANDLE h, DWORD ms, BOOL a)
{
    if (!h || h != t_pendingEvent || !GameThread()) return o_WaitEx(h, ms, a);
    t_pendingEvent = nullptr;
    const int64_t b = FrameDiag::Qpc(); const DWORD r = o_WaitEx(h, ms, a); NoteWait(b, FrameDiag::Qpc()); return r;
}

inline void Install(ID3D12CommandQueue* q)
{
    auto& s = S();
    if (s.hooked.exchange(true)) return;
    ID3D12Device* dev = nullptr; ID3D12Fence* fence = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev)))) return;
    const HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    dev->Release();
    if (FAILED(hr)) return;
    o_SetEvent = reinterpret_cast<SetEventFn>((*reinterpret_cast<void***>(fence))[9]); // ID3D12Fence::SetEventOnCompletion
    fence->Release();
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    o_Wait = k32 ? reinterpret_cast<WaitFn>(GetProcAddress(k32, "WaitForSingleObject")) : nullptr;
    o_WaitEx = k32 ? reinterpret_cast<WaitExFn>(GetProcAddress(k32, "WaitForSingleObjectEx")) : nullptr;
    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&o_SetEvent), hkSetEvent);
    if (o_Wait) DetourAttach(reinterpret_cast<PVOID*>(&o_Wait), hkWait);
    if (o_WaitEx) DetourAttach(reinterpret_cast<PVOID*>(&o_WaitEx), hkWaitEx);
    DetourTransactionCommit();
}

// Present entry/exit on the presenting (game) thread. enabled = NrFrameDiag && NrGameWaitDiag.
inline void PresentBegin(bool enabled) noexcept
{
    On().store(enabled, std::memory_order_relaxed);
    if (!enabled) return;
    auto& s = S();
    s.tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const int64_t now = FrameDiag::Qpc();
    std::lock_guard lock(s.mu);
    if (s.presentEnd) {
        Snapshot n; n.present = ++s.presents;
        n.presentMs = FrameDiag::MsBetween(s.prevPresentBegin, now);
        n.presentCpuMs = FrameDiag::MsBetween(s.prevPresentBegin, s.presentEnd);
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
    try { Install(q); } catch (...) {}
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

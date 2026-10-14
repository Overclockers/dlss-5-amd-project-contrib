#pragma once
// Minimal d3dkmthk.h for builds without the Windows Driver Kit.
//
// MochizukiNrRuntime.cpp needs exactly three user-mode entries from this header - D3DKMTOpenAdapterFromLuid,
// D3DKMTQueryVideoMemoryInfo and D3DKMTCloseAdapter - and the two structs and one enum they take. The real
// header ships with the WDK, not with the Windows SDK: a WIN32_LEAN_AND_MEAN user-mode translation unit that
// includes <windows.h> and <d3d12.h> gets D3DKMT_HANDLE from d3dukmdt.h but not the thunks, so a machine with
// only the MSVC build tools and the SDK cannot compile the VRAM check (error C1083: d3dkmthk.h).
//
// Same idea as the dirent.h next to it: the file MSVC has no equivalent of, only what this product uses.
// Field order, types and enum values are the public definitions (_D3DKMT_QUERYVIDEOMEMORYINFO,
// _D3DKMT_OPENADAPTERFROMLUID and D3DKMT_MEMORY_SEGMENT_GROUP); nothing here is private to a driver build.
// Windows has no ABI fallback for these: the adapter handle and the query result cross into gdi32.dll, whose
// exports are the same ones DXGI's QueryVideoMemoryInfo uses.
//
// Builds that do have the WDK keep using it: MochizukiNrRuntime.cpp includes this file only after
// __has_include(<d3dkmthk.h>) has failed.

#ifndef _D3DKMT_H_
#define _D3DKMT_H_

// D3DKMT_HANDLE and the segment group enum's home. The Windows SDK supplies it; the guard keeps a build that
// includes the real header first (WDK present, this file still reached) consistent.
#if __has_include(<d3dukmdt.h>)
#include <d3dukmdt.h>
#endif

#ifndef _D3DUKMDT_H_
// Fallback for the same SDK-without-WDK case: d3dukmdt.h is itself a WDK header.
typedef UINT32 D3DKMT_HANDLE;

typedef enum _D3DKMT_MEMORY_SEGMENT_GROUP
{
    D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1
} D3DKMT_MEMORY_SEGMENT_GROUP;
#endif

typedef struct _D3DKMT_OPENADAPTERFROMLUID
{
    LUID AdapterLuid;
    D3DKMT_HANDLE hAdapter;
} D3DKMT_OPENADAPTERFROMLUID;

typedef struct _D3DKMT_QUERYVIDEOMEMORYINFO
{
    D3DKMT_HANDLE hProcess;                      // NULL: the calling process
    D3DKMT_HANDLE hAdapter;
    D3DKMT_MEMORY_SEGMENT_GROUP MemorySegmentGroup;
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 CurrentReservation;
    UINT64 AvailableForReservation;
    UINT64 PhysicalAdapterIndex;
} D3DKMT_QUERYVIDEOMEMORYINFO;

typedef struct _D3DKMT_CLOSEADAPTER
{
    D3DKMT_HANDLE hAdapter;
} D3DKMT_CLOSEADAPTER;

// NTSTATUS is what the engine and the header both use; winternl.h, included just above this file's user, is
// where the user-mode build gets it. gdi32.dll exports these names.
EXTERN_C NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(_Inout_ D3DKMT_OPENADAPTERFROMLUID*);
EXTERN_C NTSTATUS WINAPI D3DKMTQueryVideoMemoryInfo(_Inout_ D3DKMT_QUERYVIDEOMEMORYINFO*);
EXTERN_C NTSTATUS WINAPI D3DKMTCloseAdapter(_In_ const D3DKMT_CLOSEADAPTER*);

#endif // _D3DKMT_H_

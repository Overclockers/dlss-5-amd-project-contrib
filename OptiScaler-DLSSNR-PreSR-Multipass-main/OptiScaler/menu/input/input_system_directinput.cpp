#include "pch.h"
#include "input_system_internal.h"
#include "directinput_registry.h"
#include <utility>

#include <detours/detours.h>

#include <cstring>

namespace OptiInput
{
namespace
{
constexpr wchar_t DirectInput8ModuleName[] = L"dinput8.dll";
constexpr wchar_t DirectInputLegacyModuleName[] = L"dinput.dll";

constexpr char DirectInput8CreateExportName[] = "DirectInput8Create";
constexpr char DirectInputCreateAExportName[] = "DirectInputCreateA";
constexpr char DirectInputCreateWExportName[] = "DirectInputCreateW";
constexpr char DirectInputCreateExExportName[] = "DirectInputCreateEx";

constexpr GUID DirectInputSysKeyboardGuid = {
    0x6f1d2b61, 0xd5a0, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
};
constexpr GUID DirectInputSysMouseGuid = {
    0x6f1d2b60, 0xd5a0, 0x11cf, { 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 }
};

using DirectInputHooks::DeviceKind;
constexpr std::size_t MethodCapacity = 32;
template <class T> using Registry = DirectInputHooks::MethodRegistry<T, MethodCapacity>;
Registry<DirectInputCreateDevice_t> CreateHooks;
Registry<DirectInputDeviceRelease_t> ReleaseHooks;
Registry<DirectInputGetDeviceState_t> StateHooks;
Registry<DirectInputGetDeviceData_t> DataHooks;
DirectInputHooks::DeviceRegistry<MaxTrackedDirectInputDevices> Devices;
HookLifetime::CallbackGate DirectInputCallbackGate;
bool Stopping = false;

HRESULT InvokeCreate(std::size_t index, void* input, REFGUID guid, void** device, LPUNKNOWN outer);
ULONG InvokeRelease(std::size_t index, void* device);
HRESULT InvokeState(std::size_t index, void* device, DWORD size, LPVOID data);
HRESULT InvokeData(std::size_t index, void* device, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags);

template <std::size_t I> HRESULT WINAPI CreateProxy(void* input, REFGUID guid, void** device, LPUNKNOWN outer)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    return InvokeCreate(I, input, guid, device, outer);
}
template <std::size_t I> ULONG WINAPI ReleaseProxy(void* device)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    return InvokeRelease(I, device);
}
template <std::size_t I> HRESULT WINAPI StateProxy(void* device, DWORD size, LPVOID data)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    return InvokeState(I, device, size, data);
}
template <std::size_t I> HRESULT WINAPI DataProxy(void* device, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count,
                                               DWORD flags)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    return InvokeData(I, device, size, data, count, flags);
}
template <std::size_t... I> auto MakeCreateProxies(std::index_sequence<I...>)
{ return std::array<DirectInputCreateDevice_t, sizeof...(I)> { &CreateProxy<I>... }; }
template <std::size_t... I> auto MakeReleaseProxies(std::index_sequence<I...>)
{ return std::array<DirectInputDeviceRelease_t, sizeof...(I)> { &ReleaseProxy<I>... }; }
template <std::size_t... I> auto MakeStateProxies(std::index_sequence<I...>)
{ return std::array<DirectInputGetDeviceState_t, sizeof...(I)> { &StateProxy<I>... }; }
template <std::size_t... I> auto MakeDataProxies(std::index_sequence<I...>)
{ return std::array<DirectInputGetDeviceData_t, sizeof...(I)> { &DataProxy<I>... }; }
const auto CreateProxies = MakeCreateProxies(std::make_index_sequence<MethodCapacity> {});
const auto ReleaseProxies = MakeReleaseProxies(std::make_index_sequence<MethodCapacity> {});
const auto StateProxies = MakeStateProxies(std::make_index_sequence<MethodCapacity> {});
const auto DataProxies = MakeDataProxies(std::make_index_sequence<MethodCapacity> {});

bool IsDirectInputKeyboardGuid(REFGUID guid) { return IsEqualGUID(guid, DirectInputSysKeyboardGuid) != FALSE; }

bool IsDirectInputMouseGuid(REFGUID guid) { return IsEqualGUID(guid, DirectInputSysMouseGuid) != FALSE; }

DirectInputDeviceKind GetDirectInputDeviceKind(REFGUID guid)
{
    if (IsDirectInputKeyboardGuid(guid))
        return DirectInputDeviceKind::Keyboard;

    if (IsDirectInputMouseGuid(guid))
        return DirectInputDeviceKind::Mouse;

    return DirectInputDeviceKind::Other;
}


HMODULE FindLoadedDirectInput8Module() { return GetModuleHandleW(DirectInput8ModuleName); }

HMODULE FindLoadedDirectInputLegacyModule() { return GetModuleHandleW(DirectInputLegacyModuleName); }

void RefreshMethodFlagsLocked()
{
    _state.DirectInputDeviceReleaseHookInstalled = ReleaseHooks.AnyAttached();
    _state.DirectInputGetDeviceStateHookInstalled = StateHooks.AnyAttached();
    _state.DirectInputGetDeviceDataHookInstalled = DataHooks.AnyAttached();
}

void TrackDirectInputDeviceLocked(void* device, DirectInputDeviceKind kind)
{
    const auto deviceKind = static_cast<DeviceKind>(kind);
    if (!Devices.Track(device, deviceKind))
    {
        LOG_WARN("DirectInput device tracking table full device:{}", device);
        return;
    }
    _state.DirectInputTrackedDeviceCount = Devices.Count();
    if (kind == DirectInputDeviceKind::Keyboard) _state.DirectInputKeyboardDeviceSeen = true;
    else if (kind == DirectInputDeviceKind::Mouse) _state.DirectInputMouseDeviceSeen = true;
    else _state.DirectInputOtherDeviceSeen = true;
}

struct TargetReference
{
    void* Target = nullptr;
    HMODULE Module = nullptr;
    std::size_t Owners = 0;
};
std::array<TargetReference, MethodCapacity * 4 + 4> TargetReferences {};

bool RetainTarget(void* target)
{
    if (!target || !RetainInputCallbackModule(reinterpret_cast<const void*>(&CreateProxy<0>))) return false;
    for (auto& reference : TargetReferences)
        if (reference.Target == target) { ++reference.Owners; return true; }
    for (auto& reference : TargetReferences)
    {
        if (reference.Target) continue;
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(target), &module)) return false;
        reference = {target, module, 1};
        return true;
    }
    return false;
}

void ReleaseTarget(void* target)
{
    for (auto& reference : TargetReferences)
        if (reference.Target == target)
        {
            if (--reference.Owners == 0)
            {
                const auto module = reference.Module;
                reference = {};
                FreeLibrary(module);
            }
            return;
        }
}

template <class T> std::size_t Prepare(Registry<T>& hooks, T target)
{
    if (!target) return hooks.Missing;
    auto index = hooks.Find(target);
    if (index != hooks.Missing) return index;
    index = hooks.Prepare(target);
    if (index == hooks.Missing) return index; // Full tables retain no module.
    if (!RetainTarget(reinterpret_cast<void*>(target)))
    {
        hooks.Rollback(index, true);
        return hooks.Missing;
    }
    return index;
}

template <class T> void Rollback(Registry<T>& hooks, std::size_t index, bool newlyReserved)
{
    if (newlyReserved && index != hooks.Missing)
    {
        auto target = hooks.Slots[index].Target;
        hooks.Rollback(index, true);
        ReleaseTarget(reinterpret_cast<void*>(target));
    }
}

template <class T> bool QueueAttach(HookLifetime::Transaction<DetourBackend>& transaction, Registry<T>& hooks,
                                    std::size_t index, const std::array<T, MethodCapacity>& proxies)
{
    return hooks.AttachPending(transaction, index, proxies);
}

bool HookDirectInputDeviceLocked(void* device, DirectInputDeviceKind kind)
{
    if (!device || Stopping) return false;
    auto** vtable = *reinterpret_cast<void***>(device);
    auto release = reinterpret_cast<DirectInputDeviceRelease_t>(vtable[2]);
    auto state = reinterpret_cast<DirectInputGetDeviceState_t>(vtable[9]);
    auto data = reinterpret_cast<DirectInputGetDeviceData_t>(vtable[10]);
    const bool newRelease = ReleaseHooks.Find(release) == ReleaseHooks.Missing;
    const bool newState = StateHooks.Find(state) == StateHooks.Missing;
    const bool newData = DataHooks.Find(data) == DataHooks.Missing;
    const auto r = Prepare(ReleaseHooks, release);
    const auto s = Prepare(StateHooks, state);
    const auto d = Prepare(DataHooks, data);
    bool success = false;
    if (r != ReleaseHooks.Missing && s != StateHooks.Missing && d != DataHooks.Missing)
    {
        if (ReleaseHooks.Slots[r].Attached && StateHooks.Slots[s].Attached && DataHooks.Slots[d].Attached)
            success = true;
        else
        {
            HookLifetime::Transaction<DetourBackend> transaction;
            QueueAttach(transaction, ReleaseHooks, r, ReleaseProxies);
            QueueAttach(transaction, StateHooks, s, StateProxies);
            QueueAttach(transaction, DataHooks, d, DataProxies);
            success = transaction.Commit();
        }
    }
    if (!success)
    {
        Rollback(ReleaseHooks, r, newRelease);
        Rollback(StateHooks, s, newState);
        Rollback(DataHooks, d, newData);
        LOG_WARN("DirectInput method registration failed device:{}", device);
        return false;
    }
    ReleaseHooks.Slots[r].Attached = StateHooks.Slots[s].Attached = DataHooks.Slots[d].Attached = true;
    RefreshMethodFlagsLocked();
    TrackDirectInputDeviceLocked(device, kind);
    return true;
}

bool HookDirectInputInterfaceLocked(void* directInput, bool wide)
{
    if (!directInput || Stopping) return false;
    auto** vtable = *reinterpret_cast<void***>(directInput);
    auto target = reinterpret_cast<DirectInputCreateDevice_t>(vtable[3]);
    const bool newlyReserved = CreateHooks.Find(target) == CreateHooks.Missing;
    auto index = Prepare(CreateHooks, target);
    if (index == CreateHooks.Missing) return false;
    if (!CreateHooks.Slots[index].Attached)
    {
        bool success = false;
        {
            HookLifetime::Transaction<DetourBackend> transaction;
            QueueAttach(transaction, CreateHooks, index, CreateProxies);
            success = transaction.Commit();
        }
        if (!success)
        {
            Rollback(CreateHooks, index, newlyReserved);
            return false;
        }
        CreateHooks.Slots[index].Attached = true;
    }
    if (wide) _state.DirectInputCreateDeviceWHookInstalled = true;
    else _state.DirectInputCreateDeviceAHookInstalled = true;
    return true;
}

bool TryGetDirectInputInterfaceWidth(REFIID riid, bool* wide)
{
    if (wide == nullptr)
        return false;

    if (IsEqualGUID(riid, IID_IDirectInput8W) || IsEqualGUID(riid, IID_IDirectInput7W) ||
        IsEqualGUID(riid, IID_IDirectInput2W) || IsEqualGUID(riid, IID_IDirectInputW))
    {
        *wide = true;
        return true;
    }

    if (IsEqualGUID(riid, IID_IDirectInput8A) || IsEqualGUID(riid, IID_IDirectInput7A) ||
        IsEqualGUID(riid, IID_IDirectInput2A) || IsEqualGUID(riid, IID_IDirectInputA))
    {
        *wide = false;
        return true;
    }

    return false;
}

void HandleDirectInputCreatedLocked(REFIID riid, void** out, const char* source)
{
    if (out == nullptr || *out == nullptr)
        return;

    bool wide = false;
    if (TryGetDirectInputInterfaceWidth(riid, &wide))
    {
        HookDirectInputInterfaceLocked(*out, wide);
        return;
    }

    OPTIINPUT_LOG_VERBOSE("{} returned unsupported riid directInput:{} riid:{}",
                          source != nullptr ? source : "DirectInput", *out, static_cast<const void*>(&riid));
}

void HandleLegacyDirectInputCreatedLocked(void** out, bool wide)
{
    if (out == nullptr || *out == nullptr)
        return;

    HookDirectInputInterfaceLocked(*out, wide);
}

bool InstallDirectInputExportHookLocked(HMODULE module, const char* exportName, void** original, void* hook,
                                        bool* installed)
{
    if (module == nullptr || exportName == nullptr || original == nullptr || hook == nullptr || installed == nullptr)
        return false;

    if (*installed)
        return true;

    void* target = reinterpret_cast<void*>(GetProcAddress(module, exportName));
    const bool alreadyRetained = target != nullptr && *original == target;
    if (!alreadyRetained && !RetainTarget(target))
    {
        *original = nullptr;
        return false;
    }
    *original = target;
    HookLifetime::Transaction<DetourBackend> transaction;
    transaction.Attach(original, hook);
    if (!transaction.Commit())
    {
        if (!alreadyRetained)
        {
            ReleaseTarget(target);
            *original = nullptr;
        }
        return false;
    }

    *installed = true;
    LOG_INFO("{} hook installed module:{}", exportName, static_cast<void*>(module));
    return true;
}

HRESULT CallDirectInputCreateDeviceOriginal(DirectInputCreateDevice_t original, void* directInput, REFGUID guid,
                                            void** device, LPUNKNOWN outer)
{
    if (original == nullptr)
        return DIERR_GENERIC;

    ScopedHookBypass bypass;
    return original(directInput, guid, device, outer);
}
} // namespace

void UpdateDirectInputIntegrationLocked()
{
    Stopping = false;
    HMODULE module8 = FindLoadedDirectInput8Module();
    HMODULE legacyModule = FindLoadedDirectInputLegacyModule();

    _state.DirectInputModule = module8;
    _state.DirectInputLegacyModule = legacyModule;
    _state.DirectInputModuleLoaded = module8 != nullptr || legacyModule != nullptr;
    _state.DirectInputLegacyModuleLoaded = legacyModule != nullptr;

    if (module8 != nullptr)
    {
        if (!InstallDirectInputExportHookLocked(module8, DirectInput8CreateExportName,
                                                reinterpret_cast<void**>(&o_DirectInput8Create), hkDirectInput8Create,
                                                &_state.DirectInput8CreateHookInstalled))
        {
            if (o_DirectInput8Create == nullptr)
            {
                OPTIINPUT_LOG_VERBOSE("DirectInput8Create export was not found module:{}", static_cast<void*>(module8));
            }
        }
    }

    if (legacyModule != nullptr)
    {
        InstallDirectInputExportHookLocked(legacyModule, DirectInputCreateAExportName,
                                           reinterpret_cast<void**>(&o_DirectInputCreateA), hkDirectInputCreateA,
                                           &_state.DirectInputCreateAHookInstalled);

        InstallDirectInputExportHookLocked(legacyModule, DirectInputCreateWExportName,
                                           reinterpret_cast<void**>(&o_DirectInputCreateW), hkDirectInputCreateW,
                                           &_state.DirectInputCreateWHookInstalled);

        InstallDirectInputExportHookLocked(legacyModule, DirectInputCreateExExportName,
                                           reinterpret_cast<void**>(&o_DirectInputCreateEx), hkDirectInputCreateEx,
                                           &_state.DirectInputCreateExHookInstalled);
    }
}

template <class T> void QueueDetach(HookLifetime::Transaction<DetourBackend>& transaction, Registry<T>& hooks,
                                    const std::array<T, MethodCapacity>& proxies)
{
    hooks.DetachAttached(transaction, proxies);
}

bool RemoveDirectInputHooksLocked()
{
    Stopping = true;
    HookLifetime::CloseLease close(DirectInputCallbackGate);
    if (!close) return false;
    HookLifetime::Transaction<DetourBackend> transaction;
    if (_state.DirectInput8CreateHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_DirectInput8Create), reinterpret_cast<void*>(hkDirectInput8Create));
    if (_state.DirectInputCreateAHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_DirectInputCreateA), reinterpret_cast<void*>(hkDirectInputCreateA));
    if (_state.DirectInputCreateWHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_DirectInputCreateW), reinterpret_cast<void*>(hkDirectInputCreateW));
    if (_state.DirectInputCreateExHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_DirectInputCreateEx), reinterpret_cast<void*>(hkDirectInputCreateEx));
    QueueDetach(transaction, CreateHooks, CreateProxies);
    QueueDetach(transaction, ReleaseHooks, ReleaseProxies);
    QueueDetach(transaction, StateHooks, StateProxies);
    QueueDetach(transaction, DataHooks, DataProxies);
    if (!transaction.Commit()) return false;
    CreateHooks.Detached();
    ReleaseHooks.Detached();
    StateHooks.Detached();
    DataHooks.Detached();
    _state.DirectInput8CreateHookInstalled = false;
    _state.DirectInputCreateAHookInstalled = false;
    _state.DirectInputCreateWHookInstalled = false;
    _state.DirectInputCreateExHookInstalled = false;
    _state.DirectInputCreateDeviceAHookInstalled = false;
    _state.DirectInputCreateDeviceWHookInstalled = false;
    RefreshMethodFlagsLocked();
    Devices.Clear();
    _state.DirectInputTrackedDeviceCount = 0;
    return true;
}

void MarkDirectInputBufferedDataForDrainLocked() { Devices.MarkDrain(); }

HRESULT WINAPI hkDirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    HRESULT result = DIERR_GENERIC;

    if (o_DirectInput8Create != nullptr)
    {
        ScopedHookBypass bypass;
        result = o_DirectInput8Create(instance, version, riid, out, outer);
    }

    {
        std::unique_lock lock(_state.Mutex);
        _state.DirectInputCreateCallCount++;

        if (SUCCEEDED(result))
        {
            _state.DirectInputCreateSucceededCount++;
            HandleDirectInputCreatedLocked(riid, reinterpret_cast<void**>(out), "DirectInput8Create");
        }
        else
        {
            _state.DirectInputCreateFailedCount++;
        }
    }

    return result;
}

HRESULT WINAPI hkDirectInputCreateA(HINSTANCE instance, DWORD version, void** out, LPUNKNOWN outer)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    HRESULT result = DIERR_GENERIC;

    if (o_DirectInputCreateA != nullptr)
    {
        ScopedHookBypass bypass;
        result = o_DirectInputCreateA(instance, version, out, outer);
    }

    {
        std::unique_lock lock(_state.Mutex);
        _state.DirectInputCreateCallCount++;

        if (SUCCEEDED(result))
        {
            _state.DirectInputCreateSucceededCount++;
            HandleLegacyDirectInputCreatedLocked(out, false);
        }
        else
        {
            _state.DirectInputCreateFailedCount++;
        }
    }

    return result;
}

HRESULT WINAPI hkDirectInputCreateW(HINSTANCE instance, DWORD version, void** out, LPUNKNOWN outer)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    HRESULT result = DIERR_GENERIC;

    if (o_DirectInputCreateW != nullptr)
    {
        ScopedHookBypass bypass;
        result = o_DirectInputCreateW(instance, version, out, outer);
    }

    {
        std::unique_lock lock(_state.Mutex);
        _state.DirectInputCreateCallCount++;

        if (SUCCEEDED(result))
        {
            _state.DirectInputCreateSucceededCount++;
            HandleLegacyDirectInputCreatedLocked(out, true);
        }
        else
        {
            _state.DirectInputCreateFailedCount++;
        }
    }

    return result;
}

HRESULT WINAPI hkDirectInputCreateEx(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    HookLifetime::CallbackLease lease(DirectInputCallbackGate);
    HRESULT result = DIERR_GENERIC;

    if (o_DirectInputCreateEx != nullptr)
    {
        ScopedHookBypass bypass;
        result = o_DirectInputCreateEx(instance, version, riid, out, outer);
    }

    {
        std::unique_lock lock(_state.Mutex);
        _state.DirectInputCreateCallCount++;

        if (SUCCEEDED(result))
        {
            _state.DirectInputCreateSucceededCount++;
            HandleDirectInputCreatedLocked(riid, reinterpret_cast<void**>(out), "DirectInputCreateEx");
        }
        else
        {
            _state.DirectInputCreateFailedCount++;
        }
    }

    return result;
}

namespace
{
HRESULT InvokeCreate(std::size_t index, void* input, REFGUID guid, void** device, LPUNKNOWN outer)
{
    DirectInputCreateDevice_t original;
    {
        std::unique_lock lock(_state.Mutex);
        original = CreateHooks.Resolve(index);
    }
    const HRESULT result = CallDirectInputCreateDeviceOriginal(original, input, guid, device, outer);
    std::unique_lock lock(_state.Mutex);
    _state.DirectInputCreateDeviceCallCount++;
    if (SUCCEEDED(result) && device && *device)
    {
        _state.DirectInputCreateDeviceSucceededCount++;
        HookDirectInputDeviceLocked(*device, GetDirectInputDeviceKind(guid));
    }
    else if (FAILED(result)) _state.DirectInputCreateDeviceFailedCount++;
    return result;
}

bool BlockDeviceLocked(void* device)
{
    const auto index = Devices.Find(device);
    const auto kind = index == Devices.Missing ? DeviceKind::Other : Devices.Slots[index].Kind;
    return DirectInputHooks::ShouldBlock(kind, !Stopping && _state.Initialized && _state.Focused &&
                                             ShouldApplyBlockingPolicyLocked(),
                                        _state.BlockKeyboard, _state.BlockMouse);
}

HRESULT InvokeState(std::size_t index, void* device, DWORD size, LPVOID data)
{
    DirectInputGetDeviceState_t original;
    {
        std::unique_lock lock(_state.Mutex);
        original = StateHooks.Resolve(index);
        _state.DirectInputGetDeviceStateCallCount++;
        if (BlockDeviceLocked(device))
        {
            if (data && size) std::memset(data, 0, size);
            _state.DirectInputGetDeviceStateBlockedCount++;
            return DI_OK;
        }
        _state.DirectInputGetDeviceStatePassedCount++;
    }
    ScopedHookBypass bypass;
    return original(device, size, data);
}

HRESULT InvokeData(std::size_t index, void* device, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags)
{
    DirectInputGetDeviceData_t original;
    bool block;
    bool drain = false;
    {
        std::unique_lock lock(_state.Mutex);
        original = DataHooks.Resolve(index);
        block = BlockDeviceLocked(device);
        auto slot = Devices.Find(device);
        if (slot != Devices.Missing)
        {
            drain = Devices.Slots[slot].Drain;
            Devices.Slots[slot].Drain = false;
        }
        _state.DirectInputGetDeviceDataCallCount++;
        if (block) _state.DirectInputGetDeviceDataBlockedCount++;
        else _state.DirectInputGetDeviceDataPassedCount++;
    }
    ScopedHookBypass bypass;
    return DirectInputHooks::ReadBuffered(block, drain,
        [&](LPDIDEVICEOBJECTDATA output, LPDWORD countOut, DWORD readFlags)
        { return original(device, size, output, countOut, readFlags); },
        data, count, flags, static_cast<DWORD>(INFINITE));
}

ULONG InvokeRelease(std::size_t index, void* device)
{
    DirectInputDeviceRelease_t original;
    std::size_t generation = 0;
    {
        std::unique_lock lock(_state.Mutex);
        original = ReleaseHooks.Resolve(index);
        auto slot = Devices.Find(device);
        if (slot != Devices.Missing) generation = Devices.Slots[slot].Generation;
    }
    ULONG result;
    {
        ScopedHookBypass bypass;
        result = original(device);
    }
    if (result == 0)
    {
        std::unique_lock lock(_state.Mutex);
        Devices.Released(device, generation);
        _state.DirectInputTrackedDeviceCount = Devices.Count();
    }
    return result;
}
} // namespace

} // namespace OptiInput

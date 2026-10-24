# Baldur's Gate 3: Vulkan NR and extension spoofing

BG3's non-NVIDIA game profile enables `VulkanExtensionSpoofing` in Auto. This
unlocks game-facing capability checks; unsupported spoofed extensions must still
be removed before the real Vulkan device is created.

The NR device-extension query now uses the original enumeration trampoline from
`VulkanSpoofing`, rather than looking the patched entry up again through
`vkGetInstanceProcAddr`. Previously NR could interpret spoofed NVX extensions as
driver support and append them after the spoofing filter had removed them.
Actual NVIDIA extension support is preserved. Enumeration failure leaves optional
NR extensions unrequested; `VK_INCOMPLETE` causes a fresh enumeration.

The Vulkan-to-DX12 upscaler bridge now explicitly creates recording proxies for
its own DIRECT command lists and prepares submission expansion before exposing
them, matching the DX11 companion. It does not depend on early EXE or any-module
wrapping. The game compatibility defaults remain unchanged, and explicit menu/INI
preferences retain their priority.

Focused regressions: `tests/host/vulkan_nr_extensions.cpp` uses an actual Detours
hook to distinguish the spoofed export from the original enumeration trampoline.
`tests/lmxxf/lmxxf_same_frame_boundary.cpp` verifies explicit proxy creation and
native submission with all early-wrap paths disabled, using WARP.

BG3 Vulkan game acceptance remains pending: use `VulkanExtensionSpoofing=auto`,
`LmxxfEarlyExeWrap=auto` and `LmxxfEarlyAnyModuleWrap=auto` with the repaired host,
then verify device creation, NR recording/execution and stable gameplay. These
focused fixtures do not establish game or AMD driver acceptance.

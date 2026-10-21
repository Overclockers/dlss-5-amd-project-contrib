# OptiScaler upstream integration

Reviewed snapshot: **97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed**, from
[optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler/commit/97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed),
2026-10-06. This is a selective integration baseline, not a full upstream merge.

## Selected changes

### v10 foundation (2026-10-21)

Fixed feature source: **9df512d6b77342a2f467040b90cd1658ae2c2f95**.
The public host adopts upstream `misc/DeferredRelease.*` without changing its
`Enqueue`, `Release`, `Process`, `Collect` or `DropOwnersExcept` API. Collection
requires eight counted presents and 500 ms, or the upstream two-second fallback
without presents. This is a heuristic, not a GPU fence-completion guarantee.
DXGI/native Vulkan presents drive `Process`; upscaler and Nvngx evaluations also
drive `Collect`. DXVK's Vulkan present does not count a second time. Only successful
game-device creation drops old Vulkan owners; `ScopedSkipVulkanHooks` returns before
that operation, preserving private Mochizuki devices and game-owned entries.

Public DX12 shader objects and resized public intermediate buffers use deferred
release. Native `DlssNr_Dx12` explicitly retains immediate base-object destruction;
none of its PSO/root signature/constant objects enters the host time queue.
This preserves existing behavior, **not** a claim that native NR already has
generation/fence ownership: its 48-slot descriptor/constants ring and scratch/model
retirement can still be referenced by retained/replayed game lists. The AMD lmxxf
and Mochizuki routes bypass that native composition class and retain their existing
recording-generation and queue-fence leases unchanged. Vulkan native NR must retain
the same time-queue opt-out when its public shader base migrates.

Vulkan headers are pinned to upstream's `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77`
dependency; consumers use the C header. The Streamline Vulkan helper is from the
fixed feature snapshot. `skipHeapCapture` is thread-local while its scoped save/
restore contract and existing member-style call sites remain compatible.
Feature and runtime acceptance are recorded separately; this foundation alone does
not certify NR hot switching, Vulkan frame generation or game behavior.

Foundation verification: source-only independent review found and resolved a missing
DXVK early-return collection call. A throwaway adapter compiled the actual upstream
`DeferredRelease.cpp` and passed pointer detachment, minimum age/present counting,
owner dropping, age-only fallback and shutdown behavior; the existing Vulkan scope
test passed its nested/200-thread-interleaving checks. The release host plus person
worker build passed after adding upstream's explicit `vulkan_beta.h` include for the
existing CUDA launch hook declarations. No GPU smoke or game acceptance ran in this
foundation window; existing native NR replay limitations above remain unverified.

### v10 public configuration and A1/A2 integration

The DX12 driver-provider source and model/preset ownership cutover use the fixed
snapshot. Capability override (`Fsr4GpuSupport`) and actual model preference
(`Fsr4Model`) are independent; the old key migrates only to capability. Per-API
version lists drive menu selection. DRS provenance lives in the originating NGX
parameter block, not global State; all three backend-recreation adapters copy that
raw input capability, while explicit INI overrides take priority. Non-FSR parameter
blocks default false. NR render-subrect and post-SR active-output sizing are unchanged.

All previous executable/UE aliases migrate to `GameConfigs`; config-level Quirk
flags and their second application table are removed. Locally required Stellar Blade
compute/extended-state restoration and TLOU Part I/ROTTR HUD exclusions remain.
Game-defaulted settings use SoftDefault so an explicit choice equal to the compile
default survives saving. Local NR defaults are registry metadata, not materialized
INI values: the existing `forza` substring enables EXE/any-module wrapping and
enhanced barriers, Unreal enables EXE wrapping, exact ACMirage/007FirstLight enables
EXE wrapping, and exact Aniimo enables UnityPlayer wrapping. Explicit preferences
and missing-Unity behavior remain unchanged. No additional games are opted in.

Independent source review's three actionable compatibility/DRS findings were repaired
and approved. The release host/worker build and focused existing config-priority tests
passed after those repairs (explicit-default save/reload, DRS context isolation and
three-API recreation, NR automatic-policy/explicit-choice behavior). Actual driver
provider/model execution and NR smoke are separate acceptance; these CPU checks do
not establish GPU behavior. The Witcher camera-motion rule needs the native XeFG
input consumer in D8 before that behavior can be claimed functional.

Hardware smoke after public integration: a throwaway export relinked 214 actual
compiled host objects with the original DLL entrypoint; production sources/exports
were unchanged. The driver was loaded from the installed 26.10.1 DriverStore, not
bundled binaries. RX 9070 XT executed FP8, destroyed the completed context, then
executed INT8; each 128×128→256×256 dispatch used reset, completed its D3D12 fence,
kept the device healthy and reported the expected actual model (not FSR3 fallback).
Radeon 780M independently executed one INT8 context with the same completion/model
checks. The earlier expectation that 780M must reject FSR4 was disproved; the official
RDNA3 effective INT8 capability path is distinct from its absent real FP8 capability.
RDNA2 INT8 and Lite were listed but not executed. This does not validate NR hardware
support, ML frame generation, pixel quality, timing or real games.

The production DLL SHA-256 was
`6d54d59f2da56671d099ae44f9422051711075beab7be3d21610f31870bf85fb`
(host source fingerprint `5548accbcac78fc0b02c494993b72544d833331907186e4f246293e64ed0dbb3`).
The private-Vulkan scope test passed again; it is CPU isolation evidence only, not
an actual Mochizuki private-device/NR-on-off smoke. That GPU acceptance remains open.


### v10 A9 dedicated exposure

Dedicated `AE_Dx12` replaces the old smaller-color padding workaround; its direct
DeferredRelease dependency and project registrations are explicit. The paired source
branch repair was reviewed, and the Release host/worker build passed. A real GPU
readback of uniform linear HDR RGB=1 with preExposure=2/reset returned 0.208333,
matching the scalar exposure reference with zero observed error.

A bounded same-driver FP8 comparison used initialized HDR quadrants, preExposure=2,
reset and a 128×128→256×256 active extent. Driver automatic exposure versus dedicated
AE plus FSR both completed their fences and retained the FP8 model. Both readbacks
were finite, RGB range 0.249878–1.999023; mean/max absolute difference were zero and
the tone-mapped images showed the same four quadrants. The single cold debug-layer
timestamp pair was 0.20140 ms versus 0.10728 ms; this is not a repeatable performance
or game-speedup claim. The comparison adapter's initial Close failure was its own
readback allocation missing the 512-byte footprint offset; D3D12 diagnostics identified
the bounds error, which was repaired only in that throwaway adapter.

This synthetic comparison does not cover game visual quality, nonlinear input,
allocation padding or NR-network replacement at runtime; NR COMPUTE_READ preservation
is source-reviewed and the combined NR smoke remains required.

### v10 host stability and DX11 HUD integration

The GLM-authored host slice and five independently reviewed Codex repairs are
integrated: depth-view format mapping, failed FG-create latching, lexical path and
ReShade naming fixes, proxy unload protection, FG release order, per-instance DX11
interop resources and retained MV/depth inputs, and DX11 HUD tracking. Transfer
retirement takes each slot's exclusive lock and invalidates all frame entries;
HUD converters follow actual output-format changes, centered captures initialize
uncovered pixels, and shader bindings survive capture-window resets.

Public swapchain creation/tearing/description hooks, loaded-ASI registration/listing,
Nvngx FG shortcut and step-marked multiplier controls accompany that source. All
runtime FG enablement consumers use the common accessor; local NR multiplier
resolution/readiness guards remain. The Release host/worker build passed.
Game-follow callback publication and the temporary XeFG capability query still
require the separate B1 hook subset before end-to-end game-follow acceptance.
DX12 HUD exit-buffer collection remains B10; DX11/game and NR-enabled GPU acceptance
are not implied by this CPU build.

The early A5/B1/B3 callback subset subsequently landed with the temporary XeFG
capability-query dependency and local MFG Unlocker export-resolution handshake.
Game DLSSG options now publish enabled/count state; the real/dummy state reports
share output capabilities. SL1 managed-plugin JSON suppression covers owned DLSSG
output while retaining the 16-slot callback leases. Dynamic XeFG arbitration is
excluded when following the game, including the static-update early-return path;
NR overrides, ForceXeLL and reinitialization gates remain. Independent source
review closed both query/arbitration findings and the coherent Release host/worker
build passed. This subset does not enable the D8 macro or certify full LL/D6 behavior.


### Local owner-generation and process-exit closure

Owned Vulkan deferred callbacks carry an explicit registered device generation.
`RegisterOwner` is called only for successful game-device creation; `DropOwner`
invalidates that exact device before destruction and drains an executing owned
callback through a dispatch gate. Extracted callbacks recheck active generation,
so a destroyed/reused handle cannot execute stale destruction. Other live devices
and unowned COM callbacks retain their behavior. Creation no longer drops another
live device. The actual implementation adapter passed extracted-callback cancellation,
handle reuse, another live owner, active-callback draining and no implicit resurrection.

A real 256×256 Small Mochizuki recording ran through the compiled host backend,
CommandListProxy split/execute and fence retirement, using the sealed baseline runtime
SHA-256 `3163fe265a377b14e320e27bb04921f9e22327546aa79233511a981339751dd9`
with 836 hash-matched assets. On the physical RX 9070 XT LUID `0:1321c`, output was
finite with mean 0.444368 and mean absolute neural correction 0.00575857. Session
release/off transition and native process exit completed with exit code zero.

The first adapter selected a second virtual DXGI alias and correctly failed Vulkan
LUID matching; selecting the physical adapter fixed the adapter, not production
identity checks. Earlier successful neural runs exposed a real process-exit SIGSEGV:
debugger plus linker map identified the CRT destructor of `LmxxfCut::Pending()`'s
last diagnostic queue after D3D12 shutdown. The diagnostic singleton now follows
the existing process-lifetime registry pattern, retaining only its final queue while
normal replacements still release old queues. Independent review and the native
post-fix exit verified this repair. This does not certify native NVIDIA NR scalers,
NR off/on replay permutations, actual game-device deferred-entry preservation during
private Vulkan creation, or game visual/performance acceptance.

### Follow-up snapshot (2026-10-09)

Reviewed through **434b9555d960e6e471a461c2b0d98b8e1fdacfa2**
([upstream](https://github.com/optiscaler/OptiScaler/commit/434b9555d960e6e471a461c2b0d98b8e1fdacfa2)).
This remains a selective integration; the older baseline and rounds below describe
previously integrated work.

- HWND swapchains with a zero width or height use the current window dimensions
  (`a676a402`). Both wrapped and detoured paths avoid misclassifying these as
  overlay windows. Only local descriptor copies change; CoreWindow and
  DirectComposition retain their own policies. Legacy and DESC1 sizing share a helper.
- FFX version discovery rejects failed calls, empty lists, invalid names and a
  count that outgrows its allocation (`b040666` / reviewed snapshot). It publishes
  only complete results and stops callers before indexing an empty list. This does
  not import upstream's temporary-device discovery or global spoofing scope.
- FakeNVAPI has no new API coverage in this interval. The upstream DX11 timing
  subtraction fix does not apply to our direct-forwarding implementation.

Deferred: game-follow FG/DMFG and SL1 resolver changes affect our external FG
ownership, NR-only XeFG override and per-plugin callback leases. Late latching,
projection overrides, zero-frame-ID HUD rotation and Vulkan timestamps require
resource/queue and real-game validation. Linux/DXVK paths need that platform;
GameConfigs/dllmain restructuring has no benefit sufficient to justify moving our
Unity early-wrap and per-game defaults in this update. SDK pins are unchanged.

The real descriptor helper tests and host build pass. Full CI/GPU was explicitly
outside this follow-up's validation scope; game resize/exit acceptance is still required.

- NVAPI DRS DLSSG OTA protection (`ed30706`): only the output owned by this host,
  with the external-frame-generation bypass and DWORD validation preserved.
- DXGI factory calls use our existing thread-local D3D creation scope. No global
  Vulkan suppression is introduced.
- DX11/DX12 companion descriptors share format, sample, buffer-count, stereo,
  usage and tearing checks across detoured and wrapped factory entry points.
  Only copied companion descriptors change. Unsupported sRGB formats fall back
  instead of silently changing color interpretation; the private windowed policy remains.
- Companion swapchain state records the FG chain and descriptor. The existing
  wrapper owns the reference and clears the borrowed state pointer on release.
- ResizeBuffers1 falls back before releasing resources if IDXGISwapChain3 is absent.
- HUD capture barriers use the caller's known state on both sides of CopyResource
  (`b9b0bec`), matching the existing CopyTextureRegion branch.
- Existing quirk mappings extended for Granblue Fantasy Relink, Trails in the Sky
  2nd Chapter and Sword and Fairy 7. The existing config precedence remains in force.

## Deliberately separate work

FakeNVAPI call tables and implementations already match this snapshot. Low-latency
timestamp renaming has no functional benefit here. FidelityFX SDK and XeSS pins
already match; this change is not an SDK upgrade.

Keep our DirectComposition, zero-size/window sizing, HDR, NR command-list tracking,
external FG ownership, and private Vulkan scopes. Upstream does not contain all of them.
Do not replace ConfigKeys, dlssnr, menu_common, or the main lifecycle hooks wholesale.

The D3D12 hook rewrite (`500ed33`), DLSS bridge lifecycle, Streamline/FG reprojection,
new Hudfix resource tracking and equivalent-resize skipping require separate analysis.
In particular, zero ResizeBuffers dimensions mean current window dimensions, and the
new XeFG resize path depends on matching present synchronization; copying only its
skip predicate would be unsafe. These need dedicated lifecycle and game validation.

The existing Unity resource flip quirk is game-specific, not a general Unity fix.
Retain the separate Aniimo NR early-wrap policy. UI changes use local components;
upstream's menu redesign is reference material, not a replacement for the NR workflow.

## Verification

The host CI entry includes descriptor acceptance/rejection and unchanged-on-failure
tests plus existing cross-thread Vulkan scope tests. Build and CI establish software
contracts; game startup, HDR and Alt+Tab/resize behavior still require game acceptance.

## Round two A: DLSS/DLSSG plugin instance lifetimes (2026-10-07)

Selective source: upstream **97e99b4c5d9e8af38f14e3b00e1b3f7b35ab5aed**, design from
`4abfd7b65d5eb002b44801421837c69e04edfd6b`. DLSS and DLSSG now keep independent
resolver and returned callback identities. A rejected/new plugin cannot evict a live
older plugin. External FG bypass, OTA policy and the Steam original-function bypass remain.

The local implementation deliberately differs from the snapshot: exact Windows DLL
unload notifications only mark atomic retirement; they do not call the loader or acquire
application locks. Callback leases hold the originating module while executing. Returned
wrappers and bounded tombstones are never reassigned to another generation, including
address reuse. Sixteen successful DLSS/DLSSG instances per process are supported; subsequent
copies remain unmodified. This bounds unreclaimable detour storage after DLL unmap and
avoids writing into a freed image. Restart resets the capacity. Initialization JSON/architecture
patches do not wait across threads under loader lock; concurrent conflicting initialization
is rejected rather than racing the shared spoofed system-capability state.

Reflex/PCL/common and the host's private local DLSSG retain their existing paths in this
round. Their parameter callbacks have separate lifetimes (notably common's setVoid detour)
and need their own ownership tests before extending this registry. This is not a claim
that all Streamline plugins or the complete upstream OTA rewrite have been merged.

The host CI fixture uses real DLL exports and Detours to cover two copies, rejected attach,
repeat notification, reference decrements, active-call leases, actual unload, reload,
concurrent callbacks and capacity fallback. Streamline game startup/exit remains a game
acceptance item; the fixture does not simulate the full proprietary plugin implementation.

## Round two B: DX11 companion resize/present

Sources at the same fixed snapshot: `4c682650f32e666c89a5a7885b2b12682fc3c27e`
and `3bc197c297d8074f0793bcd33f3d382c9ff01021`. Only the confirmed equivalent
XeFG/DX11-to-DX12 path skips companion recreation. Other outputs keep the existing
SDK resize path. Width/height zero use the current HWND client size; after the game's
resize the actual description is rechecked before skipping. Unknown descriptions,
changed flags/format/count or a previous companion failure take normal resize.

DX11 wrapper calls serialize interop buffer mutation. The equivalent XeFG path first
deactivates FG, then excludes native presents, then waits for copy and present queues.
The present queue has its own fence timeline. Wait/deactivation failure returns without
releasing buffers; a device-removed fence sentinel is not successful completion. The
writer barrier is released before calling the SDK's non-equivalent resize, which can
drain/reenter its presenter. Original game HRESULTs are preserved; a companion failure
is reported on Present until a later successful resize recovers it. DirectComposition,
HDR and descriptor creation policies are unchanged.

Tests exercise the production equivalence/transaction helpers, failed waits and original/
companion failures, controlled reader/writer exclusion, and a real WARP HWND swapchain
with zero-size resize after a window-size change. This is not a real XeFG game test;
DX11+XeFG resolution changes, Alt+Tab/fullscreen and exit remain required acceptance.

## Round two C: per-call descriptor range lookup

Design source: `a671371848bd32c78d631a396c4204f1a077b751` at the fixed upstream
snapshot. The local port only caches source/destination HeapInfo ownership inside
one CopyDescriptorsSimple call. Every reuse checks active/range; existing descriptor
stripe locks, resource attach/detach bookkeeping and tracking enablement remain.
Single-descriptor calls retain original source-then-destination lookup order.

The production range helper has mapping-parity tests for boundaries, unknown/zero
sources and clearing, overlapping traversal, retirement/address reuse, and independent
calls. A controlled /O2 CPU fixture compares the same lookup/copy workload (not game
FPS): 200,000 16-descriptor copies reduce lookups from 6,400,000 to 400,000. Single-item
lookup counts remain unchanged. No persistent cache or HUD tracking rewrite is included.

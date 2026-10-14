# Mochizuki backend (local integration)

This backend runs mochizuki0323's Vulkan network on the game's AMD adapter while the
game stays on native D3D12. This integration targets Windows and RDNA4. Other GPU
architectures and individual games are not validated by this integration.
Recordings with unsupported state (including enhanced barriers or an active render
pass) pass through without NR on the ordinary split path. Experimental `NrInline=true`
adds a separate same-list path, including validated whole-resource enhanced-layout
interop when `LmxxfAllowEnhancedBarriers=true`; partial/pending or unknown layouts
remain unsupported. See [inline execution](architecture/nr-inline-recording.md).

The network core and shaders come from the
[official repository](https://github.com/mochizuki0323/DLSSNR-AMD).
The runtime adapts native D3D12/Vulkan interop and pipeline prewarming code from
[MatheusFerreiraS/neural-amd-opti](https://github.com/MatheusFerreiraS/neural-amd-opti).
This product implements backend selection, host recording ownership, configuration,
menus and packaging, and adapts the runtime for retained recordings, replay and
completion-based resource release. Its build-progress display and execution-history
handling are described below. Exact reused components, source pins, licenses and
local changes are in [the source record](../third_party/mochizuki/UPSTREAM.md).

The vendored core follows official Windows source `a9b13dd4cdb5f4bf43db358492d72e6e5e2acd67`
(2026-10-11): native D3D12 with a Vulkan device of NR's own, the 0.0.4 feature set,
the 26.9.2 shader fixes, and ACO left off. The older statement that Windows stays on
v0.0.3 is historical. INT4 mixed, the official DX9/DX10/DX11 hosts, ReShade and the
official OptiScaler hooks are not included. Product recording, menus and packaging
stay in this host. See [the source record](../third_party/mochizuki/UPSTREAM.md).

## Installation

1. Install a current AMD display driver with Vulkan support. HIP is not used by this backend.
2. Extract the complete OptiScaler package. It contains `MochizukiNrRuntime.dll`,
   `dlssnr-amd/shaders/` and `dlssnr-amd/aco/` (records, shell aliases, and the ACO
   network, runtime and temporal shaders). The runtime and those assets must match the host in that package.
   ACO is on by default (`MochizukiAcoMode=true`); a change applies on the next game start.
   A missing or rejected bundle, or a device/driver without ACO, keeps the driver's compiler.
3. Supply your own `nvngx_dlssnr.dll` **310.8.0**. Its SHA-256 is
   `e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e`.
   The DLL and extracted weights are **not included** in our package.
4. With Python 3.10 or newer installed, put that DLL beside `Setup.bat` and run
   `Setup.bat` to select Mochizuki and extract the detected model.
   `Mochizuki-Model.bat` remains available separately; you can drag a DLL/ZIP onto it.
   Extraction only reads data and validates every model entry; it does not load the DLL.
   The result is `dlssnr-amd/dlssnr.bin`. A model extracted with the official tools is
   also usable at this path.
   If no working Python 3.10+ is found, Setup and the standalone model tool explain
   how to open Microsoft Store (Windows Store), search for **Python**, and install
   version 3.10 or newer. Close and reopen the installer afterward. They also try
   `python3` and `py` when `python` is unavailable; a Windows execution alias alone
   is not accepted as a working interpreter. Existing extracted models need no Python.
5. Run `Setup.bat`, select mochizuki alone or install all available backends. Choose
   the active backend. Ins exposes the same selection; with `NrConvenience=0`, save
   and restart after changing it. Unselected backends are not initialized.

The first network build can take minutes. Ins displays **building the network**.
while building, a noninteractive panel also appears at the bottom right, even with
Ins and the FPS overlay closed. It shows the phase, elapsed time and completed
main-network shaders. Other phases use an indeterminate activity bar; shader counts
are not an estimate of total remaining time. A step without an update for 30 seconds
shows its waiting time. The panel disappears when building ends or NR is disabled.
Build logs report at most about once per ten seconds plus phase completion.
Frames pass through until it is ready. Changing resolution, model scale or pass
capacity can require another build. Cache files belong to this machine and driver
and are not distributed in the package. NR off releases the active session; closed
command lists keep resources until they are invalidated and GPU work completes.

To extract from a source checkout:

```powershell
python -X utf8 tools/install/mochizuki-model.py <user-DLL-or-ZIP> <output>/dlssnr-amd/dlssnr.bin --work work/scratch
```

Installation uses `inspect_nr.py --extract-weights`: the existing PE/resource and
weight parser emits the manifest, weight container and layer payloads, without
scanning CUDA ELF modules or writing DLL/cubin copies. The vendor shell installer
uses the same mode. Developer analysis remains available through `inspect_nr.py`
(inventory) or `--extract` (DLL, weights and cubins). No extraction code loads the DLL.
The source SHA256, 599-entry model list, per-entry SHA256 verification and packed
model format are unchanged; the product installer still replaces the destination
only after verification succeeds. Patch 12 records the vendor extension.
`tests\mochizuki\run.cmd model` covers mode separation, skipped scanning, DLL/ZIP,
malformed weights, path safety and failed-source preservation without a GPU.

Validated on 2026-10-13 with the supported 310.8.0 DLL: full analysis extraction,
weights-only installation from DLL, weights-only installation from ZIP, and the
existing tested model all produced 147,756,560 identical bytes, SHA256
`2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd`.
All 599 entries passed the existing verifier; changing one extracted tensor was
rejected before a model was written. The 12-patch/173-file upstream replay passed.
The shell entry point received LF-normalized syntax validation on Windows; a
Linux installation was not run.

## ReShade compatibility

ReShade as `dxgi.dll` with OptiScaler as `winmm.dll` has been validated in
Where Winds Meet (DX12) with ReShade 6.8. The list-identity fix alone was insufficient:
ReShade's queue exposes its device wrapper, while its fences expose the native
device. Comparing those pointers stopped the session at `EndRecordingExecution`.
The runtime now also accepts the device identity proven by a synchronization fence
created by that same session. Foreign devices and unresolved submissions remain
rejected; synchronization and resource-retirement rules are unchanged.

`network ready` means construction completed, not that NR rendered a frame.
Confirm an increasing frame count and nonzero timings alongside the image effect.
Submission failures preserve their originating-thread error and replace stale
building status; bounded logs trace the first two recordings of each session.
This validation does not cover every game, ReShade add-on or loading arrangement.

## Controls

All keys below are in `[DlssNr]`, prefixed `Mochizuki`, independent of Daniel and
lmxxf settings. The complete defaults are in the shipped `OptiScaler.ini`.

| Group | Controls and behavior |
|---|---|
| Model response | Model Intensity, Style (official Style 0–2), LocalTone, LocalStructure, SkinStructure (-1 follows Structure), AutomaticMask |
| After the model | ApplyModel (Transfer), DetailStrength (TransferStrength), ColourStrength, MaxRatio highlight guard, enlargement and ClassicScaler |
| Resolution / Model passes | ModelScale (WorkingScale, 0.25–1), Passes (1–3). Allow up to 30 layers raises that control to 1–30; it does not select 30 by itself. Each count runs the model that many times in one network, one history image per layer, then one composition. Layers after 3 inherit pass 1 with local tone 0. The selected count rebuilds; the VRAM budget can refuse it |
| Temporal history | Temporal and HistoryStrength. NR → SR drops jittered motion. Mochizuki SR → NR passes the game vectors through and the runtime scales the render-resolution motion region |
| Before the model | Preprocess, PreprocessExposure (Off/Auto/Fixed), PreprocessBiasEv, PreprocessCurve (None/Neutral/Reinhard/Filmic/GT/ACES/AgX), PreprocessContrast and PreprocessSaturation. The transform changes the model input and is reversed from its answer |
| Advanced | WhitePoint for linear input (the manual level, which trims the game's exposure when the host supplies one and the runtime's own GPU meter otherwise), LinearInput (Auto/Linear/Encoded), MaxPasses (0 grows capacity as needed and retains it on pass reduction), DynamicResolution (Exact/Auto bucket/Always bucket) |
| Pass 2/3 | Explicit override plus that pass's style, intensity, tone, structure, skin structure and mask. With override off, inherit pass 1 but use zero LocalTone |

The NR menu uses a clickable pre-SR flow chart. **Prepare NR input** contains model
resolution (WorkingScale), preprocessing and advanced input encoding/white point/DRS. **NR model**
contains pass count, model response and enabled Pass 2/3, temporal history and prebuild
capacity. **Apply NR edit** contains the after-the-model correction and the shared intensity and
stabilizer. Processing order and the ACO checkbox sit above the pages. The model page opens first; page buttons remain available when the chart
is collapsed. Compatibility and diagnostics remain separate tools below the page.
Disabling a pass hides its controls without deleting its settings. Model resolution
displays as a percentage; resolution, pass count and prebuild-pass edits commit on
release or completion of keyboard/text editing, avoiding rebuilds during dragging.

In automatic capacity mode (`MochizukiMaxPasses=0`), changing 2→1→2 runs one or
two passes of the same network. Lowering the active count keeps the larger allocation;
it reduces GPU work but does not reclaim that network's extra memory. An explicit
prebuild count requests exact capacity, at least the active count, and can rebuild
to a smaller network. Resolution, scale, encoding and preprocessing changes still
have their own rebuild requirements.

If a capacity expansion is refused by the process VRAM budget or its build fails,
a compatible existing network and its frame buffers continue running. The status
reports requested passes, effective passes, capacity and the waiting reason. Budget
refusals retry at most once per ten seconds; out-of-memory builds retain their
bounded backoff. Required candidate buffers must be allocated before installation;
failure discards the candidate. Without a compatible ready network, frames pass
through without NR. The budget is a Windows process allowance, not a physical VRAM
free-space reading, and requested passes cannot be guaranteed under memory pressure.

Auto DRS buckets changing input subrects to reduce repeated network builds. The
network repeats the subrect edge into the unused bucket and resets history when the
valid extent changes. Unsupported blit formats fall back to exact extents.

When DRS is enabled but the current input still uses exact extents (for example,
whole allocations in Auto mode), a different size must remain requested for 300 ms
before replacing a ready network. During this wait, incompatible frames pass
through with their original colour; a return to the ready network's size resumes
NR immediately and cancels the pending resize. Sustained changes still rebuild.
Startup, same-size setting changes and actual DRS bucket growth do not incur this
extra wait. This prevents short size excursions from triggering two unnecessary
builds; it does not remove the cost of a genuine resolution change.

**Compatibility & Scheduling → Early command-list wrap** controls the shared
submission hook (`LmxxfEarlyExeWrap` in the INI, also used by lmxxf). Auto admits
early EXE calls in Unreal/Forza and early UnityPlayer.dll calls in Aniimo.exe.
Other Unity games stay on the normal post-swapchain policy unless explicitly
forced on. Force on admits both EXE and loaded UnityPlayer.dll callers; force off
disables both early paths. Save and restart after changing this setting. Forced
early wrapping can break initialization in untested games; it is not a general
Unity compatibility guarantee. The hook still accepts only DIRECT command lists
and requires a ready submission path. This addresses retained boot lists missing
NR, independently of the transient-resolution handling above.

Overall Intensity and Residual Stabilizer remain shared output effects. Intensity 0
or ApplyModel off still incurs network work; disable NR to avoid it. Group reset only
resets that group. Reset NR settings resets mochizuki and shared effects/timing,
preserving other backend controls, backend selection and hotkeys. Reset this page
resets only that page's mochizuki fields; on the model page this includes hidden
later-pass overrides. Shared effects have their own reset.

Ins and Page Up/Down show completed **Vulkan core total** GPU measurements.
The compact Mochizuki label is NR GPU. Diagnostics splits the same sample window
into network and other core processing; their means sum to the total mean.
The network includes fused temporal pre/post shaders. Other covers core copies,
input conversion and composition. Adapter buffer/image copies, D3D12 copies,
shared output effects, upscaling, frame generation and whole-frame latency are
outside this measurement. Display and timing log options default off; summaries
are limited to one per ten seconds, and query reads never wait for the GPU.
`mochizuki_nr.log` respects file logging and stops appending at 4 MiB.

Under **Apply NR edit → After the model**, **Model enlargement**
(`MochizukiEnlargeMode`) offers matched residual (0, default), edge-aware (1) or
classic (2). It applies only below 100% model scale. Edge-aware uses a 4×4
neighbourhood guided by the original pixel's brightness. Classic uses
**Classic scaler** (`MochizukiClassicScaler`): bilinear (0), Catmull-Rom (1, default),
Lanczos3 (2) or FSR1 (3). Edge-aware and classic cost more GPU time and need
scene-by-scene comparison. Below full model scale the runtime does not allocate
a second full-frame image for the enlargement pass; classic enlargement writes
`up_result` only.

**Reuse output buffer** (`MochizukiCompactTransfer=true`, default) is a product
policy on top of the official native store. Official `store_native` already writes
listed native formats back into the kept frame. When that path does not apply and
the frame is kept as RGBA32F, the same private image is reused as the answer.
Turning the option off forces a separate answer image and rebuilds the network.
U8 output keeps its rounding path. Model scale 100% is unchanged. The control is
owned by the INI/menu and does not follow `NR_*` environment variables.

**ACO mode** (`MochizukiAcoMode=true`, default) asks for AMD pipeline binaries.
The menu checkbox is **ACO fast mode (experimental)**, next to Processing order.
ACO is a standalone switch: group, page and NR reset buttons preserve both its
current-session value and any choice saved for the next launch.
The extension is chosen when the Vulkan device is created. Effective stays off
until Install publishes the network that will run; a candidate discarded by its
frame-buffer check leaves the installed network's effective state unchanged.
Changing the request while a candidate is still building leaves that candidate
alone. The effective state is the mode of the network Install publishes.
If the driver rejects the optional pipeline-binary device for a reason other than
device loss or out of memory, the same device is created without those extensions.
Turning ACO on after a device created without binaries waits for the next game start.
Each build thread selects its own bundle, so a later build can import ACO or return
to the driver's compiler while an earlier recording keeps the pipelines it already has.
The runtime configures ACO only when `dlssnr-amd/aco` is complete and every network
SPIR-V under `aco/shaders` (including temporal) has a record whose contents match
that SPIR-V. Windows `dlssnr-amd/shaders` supplies the native shell and runtime
passes. A missing bundle, a record mismatch, or an import that produces no
pipelines discards that graph and builds one complete native network.
Diagnostics show requested, effective and the reason. `NR_ACO_DIR` does not
select the bundle after `own_directory()`.
On 2026-10-11, `tests/mochizuki/run.cmd aco` used AMD Radeon RX 9070 XT
(vendor 0x1002, device 0x7550, Vulkan 26.8.1 LLPC, driverVersion 0x0080018b).
The runtime log recorded 58/58 record matches and 29 imported pipelines. The
graph builder counted 24 kernels. The test printed `ACO_FRAME_OK` for one
256x256 frame, then `ACO_SWITCH_NATIVE_OK`, `ACO_SWITCH_ACO_OK` and
`ACO_CONTRACT_OK effective=1`. A forced pipeline-binary device rejection completed
one native frame (`ACO_DEVICE_FALLBACK_OK`). Refusing the second frame-buffer
admission kept the installed ACO state (`ACO_BUDGET_KEEP_OK effective=1`).
`tests/mochizuki/run.cmd aco-rapid` on the same adapter printed
`ACO_RAPID_REENABLE requested=1 effective=0` when a native candidate installed
after ACO had been requested again, then `ACO_RAPID_OK`.
Missing, alias-less and corrupt bundles each completed one native frame
(`ACO_FALLBACK_OK`). Package and uninstall ownership is the exact 220-path pin
`tools/install/mochizuki-owned.txt`. No game scene was compared.

`MochizukiWhitePointSource` (0 off, default; 1 game; 2 meter; 3 auto) picks the exposure sources.
With 0 the white point is the manual setting exactly as before. Modes 1 and 3 hand the runtime the game's
1x1 exposure texture (`MochizukiNrSetExposureTexture`): its texel is copied to a readback in the
game's own list and picked up one frame later behind the recording's completion fence, and the
level is `texel * DLSS_Exposure_Scale / DLSS_Pre_Exposure` - the expression the lmxxf codec
evaluates on the GPU. Without a texture, a pre-exposure other than 1 gives `1 / pre-exposure`
(`MochizukiNrSetExposure`). Modes 2 and 3 let the runtime meter a linear frame (mode 2 ignores the
game's exposure; mode 3 only when the game supplies neither) - every Monster Hunter Wilds frame so far, where the
DLSS-NR exposure scan adopts no candidate: one 16x16-thread dispatch over a 64x64 grid, a
highlight-clipped mean (arithmetic mean of min(luma, 16x log-average)), read one frame later
through the same fence, blended in the log domain with a 1/16 EV deadband and clamped to +-6 EV.
The meter is skipped for display-encoded frames, where the white point is not applied. `WhitePoint` then trims the
metered level instead of setting an absolute one: at the reference luminance (mean 0.2073) the
hand-tuned value keeps its meaning, elsewhere it scales with the scene. A frame with nothing
measurable leaves the last measurement in place, and after a second without one the manual
white point is back in charge with a line in the log; it resumes on its own once it measures
again. Daniel/lmxxf exposure controls do not
configure this backend, and preprocessing keeps its own histogram meter.

## Build and verification

`tools/build/build-mochizuki-runtime.cmd` builds the pinned core, adapter, Windows
shaders and the ACO payload. The ACO step runs `linux/build/build_network.py` with
the same pinned glslang into `dlssnr-amd/aco/shaders`, then copies
`windows/data/aco/records` and `shell-aliases.txt`. A failed shader generation fails
the build. The builder downloads pinned Vulkan headers and a hash-verified glslang
16.5.0 compiler into `exports/mochizuki-toolchain`; no machine-wide Vulkan SDK is needed.
`tools/build/mochizuki-manifest.py exports/mochizuki-runtime` verifies source and
artifact hashes. The packager runs this same check.

`tests/mochizuki/run.cmd abi` validates the current package contract without a GPU.
`tests/mochizuki/run.cmd gpu` requires an AMD GPU and the user's extracted model
under `exports/mochizuki-runtime/dlssnr-amd/`. It exercises output, replay, cross-queue
ordering, geometry retention, delayed collection, cancellation, execution-order
history/reset, RGB9E5/sRGB outputs and changing DRS subrects. Both are wired
into the corresponding `tests/run-all.cmd` tiers. Local and Actions use the same
builder and ABI test; a local package is still pending actual game acceptance.

`tests/mochizuki/run.cmd pass-switch normal|budget|oom|frame-budget` runs focused
GPU output comparisons for capacity reuse, expansion, budget/OOM recovery,
candidate-buffer refusal, retained recordings and incompatible geometry/format.
The normal case also compares two/three-pass output with independent fresh sessions.
The other modes use test-only failure injection; they do not change product defaults.

`tests/mochizuki/run.cmd hdr-shader` needs the built shaders and AMD GPU, but no model.
It executes the real input/transfer shaders and checks HDR RGB ratios, ordinary
brightness, alpha, valid dispatch bounds and full/reduced model resolution.
The `gpu` entrypoint also runs it.

`tests/mochizuki/run.cmd composition` compares separate/reused output buffers on
actual consecutive temporal frames, with one/three passes, preprocessing, both
enlargement modes and FP16/sRGB. It checks replay of retained recordings after
switching buffers. Warmup precedes history comparison because a cold driver's
first dispatch can exceed the runtime's 250 ms continuity limit. This test is
also included in `gpu`.

`tests/mochizuki/run.cmd profile <output.raw> <width> <height> <scale> <passes> <preprocess> <compact> <enlarge>`
uses deterministic R11G11B10 input, 16 warmup executions and 64 completed timing
samples, and saves the final pixels. It excludes history (no motion input), CPU
build time and adapter copies; use composition/GPU tests for temporal correctness.
This is a warm GPU microbenchmark, not a game FPS measurement.

For a focused 1080p R11G11B10 startup test, use
`tests/mochizuki/run.cmd startup <asset-root> [output.raw]`. The asset root contains
`dlssnr-amd/shaders` and the user model. A fresh asset directory alone does not make
the AMD driver cache cold; use a fresh test executable name for that comparison and
record the actual cache conditions. The test checks progress, then executes and
reads back the network output. Shader defines follow the official `rdna4.sh` recipe,
including `NR_TCHAIN_TILES`. The old `NR_EDGE_BODIES=0` override is not applied.

Uninstall removes the runtime and the package-owned files under `dlssnr-amd/shaders`
and `dlssnr-amd/aco` (known shader names, `shell-aliases.txt`, and `records/*.nrp`).
Files it does not recognize stay, including a shader added beside the package.
It does not follow a linked directory. It preserves `dlssnr.bin` and `dlssnr-amd/aco-cache`,
and still removes the driver `pipeline.cache` and prewarm manifest it already deleted.
Empty directories are removed after those files. To reclaim the model or the ACO cache,
remove those after closing the game.

## Session destruction and queue ownership

Destroy rejects sessions with live recordings or an executing token. An invalidated
recording can be collected only after its submitted D3D12 consumer tails complete.
Once all recordings are collected, destruction joins the builder and waits for the
private Vulkan work, without draining later unrelated work on the game queues.
Lost-device resources remain quarantined. This avoids making NR off/backend
switching depend on game work that may need the switching thread to continue.

`tests/mochizuki/run.cmd destroy-tail` reproduces this boundary by completing and
collecting NR work, then blocking both game queues behind an unrelated fence.
Destroy must return before the helper releases that fence. The full GPU suite
includes this check alongside live-recording and cancellation coverage.

## ACO decoder-tail dependency (2026-10-12)

The C64 persistent decoder → fused C32 boundary keeps its synchronization while
the other official tile-counter chains remain enabled. This adopts Overclockers'
`60f9202f` dependency fix; see [source attribution](../third_party/mochizuki/UPSTREAM.md#decoder-tail-synchronization-correction-2026-10-12).
The ACO machine-code setting still selects the compiler. There is no additional
product scheduling switch or default-policy change in the INI/menu/controls ABI.

The scheduling oracle is a separate test build, never a release runtime:

```powershell
tools/build/build-mochizuki-runtime.cmd --skip-shaders --test-full-barriers
tests/mochizuki/run.cmd aco-temporal --case 9
tests/mochizuki/run.cmd aco-inline
```

The reference build is restricted to `exports/mochizuki-test-reference`, records
its source/DLL proof, and produces no production packaging manifest. The fixture
compares actual frame bytes against production using the same graph and assets,
with controlled diagnostic environment and mandatory actual ACO import. Case
selectors cannot report success after selecting no cases. Synthetic frame streams
are removed after equality; logs/receipts remain under `exports/mochizuki-temporal`.
The default `aco-temporal` entry contains 45 GPU cases; use selected cases for
routine local checks. It is not the full CI suite. Full CI belongs to release
Actions, and this machine-specific GPU fixture needs a suitable device.

Local cases 1–26, 42 and 45 passed; the remainder was intentionally skipped after
the user narrowed local verification. Inline ACO passed 120 fresh recordings and
retained replay with 100% observed temporal history. No game/FPS result is claimed.

## ACO 本地缓存边界

导入后产生的驱动管线缓存位于 `%LOCALAPPDATA%/OptScaler-NR/Mochizuki/aco-cache/`，
不再写入游戏或模型目录；目录不可用时禁用磁盘缓存。旧 `aco-cache` 不迁移或导入。
包内固定的 ACO records 仍是构建输入，两者用途不同。
缓存里的 FNV64 仅检测损坏，不认证写入者；不要把第三方缓存当作可信模型资产导入。
相同用户权限的进程仍可修改本地缓存，修改后的二进制可能交给 Vulkan 驱动。

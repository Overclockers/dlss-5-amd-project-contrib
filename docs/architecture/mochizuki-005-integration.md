# Mochizuki Windows 0.0.5 integration

PR base: `work/mochizuki-upstream-a9b13dd`. Upstream source moves from
`a9b13dd4cdb5f4bf43db358492d72e6e5e2acd67` to
`8a70f7f418294ff1e306b66893456e5319ff8205`, including the shared changes from
`0548d4339010425cbdaafcfb355255b991ce650a`. Product scope is Windows D3D12;
INT4 mixed and RenoDX are optional, default off, and controlled by Ins/INI.

## Feature review and inventory

The exact source closure and patch-created files are enumerated in
[`manifest.json`](../../tools/mochizuki-sync/manifest.json); the exact 794 shipped
asset paths are in [`mochizuki-owned.txt`](../../tools/install/mochizuki-owned.txt).
The following groups account for the upstream changes and their host consumers.

| Upstream group | Integration and evidence | Limits |
|---|---|---|
| `windows/src/core/nr_graph.cpp`, runtime, layouts, shader recipes | Import Windows 0.0.5 core; preserve cancellation, timing, first-pass export, host queue and recording interfaces as explicit rebased patches | Host remains responsible for completion and retained recording lifetimes |
| `windows/data/aco/int4`, `linux/shaders`, `linux/data/int4/rdna4`, `windows/src/tools/nr_int4_weights.cpp` | Windows builds the upstream mixed INT4 recipe and imports 71 matching ACO records; generator creates user weights with upstream SHA-256 `22a0e2d3db1c2101a67e0d9aad066e0916afbc32cba47820f5ad24d46602319c` | Requires supported ACO; complete default-network fallback, no native INT4. No user weights in package |
| `nr_renodx.*`, `core/renodx`, `shaders/renodx(-prep)` | Preserve upstream processing and 85 picture settings; Slang 2026.19 build checks preprocessing math, emits 20 shaders. Adapter owns stage alongside retained network and distinct per-pass histories | Graphics queue for entire Vulkan stage; timings are last network pass only. No depth bridge; person protection disabled with RenoDX |
| `nr_driver_identity.hpp`, `nr_pipeline_binary.hpp` | Import AMD ICD/LUID identity recovery for loader GPU spoofing. Preserve product scoped ACO setup and complete-network fallback | Actual game spoofing acceptance pending; no new loader hooks |
| `runtime_downscale.comp`, native output store | Import packed nonnegative downscale; full-resolution direct store respects local separate-answer policy (patch 15) | Existing local Plan A and FP16 conversion consumers retained |
| Motion/history/noise changes | Keep existing full-resolution motion bridge and host history lifecycle; pass reset into no-motion record; expose NoiseEveryFrame default false | No upstream host cache/Present lifecycle transplant |
| Decoder-tail barrier | Official graph includes the required C64-to-C32 dependency; remove redundant patch 09 | Original Overclockers attribution remains in history |
| Linux core/UI/build/installation, Vulkan layer, Wayland input | Windows source contains the applicable processing port; import only Linux files consumed by Windows shader/model builds | Linux product and deployment paths out of scope |
| PE menu/Present hooks, ReShade/alternate DX hosts, screenshots | Use existing Ins menu, ConfigKeys and host recording/installer; preserve product config priority | No End overlay, second INI, screenshot capture or alternate hosts |

Four baseline Plan A shaders and their recipe changes were already present but
absent from the old replay closure. Patch 14 records them; `added_files` makes the
checker reject missing or unrecorded local source additions. This is provenance
repair for existing behavior. Fourteen patches remain after dropping patch 09.

## Host and package contract

`MochizukiNrControls` and info structs expand together with the host. Exact struct
size checks reject mixed packages. Config identifiers are registered in
`CfgKey::kKnown`; menu labels never become INI keys. The runtime receives explicit
INT4 settings, and owned ACO mode ignores `NR_I4_WEIGHTS` without a scoped config.

Mode, working resolution and encoding changes participate in network identity.
RenoDX stage resources retire with the network; Vulkan work is recorded on each
execution, including D3D12 recording replay. Game exposure is validated by the
host, then embedded in a private image by a command-buffer clear. Unknown game
exposure uses the upstream content meter.

Setup validates every pinned asset and the matching INT4 converter before game
writes. Conversion validates SHA-256 before replacing old weights; failures keep
the prior model. Package staging copies only owned assets, keeps MIT/GPL notices,
and excludes both models and local driver caches. Uninstall preserves user models.

## Validation scope

Local Windows validation uses RX 9070 XT and installed MSVC 14.51.36231 (VS 2026
on `G:`), not the release-pinned MSVC 14.44.35207. The release pin is unchanged.
Source replay, runtime ABI, config priority, host CI and focused installation /
packaging checks accompany this integration. The Windows 0.0.5 GPU fixture covers
FP16 and sRGB, default/INT4/RenoDX switching, three RenoDX passes at 50% model
resolution, motion/look stabilization, finite output and original recording replay.
Its `--no-aco` run verifies that an INT4 request falls back to the default network
with identical output, including RenoDX operation without ACO.

This is a local test candidate. Game image quality, long sessions, game exposure,
HDR interpretation and upstream GPU-spoofing behavior require game acceptance.
RenoDX inline operation and all 85 picture settings are not individually certified
by the split-recording fixture. Full release regression remains GitHub Actions'
responsibility. Review follow-up decisions and validation are in [the final review](mochizuki-005-review.md).

# Mochizuki source pin and interface patches

Every source update follows the [shared upstream review agreement](../../docs/dev/upstream-integration-review.md).
This page covers Mochizuki-specific source closure, patch replay and validation; it is not an automated staging or integration command.

The runtime uses official core/shaders at the full SHA in `manifest.json`, plus
the explicit feature patches listed in the manifest. There are no whole-file preservation overlays.
The host loads our `MochizukiNrRuntime.dll`, which wraps that core; this does not
claim binary compatibility with an official distributed runtime.

```powershell
python -X utf8 tools/mochizuki-sync/check.py --upstream <local-official-clone>
tests\mochizuki\run.cmd source-pin
```

The first command reads **Git objects at the pin**, replays the patch series in
a temporary directory under `exports/`, and compares the upstream closure and patch-created files with
vendor. It rejects missing/extra source paths, patch conflicts and unrecorded
vendor edits. It normalizes text CRLF only; machine-code records remain opaque.
It does not fetch, change vendor, advance the pin, build, package or publish.
The second command tests the checker with synthetic Git repositories and no GPU.
Those tests also run in `run.cmd abi`.

Patch 25 also carries the runtime model inventory. Regenerate it with
`python -B tools/mochizuki-sync/generate-model-profile.py` after an intentional
change to the tracked tensor checksum lists, then update the patch. The runtime
checks the selected family, exact tensor set and SHA256 contents, and keeps a
read lease that denies file writes and renames until weight loading completes.

Patch 26 adds temporary-exposure Hold, a codec-divisor transition shader, and
independently shared 1x1 exposure state across compatible network replacements.
It also gives generated tensor arrays macro-safe names for Windows compilation.
`tests\mochizuki\run.cmd renodx-feed` checks the production feed decisions and
transition shader body without a GPU; this runs in the `abi` tier too. GPU/game
acceptance of repeated extent changes and visual continuity remains separate.

| Patch | Local purpose | Upstream preparation |
|---|---|---|
| 01 compiler-path | Bind both shader recipes to the product's pinned glslang | Small independent CLI proposal; default path unchanged |
| 02 build-cancel | Cooperatively stop a host build between pipeline compilations | Independent Windows core interface; null scope keeps old behavior |
| 03 timing-samples | Expose accepted-sample serial and network/other times; handle timestamp width | Add wrap/invalid-query unit evidence before submitting |
| 04 first-pass-output | Export pass 0 for host composition, including person protection | Add independent single-pass equality and retained recording tests |
| 05 output-buffer-policy | Optional RGBA32F reuse, or force separate answer | Prove incremental memory benefit beyond official store_native first |
| 06 ACO-builder-state | Current temporary product adaptation | Request optional configuration/result interfaces; do not transplant this implementation upstream |
| 07 drop-unused-up-shown | Remove residual unused bindings, including local first-pass paths | Official 0.0.7 already removes the independent image allocation; retain only the local layout difference |
| 08 thirty-layer-mode | Raise one network's pass cap from 16 to 30 and size the timing readback for that | The host slider stays 1..3. 30 is a separate layer mode, default off |
| 10 inline-compute-segments | Optional second-family resource access and a recording callback around network dispatches | Default stays one command buffer; device/queues, submissions, failure policy and D3D12 integration stay in the adapter |
| 11 inline-compute-writeback | Allow compatible inline frames to keep the entire Vulkan chain on compute | Unsupported conversions keep the segmented path; host queue and failure policy remain in the adapter |
| 12 weights-only-extraction | Reuse the PE/weight parser with `--extract-weights`; installation skips CUDA scanning and DLL/cubin copies | Analysis defaults and `--extract` remain unchanged; both extraction entry points keep source and model hashes |
| 13 inline-fp16-conversions | Equal-extent FP16 input and writeback on compute, retaining FLOAT/HDR and the original linear/temporal processing | Enabled only for optional second-family/compute hosts with both conversion shaders; default graphics recordings retain blits. Reduced scale, motion requiring a blit and first-pass export keep segments |

| 14 Plan A format conversions | Record the pre-existing local format conversion shaders and recipe additions | Host canonical-format compatibility; explicit patch-created file inventory |
| 15 Windows 0.0.5 product policy | Ignore external INT4 weight overrides in owned mode; preserve separate-answer output policy | Product configuration and resource ownership |
| 16 core-stage-timing | Optional diagnostic timestamp pairs around the linear-input encode and transfer pass | Off unless the host sets it per frame; reuses the timing ring, never waits |
| 17 inline-r11g11b10-compute-convert | Optional B10G11R11 input conversion on compute (copy + runtime_unpack_f16), so such inline frames can keep the whole chain on compute | Off unless RuntimeConfig::compute_convert_r11g11b10; only when the write-back already needs no blit (direct_out / store_native) and model scale is 1; otherwise the blit and its refusal remain |
| 18 inline-r11g11b10-compute-writeback | When write-back is not direct/store_native, pack the answer through runtime_pack_r11 (r11f_g11f_b10f storage image + copy) so B10G11R11 inline frames keep the whole chain on compute | Same toggle as 15; requires B10G11R11 storage-image support and runtime_pack_r11.spv |
| 19 inline-compute-convert-formats | Extends 15/16 to RGBA8/BGRA8 (UNORM and sRGB via the UNORM twin) and A2B10G10R10/A2R10G10B10: compute input via runtime_unpack_f16 and write-back via runtime_pack_u8, runtime_pack_u8_bgra, runtime_pack_u10, runtime_pack_u10_bgra into an R32_UINT image + copy; RuntimeConfig::compute_convert_r11g11b10 becomes compute_convert_colour | Off unless RuntimeConfig::compute_convert_colour; a format whose sampled/storage/transfer support or shader is missing keeps its blit and logs the reason once |
| 20 compute-motion-rescale | New `runtime_mv_rescale.comp` (bilinear, clamp-to-edge; the compute form of the adapter's LINEAR motion blit) and its rg16f/rg32f/rgba16f/rgba32f variants in both `build_network.py` tables | Product-only; used only when the host sets NrInlineComputeMotion on a dynamic-resolution geometry |
| 21 compute-downscale | `RuntimeConfig::compute_downscale`: model scale below 1 on the compute family when the frame is kept in its own format (runtime_downscale, the blit's own taps) and the full-size answer is stored natively, copied (RGBA32F) or packed on compute (runtime_pack_f16 / the per-format packs) | Off unless the host sets it (NrInlineComputeDownscale); otherwise the downscale blit and its refusal remain |
| 22 compute-first-pass-export | `RuntimeConfig::compute_export`: with first_pass_output, a compute pack of pass 0 into the frame's format (`first_pass_packed`, `record_first_pack`; runtime_pack_f16/_r11/_u8/_u8_bgra/_u10/_u10_bgra) | Off unless the host sets it (NrInlineComputeExport); other formats keep the host's blit |
| 23 multipass-motion-in-place | `RuntimeConfig::motion_in_place_passes`: with max_passes > 1, every pass samples the engine motion vectors in place through per-recording sets for the multi-pass pre/post kernels; no motion blit | Off unless the host sets it (NrInlineComputeMotion with 2+ layers); otherwise multi-pass blits the motion as before |
| 24 nonnegative-hdr-reference (removed) | Original 0.0.7 includes the same expression | Do not replay the duplicate fix |

The patches replay sequentially against `manifest.json`'s commit. Patches 02-05
share header/implementation context, so do not assume their patch files apply
independently to an arbitrary upstream checkout. For each upstream proposal,
apply its preceding series in a disposable checkout and reduce the proposal to
its own API requirements; do not send all local policy changes as one PR.

Subsequent patches in the manifest include the unused `up_shown` removal (07),
the local 30-layer mode (08). Former patch 09 (aco-decoder-tail-barrier) still does not
apply on the 0.0.5 pin after patches 01-15; it stays dropped. Official already covers the
decoder-tail case. Patch 23 also folds the former downscale-convert log tweak that was 22
on the pre-0.0.5 series.

The manifest records the product feature-patch snapshot; it is not the upstream PR
series. The upstream preparation branch has reverted the ACO, buffer-optimization
and timing-calculation snapshots and retains only optional interface proposals.

Patch 06's current thread-local change is **not** an unchanged upstream default:
official PE configures before its builder thread starts. An upstream proposal
must preserve that route and provide optional configuration/result access for
the host adapter. Isolation, fallback and product configuration priority belong
to the adapter; upstream owns its ACO/cache/recovery implementation. Do not copy
our whole header upstream or bundle internal recovery changes into an interface PR.

For an update, use an isolated worktree and follow the shared review agreement over
the full upstream diff. Then prepare the candidate closure from `manifest.json`,
replay the patch series in order, remove patches already covered upstream, rebase
the remaining patches, and update the closure inventory. Review the resulting
adapter/runtime impact and run the focused validation required by the affected
features before advancing `manifest.json` and `third_party/mochizuki/UPSTREAM.md`.
The current `check.py` is a read-only replay verifier: it does not fetch or stage a
new pin, update vendor, build the Runtime, or certify game acceptance/release.

See [extension decisions](../../docs/architecture/mochizuki-upstream-extensions.md)
and [source ownership](../../third_party/mochizuki/UPSTREAM.md).

Source comments that name sample-internal addresses are normalized by the declared
`source_comment_policy` during verification. This affects comments only; upstream
identity, authorship, licenses and executable source remain unchanged.

## Windows 0.0.7 construction policy

Patch 27 preserves official standalone defaults while letting this adapter require grade assets, own the depth policy, disable cross-runtime pipeline reuse and skip unused Facts GPU readback. Its fields are internal core construction options, not new public INI/menu controls. The product keeps model/INT4/ACO configuration and whole-graph native fallback in the adapter. The new raw core source does not mean the official PE residency manager or image-heap DRS scheduler has been adopted.

Patch 28 preserves each private Kernel's strong pipeline ownership while avoiding extra permanent arena pins when `share_pipelines=false`. Registry/deferred users keep their original pinning. This supports the adapter's bounded same-model `share_for` background rebuild without enabling global pipeline registry reuse or changing the upstream pin. Replay it after patch 27; it changes only `windows/src/core/nrvk.hpp`.

Patch 30 adds optional Windows recording-resource retention. Pair
`Runtime::begin_recording_resources()` with `end_recording_resources()` even
when recording fails. The returned `shared_ptr<void>` owns independent direct,
frame-read/write and depth pools/views, per-recording timestamp query pools and
referenced or released feature-history images. It survives arbitrary later
recordings without pool reset/reuse or recording-count reclamation of its images.
Core timing retains total/network/stage sampling without waiting; pending samples
hold only weak references and never extend a recipe's resource lifetime. A retained
recipe keeps the handle until all
executions finish and replay is impossible, then releases it before participating
runtimes (including a `carry_history` source). Caller images and runtime/scratch
synchronization remain the adapter's responsibility. Unscoped recordings keep
the official rings/retirement behaviour. Replay after patch 29; the pin is unchanged.

Patch 31 extends the same begin/end resource scope to Windows `renodx::Stage`.
Its handle owns independent borrowed views/pools, cached descriptor-set pools and
shared references to stage images, including look histories and motion/exposure
images later retired by settings changes. Optional `NR_RENODX_PROF` timestamps
also use recipe-owned pools with weak pending-sample tracking. Release handles
after every execution and replay invalidation, before the Stage. Caller images,
scratch ordering and temporal-state commit/discard decisions remain adapter-owned.
Unscoped recordings retain the original rings, caches and retirement behaviour.
Replay after patch 30; the source pin and public C ABI remain unchanged.

The current closure includes both Linux and Windows grade includes: Linux recipes generate the product's Windows ACO SPIR-V. Update the exact ownership generator/pin together with recipes and records; copying only Windows shader sources is insufficient. The original now covers HDR patch 24 and the allocation part of patch 07; the local pack_img release in patch 11 remains necessary.
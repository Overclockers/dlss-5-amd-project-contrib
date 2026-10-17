# Mochizuki source pin and interface patches

The runtime uses official core/shaders at the full SHA in `manifest.json`, plus
twenty-three explicit feature patches. There are no whole-file preservation overlays.
The host loads our `MochizukiNrRuntime.dll`, which wraps that core; this does not
claim binary compatibility with an official distributed runtime.

```powershell
python -X utf8 tools/mochizuki-sync/check.py --upstream <local-official-clone>
tests\mochizuki\run.cmd source-pin
```

The first command reads **Git objects at the pin**, replays the patch series in
a temporary directory under `exports/`, and compares 568 upstream files and six patch-created shaders (Plan A convert/unconvert and motion rescale) with
vendor. It rejects missing/extra source paths, patch conflicts and unrecorded
vendor edits. It normalizes text CRLF only; machine-code records remain opaque.
It does not fetch, change vendor, advance the pin, build, package or publish.
The second command tests the checker with synthetic Git repositories and no GPU.
Those tests also run in `run.cmd abi`.

| Patch | Local purpose | Upstream preparation |
|---|---|---|
| 01 compiler-path | Bind both shader recipes to the product's pinned glslang | Small independent CLI proposal; default path unchanged |
| 02 build-cancel | Cooperatively stop a host build between pipeline compilations | Independent Windows core interface; null scope keeps old behavior |
| 03 timing-samples | Expose accepted-sample serial and network/other times; handle timestamp width | Add wrap/invalid-query unit evidence before submitting |
| 04 first-pass-output | Export pass 0 for host composition, including person protection | Add independent single-pass equality and retained recording tests |
| 05 output-buffer-policy | Optional RGBA32F reuse, or force separate answer | Prove incremental memory benefit beyond official store_native first |
| 06 ACO-builder-state | Current temporary product adaptation | Request optional configuration/result interfaces; do not transplant this implementation upstream |
| 07 drop-unused-up-shown | Below full model scale, do not allocate the unread full-frame `up_shown` image | Official binds it and never reads or writes it; drop the binding with the image |
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
| 24 nonnegative-hdr-reference | Compose linear HDR against nonnegative reference RGB, matching OptiScaler before its resolve; retain raw keep and alpha, leave SDR unchanged | Shader regression reproduces finite negative inputs becoming extreme primary colours before this change; covers native/reduced model sizes and all transfer modes. Game acceptance remains separate |

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

This twenty-two-patch series records the product snapshot; it is not the upstream PR
series. The upstream preparation branch has reverted the ACO, buffer-optimization
and timing-calculation snapshots and retains only optional interface proposals.

Patch 06's current thread-local change is **not** an unchanged upstream default:
official PE configures before its builder thread starts. An upstream proposal
must preserve that route and provide optional configuration/result access for
the host adapter. Isolation, fallback and product configuration priority belong
to the adapter; upstream owns its ACO/cache/recovery implementation. Do not copy
our whole header upstream or bundle internal recovery changes into an interface PR.

For an update: inspect the final upstream diff by feature, review changed defaults,
recipes, resource layouts and host-facing interfaces, and stage the new Git closure
in an isolated worktree. Drop patches covered upstream; rebase remaining patches
and update the closure inventory. Advance the pin only after the needed adapter
and focused validation pass. Merely editing the SHA or passing this replay check
does not complete integration, game acceptance or release certification.

See [extension decisions](../../docs/architecture/mochizuki-upstream-extensions.md)
and [source ownership](../../third_party/mochizuki/UPSTREAM.md).

Source comments that name sample-internal addresses are normalized by the declared
`source_comment_policy` during verification. This affects comments only; upstream
identity, authorship, licenses and executable source remain unchanged.

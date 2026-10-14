# Mochizuki source pin and interface patches

The runtime uses official core/shaders at the full SHA in `manifest.json`, plus
fifteen explicit feature patches. There are no whole-file preservation overlays.
The host loads our `MochizukiNrRuntime.dll`, which wraps that core; this does not
claim binary compatibility with an official distributed runtime.

```powershell
python -X utf8 tools/mochizuki-sync/check.py --upstream <local-official-clone>
tests\mochizuki\run.cmd source-pin
```

The first command reads **Git objects at the pin**, replays the patch series in
a temporary directory under `exports/`, and compares all 173 closure files with
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
| 09 decoder-tail-barrier | Preserve the Windows ACO C64-to-fused-C32 dependency | Independent correctness proposal with its original attribution |
| 10 inline-compute-segments | Optional second-family resource access and a recording callback around network dispatches | Default stays one command buffer; device/queues, submissions, failure policy and D3D12 integration stay in the adapter |
| 11 inline-compute-writeback | Allow compatible inline frames to keep the entire Vulkan chain on compute | Unsupported conversions keep the segmented path; host queue and failure policy remain in the adapter |
| 12 weights-only-extraction | Reuse the PE/weight parser with `--extract-weights`; installation skips CUDA scanning and DLL/cubin copies | Analysis defaults and `--extract` remain unchanged; both extraction entry points keep source and model hashes |
| 13 inline-fp16-conversions | Equal-extent FP16 input and writeback on compute, retaining FLOAT/HDR and the original linear/temporal processing | Enabled only for optional second-family/compute hosts with both conversion shaders; default graphics recordings retain blits. Reduced scale, motion requiring a blit and first-pass export keep segments |
| 14 core-stage-timing | Optional diagnostic timestamp pairs around the linear-input encode and transfer pass | Off unless the host sets it per frame; reuses the timing ring, never waits |
| 15 inline-r11g11b10-compute-convert | Optional B10G11R11 input conversion on compute (copy + runtime_unpack_f16), so such inline frames can keep the whole chain on compute | Off unless RuntimeConfig::compute_convert_r11g11b10; only when the write-back already needs no blit (direct_out / store_native) and model scale is 1; otherwise the blit and its refusal remain |

The patches replay sequentially against `manifest.json`'s commit. Patches 02-05
share header/implementation context, so do not assume their patch files apply
independently to an arbitrary upstream checkout. For each upstream proposal,
apply its preceding series in a disposable checkout and reduce the proposal to
its own API requirements; do not send all local policy changes as one PR.

Subsequent patches in the manifest include the unused `up_shown` removal (07),
the local 30-layer mode (08), and the decoder-tail correctness fix (09).
Patch 09 is adapted from Overclockers' `60f9202f` in project PR #5. It excludes
the C64 persistent decoder to fused C32 pair from tile chaining, preserving the
existing barrier and associated arena lifetime boundary; other chains remain.
Its attribution and validation are in the backend guide and source record.

This fifteen-patch series records the product snapshot; it is not the upstream PR
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

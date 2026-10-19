# Mochizuki: upstream ownership and remaining interfaces

The product follows official `cc27501dc116bdcfbb43612e497c5ca12497574a` (Windows 0.0.7) core,
graph, shaders and recipes. The residual feature patches are explicit in
[the pin manifest](../../tools/mochizuki-sync/manifest.json), with a non-mutating
[replay check](../../tools/mochizuki-sync/README.md). They reproduce the existing
vendor snapshot. Local scheduling extensions are recorded separately; inference
network arithmetic follows the original, with explicit local conversion recipes and host interfaces. This is the same ownership approach used for lmxxf, not a
claim that either backend directly loads the author's distributed binary.

## What belongs to the host

Upstream proposal scope is **optional interfaces only**. The local feature-patch
inventory records the current product integration, not implementations to
transplant upstream. The upstream preparation branch has reverted the ACO,
buffer-optimization and timing-calculation snapshots. Keep official ACO and timing
behavior; request explicit configuration and result/observation APIs instead.
Product isolation, whole-graph fallback and configuration policy stay in the adapter.

INI/menu priority, backend selection, D3D12/Vulkan interop, recording leases,
replay and resource retirement, person inference/masks/composition, ACO asset
validation and whole-network fallback, package ownership, and installed/requested
compiler status belong to the product adapter. These are not reasons to fork
the model graph or shaders. In particular, the upstream first-pass proposal
contains no person model, ONNX worker, game hook, menu or INI schema.

## Residual decisions

| Interface | Necessity and consumer | Default / limits | Upstream decision |
|---|---|---|---|
| Explicit glslang path | Build wrapper supplies one pinned Windows compiler to both recipes | Existing relative path remains the default; no generator/kernel changes | Prepare first as a standalone build-tool PR |
| Cooperative cancellation | `BuildCancelScope(&abandon)` bounds obsolete/shutdown host builds | Null pointer preserves normal builds; checks between pipelines, cannot interrupt a driver's current compile; standalone graph unaffected | Prepare second as a host-neutral Windows core API |
| Accepted timing samples | Adapter uses `gpu_timing_serial()` to avoid duplicate observations, and last network/other values for window statistics | Official network EMA alone cannot provide these instantaneous samples. Timestamp masking/invalid rejection change diagnostic behavior, not image arithmetic | Keep; split query correctness from new getters and add deterministic width/wrap validation before PR |
| Optional pass-0 output | `RecordLayers` requests it for shared person composition; adapter copies the Vulkan image to its held D3D12 output | Off by default; ordinary multipass, model applied, no active preprocessing/control mask/native compose. Runtime owns allocation; contents require caller synchronization and may be overwritten by later recordings | Keep; propose generic auxiliary output. Existing distinct/nonblack test is weaker than single-pass equality; strengthen that and replay/lifetime validation before PR |
| Output-buffer policy | Adapter maps `MochizukiCompactTransfer` to optional RGBA32F reuse / separate answer | Both new core flags default false; official native store remains first choice. Reuse only covers remaining RGBA32F fallback cases, not every format or mode | Conditional optimization, not essential architecture. Keep currently tested option; prove incremental memory savings and image equality beyond official native store before proposing; remove it if not justified |
| ACO builder configuration | Adapter configures on the device thread and builder thread, reads import count, rejects mixed/empty graphs, classifies fatal Vulkan errors | Current local thread-local default differs from official process configuration. Official `nr_pe_config.cpp::configure_network` precedes `nr_pe_session.cpp`'s background builder | Keep for this integration, but do not submit the whole patch. First prepare generic typed Vulkan errors / fatal-cache-error propagation. Design opt-in scoped builder configuration preserving official host defaults; review cache identity per device before that PR |
| Unused `up_shown` | Below full model scale the enlargement passes bound a second full-frame RGBA32F image they never read or write | Official 0.0.7 removes the allocation; the residual local patch removes unused bindings, including local first-pass paths. `up_result` still holds the classic enlargement | Keep only residual layout changes; do not count the allocation saving twice |
| 30-layer cap | Host layer mode asks one network for 30 sequential passes | Default stays 1. Official core rejected above 16; the timing readback buffer was the same width. Slider stays 1–3 | Local cap for the host mode. Do not treat it as an upstream default |
| Network recording segments | Host moves inline neural work off the game's busy 3D engine | Null callback preserves the old path; optional second-family sharing, ordered timeline dependencies and same-device buffers are required | Local Windows interface extension; do not send queue creation, D3D12 polling or recovery policy as an upstream core implementation |
| FP16 compute conversions | Avoid graphics/compute round trips for full-scale FLOAT inline frames | Private FP16 images and optional shaders; graphics recordings retain blits. HDR/linear processing and native direct paths remain; reduced scale, motion blits and first-pass export retain segments | Patch 13, local conversion optimization. Compare actual shader bytes against blit and sustained inline against split; no inference-kernel or ACO record changes |

Weights-only installation extraction is patch 12: `inspect_nr.py --extract-weights`
uses the existing PE/resource/weight parser, skips CUDA inventory, and writes only
the manifest, weight container and layer payloads. Both install entry points use
it. The analysis default and `--extract` still inventory/export cubins and the DLL.
Source SHA256, the 599-entry model list/hash checks and packed format stay unchanged.
This is a small optional tool contribution; no model or NVIDIA binary is vendored.

The actual residual modifications also include the model inspection/extraction
entry points, the two `build_network.py` scripts,
`nr_graph.cpp`, `nr_runtime.cpp/.hpp`, `nr_pipeline_binary.hpp`, and the
`runtime_upscale` / `runtime_transfer` passes on both shader trees. The feature
series separates those into reviewable patches. `nrvk.hpp` now accepts optional
concurrent resource sharing for the host's second queue family; records remain
unchanged. Old HDR/unroll/ViT,
`NR_EDGE_BODIES=0`, progress callbacks and enlargement patches stay removed.

## Evidence and submission order

Existing local Windows runtime/ABI, native composition, timing, first-pass,
shutdown/inline, ACO import/fallback/switch/budget/rapid-switch tests are recorded
in the [backend guide](../mochizuki.md). They are local adapter tests, not a
standalone upstream test suite. There is no fresh Linux GPU validation, full
game acceptance or multi-frame ACO temporal image comparison.

Submit the compiler-path proposal independently first. The cancellation proposal
has a separate patch and draft, but should include a deterministic scope/polling
check when preparing its upstream branch. Timing and auxiliary output require
the evidence described above. Keep buffer policy and scoped ACO as proposals
until their design/validation criteria are met. ACO counter-only ordering remains
an unresolved image-validation question; it is not part of these interface PRs.

Overclockers' PR #5 (`7531fa40`) informed builder isolation, import counting,
fatal Vulkan error handling, whole-network fallback and record/cache tests.
Preserve that attribution in contributions derived from those changes, separately
from the official a9b13dd authorship. No upstream PR is represented as submitted
or accepted by this document.

## Windows 0.0.7 adoption boundary

- Adopt the complete original core, both shader recipe inputs, Natural/Cinematic grade pipelines and their default/INT4 ACO records. Standard remains ungraded; the existing model and INT4 controls map the three network identities without changing saved preferences. Ordinary Original and Small support native compilation or ACO; Original INT4 mixed requires ACO.
- Patch 27 keeps the original standalone defaults but gives the product explicit construction policy. This adapter supplies no depth, so it fixes motion_without_depth instead of allowing a second INI to decide it. It disables cross-runtime SharedPipeline registry lookup/publication until reuse can preserve build-thread compiler identity, import counts and whole-graph fallback. Shared ownership of each private pipeline remains. Stage Facts readback is disabled for a host that has no consumer. Required grade variants cannot silently disappear from an incomplete package.
- Retain the pack_img release fix, pass-capacity reuse, failure service, DRS settle/buckets, dual-API recording proofs, model hashes/read leases and exposure continuity. Remove duplicate HDR patch 24; shrink patch 07 to residual layouts already needed by local first-pass/transfer paths.
- Defer opting into share_for/with_slot_of, deferred image heaps, cross-model activation sharing and RAM offload. A two-region alias heap cannot invalidate still-replayable host recordings; adoption requires an explicit replacement for existing size/cache ownership, ordered GPU proofs, budget and failure recovery. The product currently retires obsolete networks rather than keeping the original PE's inactive network cache, so adding offload is not itself a fix for the old pack_img leak.
- Defer motion_point: changing the final sampler alone cannot undo the host's earlier LINEAR rescale. Define low-resolution motion, subrect/padding and multipass behavior before changing that contract. Original default behavior is available in core, but this product does not claim the NGX host's point-sampling change is enabled.

Validation is bounded to the tested routes and inputs: five supported model/compiler combinations across three styles, six fixed-source native core comparisons at 256x256, and existing focused lifecycle/recovery/composition checks. No new tracked test project was added. The existing ACO CPU fixture's record count advances to 62, and damaged-bundle fixtures explicitly select their supplied Original model instead of waiting for absent Small weights. Game coverage, long 4K stress and the full Inline matrix remain separate from this source update.

The remaining capabilities have now been assessed together in [the 0.0.7 remaining assessment](mochizuki-007-remaining-assessment.md). Its recommendations do not enable those paths: bounded background graph/weight reuse and split activation sharing are implementation candidates, while direct stale-heap adoption and adding an inactive cache solely for offload are rejected for the current product.

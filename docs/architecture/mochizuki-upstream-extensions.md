# Mochizuki: upstream ownership and remaining interfaces

The product follows official `a9b13dd4cdb5f4bf43db358492d72e6e5e2acd67` core,
graph, shaders and recipes. The residual feature patches are explicit in
[the pin manifest](../../tools/mochizuki-sync/manifest.json), with a non-mutating
[replay check](../../tools/mochizuki-sync/README.md). They reproduce the existing
vendor snapshot. Local scheduling extensions are recorded separately; inference
arithmetic and shader recipes remain official. This is the same ownership approach used for lmxxf, not a
claim that either backend directly loads the author's distributed binary.

## What belongs to the host

Upstream proposal scope is **optional interfaces only**. The local eight-patch
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
| Unused `up_shown` | Below full model scale the enlargement passes bound a second full-frame RGBA32F image they never read or write | The image and its bindings are removed. `up_result` still holds the classic enlargement | Local memory fix. Official still allocates it |
| 30-layer cap | Host layer mode asks one network for 30 sequential passes | Default stays 1. Official core rejected above 16; the timing readback buffer was the same width. Slider stays 1–3 | Local cap for the host mode. Do not treat it as an upstream default |
| Network recording segments | Host moves inline neural work off the game's busy 3D engine | Null callback preserves the old path; optional second-family sharing, ordered timeline dependencies and same-device buffers are required | Local Windows interface extension; do not send queue creation, D3D12 polling or recovery policy as an upstream core implementation |

The actual residual modifications are the two `build_network.py` scripts,
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

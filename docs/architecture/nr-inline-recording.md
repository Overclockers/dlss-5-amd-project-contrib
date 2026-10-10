# Experimental same-list NR execution

`[DlssNr] NrInline=true` enables the host's experimental same-command-list path
for lmxxf and Mochizuki. Default is false; ordinary command-list splitting remains
available. The control appears on the NR input page. Daniel's existing inline mode
is independent. Neither path artificially ends or restarts the game's queries.

## Recording and execution

The host attaches a recording lease before appending private commands. Inputs are
recorded normally, followed by a D3D12 capture flag. A Windows threadpool worker
samples that flag through an independent D3D12 compute queue; readback is read only
after that queue's fence completes. Sampling and completion use separate buffers.
No HIP/Vulkan shader is expected to see a D3D12 shader flag.

After observing capture, the worker calls the current recording ABI's Enqueue on
the same-device side compute queue. Existing HIP/Vulkan external-fence ordering
remains: side queue input Signal, network wait/run/signal, side queue output Wait.
A side-queue shader then publishes the D3D12 completion flag. Putting the input
Signal behind the entire waiting game list would create a circular dependency;
the side queue and capture proof are essential.

The game list contains 64 bounded wait dispatches, each limited to 32768 iterations.
UAV barriers serialize slices and the final success snapshot. This is a work budget,
not a portable millisecond timeout. Slices allow independent queues to progress;
there is no unbounded shader loop. The frozen snapshot predicates the runtime's
output work. A host composition shader selects either that output or this frame's
original colour into a private texture (FP32 for FP32 sources, otherwise FP16). Optional first-pass output uses the
same decision. Late completion cannot change a failed frame's frozen selection.

Pipeline/root/heap bindings captured by the proxy are restored after recording.
Unsplit observed lists remain in the caller's original contiguous ExecuteCommandLists
batch; lifecycle callbacks bracket that submission without adding Execute boundaries.
An open non-timestamp query remains open across the NR commands on its original
native list; the game closes it normally. Injected compute is inside that query's
scope, just as other inline work is; preserving the scope does not remove NR's
contribution to pipeline statistics.

## Admission and enhanced barriers

Native interface escape, untracked/unsupported command state, predication, active
render passes and open legacy split barriers still refuse inline recording.
The `open_query` split restriction remains unchanged. Query diagnostics record
heap, type, index, list identity and generation without changing query contents.

`LmxxfAllowEnhancedBarriers` initializes proxy policy during Config loading,
independently of backend construction; menu changes synchronize the same policy.
An existing recording's split rejection is not retroactively cleared.
Mochizuki's ordinary split path and shared output effects still reject enhanced
recordings; enabling the experimental inline path is a separate decision.

For sources actually consumed by the selected backend, the proxy admits known
whole-resource enhanced layouts and records a transition to COMMON before legacy
runtime operations. It returns each source to COMMON and restores the original
enhanced layout/access afterward. Partial/pending layouts and unknown source state
refuse recording. Unused depth, exposure or motion inputs are not normalized.
This is deliberately not a complete D3D12 resource/subresource state tracker.

Current composition admits single-sample, single-array textures with explicit
FP16/FP32, R11G11B10, R10G10B10A2, RGBA8 or BGRA8 views. Typeless and unsupported
source views pass through. A runtime's own format/geometry restrictions still apply.

## Ownership and package ABI

ABI v5 adds `LMXXF_NR_CREATE_FLAG_INLINE_QUEUE`; it requires recording leases.
Both host and runtimes must come from the complete current package. Old ABIs are
rejected. The lmxxf bridge's side-queue interface is an explicit optional patch
on the unchanged official pin; it does not change network modules or arithmetic.

The lease retains both side-queue completion and the actual game-list tail fence.
A separate lmxxf consumer fence certifies game completion. It never signals the
shared HIP fence: a larger game value must not falsely release a late network wait.
A game tail alone cannot release a late network. New records on that session wait
by passing through until both sides complete. A timeout resets history before the
next accepted frame. Reset/final Release during an asynchronous execution defers
runtime invalidation until the worker's End event, then collects only after both
queues finish. Uncertain signals or enqueue failure poison further NR and retain
resources without inventing completion proof. No GPU completion wait occurs under
the host recording mutex.

## Validation (2026-10-11)

RX 9070 XT / gfx1201: bounded success and exhaustion, original/current-frame and
first-pass selection, immutable late-completion decision, query preservation,
closed-list replay, whole-resource enhanced normalization/restoration, and retention
until both queue proofs passed. WARP exercises the same fixture.
`tests/host/nr_recording_admission.cpp` covers read-only query diagnostics and
partial/pending enhanced-layout rejection.

Actual HIP at 1280x720 and Vulkan at 256x256 matched the split baseline exactly:
`965376b3eb4e4fc8` and `e8da65152a88c7ff`, respectively, for the fixture settings.
Both passed cross-queue replay and successful Reset before GPU capture, followed
by deferred invalidation/collection. Commands:

- `tests\lmxxf\run.cmd inline` (hardware mechanism)
- `tests\lmxxf\run.cmd inline-gpu` (`LMXXF_ASSETS`, `LMXXF_TEST_RUNTIME`)
- `tests\mochizuki\run.cmd inline <MochizukiNrRuntime.dll>`

These are mechanism/fixture results, not Crimson Desert compatibility or performance
certification. Cold-start game testing, other adapters/drivers, full temporal/multi-pass
acceptance and long hot-switch stress remain outstanding. Multiple simultaneously
executing inline records on the same session are unsupported and rejected before
batch submission. Keep the option off by default.

# Experimental same-list NR execution

`[DlssNr] NrInline=true` enables the host's experimental same-command-list path
for lmxxf and Mochizuki. Default is false; ordinary command-list splitting remains
available. The control appears on the NR input page. Daniel's existing inline mode
is independent. Neither path artificially ends or restarts the game's queries.

## Recording and execution

The host attaches a recording lease before appending private commands. Inputs are
recorded normally, followed by a D3D12 capture flag. A Windows threadpool worker
samples that flag through an independent D3D12 compute queue (50 microseconds
exponential backoff to 5 ms, at most 2048 samples and a 10 s deadline); readback is read only
after that queue's fence completes. Sampling and completion use separate buffers.
No HIP/Vulkan shader is expected to see a D3D12 shader flag. Unique fence events
wake the worker; the completed fence value remains the proof. Registrations stay
owned by that recording until completion and are never reused across fence values.
Sampling backoff uses a private high-resolution waitable timer (ordinary timer
fallback), without changing the global timer period. This avoids 50-us sleeps
turning into whole Windows timer ticks.
Enqueue ordering uses a condition variable notified after the preceding runtime End.

After observing capture, the worker calls the current recording ABI's Enqueue on
the same-device side compute queue. Existing HIP/Vulkan external-fence ordering
remains: side queue input Signal, network wait/run/signal, side queue output Wait.
A side-queue shader then publishes the D3D12 completion flag. Putting the input
Signal behind the entire waiting game list would create a circular dependency;
the side queue and capture proof are essential.

For Mochizuki inline jobs, an available compute-only Vulkan family executes the
network dispatch loop. Graphics-required conversion, scaling and history work
remain on the original family. A timeline semaphore orders each pre/network/post
segment; core resources and adapter images support both families concurrently.
The final graphics fence certifies the entire chain before a slot can be reused.
Partial or unconfirmed submission fails closed and retains resources. A recording
failure after crossing a segment boundary poisons the session instead of retrying
potentially stale core layout/history state. The ordinary game-list split path
continues to use its original Vulkan queue. This changes network queue selection,
not the compute/pixel stage of the D3D12 waiting shader.

The game list contains 64 bounded wait dispatches, each limited to 32768 iterations.
UAV barriers serialize slices and the final success snapshot. This is a work budget,
not a portable millisecond timeout. D3D12 does not guarantee concurrent queue progress. Bounded slices prevent an
unbounded shader loop but cannot establish a frame-time or TDR-safe bound. The frozen snapshot predicates the runtime's
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
queues finish. Enqueue failure still poisons the runtime session. The host appends a fresh
side-queue tail even when no network succeeded, and reclaims its reservation only
after both that tail and the actual game tail complete. Runtime CollectRecording
independently certifies HIP/Vulkan lifetime; failed or partially submitted network
work can still retain that runtime job. Unknown signals retain resources without
inventing completion proof. These unprovable retirements do not continually rearm
the collection timer; first-failure logging remains available. No GPU completion wait occurs under
the host recording mutex.

## Crimson Desert follow-up fixes (2026-10-11)

Each session now retains one compute side queue in its pipeline cache. New recordings
reuse that queue identity while keeping their own flags, allocators, fences and output
textures. The session's single-pending-execution admission rule still applies. This
prevents artificial Mochizuki producer switches, history resets and retained producer
queues from growing with the number of new recordings. Actual game queue changes and
history discontinuities retain their safety checks.

The product lmxxf loader uses the same controlled HIP 7 search as our Daniel adapter: default
directories, absolute HIP_PATH/bin, then absolute PATH directories with dependency-aware
DLL flags. It does not mutate process-wide search paths or fall back to HIP 6. The explicit
`hip-runtime-loader.patch` supplies a product loader hook before any vendor header is
included, including LmxxfProductionOptions. Unmodified upstream users retain their
System32 policy. Attempts, selected path and queried version are written to
`lmxxf_runtime.log` beside the runtime DLL; load failures retain Windows error codes.

Host logs periodically report actual applied/timeout/aborted decisions, pending bypasses,
live recordings, side queue identity and the last wait slice's iteration count. These
decisions are read only after both queues complete. Mochizuki logs requested/pending
resets and missing motion, network/frame/time/geometry/pass discontinuities separately.
Counters do not suppress necessary history resets or change the GPU wait budget.

Focused validation: host CI, HIP loader fixtures (including Unicode/space paths and
same-directory dependencies), HIP injection/export validation, HIP 6 rejection, ABI
C/C++ and 23 runtime checks, host and both runtime builds. Hardware/WARP mechanism
tests retain open queries, enhanced layout restoration, replay, immutable timeout
selection and dual completion. Actual HIP 1280x720 and Vulkan 256x256 each passed 120
fresh recordings in success and forced-timeout modes with stable queue identity and
zero retained invalidated leases after collection. Vulkan 2560x1440 passed 30 fresh
recordings; non-temporal output hash `12cf1e45fb84c1b8` matched split, and zero-motion
temporal validation reported 85% history consumption including earlier reset/replay
negative cases. `tests/mochizuki/run.cmd inline` includes the large temporal fixture.

These fixtures do not certify Crimson Desert frame pacing, long-run VRAM use, all
feature combinations, ACO mode or other drivers. Game retesting remains required.

## Unsubmitted recording retirement (2026-10-12)

An inline reservation is relinquished when its recording is invalidated before
execution. Successful collection also removes any remaining unstarted reservation.
`ForgetUnstarted` leaves started resources alone; their normal dual-completion
reclaim still owns them. This prevents four discarded command lists from permanently
occupying the admission cap. `tests/lmxxf/run.cmd inline-discard` uses WARP and a mock
runtime to exercise 12 alternating real proxy Reset/Release operations, verify exact
job retirement and capacity reuse, and retain a started reservation until its
certified side-reset completion. No model or hardware GPU run is needed.

## Failure and capacity reports

lmxxf reports the first recording failure at error level for every game, including
the phase, result, execution queue and error captured on the callback thread.
The NR-off status retains that first error after releasing the backend's session
reference; a later poisoned-session error must not replace it.

Mochizuki logs requested passes, active passes, installed capacity and the deferred
build/refusal reason when that combination changes. A refused expansion keeps the
compatible network running at its existing capacity. This is reduced execution,
not proof that the requested pass count ran. The VRAM guard remains in force.
Pending history resets carry reason bits for startup, queue, network, extent,
passes, missing motion, passthrough, cancellation, external reset and enqueue failure.
The reason legend accompanies the bounded reset log; it does not change reset policy.

## Initial validation (2026-10-11)

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

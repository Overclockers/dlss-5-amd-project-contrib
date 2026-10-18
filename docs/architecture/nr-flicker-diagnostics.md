# NR flicker diagnostics

`[DlssNr] NrFlickerDiag=true` (Ins -> the active backend's Diagnostics ->
**NR flicker diagnostics**) enables a single capture for Mochizuki and lmxxf.
Default is off. Save Settings and restart so file logging is initialized even
when `[Log] LogToFile=false`. This enables the effective per-frame trace and
game-thread wait timing without rewriting their individual preferences. Host
logging uses at least Info while enabled. Mochizuki runtime logging is enabled too.
Disabling the bundle and restarting restores the individual logging preferences.
Inline, enhanced-barrier, early-wrap, autotune and picture preferences are unchanged.

## One reproduction

Enable the switch, save and restart. Wait for model construction to finish,
reproduce the flicker in gameplay for 30–60 seconds, then exit normally. Send:

- `OptiScaler*.log` (including rotated logs; or the configured custom log path).
- `mochizuki_nr.log` and `mochizuki_nr.log.1` if present, for Mochizuki.
- `NrFrameDiag.log` and `NrFrameDiag.log.1` if present.
- `OptiScaler.ini` and, if possible, a short video and approximate symptom time.

The runtime and frame trace live next to the installed DLL. Host logs use the
existing configured log path. Host rotation retains two 8 MiB backups;
frame trace retains one 8 MiB backup; Mochizuki retains one 4 MiB backup.
Turn the switch off, save and restart after capture. No separate capture with
each sub-diagnostic enabled is required.

## Coverage and interpretation

The host bridge and both backend entry points count every offered call, including
missing-proxy calls that never reach the runtime or increment its NR frame id.
Fixed-size counters run independently of detailed capture. With file logging
enabled, `NR coverage` reports five-second deltas, cumulative offered calls,
terminal outcomes, and a release snapshot. `mixed=true` means that window contains
both recorded calls and bypassed calls; it is evidence of mixed processing,
not proof of visible flicker. Backend reasons distinguish missing proxy, split
rejection, input rejection, session unavailability, build/settle wait, prepare
failure, busy admission, recording failure, latched fallback and probe modes.
Bridge bypasses include paths that never call a backend.

`before_first_prepare` / `after_first_prepare` refer to the first successful
PrepareFrame of the current backend session. This is a readiness marker, not a
guarantee that later prepares succeed. The bridge's marker instead means its
first recorded replacement. `recorded` means commands were recorded, not that
the GPU selected the NR result. Calls, recordings, submission callbacks, runtime
enqueue attempts and Presents are distinct units and must not be subtracted as
if they were a one-to-one sequence. Replays can produce extra execution events.

Execution summaries retain callback/failure/enqueue counts, accepted split
submissions, and inline selected/missed outcomes harvested from already retired
recordings. Split acceptance is submission evidence, not a new image-quality or
GPU-completion oracle. Retained recordings may finish after the backend release
snapshot; it does not wait to drain them. Counter snapshots across callbacks are
observational, not a transaction across all pipeline stages.

Detailed capture samples each outcome once per five seconds per readiness phase,
including sparse misses below the old every-1000th-warning threshold. Samples
include a separate call ordinal, monotonic tick, current Present serial, list
identity/generation when available, rejection reason and missing-list vtable
owner. Vtable ownership identifies the implementation module, not necessarily
the creator. `NrFrameDiag.log` supplies the existing per-recording execution trace.

Coverage adds no GPU queries, readbacks or waits, and uses existing locks for
record-side state and relaxed counters for callbacks. Five-second summaries
format and write only at that cadence; detailed module lookup is rate-limited.
The bundle also enables existing per-frame diagnostics (including GPU timestamp
collection and retired-result processing), so it has more overhead than coverage
alone. No GPU performance percentage or AC Mirage fix is established by CPU checks.

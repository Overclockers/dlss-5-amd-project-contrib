# Mochizuki 0.0.5 final review and handoff

This records the PR #11 scope agreed in the maintainer review. Version:
`1.12.1-alpha`. The original starting point was branch
`work/mochizuki-upstream-a9b13dd` at
`5ca071be826d2b5da887e26bbe839e937a13a863`. The integration was subsequently
rebased onto `bb851c4b57835409ecd72931a07cc1ed4060d513`, then the reviewed base
`c1cb8c06389af7fc2580757ce00cc6efd416f111`.
The full upstream target is `8a70f7f418294ff1e306b66893456e5319ff8205`.
The exact submitted head and Actions run are recorded on
[PR #11](https://github.com/TheAutomatic/dlss-5-amd-project/pull/11).

## Review conclusions and scope

The source closure, host execution, configuration, menu, resource lifetime,
installation ownership and Actions changes were reviewed by feature. The pin is
replayable, including Windows consumers of Linux INT4 recipes/data. Compatibility
extensions remain explicit patches. No second configuration source or overlay is
introduced. Existing explicit ACO and native picture preferences remain owned by
Ins/INI; the network hotkey changes only `MochizukiInt4Mixed`.

The latest rebase adds release notes/README changes (`430e8f2a`) and capture-worker
timeout diagnostics (`c1cb8c06`). The latter changes only failure reporting; the
shader/WARP suite verifies the affected capture worker path. Earlier rebased white
point source/trim changes remain present, with host CI/config/localization checks.

| Classification | Items and decision |
|---|---|
| Integrated, Mochizuki only | INT4 mixed, verified local weights, RenoDX picture settings, full network fallback, direct AMD ICD identity, upstream downscale/store changes, NoiseEveryFrame, Ctrl+F11 network selection |
| Existing shared host features | Backend selection, Ins/INI ownership and final output effects remain shared. Their semantics are not replaced with similarly named RenoDX settings |
| Existing lmxxf/Mochizuki feature | Person protection remains supported on native paths; it is disabled in RenoDX because its first-pass contract is unavailable |
| Deferred | Full game quality/FPS matrix, real-game GPU spoofing and HDR/exposure acceptance; no new lmxxf upstream update in this PR |
| Excluded by review scope | Whole `237f920f..base` audit, share-code import/export and presets (issue #10 steps 1 and 3) |
| Excluded by Windows host scope | Linux deployment/UI, alternate DX hosts, ReShade routes, upstream Present/screenshot hooks, depth and Present-only RenoDX settings |

See [integration inventory](mochizuki-005-integration.md) for source groups,
dependencies, retained interfaces and license ownership. MIT/GPL attribution and
RenoDX MIT notices are retained in source and package checks.

## Explicit decisions

- Ctrl+F11 switches default/INT4 **requests**, without changing the independent ACO
  setting. Plain F11 keeps its configured shortcut. The chord consumes F11 through
  release so a configured F11 binding does not also fire; holds, text/key capture,
  other backends and unfocused windows do not trigger it. Save Settings persists it.
- NR Control (End) is not compiled, installed or hooked, so it is disabled by
  construction. Ins remains the menu; End retains this project's existing NR toggle.
- No local release package, tag or Release is prepared for this review follow-up.
  The requested manual Release workflow verifies a non-tag PR branch and can
  produce a CI test artifact; publication steps remain tag-gated.
- lmxxf follow-up requires its own target pin/review/PR. Existing lmxxf failures in
  Forza and FF7RE are baseline reports, not acceptance of any new regression.

## Why patch 09 is redundant

All links below identify the pinned upstream commit, not a moving branch:

1. [`nr_graph.cpp:215`](https://github.com/mochizuki0323/DLSSNR-AMD/blob/8a70f7f418294ff1e306b66893456e5319ff8205/windows/src/core/nr_graph.cpp#L215)
   retains the C64 persistent → fused-C32 barrier. The decoder expression is
   `((g(p) && up(q)) || (up(p) && up(q)))`, exactly the former patch's expression;
   `fswinfusedup32` is no longer accepted as the second decoder pair consumer.
2. [`nr_graph.cpp:4261`](https://github.com/mochizuki0323/DLSSNR-AMD/blob/8a70f7f418294ff1e306b66893456e5319ff8205/windows/src/core/nr_graph.cpp#L4261)
   uses the same predicate when extending arena lifetimes across a chain. The
   corrected boundary therefore also governs memory reuse.
3. [`nr_graph.cpp:4469`](https://github.com/mochizuki0323/DLSSNR-AMD/blob/8a70f7f418294ff1e306b66893456e5319ff8205/windows/src/core/nr_graph.cpp#L4469)
   rejects the pair before setting its counter-only/no-barrier state;
   [line 5179](https://github.com/mochizuki0323/DLSSNR-AMD/blob/8a70f7f418294ff1e306b66893456e5319ff8205/windows/src/core/nr_graph.cpp#L5179)
   transfers only admitted pairs to `runner.no_barrier_after`.
4. [`nr_runtime.cpp:2604`](https://github.com/mochizuki0323/DLSSNR-AMD/blob/8a70f7f418294ff1e306b66893456e5319ff8205/windows/src/core/nr_runtime.cpp#L2604)
   skips a barrier only when that bit is set. The decoder-tail barrier remains.

RX 9070 XT comparisons against a separately built all-barrier DLL cover 512×512,
three passes, 128 moving frames (case 11), and 2258×1270, three passes, 16 moving
frames (case 19), with preprocessing and retained-recording replay. The complete
45-case matrix is not claimed. Overclockers' original attribution remains intact.

## Controls and history lifecycle

`DlssNr_Menu.cpp` hides native white point/source, linear encoding, resolution,
network response, native pass/output controls and prebuild capacity while RenoDX
replaces them. This is visibility only; `Config` values are not rewritten.
Person controls are disabled and `AmdBridge.cpp` also gates `Person::Prepare`, so
there is no hidden person worker/composite with RenoDX selected. Original person
settings resume when switching back.

Preprocessing stays visible because `Job::controls.preprocess` is passed to
`StageFrame::preprocess`; upstream applies preparation and its inverse inside the
stage. The core itself uses `config.preprocess=false`, avoiding a duplicate
transform. RenoDX uses its own feed/meter settings, leaving native white-point
preferences untouched. SR→NR placement is passed explicitly so Auto detail
stability sees the correct route.

The host drops the active session on NR off. On off/on, the new session begins
with `ResetStartup`. Network install requests `ResetNetwork`; extent changes
request `ResetExtent`. Execution compares network identity, frame adjacency,
geometry, pass count and the 250 ms gap. A retained recording replay is marked
discontinuous and resets its own network. RenoDX bypass sets its stage to recreate;
on resume it resets the stage and per-pass network histories. The implementation
does not introduce an upstream resident-network cache; retained recordings are the
relevant resident ownership path here.

The Windows 0.0.5 GPU fixture uses increasing frame IDs, waits for background
build completion, compares automatic mode/extent/idle resets to an explicit reset,
recreates sessions, toggles RenoDX neural processing and replays the original
recording after switches. Continuous history and barrier comparisons have their
own fixture; they do not infer success merely from finite output.

## Validation and known limits

Local checks cover host compilation, host CI (including real embedded-font glyph
coverage and shortcut behavior), configuration priority, ABI, source replay,
default/INT4 ACO record inventories, build-cache invalidation, installer ownership,
release notes and WARP shader/person capture. GPU checks cover modes, history,
native fallback, barrier equivalence and ACO inline scheduling. The PR records the
final run outcomes and exact head; the full 45-case barrier matrix, all 85 RenoDX
settings individually, RenoDX inline and actual games are not claimed.

Two test failures were investigated: large-frame synchronous file writes crossed
the product's intended 250 ms idle threshold; outputs now accumulate in memory and
are written after the sequence, keeping the strict history assertion. The native
fallback test initially inspected the old serving network while the replacement
built; it now waits for build completion before asserting reset-image equality.
Neither fix suppresses a product error or weakens comparison criteria.

The first remote Release run on `f5086a38`
([38608015656](https://github.com/Overclockers/dlss-5-amd-project/actions/runs/38608015656))
passed host compilation, Mochizuki ABI and host CI, then failed the WARP capture
test with `person worker exited (0)`. The IPC fixture incorrectly exited after
five idle seconds, unlike the production worker's indefinite request/stop wait.
The fixture now follows the production lifetime, with a regression that waits
longer than five seconds before submitting frames. The capture subtest also uses
a controlled age/schedule clock so software execution speed cannot determine
freshness; IPC deadlines retain wall time. It still checks four bounded in-flight
captures, selection of the newest completed frame and explicit rejection after
251 ms. Production code and the 250 ms threshold are unchanged. The PR records
the subsequent Release result and commit SHA.

The fixture reports INT4 image difference from the default network, network query
samples and execution/readback wall time. These are synthetic comparisons without
a quality ground truth, not game FPS or steady-state throughput claims. RenoDX's
GPU query covers the last network pass; total stage timing remains deferred.
Local toolchain is MSVC 14.51.36231; Actions keeps pinned 14.44.35207. User models
and driver caches are excluded from both cache restore paths and package checks.

One 256×256 FP16 synthetic run measured execution/readback wall means of 4.36 ms
(default), 3.66 ms (INT4), 12.21 ms (three-pass RenoDX/default) and 10.38 ms
(three-pass RenoDX/INT4), four observations per mode excluding network builds.
Native query samples were unavailable in this short run; RenoDX last-network
queries were 2.86/2.53 ms. Do not extrapolate these sparse fixture observations to
game performance. INT4/default mean absolute RGBA differences were 0.00337497
(FP16, maximum 0.01855469) and 0.00117840 (sRGB codes normalized to 0–1, maximum
0.01176471); they show the expected image change, not better or worse quality.

## Maintainer game checklist (not run here)

| Game | Route and acceptance |
|---|---|
| MH Wilds | Split; INT4/RenoDX off/on, HDR and exposure/white point, motion, saved preferences and normal exit |
| Forza Horizon 6 | Split, **no inline**; keep EXE-based early any-module proxy (restart) and enhanced barriers (live), verify no application-coverage regression |
| FF7RE | Split; retain Plan A format-switch conversion, verify format/resolution transitions and normal exit |
| Crimson Desert | Experimental inline, the only currently required inline game; verify queue stability, coverage/bypasses/timeouts and frame time |

For each game record default/INT4 image quality separately from network timing,
successful NR throughput and actual FPS. Preserve explicit compatibility settings.

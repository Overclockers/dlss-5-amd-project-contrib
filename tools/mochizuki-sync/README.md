# Mochizuki source pin and interface patches

The runtime uses official core/shaders at the full SHA in `manifest.json`, plus
six explicit feature patches. There are no whole-file preservation overlays.
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

The patches replay sequentially against `manifest.json`'s commit. Patches 02-05
share header/implementation context, so do not assume their patch files apply
independently to an arbitrary upstream checkout. For each upstream proposal,
apply its preceding series in a disposable checkout and reduce the proposal to
its own API requirements; do not send all local policy changes as one PR.

This six-patch series records the product snapshot; it is not the upstream PR
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

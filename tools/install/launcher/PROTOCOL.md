# Launcher worker protocol (schema 1)

JSON files only. The GUI starts Windows PowerShell 5.1 with `-File` and
absolute paths. It never builds `-Command` strings.

Exit codes match `status`: 0 success, 1 failed, 2 needsInput, 3 cancelled.

## Request (`-RequestJson`)

```json
{
  "schema": 1,
  "requestId": "uuid",
  "action": "inspect | install | update | uninstall | repairXess | restoreXess | keepXess",
  "gameExe": "C:\\\\absolute\\\\game.exe",
  "proxyChoice": "auto | dxgi.dll | winmm.dll | d3d12.dll | winhttp.dll | wininet.dll | dbghelp.dll",
  "backendChoice": "auto | daniel | lmxxf | mochizuki | all",
  "language": "zh | en",
  "explicitDecisions": {},
  "fingerprint": "optional inspect token; install/update refuse a stale folder"
}
```

`explicitDecisions` keys used by install/update:

- `existingInstall`: `uninstall` | `overwrite`
- `foreignProxy`: map of file name to `backup` | `keep` | `cancel`
- `existingIni`: `overwrite` | `keep`
- `mochizukiModels`: `both` | `small` | `original`; required when installing Mochizuki.
  The explicit `original` selection authorizes the older model. Missing or invalid
  sources still fail validation; there is no runtime-only installation fallback.
- `lmxxfModels`: `both` | `small` | `original`; required when installing lmxxf.
  Original and Small sources are resolved independently from the package and game.
  An explicit choice with missing/invalid weights fails before game-folder writes.
- `shortFileContinue`: `continue` | `abort`
- `modelSourceDll`: optional absolute path selected by the user for offline extraction.
  Inspect checks it without writing game files. Installation resolves it alongside
  package/game sources and validates the model's supported hashes before conversion.

Inspect never writes the game folder, never extracts weights, never downloads.
The worker passes backend arrays through the decision JSON file so Windows
PowerShell's native command line does not flatten the selected backend set.
`language` is the launcher's saved UI preference. The worker forwards it in decision
JSON for localized installer notices; omitted/unknown values use English. Raw system
and third-party diagnostics retain their original text.
The GUI also captures the worker's own stdout/stderr so startup/parse failures remain
available when no result JSON was produced. Script stderr is included in the event log.

`inspect` also returns `compatibility`: `groups` (`sr`, `dx11`, `fg`), each with
`ready`, `different`, and `files` (`name`, `gameVersion`, `packageVersion`,
`state=missing|same|different`); `active`, `restoreFiles`, `fingerprint`, and
historical `evidence`. An inspection failure returns `error` without blocking
normal NR installation. Version differences do not prove incompatibility.

`repairXess` requires `explicitDecisions.xessGroups` and the inspected
`compatibilityFingerprint`. It copies only selected known XeSS DLLs from the
selected complete package after saving all originals; `fg` always includes XeLL.
`restoreXess` needs the game EXE, valid stored backups and the preview's
`compatibilityFingerprint`, independent of NR backends and the original package.
The worker rejects changed previews and passes the checked transaction identity to
restoration. `keepXess` uses the same preview check and explicitly archives the old
record without changing any DLL or deleting backups, allowing a new repair after
game updates. An inspection error still returns recovery metadata when the stored
record is valid. Repair and restoration require the game to be closed and return
updated `compatibility`, including after a failed operation when possible.
The GUI uses the existing target mutex for both actions and previews the selected
files before dispatch. Original backups persist under `OptiScaler-compat-backup/`.
Restore refuses changed game files or damaged backups before modifying any target.
Commits hash and rename the same exclusively held file handle. Installing the staged
file never overwrites a concurrently created path; interrupted moves remain recoverable.

## Events (`-EventsJsonl`, append-only JSONL)

Launcher library records additionally retain `root`, optional `exe`, `customName`
and `customCover`. Manual entries use a stable `manual:<GUID>` ID. Display overrides
never change canonical game identity or recommendation matching. Folder-only entries
remain local until an executable is explicitly selected; worker write requests still
require `gameExe`. Per-game proxy overrides live in GUI `settings.json/ProxyChoices`.

```json
{"schema":1,"requestId":"...","stage":"inspect","message":"..."}
```

Stages: inspect, validate, prepare-model, backup, install, verify, done.
A stage without a known total uses indeterminate progress. Do not fake 1–99%.

## Result (`-ResultJson`, written last, atomic replace)

```json
{
  "schema": 1,
  "requestId": "...",
  "status": "success | failed | cancelled | needsInput",
  "effectiveGameDir": "...",
  "resolvedProxy": "dxgi.dll",
  "candidates": [{"name":"dxgi.dll","rank":1,"confidence":"high","reason":"..."}],
  "conflicts": [{"name":"dxgi.dll","kind":"foreign-proxy","detail":"..."}],
  "requiredDecisions": [{"id":"foreignProxy","file":"dxgi.dll","prompt":"...","options":["backup","keep","cancel"]}],
  "logPath": "...",
  "error": null
}
```

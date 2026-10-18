# Launcher worker protocol (schema 1)

JSON files only. The GUI starts Windows PowerShell 5.1 with `-File` and
absolute paths. It never builds `-Command` strings.

Exit codes match `status`: 0 success, 1 failed, 2 needsInput, 3 cancelled.

## Request (`-RequestJson`)

```json
{
  "schema": 1,
  "requestId": "uuid",
  "action": "inspect | install | update | uninstall",
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
- `shortFileContinue`: `continue` | `abort`

Inspect never writes the game folder, never extracts weights, never downloads.
The worker passes backend arrays through the decision JSON file so Windows
PowerShell's native command line does not flatten the selected backend set.
`language` is the launcher's saved UI preference. The worker forwards it in decision
JSON for localized installer notices; omitted/unknown values use English. Raw system
and third-party diagnostics retain their original text.

## Events (`-EventsJsonl`, append-only JSONL)

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

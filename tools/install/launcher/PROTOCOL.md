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
  "explicitDecisions": {},
  "fingerprint": "optional inspect token; install/update refuse a stale folder"
}
```

`explicitDecisions` keys used by install/update:

- `existingInstall`: `uninstall` | `overwrite`
- `foreignProxy`: map of file name to `backup` | `keep` | `cancel`
- `existingIni`: `overwrite` | `keep`
- `missingMochizukiModel`: `extract` | `skip`
- `shortFileContinue`: `continue` | `abort`

Inspect never writes the game folder, never extracts weights, never downloads.

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

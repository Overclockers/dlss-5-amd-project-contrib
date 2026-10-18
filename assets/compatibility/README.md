# Proxy recommendations

`proxy-profiles.json` is reviewed data, bundled and embedded in the launcher.
Proxy names describe the host's supported system aliases. Profile recommendations
are separate from the user's per-game choice and never modify game files.

Sources: [OptiScaler compatibility list](https://github.com/optiscaler/OptiScaler/wiki/Compatibility-List)
and the linked game details. The NTE detail is AsciiDoc, not Markdown. Reviewed on
2026-10-19. Notes are paraphrased with links and retain platform/version restrictions;
an upstream OptiScaler report does not certify this fork's NR implementation.

To add/update a record, inspect the game detail and preserve its source URL,
conditions, exact names/path segments and upstream version when supplied. Do not
infer an injection alias solely from an API import or a passing mention on a page.
Increase the numeric revision for each published data change. Never add commands,
downloadable DLLs or automatic INI edits to this file.

The launcher requests this reviewed JSON from this project's `main` branch once
per launch in the background, with a ten-second total deadline and a 256 KiB cap.
Until the file has been published there, a 404 simply leaves the bundled data in use.
Valid newer data is cached under the launcher's local application data directory.

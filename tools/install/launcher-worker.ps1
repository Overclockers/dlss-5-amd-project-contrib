# Schema 1 worker. Inspect is read-only. Install/uninstall call the existing scripts.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RequestJson,
    [Parameter(Mandatory = $true)][string]$EventsJsonl,
    [Parameter(Mandatory = $true)][string]$ResultJson
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
. (Join-Path $here 'proxy-probe.ps1')

function Write-Event([string]$stage, [string]$message) {
    $line = (@{ schema = 1; requestId = $script:rid; stage = $stage; message = $message } | ConvertTo-Json -Compress)
    [IO.File]::AppendAllText($EventsJsonl, $line + [Environment]::NewLine)
}
function Write-Result($obj) {
    $tmp = $ResultJson + '.tmp'
    [IO.File]::WriteAllText($tmp, ($obj | ConvertTo-Json -Depth 8))
    [IO.File]::Copy($tmp, $ResultJson, $true)
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
}
function New-Result([string]$status, [string]$error) {
    return @{
        schema = 1
        requestId = $script:rid
        status = $status
        effectiveGameDir = $script:gameDir
        resolvedProxy = $script:resolved
        candidates = $script:candidates
        conflicts = $script:conflicts
        requiredDecisions = $script:needed
        logPath = $EventsJsonl
        error = $error
        fingerprint = $script:fingerprint
        notes = $script:notes
        partial = $script:partial
    }
}
function Fail-Result([string]$status, [string]$error, [int]$code) {
    Write-Result (New-Result $status $error)
    exit $code
}
function Find-Sibling([string]$name) {
    $dirs = @(
        $here,
        (Split-Path $here -Parent),
        (Join-Path (Split-Path $here -Parent) 'tools'),
        (Join-Path $here '..\..\tools')
    )
    foreach ($dir in $dirs) {
        try {
            $p = [IO.Path]::GetFullPath((Join-Path $dir $name))
            if (Test-Path -LiteralPath $p -PathType Leaf) { return $p }
        } catch { }
    }
    return $null
}

if (-not [IO.Path]::IsPathRooted($RequestJson) -or -not [IO.Path]::IsPathRooted($EventsJsonl) -or -not [IO.Path]::IsPathRooted($ResultJson)) {
    throw 'RequestJson, EventsJsonl and ResultJson must be absolute paths'
}
$req = Get-Content -LiteralPath $RequestJson -Raw -Encoding UTF8 | ConvertFrom-Json
if ([int]$req.schema -ne 1) { throw "unsupported schema $($req.schema)" }
$script:rid = [string]$req.requestId
$script:gameDir = ''
$script:resolved = $null
$script:candidates = @()
$script:conflicts = @()
$script:needed = @()
$script:notes = @()
$script:fingerprint = $null
$script:partial = $false
$allowedProxy = @('dxgi.dll','winmm.dll','d3d12.dll','winhttp.dll','wininet.dll','dbghelp.dll')
$allowedAction = @('inspect','install','update','uninstall')
$allowedBackend = @('auto','daniel','lmxxf','mochizuki','all')
if ($allowedAction -notcontains [string]$req.action) { throw "invalid action $($req.action)" }
function Resolve-XboxGamePath([string]$exe) {
    if ($exe -notmatch '(?i)\\WindowsApps\\') { return $exe }
    $name = [IO.Path]::GetFileName($exe)
    if (-not $name) { return $exe }
    $hits = New-Object System.Collections.Generic.List[string]
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        if (-not $drive.IsReady) { continue }
        $root = Join-Path $drive.Name 'XboxGames'
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $stack = New-Object System.Collections.Generic.Stack[object]
        $stack.Push(@{ Path = $root; Depth = 0 })
        while ($stack.Count -gt 0 -and $hits.Count -lt 32) {
            $cur = $stack.Pop()
            if ($cur.Depth -gt 8) { continue }
            $entries = @()
            try { $entries = [IO.Directory]::GetFileSystemEntries($cur.Path) } catch { continue }
            foreach ($entry in $entries) {
                $leaf = [IO.Path]::GetFileName($entry)
                $isDir = $false
                $reparse = $false
                try {
                    $attr = [IO.File]::GetAttributes($entry)
                    $isDir = [bool]($attr -band [IO.FileAttributes]::Directory)
                    $reparse = [bool]($attr -band [IO.FileAttributes]::ReparsePoint)
                } catch {
                    if ([string]::Equals($leaf, $name, [StringComparison]::OrdinalIgnoreCase)) {
                        [void]$hits.Add($entry)
                    }
                    continue
                }
                if ($reparse) { continue }
                if ($isDir) {
                    $stack.Push(@{ Path = $entry; Depth = $cur.Depth + 1 })
                    continue
                }
                if ([string]::Equals($leaf, $name, [StringComparison]::OrdinalIgnoreCase)) {
                    [void]$hits.Add($entry)
                }
            }
        }
    }
    if ($hits.Count -eq 0) { return $exe }
    return @($hits | Sort-Object {
        if ($_ -match '(?i)\\WinGDK\\') { 0 } elseif ($_ -match '(?i)\\Win64\\') { 1 } elseif ($_ -match '(?i)\\Binaries\\') { 2 } else { 3 }
    }, { $_.Length })[0]
}

$exe = [string]$req.gameExe
if (-not $exe -or -not [IO.Path]::IsPathRooted($exe)) { Fail-Result 'failed' 'gameExe must be an absolute path' 1 }
$exe = Resolve-XboxGamePath $exe
try {
    $item = Get-Item -LiteralPath $exe -Force -ErrorAction Stop
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail-Result 'failed' 'gameExe cannot be a reparse point' 1 }
} catch [UnauthorizedAccessException] {
    Write-Event 'inspect' 'selected exe is protected; using its folder'
} catch {
    $parent = [IO.Path]::GetDirectoryName($exe)
    if (-not $parent -or -not (Test-Path -LiteralPath $parent -PathType Container)) {
        Fail-Result 'failed' 'gameExe not found' 1
    }
    Write-Event 'inspect' 'selected exe metadata unavailable; using its folder'
}

Write-Event 'inspect' 'reading PE imports and local dependencies'
$probe = Get-ProxyRecommendation $exe
$script:gameDir = [string]$probe.gameDir
$script:candidates = @($probe.candidates)
$script:conflicts = @($probe.conflicts)
$script:fingerprint = $probe.fingerprint
$script:partial = [bool]$probe.partial
$choice = [string]$req.proxyChoice
if (-not $choice -or $choice -eq 'auto') {
    $script:resolved = $probe.candidates[0].name
    if ($probe.candidates[0].confidence -eq 'low') {
        $script:notes += 'No strong import evidence. Manual proxy selection is recommended.'
    }
} elseif ($allowedProxy -contains $choice) {
    $script:resolved = $choice
} else {
    Fail-Result 'failed' 'invalid proxyChoice' 1
}
$backend = [string]$req.backendChoice
if ($backend -and $allowedBackend -notcontains $backend) { Fail-Result 'failed' 'invalid backendChoice' 1 }

$pkg = $null
foreach ($c in @((Find-Sibling 'Setup.ps1'), (Find-Sibling 'install-amd-presr.ps1'))) {
    if ($c) { $pkg = Split-Path $c -Parent; break }
}
if ($pkg) {
    $pythonHelper = Join-Path $pkg 'mochizuki-python.ps1'
    if (-not (Test-Path $pythonHelper)) { $pythonHelper = Join-Path $here 'mochizuki-python.ps1' }
    $hasModel = Test-Path -LiteralPath (Join-Path $pkg 'dlssnr-amd\dlssnr.bin')
    $hasNv = Test-Path -LiteralPath (Join-Path $pkg 'nvngx_dlssnr.dll')
    $script:notes += ('Package model file present: {0}. That is not a GPU compatibility test.' -f $hasModel)
    $script:notes += ('NVIDIA DLL for optional extraction present: {0}.' -f $hasNv)
}

if ($req.action -eq 'inspect') {
    if ($probe.hint) { $script:notes += $probe.hint }
    Write-Event 'done' 'inspect complete'
    Fail-Result 'success' $null 0
}

if ($req.action -in @('install','update') -and $req.fingerprint -and $req.fingerprint -ne $script:fingerprint) {
    $script:needed += @{ id = 'reinspect'; prompt = 'Game folder files changed since the last inspect. Inspect again.'; options = @('ok') }
    Fail-Result 'needsInput' 'stale inspect fingerprint' 2
}

function Find-Setup([string]$name) { Find-Sibling $name }
$install = Find-Setup 'install-amd-presr.ps1'
if (-not $install) { $install = Find-Setup 'Setup.ps1' }
$uninstall = Find-Setup 'uninstall-amd-presr.ps1'
if (-not $uninstall) { $uninstall = Find-Setup 'Uninstall_OptiScaler_NR.ps1' }
if ($req.action -eq 'uninstall') { $install = $uninstall }
if (-not $install) {
    Fail-Result 'failed' 'Complete package required: setup scripts are missing. Unzip the full package again.' 1
}

function ConvertTo-ForeignProxyMap($raw) {
    $map = @{}
    if ($null -eq $raw) { return $map }
    $items = @($raw)
    if ($items.Count -gt 0 -and $null -ne $items[0].PSObject.Properties['Key']) {
        foreach ($row in $items) {
            if ($row.Key) { $map[[string]$row.Key] = [string]$row.Value }
        }
        return $map
    }
    foreach ($p in $raw.PSObject.Properties) {
        $map[[string]$p.Name] = [string]$p.Value
    }
    return $map
}

$dec = $req.explicitDecisions
if ($req.action -ne 'uninstall') {
    if ($probe.conflicts.Count -gt 0) {
        $map = @{}
        if ($dec -and $dec.foreignProxy) { $map = ConvertTo-ForeignProxyMap $dec.foreignProxy }
        foreach ($c in $probe.conflicts) {
            $name = $c.name
            $has = $map.ContainsKey($name) -or $map.ContainsKey($name.ToLowerInvariant())
            if (-not $has) {
                $script:needed += @{
                    id = 'foreignProxy'
                    file = $name
                    prompt = "$name exists and is not this project. Backup, keep, or cancel?"
                    options = @('backup','keep','cancel')
                }
            }
        }
    }
    if ($script:needed.Count -gt 0) {
        Write-Event 'validate' 'waiting for explicit decisions'
        Fail-Result 'needsInput' 'missing decisions' 2
    }
}

Write-Event 'validate' 're-checking paths and locks'
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$stdoutLog = Join-Path ([IO.Path]::GetDirectoryName($ResultJson)) 'setup.stdout.log'
$stderrLog = Join-Path ([IO.Path]::GetDirectoryName($ResultJson)) 'setup.stderr.log'
$dj = $null
if ($req.action -eq 'uninstall') {
    Write-Event 'install' 'starting uninstall'
} else {
    $dj = Join-Path ([IO.Path]::GetDirectoryName($ResultJson)) 'decisions.json'
    $payload = @{
        existingInstall = 'overwrite'
        existingIni = 'keep'
        missingMochizukiModel = 'skip'
        shortFileContinue = 'abort'
        optiPatcher = 'skip'
    }
    if ($dec) {
        foreach ($name in @('existingInstall','existingIni','missingMochizukiModel','shortFileContinue','optiPatcher','foreignProxy')) {
            if ($dec.PSObject.Properties[$name] -and $null -ne $dec.$name) {
                if ($name -eq 'foreignProxy') { $payload[$name] = ConvertTo-ForeignProxyMap $dec.$name }
                else { $payload[$name] = $dec.$name }
            }
        }
    }
    ($payload | ConvertTo-Json -Depth 6) | Set-Content -LiteralPath $dj -Encoding UTF8
    Write-Event 'install' "starting $($req.action)"
}
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
if ($req.action -eq 'uninstall') {
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $install -GameDir $script:gameDir -NonInteractive -NoPause 1>$stdoutLog 2>$stderrLog
} elseif ($backend -and $backend -ne 'auto') {
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $install -GameDir $script:gameDir -Proxy $script:resolved -NoPause -NonInteractive -Backend $backend -DecisionJson $dj 1>$stdoutLog 2>$stderrLog
} else {
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $install -GameDir $script:gameDir -Proxy $script:resolved -NoPause -NonInteractive -DecisionJson $dj 1>$stdoutLog 2>$stderrLog
}
$code = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if (Test-Path -LiteralPath $stdoutLog) {
    Get-Content -LiteralPath $stdoutLog -ErrorAction SilentlyContinue | ForEach-Object { Write-Event 'log' $_ }
}
if ($code -eq 0) {
    Write-Event 'verify' 'setup process exit 0'
    Write-Event 'done' "$($req.action) complete"
    Fail-Result 'success' $null 0
}
Write-Event 'done' "$($req.action) failed ($code)"
$tail = ''
if (Test-Path -LiteralPath $stderrLog) { $tail = (Get-Content -LiteralPath $stderrLog -Raw -ErrorAction SilentlyContinue) }
Fail-Result 'failed' ("setup exit $code $tail").Trim() 1

# No GPU or external DLL execution: exercise repair/restore against synthetic PE files.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo 'tools/install/xess-compat.ps1')
$fixture = Join-Path $repo ('exports/tests/xess-compat/' + [Guid]::NewGuid().ToString('N'))
$game = Join-Path $fixture 'game space'
$package = Join-Path $fixture 'package'
$deps = Join-Path $package 'OptiScaler'
New-Item -ItemType Directory -Path $game,$deps -Force | Out-Null
$script:checks = 0
function Check([bool]$Condition, [string]$Why) { if (-not $Condition) { throw $Why }; $script:checks++ }
function Pe([string]$Path, [byte]$Marker, [uint16]$Machine = 0x8664) {
    $bytes = New-Object byte[] 512
    $bytes[0] = 0x4d; $bytes[1] = 0x5a; $bytes[60] = 64
    $bytes[64] = 0x50; $bytes[65] = 0x45
    [BitConverter]::GetBytes($Machine).CopyTo($bytes,68)
    $bytes[88] = 0x0b; $bytes[89] = 0x02; $bytes[511] = $Marker
    [IO.File]::WriteAllBytes($Path, $bytes)
}
function Reject([scriptblock]$Action, [string]$Match) {
    $caught = $false
    try { & $Action } catch { $caught = $true; Check ($_.Exception.Message -match $Match) ("Wrong error: " + $_.Exception.Message) }
    Check $caught "Expected rejection: $Match"
}
function Report { Get-XessCompatibility $game $package }
function Repair([string[]]$Groups) { Repair-XessCompatibility $game $package $Groups (Report).fingerprint }

foreach ($name in @('libxess.dll','libxess_dx11.dll','libxess_fg.dll','libxell.dll')) { Pe (Join-Path $deps $name) 2 }
Pe (Join-Path $game 'Game.exe') 0
Pe (Join-Path $game 'libxess.dll') 1
Pe (Join-Path $game 'libxess_fg.dll') 1
[IO.File]::WriteAllText((Join-Path $game 'OptiScaler.ini'), '[Libraries] custom preferences')
[IO.File]::WriteAllText((Join-Path $game 'unrelated.dll'), 'keep this')
$oldSr = Get-XessHash (Join-Path $game 'libxess.dll')
$oldFg = Get-XessHash (Join-Path $game 'libxess_fg.dll')
$before = @(Get-ChildItem -LiteralPath $game -Recurse -File | ForEach-Object { $_.FullName + ':' + (Get-XessHash $_.FullName) }) -join '|'
$report = Report
Check ($report.groups.Count -eq 3) 'Wrong component grouping'
Check ($report.groups[2].files.Count -eq 2) 'XeFG/XeLL not paired'
Check $report.groups[2].ready 'Complete XeFG group not ready'
Check ($report.groups[0].files[0].state -eq 'different') 'Did not detect different content with unknown versions'
Check ($report.groups[1].files[0].state -eq 'missing') 'Did not detect absent game DLL'
$after = @(Get-ChildItem -LiteralPath $game -Recurse -File | ForEach-Object { $_.FullName + ':' + (Get-XessHash $_.FullName) }) -join '|'
Check ($before -eq $after) 'Inspection wrote to the game folder'

# Explicit choices and stale previews must fail before any backup/game writes.
Reject { Repair @() } 'Select XeSS'
Reject { Repair @('unknown') } 'Unknown XeSS'
Reject { Repair @('fg','fg') } 'Select XeSS'
$stale = $report.fingerprint
Pe (Join-Path $deps 'libxess.dll') 3
Reject { Repair-XessCompatibility $game $package @('sr') $stale } 'Files changed'
Check (-not (Test-Path (Join-Path $game 'OptiScaler-compat-backup'))) 'Rejected preview created backups'

# Pair completeness and architecture validation; version age alone never triggers writes.
$xell = Join-Path $deps 'libxell.dll'
Remove-Item -LiteralPath $xell
Reject { Repair @('fg') } 'missing or invalid'
Pe $xell 2 0x14c
Reject { Repair @('fg') } 'missing or invalid'
Pe $xell 2
Check ((Get-XessHash (Join-Path $game 'libxess_fg.dll')) -eq $oldFg) 'Rejected pair changed game FG'

# All-target lock preflight avoids a partial pair replacement.
$locked = [IO.File]::Open((Join-Path $game 'libxess_fg.dll'), 'Open', 'Read', 'Read')
try { Reject { Repair @('fg') } 'being used|used by another|另一个进程|正由另一' }
finally { $locked.Dispose() }
Check (-not (Test-Path (Join-Path $game 'libxell.dll'))) 'Locked group installed XeLL anyway'

# Replace a complete group; leave SR, INI, unrelated files and the inner package intact.
Repair @('fg')
Check (Report).active 'Repair did not retain an active journal'
Check ((Report).restoreFiles.Count -eq 2) 'Restore preview does not match actual files'
foreach ($name in @('libxess_fg.dll','libxell.dll')) {
    Check ((Get-XessHash (Join-Path $game $name)) -eq (Get-XessHash (Join-Path $deps $name))) "Repair mismatch $name"
    Check (Test-Path -LiteralPath (Join-Path $deps $name)) 'Package source was moved'
}
Check ((Get-XessHash (Join-Path $game 'libxess.dll')) -eq $oldSr) 'Unselected SR was changed'
Check ((Get-Content -LiteralPath (Join-Path $game 'OptiScaler.ini') -Raw) -eq '[Libraries] custom preferences') 'INI preferences changed'
Reject { Repair @('sr') } 'Restore the previous'
Restore-XessCompatibility $game
Check ((Get-XessHash (Join-Path $game 'libxess_fg.dll')) -eq $oldFg) 'Original FG not restored'
Check (-not (Test-Path (Join-Path $game 'libxell.dll'))) 'Added XeLL not removed'
Check (-not (Report).active) 'Restore left active state'
Check ((Get-Content -LiteralPath (Join-Path $game 'unrelated.dll') -Raw) -eq 'keep this') 'Unrelated DLL changed'

# Game updates take priority. Preflight checks every target before restoring any of them.
Repair @('sr','fg')
Pe (Join-Path $game 'libxell.dll') 7
$appliedSr = Get-XessHash (Join-Path $game 'libxess.dll')
Reject { Restore-XessCompatibility $game } 'changed after repair'
Check ((Get-XessHash (Join-Path $game 'libxess.dll')) -eq $appliedSr) 'Restore changed SR before discovering updated XeLL'
Check (Report).active 'Failed restore discarded backups'
Pe (Join-Path $game 'libxell.dll') 2

# Corrupt backup is rejected before any target changes.
$journal = Get-XessJournal $game
$backup = Join-Path $game ('OptiScaler-compat-backup/' + $journal.transaction)
$saved = [IO.File]::ReadAllBytes((Join-Path $backup 'libxess.dll'))
Pe (Join-Path $backup 'libxess.dll') 9
Reject { Restore-XessCompatibility $game } 'backup is damaged'
Check ((Get-XessHash (Join-Path $game 'libxess.dll')) -eq $appliedSr) 'Corrupt backup touched target'
[IO.File]::WriteAllBytes((Join-Path $backup 'libxess.dll'), $saved)

# An interrupted atomic repair can contain a mix of originals and applied files.
$journal.state = 'pending'
Write-XessJournal $game $journal
[IO.File]::Copy((Join-Path $backup 'libxess.dll'), (Join-Path $game 'libxess.dll'), $true)
Restore-XessCompatibility $game
Check ((Get-XessHash (Join-Path $game 'libxess.dll')) -eq $oldSr) 'Interrupted repair could not restore originals'
Check (-not (Test-Path (Join-Path $game 'libxell.dll'))) 'Interrupted repair retained an added DLL'
Check (Test-Path (Join-Path $backup 'libxess.dll')) 'Original backups were discarded'

# Source mutation after confirmation: roll back the whole group, retain original backups.
$atomic = (Get-Command Set-XessFileAtomic).ScriptBlock
$script:copies = 0
function Set-XessFileAtomic([string]$Source, [string]$Target, [string]$ExpectedHash) {
    $script:copies++
    if ($script:copies -eq 2) { throw 'Injected second-copy failure' }
    & $atomic $Source $Target $ExpectedHash
}
Reject { Repair @('fg') } 'Original game files restored'
Check ((Get-XessHash (Join-Path $game 'libxess_fg.dll')) -eq $oldFg) 'Failed pair did not roll back first DLL'
Check (-not (Test-Path (Join-Path $game 'libxell.dll'))) 'Failed pair retained new XeLL'
Set-Item -Path Function:Set-XessFileAtomic -Value $atomic

# Tampered manifests cannot escape the controlled filename list.
Repair @('sr')
$journal = Get-XessJournal $game
$journal.files[0].name = '../unrelated.dll'
Write-XessJournal $game $journal
Reject { Restore-XessCompatibility $game } 'Invalid XeSS repair file entry'
$journal.files[0].name = 'libxess.dll'
Write-XessJournal $game $journal
Restore-XessCompatibility $game

# A junction in any source/target/backup path must not be followed.
$outside = Join-Path $fixture 'outside'
New-Item -ItemType Directory -Path $outside | Out-Null
$linkedGame = Join-Path $fixture 'linked-game'
New-Item -ItemType Junction -Path $linkedGame -Target $outside | Out-Null
Reject { Get-XessCompatibility $linkedGame $package } 'Linked paths'

# Real worker routing: no proxy/backend/model choices required, no install script executed.
Pe (Join-Path $package 'OptiScaler.dll') 0
[IO.File]::WriteAllText((Join-Path $package 'Setup.ps1'), "throw 'Installer must not run for XeSS repair'")
$worker = Join-Path $repo 'tools/install/launcher-worker.ps1'
$ps51 = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
function Worker([string]$Action, [string[]]$Groups = @()) {
    $id = [Guid]::NewGuid().ToString('N')
    $req = Join-Path $fixture "$id-request.json"
    $result = Join-Path $fixture "$id-result.json"
    $events = Join-Path $fixture "$id-events.jsonl"
    $payload = @{ schema=1; requestId=$id; action=$Action; gameExe=(Join-Path $game 'Game.exe'); packageRoot=$package;
        compatibilityFingerprint=(Report).fingerprint; explicitDecisions=@{ xessGroups=$Groups } }
    [IO.File]::WriteAllText($req, ($payload | ConvertTo-Json -Depth 5))
    & $ps51 -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $worker -RequestJson $req -ResultJson $result -EventsJsonl $events
    Check ($LASTEXITCODE -eq 0) "Worker $Action failed: $(Get-Content -LiteralPath $result -Raw -Encoding UTF8)"
    return (Get-Content -LiteralPath $result -Raw -Encoding UTF8 | ConvertFrom-Json)
}
$result = Worker 'inspect'
Check ($result.compatibility.groups.Count -eq 3) 'Worker inspection omitted XeSS inventory'
$result = Worker 'repairXess' @('fg')
Check $result.compatibility.active 'Worker repair failed to report backup'
$result = Worker 'restoreXess'
Check (-not $result.compatibility.active) 'Worker restore failed'
Write-Host "xess compatibility: PASS ($script:checks checks; fixtures $fixture)"

# Real WPF controller, isolated preferences, no windows shown or workers launched.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $repo 'exports/tests/launcher-state'
& (Join-Path $repo 'tools/build/build-launcher.ps1') -OutDir $out
Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Xaml
$assembly = [Reflection.Assembly]::LoadFrom((Join-Path $out 'OptScalerNR.Launcher.exe'))
$controllerType = $assembly.GetType('OptScalerNR.Launcher.MainController', $true)
$flags = [Reflection.BindingFlags]'NonPublic,Instance'
$arguments = New-Object object[] 2
$arguments[0] = [string[]]@()
$arguments[1] = [string](Join-Path $out ('preferences-' + [Guid]::NewGuid().ToString('N')))
$window = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $arguments)
$controller = $window.Tag
function Field([string]$name) { $controllerType.GetField($name, $flags).GetValue($controller) }
function Call([string]$name, [object[]]$values = @()) {
    for ($i = 0; $i -lt $values.Length; $i++) {
        if ($null -ne $values[$i]) { $values[$i] = $values[$i].PSObject.BaseObject }
    }
    $controllerType.GetMethod($name, $flags).Invoke($controller, $values)
}
function Check([bool]$condition, [string]$why) { if (-not $condition) { throw $why } }
try {
    $backendType = $assembly.GetType('OptScalerNR.Launcher.BackendInfo', $true)
    $resultType = $assembly.GetType('OptScalerNR.Launcher.LauncherResult', $true)
    $capabilities = [Activator]::CreateInstance($resultType)
    $capabilities.Backends = [Array]::CreateInstance($backendType, 3)
    $ids = @('lmxxf', 'daniel', 'mochizuki')
    for ($i = 0; $i -lt $ids.Count; $i++) {
        $backend = [Activator]::CreateInstance($backendType)
        $backend.Id = $ids[$i]; $backend.Enabled = $true; $backend.ReasonCode = 'ready'
        $capabilities.Backends[$i] = $backend
    }
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'mochizuki') 'Fresh default does not match the first ready backend card'
    Check ($window.FindName('MochiActiveLabel').Visibility -eq 'Visible') 'Default active marker is on the wrong card'
    Call 'SetActiveBackend' @('lmxxf') | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'lmxxf') 'Capability refresh discarded the selected active backend'
    Call 'SetActiveBackend' @('daniel') | Out-Null
    $capabilities.Backends[1].Enabled = $false
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'mochizuki') 'Fallback does not match the visible backend order'
    $capabilities.Backends[2].Enabled = $false
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'lmxxf') 'Default did not fall back when Mochi is unavailable'

    $gameType = $assembly.GetType('OptScalerNR.Launcher.GameInfo', $true)
    $a = [Activator]::CreateInstance($gameType)
    $a.Id = 'A'; $a.Name = 'A'; $a.Exe = ''
    $b = [Activator]::CreateInstance($gameType)
    $b.Id = 'B'; $b.Name = 'B'; $b.Exe = ''
    Call 'SelectGame' @($a) | Out-Null
    (Field '_decisions').ForeignProxy['dxgi.dll'] = 'backup'
    Check (Call 'TryLockTarget' @([string]$arguments[1])) 'Cannot acquire isolated target lock'
    Call 'SetBusy' @($true, 'install') | Out-Null
    Check (-not (Field '_gameList').IsEnabled) 'Game list enabled during write'
    Call 'SelectGame' @($b) | Out-Null
    Check ((Field '_selectedGame').Id -eq 'A') 'Write target changed during installation'
    Check ((Field '_decisions').ForeignProxy['dxgi.dll'] -eq 'backup') 'In-flight decisions changed'
    # Also covers a worker that exited before its queued completion callback runs.
    $closing = New-Object System.ComponentModel.CancelEventArgs
    Call 'OnClosing' @($window, $closing) | Out-Null
    Check $closing.Cancel 'Window closed before write completion'
    Check ($null -ne (Field '_targetMutex')) 'Closing released the live target lock'
    Call 'SetBusy' @($false, '') | Out-Null
    Call 'SelectGame' @($b) | Out-Null
    Check ((Field '_selectedGame').Id -eq 'B') 'Game selection did not resume'
    Check ((Field '_decisions').ForeignProxy.Count -eq 0) 'Foreign proxy consent leaked to the next game'
    $closing = New-Object System.ComponentModel.CancelEventArgs
    Call 'OnClosing' @($window, $closing) | Out-Null
    Check (-not $closing.Cancel) 'Window cannot close after completion'
    Check ($null -eq (Field '_targetMutex')) 'Completed target lock was retained'
    Write-Host 'launcher state: PASS'
} finally {
    Call 'SetBusy' @($false, '') | Out-Null
    Call 'ReleaseLock' | Out-Null
    $window.Close()
}

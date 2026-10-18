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
function Dialog-Body($node) {
    if ($node -is [Windows.Controls.StackPanel] -and @($node.Children | Where-Object { $_ -is [Windows.Controls.TextBlock] }).Count) { return $node }
    if ($node -is [Windows.DependencyObject]) {
        foreach ($child in [Windows.LogicalTreeHelper]::GetChildren($node)) {
            $found = Dialog-Body $child
            if ($null -ne $found) { return $found }
        }
    }
    return $null
}
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
    foreach ($backend in $capabilities.Backends) { $backend.Enabled = $true }
    Call 'SelectGame' @($a) | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    $window.FindName('DanielCheck').IsChecked = $false
    Call 'SetActiveBackend' @('lmxxf') | Out-Null
    Call 'SelectGame' @($b) | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'mochizuki') 'Backend choice leaked to another game'
    Call 'SelectGame' @($a) | Out-Null
    $capabilities.Backends[0].Enabled = $false
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Field '_activeBackend') -eq 'lmxxf') 'Missing files silently changed the chosen backend'
    Check ($window.FindName('LmxxfCheck').IsChecked -eq $true) 'Inspection discarded the selected missing backend'
    Check ($window.FindName('DanielCheck').IsChecked -eq $false) 'Inspection reselected an unchecked backend'
    $reopened = $controllerType.GetMethod('Create', [Reflection.BindingFlags]'NonPublic,Static').Invoke($null, $arguments)
    try {
        $controllerType.GetMethod('SelectGame', $flags).Invoke($reopened.Tag, @($a)) | Out-Null
        Check ($controllerType.GetField('_activeBackend', $flags).GetValue($reopened.Tag) -eq 'lmxxf') 'Restart lost the active backend'
        Check ($reopened.FindName('DanielCheck').IsChecked -eq $false) 'Restart lost the backend checkboxes'
    } finally { $reopened.Close() }
    Call 'SelectGame' @($b) | Out-Null
    Call 'ApplyInspect' @($capabilities) | Out-Null
    foreach ($name in @('MochiCheck', 'LmxxfCheck', 'DanielCheck')) { $window.FindName($name).IsChecked = $false }
    Call 'ApplyInspect' @($capabilities) | Out-Null
    Check ((Call 'CheckedBackends').Count -eq 0) 'Inspection ignored the deliberate empty backend selection'
    Check ((Field '_activeBackend') -eq '') 'Empty selection retained a misleading active marker'

    # Compatibility repair is independent of backend/model/proxy installation choices.
    Call 'SelectGame' @($a) | Out-Null
    $jsonType = $assembly.GetType('OptScalerNR.Launcher.JsonUtil', $true)
    $read = $jsonType.GetMethod('Read').MakeGenericMethod($resultType)
    $compat = $read.Invoke($null, @('{"schema":1,"compatibility":{"groups":[{"id":"fg","ready":true,"different":true,"files":[{"name":"libxess_fg.dll","state":"different","gameVersion":"1.0","packageVersion":"3.0"},{"name":"libxell.dll","state":"missing","packageVersion":"2.0"}]}],"active":false,"fingerprint":"token","evidence":[]}}'))
    Call 'ApplyInspect' @($compat) | Out-Null
    Check $window.FindName('CompatButton').IsEnabled 'Repair requires an installed backend or selected proxy'
    Check (-not $window.FindName('RestoreCompatButton').IsEnabled) 'Restore enabled without backups'
    Check ($window.FindName('CompatSummary').Text.Contains('libxess_fg.dll')) 'Automatic comparison summary hidden'
    $dialog = Call 'CreateCompatibilityDialog' @($false)
    try {
        $panel = Dialog-Body $dialog
        $checks = @($panel.Children | Where-Object { $_ -is [Windows.Controls.CheckBox] })
        Check ($checks.Count -eq 1) 'Paired XeFG/XeLL exposed as separate choices'
        Check ($checks[0].IsChecked -ne $true) 'Repair was selected without a user choice'
        $buttons = $panel.Children[$panel.Children.Count - 1]
        Check (-not $buttons.Children[1].IsEnabled) 'Repair allowed without component selection'
        $checks[0].IsChecked = $true
        Check $buttons.Children[1].IsEnabled 'Selecting a group did not enable repair'
        $size = New-Object Windows.Size 650,650
        $dialog.Content.Measure($size)
        $dialog.Content.Arrange((New-Object Windows.Rect $size))
        $dialog.Content.UpdateLayout()
        $bitmap = New-Object Windows.Media.Imaging.RenderTargetBitmap 650,650,96,96,([Windows.Media.PixelFormats]::Pbgra32)
        $bitmap.Render($dialog.Content)
        $encoder = New-Object Windows.Media.Imaging.PngBitmapEncoder
        $encoder.Frames.Add([Windows.Media.Imaging.BitmapFrame]::Create($bitmap))
        $stream = [IO.File]::Create((Join-Path $out 'xess-repair-dialog.png'))
        try { $encoder.Save($stream) } finally { $stream.Dispose() }
    } finally { $dialog.Close() }
    $compat.Compatibility.Active = $true
    $compat.Compatibility.RestoreFiles = [string[]]@('libxess_fg.dll','libxell.dll')
    Call 'ApplyInspect' @($compat) | Out-Null
    Check $window.FindName('RestoreCompatButton').IsEnabled 'Restoration hidden after repair'
    $dialog = Call 'CreateCompatibilityDialog' @($true)
    try {
        $texts = @((Dialog-Body $dialog).Children | Where-Object { $_ -is [Windows.Controls.TextBlock] } | ForEach-Object { $_.Text }) -join '|'
        Check ($texts.Contains('libxess_fg.dll') -and $texts.Contains('libxell.dll')) 'Restore preview omitted actual files'
        Check (-not $texts.Contains('libxess_dx11.dll')) 'Restore preview claims unrelated files'
    } finally { $dialog.Close() }
    Call 'SetBusy' @($true, 'repairXess') | Out-Null
    Call 'UpdatePrimaryButtonState' | Out-Null
    Check (-not $window.FindName('CompatButton').IsEnabled) 'Repair button enabled during writes'
    Check (-not $window.FindName('RestoreCompatButton').IsEnabled) 'Restore button enabled during writes'
    Call 'SetBusy' @($false, '') | Out-Null
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

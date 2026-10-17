# Compile OptScalerNR.Launcher.exe with the machine Framework 4 WPF assemblies.
# Output: exports/launcher/ (never the repo root).
[CmdletBinding()]
param(
    [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root 'tools/install/launcher'
if (-not $OutDir) { $OutDir = Join-Path $root 'exports/launcher' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$fw = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Join-Path $fw 'csc.exe'
$wpf = Join-Path $fw 'WPF'
foreach ($need in @($csc, (Join-Path $wpf 'PresentationFramework.dll'))) {
    if (-not (Test-Path -LiteralPath $need)) { throw "Missing compiler dependency: $need" }
}

$exe = Join-Path $OutDir 'OptScalerNR.Launcher.exe'
$obj = Join-Path $OutDir 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$cs = @(
    'Program.cs', 'MainWindow.cs', 'Protocol.cs', 'WorkerClient.cs', 'GamePaths.cs'
) | ForEach-Object { Join-Path $src $_ }
$refs = @(
    (Join-Path $fw 'mscorlib.dll'),
    (Join-Path $fw 'System.dll'),
    (Join-Path $fw 'System.Core.dll'),
    (Join-Path $fw 'System.Runtime.Serialization.dll'),
    (Join-Path $fw 'System.Xaml.dll'),
    (Join-Path $wpf 'WindowsBase.dll'),
    (Join-Path $wpf 'PresentationCore.dll'),
    (Join-Path $wpf 'PresentationFramework.dll'),
    (Join-Path $fw 'System.Drawing.dll')
)
$win32 = Join-Path $fw 'Microsoft.Build.Tasks.v4.0.dll'
$res = @(
    "/resource:`"$(Join-Path $src 'App.xaml')`",OptScalerNR.Launcher.App.xaml",
    "/resource:`"$(Join-Path $src 'MainWindow.xaml')`",OptScalerNR.Launcher.MainWindow.xaml"
)
$argFile = Join-Path $obj 'csc.rsp'
$lines = @(
    '/nologo',
    '/t:winexe',
    '/platform:x64',
    '/optimize+',
    '/debug:pdbonly',
    "/out:`"$exe`"",
    "/win32manifest:`"$(Join-Path $src 'app.manifest')`"",
    "/define:TRACE"
)
$lines += $refs | ForEach-Object { "/r:`"$_`"" }
$lines += $res
$lines += $cs | ForEach-Object { "`"$_`"" }
Set-Content -LiteralPath $argFile -Value $lines -Encoding ASCII
& $csc "@$argFile"
if ($LASTEXITCODE -ne 0) { throw "csc failed: $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $exe)) { throw "Launcher EXE was not produced" }
Copy-Item -LiteralPath (Join-Path $src 'mock-worker.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $src 'PROTOCOL.md') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/launcher-worker.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/proxy-probe.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/library-scan.ps1') -Destination $OutDir -Force
Copy-Item -LiteralPath (Join-Path $root 'tools/install/install-capabilities.ps1') -Destination $OutDir -Force
$srcFiles = Get-ChildItem -LiteralPath $src -File | Where-Object { $_.Extension -match '\.(cs|xaml|manifest|md|ps1)$' }
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/launcher-worker.ps1')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/proxy-probe.ps1')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/library-scan.ps1')
$srcFiles += Get-Item -LiteralPath (Join-Path $root 'tools/install/install-capabilities.ps1')
$sha = [Security.Cryptography.SHA256]::Create()
$concat = New-Object IO.MemoryStream
foreach ($f in ($srcFiles | Sort-Object FullName)) {
    $bytes = [IO.File]::ReadAllBytes($f.FullName)
    $concat.Write($bytes, 0, $bytes.Length)
}
$sourceHash = ([BitConverter]::ToString($sha.ComputeHash($concat.ToArray()))).Replace('-','').ToLowerInvariant()
function Get-Sha256File([string]$path) {
    $h = [Security.Cryptography.SHA256]::Create()
    try {
        $fs = [IO.File]::OpenRead($path)
        try { return ([BitConverter]::ToString($h.ComputeHash($fs))).Replace('-','').ToLowerInvariant() }
        finally { $fs.Dispose() }
    } finally { $h.Dispose() }
}
$refHash = @{}
foreach ($r in $refs) {
    $refHash[[IO.Path]::GetFileName($r)] = Get-Sha256File $r
}
$stamp = Join-Path $OutDir 'build.json'
@{
    exe = $exe
    length = (Get-Item -LiteralPath $exe).Length
    utc = [DateTime]::UtcNow.ToString('o')
    csc = $csc
    sourceHash = $sourceHash
    references = $refHash
    target = 'netfx4-wpf-x64'
} | ConvertTo-Json | Set-Content -LiteralPath $stamp -Encoding UTF8
Write-Host "LAUNCHER_OK $exe"

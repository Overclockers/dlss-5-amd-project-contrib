# Shared Original/Small weight validation and model-granular installation.
function Assert-LmxxfModelPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $at = $full
    while ($at) {
        if (Test-Path -LiteralPath $at) {
            if ([IO.File]::GetAttributes($at) -band [IO.FileAttributes]::ReparsePoint) { throw "Linked model path: $at" }
        }
        $at = [IO.Path]::GetDirectoryName($at)
    }
    return $full
}

function Get-LmxxfAssetHash([string]$Path) {
    $hash = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally { if ($stream) { $stream.Dispose() }; $hash.Dispose() }
}

function Get-LmxxfOriginalLayout {
    # Logical element counts match hip_reference::Network::WeightElements.
    $layout = @{}
    for ($b = 0; $b -lt 70; $b++) {
        $c = if ($b -le 4) {32} elseif ($b -le 8) {64} elseif ($b -le 14) {128} elseif ($b -le 22) {256} elseif ($b -le 30) {512} elseif ($b -le 38) {1024} elseif ($b -le 47) {512} elseif ($b -le 55) {256} elseif ($b -le 61) {128} elseif ($b -le 65) {64} else {32}
        if ($b -eq 39) { $layout['decoder39-weights'] = 524800; continue }
        if ($b -ge 31 -and $b -le 38) {
            foreach ($pair in @(@('expand',4194304),@('contract',4195328),@('qkv',3145760),@('projection',1049600))) { $layout["block$b-$($pair[0])"] = $pair[1] }
        } else {
            $layout["block$b-attention"] = if ($c -eq 32) {8225} else {4*$c*$c+($c/32)*4096+$c/32+$c}
            if ($c -eq 512) { $layout["block$b-ffwd"] = 524288; $layout["block$b-ffwd-projection"] = 262656 }
            else { $layout["block$b-ffn"] = if ($c -eq 32) {8736} else {9*$c*$c+$c} }
        }
        if ($b -in @(4,8,14,22)) { $layout["block$b-ds"] = 2*$c*$c }
        if ($b -in @(48,56,62,66)) { $layout["block$b-weights"] = 2*$c*$c+$c }
    }
    foreach ($pair in @(@('head-matrix',524288),@('post70-scales',64),@('post70-head',96),@('post70-ffn',8736),@('post70-attention',8225),@('post70-history-head',32))) { $layout[$pair[0]] = $pair[1] }
    return $layout
}

function Get-LmxxfModelFiles {
    param([string]$Directory, [ValidateSet('original','small')][string]$Model, [switch]$Quick)
    $root = Assert-LmxxfModelPath $Directory
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { throw "Missing lmxxf $Model weights: $root" }
    if ($Model -eq 'original') {
        $layout = Get-LmxxfOriginalLayout
        foreach ($name in @($layout.Keys | Sort-Object)) {
            $file = Join-Path $root ($name + '.f32'); $bytes = [long]$layout[$name]*4
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { $file = Join-Path $root ($name + '.f16'); $bytes = [long]$layout[$name]*2 }
            $file = Assert-LmxxfModelPath $file
            if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne $bytes) { throw "Missing or invalid lmxxf Original tensor: $file (expected $bytes bytes)" }
            [pscustomobject]@{ Name = [IO.Path]::GetFileName($file); Path = $file }
        }
        foreach ($name in @('noise.f32','normalized-output.f32')) {
            $file = Join-Path $root $name
            if (Test-Path -LiteralPath $file -PathType Leaf) { $null = Assert-LmxxfModelPath $file; [pscustomobject]@{Name=$name; Path=$file} }
        }
    } else {
        $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'lmxxf-small-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $metadataPath = Assert-LmxxfModelPath (Join-Path $root 'model-manifest.json')
        $text = Get-Content -LiteralPath $metadataPath -Raw -Encoding UTF8
        $metadata = $text | ConvertFrom-Json
        if ($metadata.model -ne 'small' -or $metadata.source_dll_sha256 -ne $manifest.source_dll_sha256 -or -not $text.Contains('"model": "small"')) { throw "Invalid lmxxf Small model manifest: $metadataPath" }
        foreach ($entry in $manifest.files.PSObject.Properties) {
            $file = Assert-LmxxfModelPath (Join-Path $root $entry.Name)
            if ([IO.Path]::GetExtension($file) -eq '.f16' -and (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($file,'.f32')))) { throw "Unverified f32 tensor shadows lmxxf Small: $file" }
            if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-Item -LiteralPath $file).Length -ne $entry.Value.bytes) { throw "Missing or invalid lmxxf Small tensor: $file" }
            if (-not $Quick -and (Get-LmxxfAssetHash $file) -ne $entry.Value.sha256) { throw "lmxxf Small tensor hash mismatch: $file" }
            [pscustomobject]@{Name=$entry.Name; Path=$file}
        }
        [pscustomobject]@{Name='model-manifest.json'; Path=$metadataPath}
    }
}

function Test-LmxxfModel {
    param([string]$Directory, [ValidateSet('original','small')][string]$Model, [switch]$Quick)
    try { $null = @(Get-LmxxfModelFiles $Directory $Model -Quick:$Quick); return $true } catch { return $false }
}

function Copy-LmxxfModel {
    param([string]$Source, [string]$Destination, [ValidateSet('original','small')][string]$Model)
    $Source = Assert-LmxxfModelPath $Source; $Destination = Assert-LmxxfModelPath $Destination
    if (Test-LmxxfModel $Destination $Model) { return }
    $files = @(Get-LmxxfModelFiles $Source $Model)
    if ($Source -ieq $Destination) { throw "Invalid lmxxf $Model destination is also the source." }
    $parent = [IO.Path]::GetDirectoryName($Destination)
    [void][IO.Directory]::CreateDirectory($parent)
    $stage = Join-Path $parent ('.lmxxf-model-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($stage)
    try {
        foreach ($file in $files) {
            $target = Join-Path $stage $file.Name
            Copy-Item -LiteralPath $file.Path -Destination $target
            if ((Get-LmxxfAssetHash $file.Path) -ne (Get-LmxxfAssetHash $target)) { throw "Model copy mismatch: $($file.Name)" }
        }
        if (-not (Test-LmxxfModel $stage $Model)) { throw "Staged lmxxf $Model weights failed validation." }
        [void][IO.Directory]::CreateDirectory($Destination)
        foreach ($file in $files) {
            $target = Assert-LmxxfModelPath (Join-Path $Destination $file.Name)
            if ([IO.Path]::GetExtension($target) -eq '.f16') {
                $alternate = [IO.Path]::ChangeExtension($target,'.f32')
                if (Test-Path -LiteralPath $alternate) { $null = Assert-LmxxfModelPath $alternate; Move-Item -LiteralPath $alternate -Destination ($alternate + '.mismatch-' + [guid]::NewGuid().ToString('N')) }
            }
            if ((Test-Path -LiteralPath $target -PathType Leaf) -and (Get-LmxxfAssetHash $target) -eq (Get-LmxxfAssetHash (Join-Path $stage $file.Name))) { continue }
            if (Test-Path -LiteralPath $target) { Move-Item -LiteralPath $target -Destination ($target + '.mismatch-' + [guid]::NewGuid().ToString('N')) }
            Move-Item -LiteralPath (Join-Path $stage $file.Name) -Destination $target
        }
        if (-not (Test-LmxxfModel $Destination $Model)) { throw "Installed lmxxf $Model weights failed validation." }
    } finally {
        if (Test-Path -LiteralPath $stage) {
            $clean = Assert-LmxxfModelPath $stage
            if ([IO.Path]::GetDirectoryName($clean) -ine $parent -or [IO.Path]::GetFileName($clean) -notmatch '^\.lmxxf-model-[0-9a-f]{32}$') { throw "Unexpected model staging directory: $clean" }
            Remove-Item -LiteralPath $clean -Recurse -Force
        }
    }
}

# Small assets can be prepared independently of installing Mochizuki.
function Initialize-LmxxfSmall {
    param([string]$Pack, [string]$Dll,
          [Parameter(Mandatory=$true)][string]$Output,
          [string]$Python = '', [string]$Tool = (Join-Path $PSScriptRoot 'lmxxf-model.py'))
    if (-not (Test-Path -LiteralPath $Tool -PathType Leaf)) {
        throw 'Small converter missing. Re-extract the complete package.'
    }
    if (-not $Python) {
        . (Join-Path $PSScriptRoot 'mochizuki-python.ps1')
        $Python = Find-MochizukiPython
    }
    if (-not $Python) { throw 'lmxxf Small conversion needs Python 3.10+. Install Python and rerun Setup.' }
    & $Python -X utf8 $Tool --verify $Output
    if ($LASTEXITCODE -eq 0) { Write-Host 'lmxxf Small assets verified.'; return }
    Write-Host 'Preparing lmxxf Small from your local model pack...' -ForegroundColor Cyan
    if (-not $Pack -and -not $Dll) { throw 'Supply a verified Small pack or nvngx_dlssnr.dll 310.9.16.' }
    if ($Pack) { & $Python -X utf8 $Tool --pack $Pack --output $Output --replace }
    else { & $Python -X utf8 $Tool --dll $Dll --output $Output --replace }
    if ($LASTEXITCODE -ne 0) { throw 'lmxxf Small conversion failed; previous model assets were preserved.' }
}

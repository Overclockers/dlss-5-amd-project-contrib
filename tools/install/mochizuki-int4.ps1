# Generate user-owned INT4 weights with the matching package's graph decoder.
# No GPU or Python is required. Never replace an existing model before hash validation.
# Setup prepares and validates the weights in a temporary folder before it changes
# the game folder (New-MochizukiInt4Candidate), then commits them at install time.

function Get-MochizukiInt4ExpectedHash([string]$Tools) {
    $hashFile = Join-Path $Tools 'dlssnr-int4.sha256'
    if (-not (Test-Path -LiteralPath $hashFile -PathType Leaf)) {
        throw "INT4 component missing: $hashFile. Extract and install the complete package."
    }
    $expected = ([IO.File]::ReadAllText($hashFile).Trim() -split '\s+')[0].ToLowerInvariant()
    if ($expected -notmatch '^[0-9a-f]{64}$') { throw 'Invalid INT4 model hash in package.' }
    return $expected
}

function Get-MochizukiInt4FileHash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Returns $null when $Existing already holds this package's weights, otherwise the path
# of a verified candidate in $WorkDir. A conversion failure or hash mismatch throws an
# exception whose Data['MochizukiInt4Mismatch'] is $true (Expected/Actual/Model set).
function New-MochizukiInt4Candidate {
    param([Parameter(Mandatory=$true)][string]$Model,
          [Parameter(Mandatory=$true)][string]$Data,
          [Parameter(Mandatory=$true)][string]$Tools,
          [string]$Existing,
          [string]$WorkDir = [IO.Path]::GetTempPath())
    $generator = Join-Path $Tools 'dlssnr-int4-weights.exe'
    foreach ($required in @($Model, $generator, (Join-Path $Data 'settings.txt'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "INT4 component missing: $required. Extract and install the complete package."
        }
    }
    $expected = Get-MochizukiInt4ExpectedHash $Tools
    if ($Existing -and (Test-Path -LiteralPath $Existing -PathType Leaf) -and
        (Get-MochizukiInt4FileHash $Existing) -eq $expected) {
        return $null
    }
    $temporary = Join-Path $WorkDir ('.mochizuki-int4-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $ok = $false
    try {
        Write-Host 'Preparing INT4 weights from your Mochizuki model...'
        & $generator $Model $Data $temporary | Out-Host
        $code = $LASTEXITCODE
        $actual = if ($code -ne 0) { "conversion failed (exit code $code)" }
                  elseif (-not (Test-Path -LiteralPath $temporary -PathType Leaf)) { 'conversion produced no file' }
                  else { Get-MochizukiInt4FileHash $temporary }
        if ($actual -ne $expected) {
            $error0 = [Exception]::new("Generated INT4 model hash differs from the package pin; original weights were kept. Expected $expected, got $actual.")
            $error0.Data['MochizukiInt4Mismatch'] = $true
            $error0.Data['Expected'] = $expected
            $error0.Data['Actual'] = $actual
            $error0.Data['Model'] = $Model
            throw $error0
        }
        $ok = $true
        return $temporary
    } finally {
        if (-not $ok -and (Test-Path -LiteralPath $temporary)) { Remove-Item -LiteralPath $temporary -Force }
    }
}

# Copies a verified candidate beside $Output, re-checks it, then replaces $Output.
function Install-MochizukiInt4Candidate([string]$Candidate, [string]$Output, [string]$Expected) {
    $staged = Join-Path (Split-Path -Parent $Output) ('.mochizuki-int4-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Copy-Item -LiteralPath $Candidate -Destination $staged -Force -ErrorAction Stop
        if ((Get-MochizukiInt4FileHash $staged) -ne $Expected) { throw 'INT4 weights changed while installing; original weights were kept.' }
        Move-Item -LiteralPath $staged -Destination $Output -Force -ErrorAction Stop
    } finally {
        if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged -Force }
    }
}

# Player-facing text for a mismatch (English, then Chinese).
function Get-MochizukiInt4MismatchText($Exception) {
    $expected = $Exception.Data['Expected']; $actual = $Exception.Data['Actual']; $model = $Exception.Data['Model']
    return @(
        'The Mochizuki model you supplied does not match the version this package expects.',
        "  Model file: $model (dlssnr.bin, extracted from your nvngx_dlssnr.dll)",
        "  Expected:   nvngx_dlssnr.dll 310.8.0; INT4 weights SHA-256 $expected",
        "  Got:        INT4 weights $actual",
        '  Nothing was installed. The game folder was not changed.',
        '  Fix: get nvngx_dlssnr.dll version 310.8.0, put it beside Setup.bat, delete the old dlssnr-amd\dlssnr.bin',
        '       (beside Setup.bat and in the game folder), run Mochizuki-Model.bat, then run Setup again.',
        '',
        '你提供的 Mochizuki 模型与本安装包要求的版本不一致。',
        "  模型文件：$model（dlssnr.bin，由你的 nvngx_dlssnr.dll 提取）",
        "  期望：nvngx_dlssnr.dll 310.8.0；INT4 权重 SHA-256 $expected",
        "  实际：INT4 权重 $actual",
        '  没有安装任何文件，游戏目录未作改动。',
        '  解决：换用 310.8.0 版本的 nvngx_dlssnr.dll，放到 Setup.bat 旁边，删除旧的 dlssnr-amd\dlssnr.bin',
        '       （Setup.bat 旁边和游戏目录里的都删），运行 Mochizuki-Model.bat，然后重新运行 Setup。'
    )
}

# Model-extraction path (Mochizuki-Model.bat): prepare beside the model and commit.
function Initialize-MochizukiInt4 {
    param([Parameter(Mandatory=$true)][string]$Model,
          [Parameter(Mandatory=$true)][string]$Data,
          [Parameter(Mandatory=$true)][string]$Tools)
    $output = Join-Path (Split-Path -Parent $Model) 'dlssnr-int4.bin'
    $candidate = New-MochizukiInt4Candidate -Model $Model -Data $Data -Tools $Tools -Existing $output -WorkDir (Split-Path -Parent $Model)
    if (-not $candidate) { Write-Host 'INT4 weights already match this package.'; return }
    try {
        Move-Item -LiteralPath $candidate -Destination $output -Force -ErrorAction Stop
        Write-Host 'INT4 weights verified.'
    } finally {
        if (Test-Path -LiteralPath $candidate) { Remove-Item -LiteralPath $candidate -Force }
    }
}

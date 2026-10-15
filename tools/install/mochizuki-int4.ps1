# Generate user-owned INT4 weights with the matching package's graph decoder.
# No GPU or Python is required. Never replace an existing model before hash validation.
function Initialize-MochizukiInt4 {
    param([Parameter(Mandatory=$true)][string]$Model,
          [Parameter(Mandatory=$true)][string]$Data,
          [Parameter(Mandatory=$true)][string]$Tools)
    $generator = Join-Path $Tools 'dlssnr-int4-weights.exe'
    $hashFile = Join-Path $Tools 'dlssnr-int4.sha256'
    foreach ($required in @($Model, $generator, $hashFile, (Join-Path $Data 'settings.txt'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "INT4 component missing: $required. Extract and install the complete package."
        }
    }
    $expected = ([IO.File]::ReadAllText($hashFile).Trim() -split '\s+')[0].ToLowerInvariant()
    if ($expected -notmatch '^[0-9a-f]{64}$') { throw 'Invalid INT4 model hash in package.' }
    $output = Join-Path (Split-Path -Parent $Model) 'dlssnr-int4.bin'
    if ((Test-Path -LiteralPath $output -PathType Leaf) -and
        (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expected) {
        Write-Host 'INT4 weights already match this package.'
        return
    }
    $temporary = Join-Path (Split-Path -Parent $Model) ('.mochizuki-int4-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Write-Host 'Preparing INT4 weights from your Mochizuki model...'
        & $generator $Model $Data $temporary
        if ($LASTEXITCODE -ne 0) { throw "INT4 weight conversion failed ($LASTEXITCODE)." }
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
            throw 'Generated INT4 model hash differs from the package pin; original weights were kept.'
        }
        Move-Item -LiteralPath $temporary -Destination $output -Force -ErrorAction Stop
        Write-Host 'INT4 weights verified.'
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

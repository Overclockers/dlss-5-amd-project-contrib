# Common backend capability and weight checking for OptScaler NR.
# Shared between launcher-worker.ps1 and install-amd-presr.ps1.

function Test-LmxxfWeightsDir([string]$Directory) {
    if (-not $Directory -or -not (Get-Command Test-LmxxfModel -ErrorAction SilentlyContinue)) { return $false }
    return Test-LmxxfModel $Directory original -Quick
}

function Test-MochizukiModelHeaderFile([string]$Path, [uint32]$Entries = 599) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $header = New-Object byte[] 16
        if ($stream.Read($header, 0, 16) -ne 16) { return $false }
        return ([Text.Encoding]::ASCII.GetString($header, 0, 8) -ceq 'NRMODEL1' -and
            [BitConverter]::ToUInt32($header, 8) -eq $Entries -and $stream.Length -gt 16)
    } catch { return $false } finally { if ($stream) { $stream.Dispose() } }
}

function Get-OptScalerFileSha256([string]$Path) {
    $stream = $null
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

function Get-OptScalerBackendCapabilities([string]$pkgRoot, [string]$gameDir) {
    $backends = @()
    $modelHelper = Join-Path $PSScriptRoot 'lmxxf-small.ps1'
    if (Test-Path -LiteralPath $modelHelper) { . $modelHelper }

    # --- lmxxf ---
    $lmxxfRuntime = $null
    $lmxxfMods = $null
    $lmxxfShaders = $null
    $lmxxfWeights = $null
    $lmxxfSmall = $null
    if ($pkgRoot) {
        $cand = Join-Path $pkgRoot 'LmxxfNrRuntime.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $lmxxfRuntime = $cand }
        $cand = Join-Path $pkgRoot 'lmxxf-modules'
        if (Test-Path -LiteralPath $cand -PathType Container) { $lmxxfMods = $cand }
        $cand = Join-Path $pkgRoot 'shaders'
        if (Test-Path -LiteralPath (Join-Path $cand 'native_codec_encode.hlsl') -PathType Leaf) { $lmxxfShaders = $cand }
        $cand = Join-Path $pkgRoot 'native-game-tiled-assets'
        if (Test-LmxxfWeightsDir $cand) { $lmxxfWeights = $cand }
    }
    if ($gameDir -and -not $lmxxfWeights) {
        $cand = Join-Path $gameDir 'native-game-tiled-assets'
        if (Test-LmxxfWeightsDir $cand) { $lmxxfWeights = $cand }
    }
    if (Get-Command Test-LmxxfModel -ErrorAction SilentlyContinue) {
        foreach ($root in @($pkgRoot, $gameDir)) {
            if (-not $root) { continue }
            $candidate = Join-Path $root 'native-game-tiled-assets/small'
            if (-not $lmxxfSmall -and (Test-LmxxfModel $candidate small -Quick)) { $lmxxfSmall = $candidate }
        }
    }
    $smallSource = $false
    if (-not $lmxxfSmall) {
        foreach ($root in @($pkgRoot, $gameDir)) {
            if (-not $root) { continue }
            $packs = @((Join-Path $root 'dlssnr-amd/dlssnr-small.bin'))
            if ($root -eq $pkgRoot) { $packs += Join-Path $root 'dlssnr-small.bin' }
            $dll = Join-Path $root 'nvngx_dlssnr.dll'
            foreach ($pack in $packs) {
                if (Test-MochizukiModelHeaderFile $pack 255) { $smallSource = $true; break }
            }
            if ($smallSource) { break }
            if (Test-Path -LiteralPath $dll -PathType Leaf) {
                try { if ((Get-OptScalerFileSha256 $dll) -eq '2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f') { $smallSource = $true; break } } catch { }
            }
        }
    }
    # Installed models may be reused; installed/developer code cannot fill an incomplete update.

    $lmxxfHelperReady = $pkgRoot -and (Test-Path -LiteralPath (Join-Path $pkgRoot 'lmxxf-small.ps1') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $pkgRoot 'lmxxf-small-manifest.json') -PathType Leaf)
    $canLmxxfRuntime = [bool]($lmxxfRuntime -and $lmxxfMods -and $lmxxfShaders -and $lmxxfHelperReady)
    $hasLmxxfWeights = [bool]($lmxxfWeights -or $lmxxfSmall)
    $lmxxfEnabled = $canLmxxfRuntime -and $hasLmxxfWeights
    $lmxxfReason = if (-not $canLmxxfRuntime) { 'Runtime missing' } elseif (-not $hasLmxxfWeights) { 'Weights missing' } else { 'Ready' }
    $backends += @{
        id = 'lmxxf'
        runtimeState = if ($canLmxxfRuntime) { 'ready' } else { 'missing' }
        modelState = if ($lmxxfWeights -and $lmxxfSmall) { 'ready' } elseif ($hasLmxxfWeights) { 'partial' } elseif ($smallSource) { 'preparable' } else { 'missing' }
        originalState = if ($lmxxfWeights) { 'ready' } else { 'missing' }
        smallState = if ($lmxxfSmall) { 'ready' } elseif ($smallSource) { 'preparable' } else { 'missing' }
        enabled = $lmxxfEnabled
        reasonCode = if (-not $canLmxxfRuntime) { 'missing-runtime' } elseif ($lmxxfWeights -and $lmxxfSmall) { 'ready' } elseif ($hasLmxxfWeights) { 'partial' } elseif ($smallSource) { 'preparable' } else { 'missing-weights' }
        reason = $lmxxfReason
        hints = @(
            if (-not $lmxxfWeights) { 'lmxxf-original' }
            if (-not $lmxxfSmall) { if ($smallSource) { 'lmxxf-small-preparable' } else { 'lmxxf-small' } }
        )
        missingPackageFiles = @(
            if (-not $lmxxfRuntime) { 'LmxxfNrRuntime.dll' }
            if (-not $lmxxfMods) { 'lmxxf-modules/' }
            if (-not $lmxxfShaders) { 'shaders/native_codec_encode.hlsl' }
            if (-not $pkgRoot -or -not (Test-Path -LiteralPath (Join-Path $pkgRoot 'lmxxf-small.ps1') -PathType Leaf)) { 'lmxxf-small.ps1' }
            if (-not $pkgRoot -or -not (Test-Path -LiteralPath (Join-Path $pkgRoot 'lmxxf-small-manifest.json') -PathType Leaf)) { 'lmxxf-small-manifest.json' }
        )
    }

    # --- daniel ---
    $danielRuntime = $null
    $danielWeights = $null
    if ($pkgRoot) {
        foreach ($name in @('version.dll', 'dlssnr_amd_pass1.dll')) {
            $cand = Join-Path $pkgRoot $name
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielRuntime = $cand; break }
        }
        $cand = Join-Path $pkgRoot 'dlssnr_on_amd_weights.bin'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielWeights = $cand }
    }
    if ($gameDir -and -not $danielWeights) {
        $cand = Join-Path $gameDir 'dlssnr_on_amd_weights.bin'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielWeights = $cand }
    }
    if ($gameDir -and -not $danielRuntime) {
        foreach ($name in @('version.dll', 'dlssnr_amd_pass1.dll')) {
            $cand = Join-Path $gameDir $name
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielRuntime = $cand; break }
        }
    }
    # Presence is only a capability hint; Setup validates the supported runtime SHA before installation.
    $canDanielRuntime = [bool]$danielRuntime
    $hasDanielWeights = [bool]$danielWeights
    $danielSetup = $pkgRoot -and (Test-Path -LiteralPath (Join-Path $pkgRoot 'dlssnr_on_amd_setup.exe') -PathType Leaf)
    $danielNv = $false
    foreach ($root in @($pkgRoot, $gameDir)) {
        if ($root -and (Test-Path -LiteralPath (Join-Path $root 'nvngx_dlssnr.dll') -PathType Leaf)) { $danielNv = $true }
    }
    $danielPreparable = $danielSetup -and $danielNv -and (-not $canDanielRuntime -or -not $hasDanielWeights)
    $danielEnabled = $canDanielRuntime -and $hasDanielWeights
    $danielReason = if (-not $canDanielRuntime) { 'Runtime missing' } elseif (-not $hasDanielWeights) { 'Weights missing' } else { 'Ready' }
    $backends += @{
        id = 'daniel'
        runtimeState = if ($canDanielRuntime) { 'ready' } elseif ($danielPreparable) { 'preparable' } else { 'missing' }
        modelState = if ($hasDanielWeights) { 'ready' } elseif ($danielPreparable) { 'preparable' } else { 'missing' }
        enabled = $danielEnabled
        reasonCode = if ($danielPreparable) { 'preparable' } elseif (-not $canDanielRuntime) { 'missing-runtime' } elseif (-not $hasDanielWeights) { 'missing-weights' } else { 'ready' }
        reason = $danielReason
        hints = @(
            if ($danielPreparable) { 'daniel-preparable' }
            else {
                if (-not $canDanielRuntime) { 'daniel-runtime' }
                if (-not $hasDanielWeights) {
                    'daniel-weights'
                    if (-not $danielSetup) { 'daniel-setup' }
                    if (-not $danielNv) { 'daniel-source' }
                }
            }
        )
        missingPackageFiles = @()
    }

    # --- mochizuki ---
    $mochiRuntime = $null
    $mochiModel = $null
    $mochiNv = $null
    if ($pkgRoot) {
        $cand = Join-Path $pkgRoot 'MochizukiNrRuntime.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiRuntime = $cand }
        $cand = Join-Path $pkgRoot 'dlssnr-amd\dlssnr.bin'
        if (Test-MochizukiModelHeaderFile $cand) { $mochiModel = $cand }
        $cand = Join-Path $pkgRoot 'dlssnr-amd\dlssnr-small.bin'
        if (-not $mochiModel -and (Test-MochizukiModelHeaderFile $cand 255)) { $mochiModel = $cand }
        $cand = Join-Path $pkgRoot 'nvngx_dlssnr.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiNv = $cand }
    }
    if ($gameDir) {
        if (-not $mochiModel) {
            $cand = Join-Path $gameDir 'dlssnr-amd\dlssnr.bin'
            if (Test-MochizukiModelHeaderFile $cand) { $mochiModel = $cand }
            $cand = Join-Path $gameDir 'dlssnr-amd\dlssnr-small.bin'
            if (-not $mochiModel -and (Test-MochizukiModelHeaderFile $cand 255)) { $mochiModel = $cand }
        }
        if (-not $mochiNv) {
            $cand = Join-Path $gameDir 'nvngx_dlssnr.dll'
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiNv = $cand }
        }
    }
    $missingMochiPackage = @()
    if (-not $mochiRuntime) { $missingMochiPackage += 'MochizukiNrRuntime.dll' }
    else {
        $inventory = Join-Path $pkgRoot 'mochizuki-owned.txt'
        if (-not (Test-Path -LiteralPath $inventory -PathType Leaf)) { $missingMochiPackage += 'mochizuki-owned.txt' }
        else {
            $assets = @(Get-Content -LiteralPath $inventory -ErrorAction SilentlyContinue | Where-Object { $_.Trim() -and -not $_.StartsWith('#') })
            if ($assets.Count -eq 0) { $missingMochiPackage += 'mochizuki-owned.txt' }
            foreach ($asset in $assets) {
                $relative = 'dlssnr-amd/' + $asset.Trim()
                if (-not (Test-Path -LiteralPath (Join-Path $pkgRoot $relative) -PathType Leaf)) { $missingMochiPackage += $relative }
            }
        }
        foreach ($relative in @('mochizuki-int4.ps1', 'model-tools/dlssnr-int4-weights.exe', 'model-tools/dlssnr-int4.sha256')) {
            if (-not (Test-Path -LiteralPath (Join-Path $pkgRoot $relative) -PathType Leaf)) { $missingMochiPackage += $relative }
        }
    }
    $canMochiRuntime = $missingMochiPackage.Count -eq 0
    $hasMochiModel = [bool]$mochiModel
    $unsupportedMochiSource = $false
    if (-not $hasMochiModel -and $mochiNv) {
        # Match Setup's accepted sources; presence alone must not claim preparation is ready.
        $unsupportedMochiSource = $true
        foreach ($root in @($pkgRoot, $gameDir)) {
            if (-not $root) { continue }
            $candidate = Join-Path $root 'nvngx_dlssnr.dll'
            if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
            try { $hash = Get-OptScalerFileSha256 $candidate } catch { continue }
            if ($hash -in @('2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f',
                           'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e')) {
                $unsupportedMochiSource = $false
                break
            }
        }
        if ($unsupportedMochiSource) { $mochiNv = $null }
    }
    $mochiPreparable = (-not $hasMochiModel) -and [bool]$mochiNv
    $mochiEnabled = $canMochiRuntime -and $hasMochiModel
    $mochiReason = if (-not $canMochiRuntime) { 'Runtime missing' } elseif ($hasMochiModel) { 'Ready' } elseif ($mochiPreparable) { 'Model preparation required' } else { 'Model missing' }
    $backends += @{
        id = 'mochizuki'
        runtimeState = if ($canMochiRuntime) { 'ready' } else { 'missing' }
        modelState = if ($hasMochiModel) { 'ready' } elseif ($mochiPreparable) { 'preparable' } else { 'missing' }
        enabled = $mochiEnabled
        reasonCode = if (-not $canMochiRuntime) { 'missing-runtime' } elseif ($hasMochiModel) { 'ready' } elseif ($mochiPreparable) { 'preparable' } else { 'missing-model' }
        reason = $mochiReason
        hints = @(
            if (-not $hasMochiModel) {
                if ($mochiPreparable) { 'mochi-preparable' }
                elseif ($unsupportedMochiSource) { 'mochi-unsupported-source' }
                else { 'mochi-source' }
                if (-not $mochiPreparable) { 'mochi-versions' }
            }
        )
        missingPackageFiles = $missingMochiPackage
    }

    return @($backends)
}

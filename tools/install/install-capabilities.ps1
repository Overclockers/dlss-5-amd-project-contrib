# Common backend capability and weight checking for OptScaler NR.
# Shared between launcher-worker.ps1 and install-amd-presr.ps1.

function Test-LmxxfWeightsDir([string]$Directory) {
    if (-not $Directory -or -not (Test-Path -LiteralPath $Directory -PathType Container)) { return $false }
    foreach ($name in @('block0-ffn.f16', 'block0-ffn.f32')) {
        $file = Get-Item -LiteralPath (Join-Path $Directory $name) -ErrorAction SilentlyContinue
        if ($file -and -not $file.PSIsContainer -and $file.Length -gt 0) { return $true }
    }
    return $false
}

function Test-MochizukiModelHeaderFile([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead($Path)
        $header = New-Object byte[] 16
        if ($stream.Read($header, 0, 16) -ne 16) { return $false }
        return ([Text.Encoding]::ASCII.GetString($header, 0, 8) -ceq 'NRMODEL1' -and
            [BitConverter]::ToUInt32($header, 8) -eq 599 -and $stream.Length -gt 16)
    } catch { return $false } finally { if ($stream) { $stream.Dispose() } }
}

function Get-OptScalerBackendCapabilities([string]$pkgRoot, [string]$gameDir) {
    $backends = @()

    # --- lmxxf ---
    $lmxxfRuntime = $null
    $lmxxfMods = $null
    $lmxxfShaders = $null
    $lmxxfWeights = $null
    if ($pkgRoot) {
        $cand = Join-Path $pkgRoot 'LmxxfNrRuntime.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $lmxxfRuntime = $cand }
        $cand = Join-Path $pkgRoot 'modules'
        if (Test-Path -LiteralPath $cand -PathType Container) { $lmxxfMods = $cand }
        $cand = Join-Path $pkgRoot 'shaders'
        if (Test-Path -LiteralPath $cand -PathType Container) { $lmxxfShaders = $cand }
        $cand = Join-Path $pkgRoot 'native-game-tiled-assets'
        if (Test-LmxxfWeightsDir $cand) { $lmxxfWeights = $cand }
    }
    if ($gameDir -and -not $lmxxfWeights) {
        $cand = Join-Path $gameDir 'native-game-tiled-assets'
        if (Test-LmxxfWeightsDir $cand) { $lmxxfWeights = $cand }
    }
    if (-not $lmxxfRuntime -or -not $lmxxfMods -or -not $lmxxfShaders) {
        $treeMods = Join-Path $PSScriptRoot '../../third_party/lmxxf/modules'
        $treeShaders = Join-Path $PSScriptRoot '../../shaders'
        if (Test-Path -LiteralPath $treeMods -PathType Container) { $lmxxfMods = $treeMods }
        if (Test-Path -LiteralPath $treeShaders -PathType Container) { $lmxxfShaders = $treeShaders }
        foreach ($candPath in @(
            (Join-Path $PSScriptRoot '../../exports/lmxxf-runtime/LmxxfNrRuntime.dll'),
            (Join-Path $PSScriptRoot '../../exports/release-local/LmxxfNrRuntime.dll'),
            (Join-Path $PSScriptRoot '../../exports/launcher/LmxxfNrRuntime.dll')
        )) {
            if (Test-Path -LiteralPath $candPath -PathType Leaf) { $lmxxfRuntime = $candPath; break }
        }
        if (-not $lmxxfRuntime) {
            $distCand = Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '../../dist') -Filter 'LmxxfNrRuntime.dll' -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($distCand) { $lmxxfRuntime = $distCand.FullName }
        }
    }
    if ($gameDir -and -not $lmxxfRuntime) {
        $cand = Join-Path $gameDir 'LmxxfNrRuntime.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $lmxxfRuntime = $cand }
    }

    $canLmxxfRuntime = [bool]($lmxxfRuntime -and $lmxxfMods -and $lmxxfShaders)
    $hasLmxxfWeights = [bool]$lmxxfWeights
    $lmxxfEnabled = $canLmxxfRuntime -and $hasLmxxfWeights
    $lmxxfReason = if (-not $canLmxxfRuntime) { 'runtime 缺失' } elseif (-not $hasLmxxfWeights) { '权重缺失' } else { '就绪' }
    $backends += @{
        id = 'lmxxf'
        runtimeState = if ($canLmxxfRuntime) { 'ready' } else { 'missing' }
        modelState = if ($hasLmxxfWeights) { 'ready' } else { 'missing' }
        enabled = $lmxxfEnabled
        reasonCode = if (-not $canLmxxfRuntime) { 'missing-runtime' } elseif (-not $hasLmxxfWeights) { 'missing-weights' } else { 'ready' }
        reason = $lmxxfReason
    }

    # --- daniel ---
    $danielRuntime = $null
    $danielWeights = $null
    if ($pkgRoot) {
        $cand = Join-Path $pkgRoot 'dlssnr_on_amd_setup.exe'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielRuntime = $cand }
        $cand = Join-Path $pkgRoot 'dlssnr_on_amd_weights.bin'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielWeights = $cand }
    }
    if ($gameDir -and -not $danielWeights) {
        $cand = Join-Path $gameDir 'dlssnr_on_amd_weights.bin'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $danielWeights = $cand }
    }
    $canDanielRuntime = [bool]$danielRuntime
    $hasDanielWeights = [bool]$danielWeights
    $danielEnabled = $canDanielRuntime -and $hasDanielWeights
    $danielReason = if (-not $canDanielRuntime) { 'runtime 缺失' } elseif (-not $hasDanielWeights) { '权重缺失' } else { '就绪' }
    $backends += @{
        id = 'daniel'
        runtimeState = if ($canDanielRuntime) { 'ready' } else { 'missing' }
        modelState = if ($hasDanielWeights) { 'ready' } else { 'missing' }
        enabled = $danielEnabled
        reasonCode = if (-not $canDanielRuntime) { 'missing-runtime' } elseif (-not $hasDanielWeights) { 'missing-weights' } else { 'ready' }
        reason = $danielReason
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
        $cand = Join-Path $pkgRoot 'nvngx_dlssnr.dll'
        if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiNv = $cand }
    }
    if ($gameDir) {
        if (-not $mochiModel) {
            $cand = Join-Path $gameDir 'dlssnr-amd\dlssnr.bin'
            if (Test-MochizukiModelHeaderFile $cand) { $mochiModel = $cand }
        }
        if (-not $mochiNv) {
            $cand = Join-Path $gameDir 'nvngx_dlssnr.dll'
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $mochiNv = $cand }
        }
    }
    $canMochiRuntime = [bool]$mochiRuntime
    $hasMochiModel = [bool]$mochiModel
    $mochiPreparable = (-not $hasMochiModel) -and [bool]$mochiNv
    $mochiEnabled = $canMochiRuntime -and $hasMochiModel
    $mochiReason = if (-not $canMochiRuntime) { 'runtime 缺失' } elseif ($hasMochiModel) { '就绪' } elseif ($mochiPreparable) { '需准备模型' } else { '模型缺失' }
    $backends += @{
        id = 'mochizuki'
        runtimeState = if ($canMochiRuntime) { 'ready' } else { 'missing' }
        modelState = if ($hasMochiModel) { 'ready' } elseif ($mochiPreparable) { 'preparable' } else { 'missing' }
        enabled = $mochiEnabled
        reasonCode = if (-not $canMochiRuntime) { 'missing-runtime' } elseif ($hasMochiModel) { 'ready' } elseif ($mochiPreparable) { 'preparable' } else { 'missing-model' }
        reason = $mochiReason
    }

    return @($backends)
}

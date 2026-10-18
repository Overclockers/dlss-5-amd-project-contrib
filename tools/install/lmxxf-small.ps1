# Optional Small assets reuse the locally supplied, verified Mochizuki pack.
function Initialize-LmxxfSmall {
    param([Parameter(Mandatory=$true)][string]$Pack,
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
    & $Python -X utf8 $Tool --pack $Pack --output $Output --replace
    if ($LASTEXITCODE -ne 0) { throw 'lmxxf Small conversion failed; previous model assets were preserved.' }
}

param([string]$Source, [string]$Output, [string]$Models)

# Dot-sourced by Setup, or run directly by Mochizuki-Model.bat.
function Find-MochizukiPython {
    foreach ($name in @('python', 'python3', 'py')) {
        foreach ($command in @(Get-Command $name -CommandType Application -ErrorAction SilentlyContinue)) {
            try {
                # A Windows execution alias or broken launcher may exist without Python.
                # Require code execution, not just an executable or a zero exit status.
                $probe = & $command.Source -c "import sys; print('MOCHI_PYTHON_OK' if sys.version_info >= (3,10) else 'MOCHI_PYTHON_TOO_OLD')" 2>$null
                if ($LASTEXITCODE -eq 0 -and @($probe) -contains 'MOCHI_PYTHON_OK') {
                    return $command.Source
                }
            } catch {
                # PS5.1 can throw on redirected native stderr under ErrorAction=Stop.
                # Continue to python3/py before reporting that no usable Python exists.
            }
        }
    }
    return $null
}

function Write-MochizukiPythonHelp {
    Write-Host 'Model extraction cannot run: no working Python 3.10+ was found (missing, too old, or unavailable).' -ForegroundColor Yellow
    Write-Host 'Open Microsoft Store (Windows Store), search for Python, and install Python 3.10 or newer.' -ForegroundColor Yellow
    Write-Host 'After installation, close and reopen Setup.bat or Mochizuki-Model.bat, then retry model extraction.' -ForegroundColor Yellow
}

if ($MyInvocation.InvocationName -ne '.') {
    $python = Find-MochizukiPython
    if (-not $python) { Write-MochizukiPythonHelp; exit 1 }
    if (-not $Source) { $Source = Join-Path $PSScriptRoot 'nvngx_dlssnr.dll' }
    if (-not $Output) { $Output = Join-Path $PSScriptRoot 'dlssnr-amd/dlssnr.bin' }

    if (-not $Models) {
        Write-Host ''
        Write-Host 'Select Mochizuki models to install:' -ForegroundColor Cyan
        Write-Host '1. Both models (recommended)'
        Write-Host '2. New model only'
        Write-Host '3. Original model only'
        Write-Host '0. Cancel'
        Write-Host ''
        $choice = Read-Host 'Enter your choice (1/2/3/0)'
        switch ($choice.Trim()) {
            '1' { $Models = 'both' }
            '2' { $Models = 'small' }
            '3' { $Models = 'original' }
            '0' { Write-Host 'Operation cancelled.'; exit 0 }
            default {
                Write-Host 'Invalid selection. Operation cancelled.' -ForegroundColor Red
                exit 1
            }
        }
    }

    if ($Models -notin @('both', 'small', 'original')) {
        Write-Host "Invalid models parameter: '$Models'. Expected 'both', 'small', or 'original'." -ForegroundColor Red
        exit 1
    }

    $extractor = Join-Path $PSScriptRoot 'mochizuki-model.py'
    if (-not (Test-Path -LiteralPath $extractor -PathType Leaf)) {
        Write-Host 'Model extractor missing. Re-extract the complete OptiScaler package.' -ForegroundColor Yellow
        exit 1
    }

    & $python -X utf8 $extractor $Source $Output --models $Models
    $result = $LASTEXITCODE
    if ($result -ne 0) {
        Write-Host 'Model extraction failed. Python was found; see the error above and check nvngx_dlssnr.dll 310.9.16 / 310.8.0. See docs/mochizuki.md.' -ForegroundColor Yellow
    }
    if ($result -eq 0 -and $Models -in @('both', 'original')) {
        try {
            . (Join-Path $PSScriptRoot 'mochizuki-int4.ps1')
            Initialize-MochizukiInt4 -Model $Output -Data (Join-Path $PSScriptRoot 'dlssnr-amd/int4') -Tools (Join-Path $PSScriptRoot 'model-tools')
        } catch {
            Write-Host $_.Exception.Message -ForegroundColor Yellow
            exit 1
        }
    }
    exit $result
}

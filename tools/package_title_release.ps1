# Windows launcher for tools/package_title_release.py (the one release command).
# Run from the title repository root:
#   psxrecomp/tools/package_title_release.ps1 --channel public|playtest [--platform windows|linux|all]
# Arguments pass through unchanged; --channel is required (no default).
# Exit code is the packager's; failures end with "PACKAGING FAILED: <reason>".
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot 'package_title_release.py'
$python = $env:PSX_RELEASE_PYTHON
$prefix = @()
if (-not $python) {
    if (Get-Command py -ErrorAction SilentlyContinue) { $python = 'py'; $prefix = @('-3') }
    elseif (Get-Command python -ErrorAction SilentlyContinue) { $python = 'python' }
    else {
        Write-Host 'PACKAGING FAILED: Python 3.11+ not found (install it or set PSX_RELEASE_PYTHON)' -ForegroundColor Red
        exit 1
    }
}
& $python @prefix $script @args
exit $LASTEXITCODE

<#
.SYNOPSIS
    Initialize a Heartbeat Monitor install directory on Windows (PowerShell port
    of bootstrap_heartbeat_monitor.sh): create the venv, install dependencies,
    and create the data/logs/config directories.

.DESCRIPTION
    The C++ cpp_sender is built separately with CMake (not part of this
    bootstrap; needs a compiler + CMake, which are not required on the monitor
    host).
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Here

# Prefer 'python'; fall back to the py launcher ('python3' on Windows is the
# Microsoft Store alias stub, so it is not used here).
$Python = $env:PYTHON
if (-not $Python) {
    if (Get-Command python -ErrorAction SilentlyContinue) { $Python = 'python' }
    elseif (Get-Command py -ErrorAction SilentlyContinue) { $Python = 'py' }
    else { Write-Error 'Python 3.9+ not found on PATH. Install it first.'; exit 1 }
}

& $Python -m venv venv
$VenvPy = Join-Path $Here 'venv\Scripts\python.exe'

# Prefer the sibling submodule checkout of mu2edaq-discovery when present.
if (Test-Path '..\mu2edaq-discovery') {
    & $VenvPy -m pip install -e '..\mu2edaq-discovery'
}
& $VenvPy -m pip install -r requirements.txt

New-Item -ItemType Directory -Force -Path data, logs, config | Out-Null
Write-Host 'Heartbeat Monitor server environment initialized successfully.'

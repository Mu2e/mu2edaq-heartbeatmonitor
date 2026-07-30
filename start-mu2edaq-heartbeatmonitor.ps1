<#
.SYNOPSIS
    Standardized Mu2e control-room start script (PowerShell port of
    start-mu2edaq-heartbeatmonitor.sh) for the Heartbeat Monitor.

.DESCRIPTION
    heartbeat_monitor.py honors CRS_PORT_HTTP (web) and CRS_PORT_UDP (listener).
    POSIX daemon mode uses os.fork(), which the monitor refuses on Windows, so
    this backgrounds the process via Start-Process and writes the pid file
    itself (the same file stop-mu2edaq-heartbeatmonitor.ps1 reads).

    Port precedence: $env:CRS_PORT_* > config file > built-in default.
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Config
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

if (-not $env:CRS_PORT_HTTP) { $env:CRS_PORT_HTTP = '8081' }
if (-not $env:CRS_PORT_UDP)  { $env:CRS_PORT_UDP  = '9999' }
if (-not $Config) { $Config = Join-Path $ScriptDir 'config\heartbeat_config.yaml' }
$PidFile = Join-Path $ScriptDir 'heartbeat_monitor.pid'

$VenvPy = Join-Path $ScriptDir 'venv\Scripts\python.exe'
if (-not (Test-Path $VenvPy)) {
    Write-Error 'virtual environment not found; run .\bootstrap_heartbeat_monitor.ps1 first'
    exit 1
}

function Test-ProcessAlive([int]$ProcId) {
    return [bool](Get-Process -Id $ProcId -ErrorAction SilentlyContinue)
}
if (Test-Path $PidFile) {
    $old = (Get-Content $PidFile -Raw).Trim()
    if ($old -match '^\d+$' -and (Test-ProcessAlive ([int]$old))) {
        Write-Host "Heartbeat Monitor already running (pid $old)."
        exit 0
    }
    Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
}

Write-Host "Starting Heartbeat Monitor (http=$($env:CRS_PORT_HTTP) udp=$($env:CRS_PORT_UDP), config: $Config)"
$log = Join-Path $ScriptDir 'logs\heartbeat_monitor.out'
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
# No --daemon: fork is unavailable on Windows; background via Start-Process.
$proc = Start-Process -FilePath $VenvPy `
    -ArgumentList @((Join-Path $ScriptDir 'heartbeat_monitor.py'), '--config', $Config) `
    -WorkingDirectory $ScriptDir -RedirectStandardOutput $log `
    -RedirectStandardError "$log.err" -WindowStyle Hidden -PassThru
Set-Content -Path $PidFile -Value $proc.Id -Encoding ascii
Write-Host "Heartbeat Monitor started (pid $($proc.Id)); pid file: $PidFile"

<#
.SYNOPSIS
    Standardized Mu2e control-room stop script (PowerShell port of
    stop-mu2edaq-heartbeatmonitor.sh). Stops the monitor via its pid file:
    graceful close, then a forced kill after a timeout, and clean up the pid file.

.PARAMETER PidFile
    Path to the pid file (defaults to heartbeat_monitor.pid next to this script).
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$PidFile
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PidFile) { $PidFile = Join-Path $ScriptDir 'heartbeat_monitor.pid' }
$Timeout = if ($env:CRS_STOP_TIMEOUT) { [int]$env:CRS_STOP_TIMEOUT } else { 10 }

function Test-ProcessAlive([int]$ProcId) {
    return [bool](Get-Process -Id $ProcId -ErrorAction SilentlyContinue)
}

if (-not (Test-Path $PidFile)) {
    Write-Host "Heartbeat Monitor not running (no pid file: $PidFile)"
    exit 0
}

$ProcId = [int]((Get-Content $PidFile -Raw).Trim())
if (-not (Test-ProcessAlive $ProcId)) {
    Write-Host "Heartbeat Monitor not running (stale pid $ProcId); cleaning up"
    Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
    exit 0
}

Write-Host "Stopping Heartbeat Monitor (pid $ProcId)..."
$proc = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
if ($proc) { $proc.CloseMainWindow() | Out-Null }
for ($i = 0; $i -lt $Timeout; $i++) {
    if (-not (Test-ProcessAlive $ProcId)) { break }
    Start-Sleep -Seconds 1
}
if (Test-ProcessAlive $ProcId) {
    Write-Host "did not exit within ${Timeout}s; forcing"
    Stop-Process -Id $ProcId -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
}
Remove-Item $PidFile -Force -ErrorAction SilentlyContinue
Write-Host 'Heartbeat Monitor stopped'

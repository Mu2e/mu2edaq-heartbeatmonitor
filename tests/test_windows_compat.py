"""Windows compatibility tests.

Added in the windows-compat sweep. The heartbeat monitor is a Flask + UDP app
with an optional POSIX daemon mode. This locks in:
  * the daemon pid/log defaults live under the platform temp dir, not a
    hard-coded /tmp (invalid on Windows),
  * daemonize() refuses cleanly on a platform without os.fork(), and
  * the control-room scripts ship PowerShell ports.
"""
import importlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

import pytest

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

import heartbeat_monitor  # noqa: E402

PWSH = shutil.which("pwsh") or shutil.which("powershell")

SCRIPT_STEMS = [
    "bootstrap_heartbeat_monitor",
    "start-mu2edaq-heartbeatmonitor",
    "stop-mu2edaq-heartbeatmonitor",
]


def test_daemon_defaults_live_under_platform_tempdir():
    daemon = heartbeat_monitor.DEFAULT_CONFIG["daemon"]
    tmp = tempfile.gettempdir()
    assert daemon["pid_file"] == os.path.join(tmp, "heartbeat_monitor.pid")
    assert daemon["log_file"] == os.path.join(tmp, "heartbeat_monitor.log")
    if os.name == "nt":
        assert not daemon["pid_file"].startswith("/tmp")


@pytest.mark.skipif(hasattr(os, "fork"),
                    reason="only meaningful where os.fork is absent (Windows)")
def test_daemonize_refuses_without_fork():
    with pytest.raises(Exception):
        heartbeat_monitor.daemonize(
            os.path.join(tempfile.gettempdir(), "hb.pid"),
            os.path.join(tempfile.gettempdir(), "hb.log"))


def test_scripts_have_both_forms():
    for stem in SCRIPT_STEMS:
        assert (REPO / f"{stem}.sh").is_file(), f"missing bash script: {stem}.sh"
        assert (REPO / f"{stem}.ps1").is_file(), f"missing PowerShell port: {stem}.ps1"


@pytest.mark.skipif(not PWSH, reason="PowerShell not available")
@pytest.mark.parametrize("stem", SCRIPT_STEMS)
def test_powershell_scripts_parse(stem):
    path = (REPO / f"{stem}.ps1").as_posix()
    code = (
        "$e=$null;"
        f"[System.Management.Automation.Language.Parser]::ParseFile('{path}',[ref]$null,[ref]$e)|Out-Null;"
        "if($e){$e|ForEach-Object{Write-Error $_};exit 1}else{exit 0}"
    )
    result = subprocess.run(
        [PWSH, "-NoProfile", "-NonInteractive", "-Command", code],
        capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stderr

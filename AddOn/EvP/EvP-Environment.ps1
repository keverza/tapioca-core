# Tapioca embedded-Python environment manager - install / repair / reset / status.
#
# NOTE: the user-facing name is "Tapioca"; the FILENAMES and the runtime path
# (%LOCALAPPDATA%\Tapioca\runtime) use the public Tapioca name. The binary and
# internal Python package remain EvP/evp for compatibility.
#
# WHY this lives OUTSIDE Archicad (and there is no in-palette button): once Archicad
# imports a compiled package (numpy after any geometry command; ezdxf pulls numpy in),
# Windows LOCKS that .pyd for the life of the process, so pip cannot reinstall/upgrade/
# remove it (WinError 5). Repair and Reset therefore only work reliably with Archicad
# CLOSED - which is exactly what a standalone tool can guarantee and a palette button
# cannot. A fresh install of a not-yet-imported package still works in-palette on demand
# (that is the normal `requires=[...]` path); this tool is for setup and recovery.
#
# Double-click EvP-Setup.cmd for the menu, or run with -Action:
#   .\EvP-Environment.ps1 -Action Install    first-time bring-up (bundled 3.12 + pip + baseline)
#   .\EvP-Environment.ps1 -Action Repair     rebuild the whole runtime from scratch (-Force)
#   .\EvP-Environment.ps1 -Action Reset      wipe managed packages, reinstall the baseline
#   .\EvP-Environment.ps1 -Action Status     show what is installed
#   .\EvP-Environment.ps1 -Action Uninstall  delete the runtime folder (fall back to system Python)
#
[CmdletBinding()]
param(
    [ValidateSet('Install', 'Repair', 'Reset', 'Status', 'Uninstall')]
    [string] $Action,
    [string] $Version = '3.12.10'
)

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$runtime = Join-Path $env:LOCALAPPDATA 'Tapioca\runtime'
$py      = Join-Path $runtime 'python.exe'
$envPy   = Join-Path $root 'Sources\PyPackage\evp\_env.py'   # stdlib-only; version-independent

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }

# Locked-binary guard: warn (and let the user bail) when Archicad holds the runtime open.
function Confirm-ArchicadClosed($what) {
    $procs = Get-Process -Name 'Archicad' -ErrorAction SilentlyContinue
    if (-not $procs) { return $true }
    Say ""
    Say "Archicad is RUNNING. $what needs to rewrite files the live session has locked" Yellow
    Say "(numpy/PIL .pyd's), so it will fail with 'Access is denied' until Archicad exits." Yellow
    $answer = Read-Host "Close Archicad, then type Y to continue (or anything else to cancel)"
    if ($answer -notmatch '^[Yy]') { Say "Cancelled." Yellow; return $false }
    # Re-check after they (hopefully) closed it.
    if (Get-Process -Name 'Archicad' -ErrorAction SilentlyContinue) {
        Say "Archicad is still running - cancelling to avoid a half-written runtime." Red
        return $false
    }
    return $true
}

function Invoke-Install {
    if (Test-Path $py) {
        Say "Runtime already present at $runtime." Green
        Say "Use Repair to rebuild it, or Reset to fix its packages." Gray
        return
    }
    Say "Installing the Tapioca runtime..." Cyan
    & "$root\Install-Runtime.ps1" -Version $Version
}

function Invoke-Repair {
    if (-not (Confirm-ArchicadClosed 'Repair')) { return }
    Say "Repairing (rebuilding the runtime from scratch)..." Cyan
    & "$root\Install-Runtime.ps1" -Version $Version -Force
}

function Invoke-Reset {
    if (-not (Test-Path $py)) {
        Say "No runtime at $runtime. Run Install first." Red
        return
    }
    if (-not (Confirm-ArchicadClosed 'Reset')) { return }
    Say "Resetting managed packages (wipe + reinstall baseline)..." Cyan
    Say "Per-command requires reinstall automatically the next time you run that command." Gray
    # _env.py prints a JSON result line to stdout, its pip log to stderr, and exits 0 on
    # success / non-zero on failure - use the exit code rather than parsing the JSON here.
    & $py -s -E $envPy reset
    if ($LASTEXITCODE -eq 0) { Say "Reset OK." Green } else { Say "Reset FAILED (see output above)." Red }
}

function Invoke-Status {
    Say "Runtime:  $runtime" Gray
    if (-not (Test-Path $py)) {
        Say "  NOT installed (Archicad falls back to the system Python 3.12)." Yellow
        return
    }
    $ver = & $py -s -E -c 'import platform; print(platform.python_version())'
    Say "  python:  $ver" Green
    $lock = Join-Path $runtime 'evp-env.lock.json'
    if (Test-Path $lock) { Say "  lockfile: $lock" Gray }
    Say "  installed packages:" Gray
    & $py -s -E -m pip list 2>$null | Where-Object { $_ -and $_ -notmatch '^(Package|-------)' } |
        ForEach-Object { Say "    $_" Gray }
}

function Invoke-Uninstall {
    if (-not (Test-Path $runtime)) {
        Say "Nothing to remove - no runtime at $runtime." Yellow
        return
    }
    if (-not (Confirm-ArchicadClosed 'Uninstall')) { return }
    $answer = Read-Host "Delete $runtime ? Archicad will fall back to the system Python. Type Y to confirm"
    if ($answer -notmatch '^[Yy]') { Say "Cancelled." Yellow; return }
    Say "Removing $runtime ..." Cyan
    try {
        Remove-Item $runtime -Recurse -Force -ErrorAction Stop
        Say "Runtime removed. Archicad now falls back to the system Python 3.12." Green
    } catch {
        Say ("Could not remove the runtime - a running Archicad session likely still has " +
             "a package binary locked. Close Archicad and try again. ($($_.Exception.Message))") Red
    }
}

function Show-Menu {
    while ($true) {
        Say ""
        Say "===== Tapioca Python environment =====" Cyan
        Say "  1) Install / update   (first-time bring-up)"
        Say "  2) Repair             (rebuild the runtime from scratch)"
        Say "  3) Reset packages     (wipe + reinstall baseline)"
        Say "  4) Status             (show what's installed)"
        Say "  5) Uninstall          (delete the runtime folder)"
        Say "  6) Exit"
        $choice = Read-Host "Choose"
        switch ($choice) {
            '1' { Invoke-Install }
            '2' { Invoke-Repair }
            '3' { Invoke-Reset }
            '4' { Invoke-Status }
            '5' { Invoke-Uninstall }
            '6' { return }
            default { Say "Enter 1-6." Yellow }
        }
    }
}

switch ($Action) {
    'Install' { Invoke-Install }
    'Repair'  { Invoke-Repair }
    'Reset'   { Invoke-Reset }
    'Status'    { Invoke-Status }
    'Uninstall' { Invoke-Uninstall }
    default     { Show-Menu }
}

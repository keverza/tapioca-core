# Provision the self-contained CPython 3.12 runtime Tapioca loads (E7).
#
# WHY this is a script and not a git checkout: the runtime is ~15 MB of binaries
# (the embeddable CPython distro + pip + baseline wheels), which does not belong in
# version control. Run it once per machine; it populates
#   %LOCALAPPDATA%\Tapioca\runtime
# which is slot 2 of PythonHost::ResolveRuntimeHome - so once this exists, Archicad
# loads THIS interpreter instead of whatever Python the user happens to have
# installed. Delete the folder (or -Force) to go back to the fallback.
#
# THE LAYOUT IS NOT OPTIONAL (verified empirically, see plan E7 findings):
# EvPPy initialises the embedded interpreter with PyConfig_InitIsolatedConfig, which
# IGNORES the embeddable's python312._pth and computes a STANDARD layout from home:
# home\python312.zip, home\DLLs, home\Lib, home\Lib\site-packages. The embeddable
# ships its stdlib zipped at the root with no Lib\/DLLs\, so as-is the interpreter
# cannot find `encodings` and Py_InitializeFromConfig fails. So we:
#   1. expand python312.zip           -> runtime\Lib\        (stdlib as a real dir)
#   2. move *.pyd                      -> runtime\DLLs\       (extension modules)
#   3. DELETE python312._pth                                 (so python.exe agrees:
#      standard layout + site enabled -> same Lib\site-packages the embedded zone uses)
# site then runs in BOTH zones and Lib\site-packages is on sys.path automatically -
# no EvPPy.cpp change, and pip installs land where both zones import from.
#
#   .\Install-Runtime.ps1                 populate %LOCALAPPDATA%\Tapioca\runtime
#   .\Install-Runtime.ps1 -Force          wipe and rebuild it
#   .\Install-Runtime.ps1 -Target <dir>   stage somewhere else (for verification)
#   .\Install-Runtime.ps1 -NoBaseline     skip the numpy/pillow/requests/pydantic install
#
[CmdletBinding()]
param(
    [string]   $Version    = '3.12.10',
    [string]   $Target     = (Join-Path $env:LOCALAPPDATA 'Tapioca\runtime'),
    [string[]] $Baseline   = @('numpy==2.0.2', 'pillow', 'requests', 'pydantic>=2.7,<3'),
    [switch]   $NoBaseline,
    [switch]   $Force
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$zipUrl    = "https://www.python.org/ftp/python/$Version/python-$Version-embed-amd64.zip"
$getPipUrl = 'https://bootstrap.pypa.io/get-pip.py'
$cache     = Join-Path $env:LOCALAPPDATA 'Tapioca\_provision-cache'

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }

# --- guard: never clobber silently -----------------------------------------
if (Test-Path $Target) {
    if (-not $Force) {
        throw "$Target already exists. Re-run with -Force to wipe and rebuild it, " +
              "or delete it by hand. (It is slot 2 of the runtime resolution order.)"
    }
    Say "removing existing $Target" Yellow
    try {
        Remove-Item $Target -Recurse -Force -ErrorAction Stop
    } catch {
        throw "Could not remove $Target - a running Archicad session likely has a " +
              "package binary (e.g. numpy's .pyd) loaded and locked. CLOSE Archicad " +
              "and run this again. ($($_.Exception.Message))"
    }
}

New-Item -ItemType Directory -Path $Target -Force | Out-Null
New-Item -ItemType Directory -Path $cache  -Force | Out-Null

# --- 1. fetch + extract the embeddable distro ------------------------------
$zipPath = Join-Path $cache "python-$Version-embed-amd64.zip"
if (-not (Test-Path $zipPath)) {
    Say "downloading $zipUrl"
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath
}
Say "extracting embeddable -> $Target"
Expand-Archive -Path $zipPath -DestinationPath $Target -Force

# --- 2. relayout for the isolated-config path calc -------------------------
$stdlibZip = Join-Path $Target "python$($Version.Split('.')[0])$($Version.Split('.')[1]).zip"
$libDir    = Join-Path $Target 'Lib'
$dllsDir   = Join-Path $Target 'DLLs'
New-Item -ItemType Directory -Path $libDir  -Force | Out-Null
New-Item -ItemType Directory -Path $dllsDir -Force | Out-Null

Say "expanding stdlib $([IO.Path]::GetFileName($stdlibZip)) -> Lib\"
Expand-Archive -Path $stdlibZip -DestinationPath $libDir -Force
Remove-Item $stdlibZip -Force

Say "moving *.pyd -> DLLs\"
Get-ChildItem $Target -Filter '*.pyd' -File | ForEach-Object {
    Move-Item $_.FullName (Join-Path $dllsDir $_.Name) -Force
}

# Delete the ._pth: under isolated config the embedded interpreter ignores it, and
# leaving it would make python.exe (external zone + pip) use a DIFFERENT, site-less
# path than the embedded zone. Removing it unifies both on the standard layout.
$pth = Join-Path $Target "python$($Version.Split('.')[0])$($Version.Split('.')[1])._pth"
if (Test-Path $pth) { Remove-Item $pth -Force; Say "removed $([IO.Path]::GetFileName($pth)) (unifies both zones on the standard layout)" }

New-Item -ItemType Directory -Path (Join-Path $libDir 'site-packages') -Force | Out-Null

# --- 3. sanity-check the interpreter starts --------------------------------
$py = Join-Path $Target 'python.exe'
Say "verifying interpreter startup"
$ver = & $py -s -E -c "import sys, encodings; print(sys.version.split()[0])"
if ($LASTEXITCODE -ne 0) { throw "the provisioned interpreter failed to start" }
Say "  interpreter OK: $ver" Green

# --- 4. bootstrap pip (embeddable ships without ensurepip) -----------------
$getPip = Join-Path $cache 'get-pip.py'
if (-not (Test-Path $getPip)) {
    Say "downloading get-pip.py"
    Invoke-WebRequest -Uri $getPipUrl -OutFile $getPip
}
Say "bootstrapping pip"
# -s -E: hermetic, so the user's %APPDATA% user-site never leaks into the bootstrap.
& $py -s -E $getPip --no-warn-script-location | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "pip bootstrap failed" }
$pipVer = & $py -s -E -m pip --version
Say "  $pipVer" Green

# --- 5. baseline packages (numpy for evp.geometry, etc.) -------------------
if (-not $NoBaseline -and $Baseline.Count -gt 0) {
    Say "installing baseline: $($Baseline -join ', ')"
    & $py -s -E -m pip install --no-warn-script-location @Baseline | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "baseline install failed" }
}

Say ""
Say "Provisioned $Version -> $Target" Green
Say "Slot 2 is now populated: Archicad will load this runtime on next start." Cyan
Say "Per-command 'requires' are installed on demand by evp._env at run time." Gray

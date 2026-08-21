# Install the self-contained CPython 3.12 runtime used by Tapioca.
#
# This is the release-facing installer. It has no dependency on the repository
# source tree, so it can be shipped beside a Tapioca release and run directly.
# The runtime is installed under:
#   %LOCALAPPDATA%\Tapioca\runtime
#
# The layout is required by the isolated-config path used by EvPPy:
#   1. expand python312.zip       -> runtime\Lib\
#   2. move *.pyd                  -> runtime\DLLs\
#   3. delete python312._pth       -> use the standard layout in both zones
#
#   .\Install-Runtime.ps1                 install the default runtime
#   .\Install-Runtime.ps1 -Force          wipe and rebuild it
#   .\Install-Runtime.ps1 -Target <dir>   stage somewhere else
#   .\Install-Runtime.ps1 -NoBaseline     skip baseline package installation
#
#requires -Version 5.1
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

# Never clobber an existing runtime without an explicit -Force.
if (Test-Path $Target) {
    if (-not $Force) {
        throw "$Target already exists. Re-run with -Force to wipe and rebuild it, " +
              "or delete it by hand. (It is the Tapioca runtime.)"
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
New-Item -ItemType Directory -Path $cache -Force | Out-Null

# Fetch and extract the embeddable distribution.
$zipPath = Join-Path $cache "python-$Version-embed-amd64.zip"
if (-not (Test-Path $zipPath)) {
    Say "downloading $zipUrl"
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath
}
Say "extracting embeddable -> $Target"
Expand-Archive -Path $zipPath -DestinationPath $Target -Force

# Relayout the embeddable distribution for EvPPy's isolated-config path.
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

# The embeddable ._pth file would make python.exe use a different site-less path
# than the embedded interpreter, so remove it and use the standard layout.
$pth = Join-Path $Target "python$($Version.Split('.')[0])$($Version.Split('.')[1])._pth"
if (Test-Path $pth) {
    Remove-Item $pth -Force
    Say "removed $([IO.Path]::GetFileName($pth)) (unifies both zones on the standard layout)"
}

New-Item -ItemType Directory -Path (Join-Path $libDir 'site-packages') -Force | Out-Null

# Verify the interpreter before bootstrapping pip.
$py = Join-Path $Target 'python.exe'
Say "verifying interpreter startup"
$ver = & $py -s -E -c "import sys, encodings; print(sys.version.split()[0])"
if ($LASTEXITCODE -ne 0) { throw "the provisioned interpreter failed to start" }
Say "  interpreter OK: $ver" Green

# Bootstrap pip; the embeddable distribution does not include ensurepip.
$getPip = Join-Path $cache 'get-pip.py'
if (-not (Test-Path $getPip)) {
    Say "downloading get-pip.py"
    Invoke-WebRequest -Uri $getPipUrl -OutFile $getPip
}
Say "bootstrapping pip"
& $py -s -E $getPip --no-warn-script-location | Select-Object -Last 1
if ($LASTEXITCODE -ne 0) { throw "pip bootstrap failed" }
$pipVer = & $py -s -E -m pip --version
Say "  $pipVer" Green

if (-not $NoBaseline -and $Baseline.Count -gt 0) {
    Say "installing baseline: $($Baseline -join ', ')"
    & $py -s -E -m pip install --no-warn-script-location @Baseline | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "baseline install failed" }
}

Say ""
Say "Installed Tapioca runtime $Version -> $Target" Green
Say "Archicad will load this runtime on next start." Cyan
Say "Per-command 'requires' are installed on demand by evp._env at run time." Gray

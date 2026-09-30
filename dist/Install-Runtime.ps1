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
#   .\Install-Runtime.ps1                 install or verify the default runtime
#   .\Install-Runtime.ps1 -Force          wipe and rebuild it
#   .\Install-Runtime.ps1 -Target <dir>   stage somewhere else
#   .\Install-Runtime.ps1 -NoBaseline     skip baseline package installation
#
#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidatePattern('^3\.12\.\d+$')]
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

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'Tapioca requires 64-bit Windows.'
}
if (Get-Process -Name 'Archicad*' -ErrorAction SilentlyContinue) {
    throw 'Close every Archicad session before installing or repairing the runtime.'
}
if ([string]::IsNullOrWhiteSpace($Target)) { throw 'Target cannot be empty.' }
$Target = [IO.Path]::GetFullPath($Target).TrimEnd('\', '/')
$protectedPaths = @([IO.Path]::GetPathRoot($Target), $env:USERPROFILE,
    $env:LOCALAPPDATA, $env:APPDATA, $env:WINDIR, $env:ProgramFiles,
    (Join-Path $env:LOCALAPPDATA 'Tapioca'), $PSScriptRoot)
foreach ($protected in $protectedPaths) {
    if ($protected -and $Target -ieq $protected.TrimEnd('\', '/')) {
        throw "Refusing to use a protected directory as the runtime: $Target"
    }
}

# Re-use a healthy runtime. Force is deliberately limited to a recognizable,
# non-junction Python directory rather than recursively deleting an arbitrary path.
$existing = Test-Path -LiteralPath $Target
$marker = Join-Path $Target '.tapioca-runtime'
if ($existing -and $Force) {
    $item = Get-Item -LiteralPath $Target
    $owned = (Test-Path -LiteralPath $marker -PathType Leaf) -and
        ((Get-Content -LiteralPath $marker -Raw).Trim() -eq 'Tapioca CPython 3.12 x64')
    if (-not $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
        (-not $owned -and -not (Test-Path -LiteralPath (Join-Path $Target 'python312.dll') -PathType Leaf))) {
        throw "Refusing to erase an unrecognized runtime directory: $Target"
    }
    if (Get-ChildItem -LiteralPath $Target -Recurse -Force |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
        throw "Refusing to erase a runtime containing links or junctions: $Target"
    }
    Say "removing existing $Target" Yellow
    try {
        Remove-Item -LiteralPath $Target -Recurse -Force -ErrorAction Stop
    } catch {
        throw "Could not remove $Target - a running Archicad session likely has a " +
              "package binary (e.g. numpy's .pyd) loaded and locked. CLOSE Archicad " +
              "and run this again. ($($_.Exception.Message))"
    }
    $existing = $false
}

New-Item -ItemType Directory -Path $cache -Force | Out-Null

if (-not $existing) {
    # Fetch and extract the embeddable distribution.
    $zipPath = Join-Path $cache "python-$Version-embed-amd64.zip"
    if (-not (Test-Path $zipPath)) {
        Say "downloading $zipUrl"
        Invoke-WebRequest -UseBasicParsing -Uri $zipUrl -OutFile "$zipPath.partial"
        Move-Item -LiteralPath "$zipPath.partial" -Destination $zipPath -Force
    }
    New-Item -ItemType Directory -Path $Target -Force | Out-Null
    Set-Content -LiteralPath $marker -Value 'Tapioca CPython 3.12 x64' -Encoding ASCII
    Say "extracting embeddable -> $Target"
    Expand-Archive -Path $zipPath -DestinationPath $Target -Force

    # Relayout the embeddable distribution for EvPPy's isolated-config path.
    $stdlibZip = Join-Path $Target 'python312.zip'
    $libDir    = Join-Path $Target 'Lib'
    $dllsDir   = Join-Path $Target 'DLLs'
    New-Item -ItemType Directory -Path $libDir  -Force | Out-Null
    New-Item -ItemType Directory -Path $dllsDir -Force | Out-Null

    Say "expanding stdlib $([IO.Path]::GetFileName($stdlibZip)) -> Lib\"
    Expand-Archive -Path $stdlibZip -DestinationPath $libDir -Force
    Remove-Item $stdlibZip -Force

    Say "moving *.pyd -> DLLs\"
    Get-ChildItem -LiteralPath $Target -Filter '*.pyd' -File | ForEach-Object {
        Move-Item -LiteralPath $_.FullName -Destination (Join-Path $dllsDir $_.Name) -Force
    }

    # Remove ._pth so python.exe and the embedded interpreter use the same layout.
    $pth = Join-Path $Target 'python312._pth'
    if (Test-Path -LiteralPath $pth) {
        Remove-Item -LiteralPath $pth -Force
        Say 'removed python312._pth (unifies both zones on the standard layout)'
    }

    New-Item -ItemType Directory -Path (Join-Path $libDir 'site-packages') -Force | Out-Null
} else {
    foreach ($relative in @('python.exe', 'python312.dll', 'Lib\encodings\__init__.py')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Target $relative) -PathType Leaf)) {
            throw "Existing runtime is incomplete ($relative missing). Close Archicad and re-run with -Force."
        }
    }
    if (Test-Path -LiteralPath (Join-Path $Target 'python312._pth')) {
        throw 'Existing runtime uses the unsupported embeddable layout. Re-run with -Force.'
    }
    Say "reusing runtime -> $Target"
}

# Verify the interpreter before bootstrapping pip.
$py = Join-Path $Target 'python.exe'
Say "verifying interpreter startup"
$ver = & $py -s -E -c "import sys, struct, encodings, ssl, ctypes; assert sys.version_info[:2] == (3, 12) and struct.calcsize('P') == 8; print(sys.version.split()[0])"
if ($LASTEXITCODE -ne 0) { throw "the provisioned interpreter failed to start" }
Say "  interpreter OK: $ver" Green

# Bootstrap pip; the embeddable distribution does not include ensurepip.
$pipAvailable = & $py -s -E -c "import importlib.util; print(int(importlib.util.find_spec('pip') is not None))"
if ($LASTEXITCODE -ne 0) { throw 'Could not check pip in the managed runtime.' }
if ($pipAvailable -ne '1') {
    $getPip = Join-Path $cache 'get-pip.py'
    if (-not (Test-Path $getPip)) {
        Say "downloading get-pip.py"
        Invoke-WebRequest -UseBasicParsing -Uri $getPipUrl -OutFile "$getPip.partial"
        Move-Item -LiteralPath "$getPip.partial" -Destination $getPip -Force
    }
    Say "bootstrapping pip"
    & $py -s -E $getPip --no-warn-script-location --disable-pip-version-check | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "pip bootstrap failed" }
}
$pipVer = & $py -s -E -m pip --version
if ($LASTEXITCODE -ne 0) { throw 'pip verification failed' }
Say "  $pipVer" Green

if (-not $NoBaseline -and $Baseline.Count -gt 0) {
    Say "installing baseline: $($Baseline -join ', ')"
    & $py -s -E -m pip install --no-warn-script-location --disable-pip-version-check --no-input @Baseline | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "baseline install failed" }
}

& $py -s -E -m pip check
if ($LASTEXITCODE -ne 0) { throw 'Managed runtime has conflicting dependencies. Re-run with -Force to restore the baseline.' }
if (-not $NoBaseline -and
    ($Baseline -join ';') -eq 'numpy==2.0.2;pillow;requests;pydantic>=2.7,<3') {
    & $py -s -E -c "import numpy, PIL, requests, pydantic; assert numpy.__version__ == '2.0.2'; assert 2 <= int(pydantic.__version__.split('.')[0]) < 3; print('Baseline imports OK')"
    if ($LASTEXITCODE -ne 0) { throw 'Baseline package import verification failed.' }
}

Say ""
Say "Tapioca runtime $ver ready -> $Target" Green
Say "Archicad will load this runtime on next start." Cyan
Say "Per-command 'requires' are installed on demand by evp._env at run time." Gray
if ($Target -ine (Join-Path $env:LOCALAPPDATA 'Tapioca\runtime')) {
    Say "Custom target: set EVP_PYTHON_HOME to $Target before starting Archicad." Yellow
}
if ($env:EVP_PYTHON_HOME -and $env:EVP_PYTHON_HOME -ine $Target) {
    Say "EVP_PYTHON_HOME currently points elsewhere ($env:EVP_PYTHON_HOME); it takes precedence over this runtime." Yellow
}

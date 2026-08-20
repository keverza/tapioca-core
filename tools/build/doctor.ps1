#requires -Version 5.1
<##
.SYNOPSIS
    Report whether this checkout can run its offline checks and AC29 build.

.DESCRIPTION
    Doctor is deliberately report-only. It checks machine prerequisites and invokes
    existing validation/test tools, but it never provisions software, builds the
    add-on, syncs commands, edits a registry, or moves files.

    The default dry-run is the public HelloCommand example because it has a complete default
    signature and exercises the real Python package above the fake wire. Use
    -DryRunCommand to select another command folder when its required inputs have
    suitable defaults.

.EXAMPLE
    .\tools\build\doctor.ps1

.EXAMPLE
    .\tools\build\doctor.ps1 -Strict -SkipTests

.EXAMPLE
    .\tools\build\doctor.ps1 -DryRunCommand Examples/HelloCommand
##>
[CmdletBinding()]
param (
    [switch] $Strict,
    [switch] $SkipTests,
    [switch] $SkipDryRun,
    [string] $DryRunCommand = "Examples/HelloCommand"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script:DoctorResults = @()

function Add-Result {
    param (
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][ValidateSet("PASS", "FAIL", "WARN", "SKIP")][string] $Status,
        [string] $Detail = ""
    )

    $script:DoctorResults += [pscustomobject]@{
        Name   = $Name
        Status = $Status
        Detail = $Detail
    }
}

function Find-CommandPath {
    param ([Parameter(Mandatory)][string[]] $Names)

    foreach ($name in $Names) {
        $command = Get-Command -Name $name -ErrorAction SilentlyContinue
        if ($null -ne $command) {
            if (-not [string]::IsNullOrWhiteSpace([string] $command.Source)) {
                return [string] $command.Source
            }
            return [string] $command.Path
        }
    }
    return $null
}

function Get-OutputSummary {
    param ([object[]] $Output)

    $lines = @(
        $Output |
            ForEach-Object { [string] $_ } |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
    )
    if ($lines.Count -eq 0) {
        return ""
    }

    $count = [Math]::Min(6, $lines.Count)
    $start = $lines.Count - $count
    return ($lines[$start..($lines.Count - 1)] -join " | ")
}

function Invoke-Captured {
    param (
        [Parameter(Mandatory)][string] $Path,
        [string[]] $Arguments = @(),
        [string] $WorkingDirectory = $repoRoot
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{
            Started = $false
            ExitCode = 127
            Output = @()
            Error = "not found: $Path"
        }
    }

    $output = @()
    $errorText = ""
    $exitCode = 0
    $pushed = $false
    try {
        Push-Location -LiteralPath $WorkingDirectory
        $pushed = $true
        # Reset the inherited native status so a PowerShell script that does not
        # launch a native process cannot accidentally inherit an earlier failure.
        $global:LASTEXITCODE = 0
        $output = @(& $Path @Arguments 2>&1)
        $exitCode = [int] $global:LASTEXITCODE
    } catch {
        $errorText = $_.Exception.Message
        $exitCode = 1
    } finally {
        if ($pushed) {
            Pop-Location
        }
    }

    return [pscustomobject]@{
        Started = [string]::IsNullOrWhiteSpace($errorText)
        ExitCode = $exitCode
        Output = $output
        Error = $errorText
    }
}

function Invoke-ProgramCheck {
    param (
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Path,
        [string[]] $Arguments = @(),
        [string] $WorkingDirectory = $repoRoot
    )

    $result = Invoke-Captured -Path $Path -Arguments $Arguments -WorkingDirectory $WorkingDirectory
    if ($result.Started -and $result.ExitCode -eq 0) {
        $detail = Get-OutputSummary $result.Output
        Add-Result -Name $Name -Status PASS -Detail $detail
        return $true
    }

    $detailParts = @()
    if (-not [string]::IsNullOrWhiteSpace($result.Error)) {
        $detailParts += $result.Error
    }
    $detailParts += "exit $($result.ExitCode)"
    $outputSummary = Get-OutputSummary $result.Output
    if (-not [string]::IsNullOrWhiteSpace($outputSummary)) {
        $detailParts += $outputSummary
    }
    Add-Result -Name $Name -Status FAIL -Detail ($detailParts -join "; ")
    return $false
}

function Add-Skip {
    param ([Parameter(Mandatory)][string] $Name, [Parameter(Mandatory)][string] $Reason)
    Add-Result -Name $Name -Status SKIP -Detail $Reason
}

Write-Host "Tapioca doctor: $repoRoot" -ForegroundColor Cyan
Write-Host "Report-only; no build, provisioning, sync, task edits, or file moves are performed."
Write-Host ""

# ---------------------------------------------------------------- prerequisites --
$pythonPath = Find-CommandPath @("python.exe", "python")
$pythonAvailable = $null -ne $pythonPath
$pythonDependencies = $false
$cmakePath = $null
$powershellPath = Find-CommandPath @("powershell.exe", "pwsh")
$vsInfo = $null

if ($pythonAvailable) {
    $version = Invoke-Captured -Path $pythonPath -Arguments @(
        "-c",
        "import sys; print('Python ' + '.'.join(str(part) for part in sys.version_info[:3])); raise SystemExit(0 if sys.version_info >= (3, 10) else 1)"
    )
    if ($version.Started -and $version.ExitCode -eq 0) {
        Add-Result -Name "Python" -Status PASS -Detail (Get-OutputSummary $version.Output)
    } else {
        Add-Result -Name "Python" -Status FAIL -Detail "Python could not report its version."
        $pythonAvailable = $false
    }

    $packages = Invoke-Captured -Path $pythonPath -Arguments @(
        "-c",
        "import importlib.util; required=['pytest','yaml','jsonschema','ruff']; missing=[name for name in required if importlib.util.find_spec(name) is None]; print('missing: ' + ', '.join(missing) if missing else 'pytest, PyYAML, jsonschema, and Ruff available'); raise SystemExit(1 if missing else 0)"
    )
    if ($packages.Started -and $packages.ExitCode -eq 0) {
        Add-Result -Name "Python development packages" -Status PASS -Detail (Get-OutputSummary $packages.Output)
        $pythonDependencies = $true
    } else {
        Add-Result -Name "Python development packages" -Status FAIL -Detail (Get-OutputSummary $packages.Output)
    }
} else {
    Add-Result -Name "Python" -Status FAIL -Detail "python.exe was not found on PATH."
}

$gitPath = Find-CommandPath @("git.exe", "git")
if ($null -eq $gitPath) {
    Add-Result -Name "Git" -Status FAIL -Detail "git.exe was not found on PATH."
} else {
    Invoke-ProgramCheck -Name "Git" -Path $gitPath -Arguments @("--version") | Out-Null
}

if ($PSVersionTable.PSVersion.Major -ge 5) {
    Add-Result -Name "Windows PowerShell" -Status PASS -Detail $PSVersionTable.PSVersion.ToString()
} else {
    Add-Result -Name "Windows PowerShell" -Status FAIL -Detail "PowerShell 5.1 or newer is required."
}

$findCMake = Join-Path $repoRoot "AddOn\EvP\Find-CMake.ps1"
if (-not (Test-Path -LiteralPath $findCMake -PathType Leaf)) {
    Add-Result -Name "CMake" -Status FAIL -Detail "Find-CMake.ps1 is missing."
} else {
    try {
        . $findCMake
        $cmakePath = Find-CMake
        $cmakeVersion = Invoke-Captured -Path $cmakePath -Arguments @("--version")
        if ($cmakeVersion.Started -and $cmakeVersion.ExitCode -eq 0) {
            Add-Result -Name "CMake" -Status PASS -Detail (Get-OutputSummary $cmakeVersion.Output)
        } else {
            Add-Result -Name "CMake" -Status FAIL -Detail "CMake was found but did not report a version."
            $cmakePath = $null
        }
    } catch {
        Add-Result -Name "CMake" -Status FAIL -Detail $_.Exception.Message
    }
}

$findVSToolset = Join-Path $repoRoot "AddOn\EvP\Find-VSToolset.ps1"
if (-not (Test-Path -LiteralPath $findVSToolset -PathType Leaf)) {
    Add-Result -Name "Visual Studio v143 x64 toolset" -Status FAIL -Detail "Find-VSToolset.ps1 is missing."
} else {
    try {
        . $findVSToolset
        $vsInfo = Find-VSToolset
        Add-Result -Name "Visual Studio v143 x64 toolset" -Status PASS -Detail "$($vsInfo.Name), MSVC $($vsInfo.ToolsVersion)"
    } catch {
        Add-Result -Name "Visual Studio v143 x64 toolset" -Status FAIL -Detail $_.Exception.Message
    }
}

if ($null -eq $vsInfo) {
    Add-Skip -Name "ATL headers" -Reason "No usable v143 Visual Studio instance was found."
} else {
    $msvcMajorMinor = [regex]::Match([string] $vsInfo.ToolsVersion, "^[0-9]+\.[0-9]+").Value
    $atlPattern = Join-Path $vsInfo.Instance "VC\Tools\MSVC\$msvcMajorMinor*\atlmfc\include\atlbase.h"
    $atlHeader = Get-ChildItem -Path $atlPattern -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $atlHeader) {
        Add-Result -Name "ATL headers" -Status FAIL -Detail "atlbase.h was not found for pinned MSVC $msvcMajorMinor; install C++ ATL for v143 x64/x86."
    } else {
        Add-Result -Name "ATL headers" -Status PASS -Detail $atlHeader.FullName
    }
}

$nodePath = Find-CommandPath @("node.exe", "node")
if ($null -eq $nodePath) {
    Add-Result -Name "Node.js" -Status FAIL -Detail "node.exe was not found; the NotebookUI CMake target needs npm."
} else {
    Invoke-ProgramCheck -Name "Node.js" -Path $nodePath -Arguments @("--version") | Out-Null
}

$npmPath = Find-CommandPath @("npm.cmd", "npm.exe", "npm")
if ($null -eq $npmPath) {
    Add-Result -Name "npm" -Status FAIL -Detail "npm was not found; CMake invokes npm ci for NotebookUI."
} else {
    Invoke-ProgramCheck -Name "npm" -Path $npmPath -Arguments @("--version") | Out-Null
}

$buildFiles = @(
    "tools\build\Build-AddOn29.ps1",
    "AddOn\EvP\Initialize-Build29.ps1",
    "AddOn\EvP\CMakeLists.txt",
    "AddOn\EvP\tests\cpp\Invoke-CppTests.ps1"
)
$missingBuildFiles = @($buildFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $repoRoot $_) -PathType Leaf) })
if ($missingBuildFiles.Count -eq 0) {
    Add-Result -Name "Build entry points" -Status PASS -Detail "build, configure, and offline C++ test scripts are present."
} else {
    Add-Result -Name "Build entry points" -Status FAIL -Detail ("missing " + ($missingBuildFiles -join ", "))
}

$referenceProvisioner = Join-Path $repoRoot "tools\build\provision-reference.ps1"
$referenceCatalog = @(
    (Join-Path $repoRoot "AddOn\reference\CATALOG.yaml"),
    (Join-Path $repoRoot "tools\build\reference-catalog.yaml")
) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not (Test-Path -LiteralPath $referenceProvisioner -PathType Leaf)) {
    Add-Result -Name "Reference provisioner" -Status FAIL -Detail "tools/build/provision-reference.ps1 is missing."
} elseif (-not (Test-Path -LiteralPath $referenceCatalog -PathType Leaf)) {
    Add-Result -Name "Reference catalog" -Status FAIL -Detail "AddOn/reference/CATALOG.yaml is missing."
} elseif (-not $pythonAvailable) {
    Add-Skip -Name "Reference catalog" -Reason "cannot validate CATALOG.yaml without Python."
} elseif ($null -eq $powershellPath) {
    Add-Result -Name "Reference catalog" -Status FAIL -Detail "PowerShell executable was not found for the catalog validation child process."
} else {
    $referenceBuild = Invoke-Captured -Path $powershellPath -Arguments @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $referenceProvisioner,
        "-ValidateOnly", "-Profile", "build"
    )
    if ($referenceBuild.Started -and $referenceBuild.ExitCode -eq 0) {
        Add-Result -Name "Build reference tree" -Status PASS -Detail (Get-OutputSummary $referenceBuild.Output)
    } else {
        Add-Result -Name "Build reference tree" -Status FAIL -Detail (Get-OutputSummary $referenceBuild.Output)
    }

    $referenceTest = Invoke-Captured -Path $powershellPath -Arguments @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $referenceProvisioner,
        "-ValidateOnly", "-Profile", "test"
    )
    if ($referenceTest.Started -and $referenceTest.ExitCode -eq 0) {
        Add-Result -Name "Test reference tree" -Status PASS -Detail (Get-OutputSummary $referenceTest.Output)
    } else {
        Add-Result -Name "Test reference tree" -Status FAIL -Detail (Get-OutputSummary $referenceTest.Output)
    }
}

$runtimeCandidates = @()
if (-not [string]::IsNullOrWhiteSpace($env:EVP_PYTHON_HOME)) {
    $runtimeCandidates += Join-Path $env:EVP_PYTHON_HOME "python312.dll"
}
if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
    $runtimeCandidates += Join-Path $env:LOCALAPPDATA "Tapioca\runtime\python312.dll"
    $runtimeCandidates += Join-Path $env:LOCALAPPDATA "Programs\Python\Python312\python312.dll"
}
$runtime = $runtimeCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($null -ne $runtime) {
    Add-Result -Name "Embedded Python runtime" -Status PASS -Detail $runtime
} else {
    Add-Result -Name "Embedded Python runtime" -Status WARN -Detail "No python312.dll found; run AddOn/EvP/Install-Runtime.ps1 before live add-on use."
}

$archicadCandidates = @()
foreach ($programRoot in @($env:ProgramFiles, ${env:ProgramW6432}, ${env:ProgramFiles(x86)})) {
    if (-not [string]::IsNullOrWhiteSpace($programRoot)) {
        $archicadCandidates += Join-Path $programRoot "GRAPHISOFT\Archicad 29\Archicad.exe"
    }
}
$archicad = $archicadCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($null -ne $archicad) {
    Add-Result -Name "Archicad 29 installation" -Status PASS -Detail $archicad
} else {
    Add-Result -Name "Archicad 29 installation" -Status WARN -Detail "Archicad 29 was not found; live add-on checks will not be possible."
}

$runningArchicad = @(Get-Process -Name "Archicad" -ErrorAction SilentlyContinue)
if ($runningArchicad.Count -eq 0) {
    Add-Result -Name "Archicad build lock" -Status PASS -Detail "Archicad is not running."
} else {
    Add-Result -Name "Archicad build lock" -Status WARN -Detail "Close Archicad before building; its loaded add-on can lock build outputs."
}

$clangFormat = Find-CommandPath @("clang-format.exe", "clang-format")
if ($null -eq $clangFormat) {
    Add-Result -Name "clang-format" -Status WARN -Detail "Not found; check_cpp.py will report the touched-file style check as skipped."
} else {
    Add-Result -Name "clang-format" -Status PASS -Detail $clangFormat
}

# ---------------------------------------------------------------- deterministic checks --
$governanceTool = Join-Path $repoRoot "tools\tapioca.py"
if (-not $pythonAvailable) {
    Add-Skip -Name "Governance validation" -Reason "Python is unavailable."
    Add-Skip -Name "Architecture checks" -Reason "Python is unavailable."
    Add-Skip -Name "Command palette scan" -Reason "Python is unavailable."
} else {
    if (-not (Test-Path -LiteralPath $governanceTool -PathType Leaf)) {
        Add-Skip -Name "Governance validation" -Reason "tools/tapioca.py is not present; this is a public core checkout without the private registry."
    } elseif (-not $pythonDependencies) {
        Add-Skip -Name "Governance validation" -Reason "PyYAML or jsonschema is unavailable."
    } else {
        $validateArgs = @($governanceTool, "validate")
        if ($Strict) {
            $validateArgs += "--strict"
        } else {
            $validateArgs += "--warn-only"
        }
        Invoke-ProgramCheck -Name "Governance validation" -Path $pythonPath -Arguments $validateArgs | Out-Null
    }

    Invoke-ProgramCheck -Name "Architecture checks" -Path $pythonPath -Arguments @(
        (Join-Path $repoRoot "tools\quality\check_cpp.py"), "-v"
    ) | Out-Null
    Invoke-ProgramCheck -Name "Palette seam check" -Path $pythonPath -Arguments @(
        (Join-Path $repoRoot "tools\quality\check_structure.py")
    ) | Out-Null
    Invoke-ProgramCheck -Name "Command palette scan" -Path $pythonPath -Arguments @(
        (Join-Path $repoRoot "tools\quality\check_python.py"), "--scan-only"
    ) | Out-Null
    # Needs no third-party package, so it runs even when the dev dependencies are missing.
    Invoke-ProgramCheck -Name "Secret and provider isolation" -Path $pythonPath -Arguments @(
        (Join-Path $repoRoot "tools\quality\check_secrets.py")
    ) | Out-Null
}

if ($SkipTests) {
    Add-Skip -Name "Python test suite" -Reason "-SkipTests was requested."
    Add-Skip -Name "Offline C++ test suite" -Reason "-SkipTests was requested."
} elseif (-not $pythonAvailable -or -not $pythonDependencies) {
    Add-Skip -Name "Python test suite" -Reason "Python test dependencies are unavailable."
    Add-Skip -Name "Offline C++ test suite" -Reason "Python/CMake prerequisites are unavailable."
} else {
    Invoke-ProgramCheck -Name "Python test suite" -Path $pythonPath -Arguments @("-m", "pytest", "-q") | Out-Null
    if ($null -eq $cmakePath) {
        Add-Skip -Name "Offline C++ test suite" -Reason "CMake is unavailable."
    } elseif ($null -eq $powershellPath) {
        Add-Skip -Name "Offline C++ test suite" -Reason "PowerShell executable is unavailable."
    } else {
        Invoke-ProgramCheck -Name "Offline C++ test suite" -Path $powershellPath -Arguments @(
            "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            (Join-Path $repoRoot "AddOn\EvP\tests\cpp\Invoke-CppTests.ps1")
        ) | Out-Null
    }
}

if ($SkipDryRun) {
    Add-Skip -Name "Command dry-run" -Reason "-SkipDryRun was requested."
} elseif (-not $pythonAvailable) {
    Add-Skip -Name "Command dry-run" -Reason "Python is unavailable."
} else {
    $dryRunFolder = if ([IO.Path]::IsPathRooted($DryRunCommand)) {
        $DryRunCommand
    } elseif (Test-Path -LiteralPath (Join-Path $repoRoot $DryRunCommand) -PathType Container) {
        Join-Path $repoRoot $DryRunCommand
    } else {
        Join-Path $repoRoot (Join-Path "AddOn\EvP\Commands" $DryRunCommand)
    }
    if (-not (Test-Path -LiteralPath $dryRunFolder -PathType Container)) {
        Add-Result -Name "Command dry-run" -Status FAIL -Detail "Command folder was not found: $dryRunFolder"
    } else {
        $dryRunScript = Join-Path $repoRoot "AddOn\EvP\tests\dryrun_command.py"
        Invoke-ProgramCheck -Name "Command dry-run" -Path $pythonPath -Arguments @(
            $dryRunScript, (Resolve-Path -LiteralPath $dryRunFolder).Path
        ) | Out-Null
    }
}

# ---------------------------------------------------------------- report --
Write-Host ""
Write-Host "Doctor report" -ForegroundColor Cyan
foreach ($result in $script:DoctorResults) {
    $detail = if ([string]::IsNullOrWhiteSpace($result.Detail)) { "" } else { " - $($result.Detail)" }
    $color = switch ($result.Status) {
        "PASS" { "Green" }
        "FAIL" { "Red" }
        "WARN" { "Yellow" }
        default { "DarkGray" }
    }
    Write-Host ("[{0,-4}] {1}{2}" -f $result.Status, $result.Name, $detail) -ForegroundColor $color
}

$failures = @($script:DoctorResults | Where-Object { $_.Status -eq "FAIL" })
$warnings = @($script:DoctorResults | Where-Object { $_.Status -eq "WARN" })
Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host ("Doctor found {0} failure(s) and {1} warning(s)." -f $failures.Count, $warnings.Count) -ForegroundColor Red
    exit 1
}
if ($Strict -and $warnings.Count -gt 0) {
    Write-Host ("Doctor found {0} warning(s) in strict mode." -f $warnings.Count) -ForegroundColor Yellow
    exit 1
}
Write-Host ("Doctor passed with {0} warning(s)." -f $warnings.Count) -ForegroundColor Green
exit 0

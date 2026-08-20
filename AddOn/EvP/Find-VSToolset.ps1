# Picks a Visual Studio instance that can actually build with the v143 platform
# toolset, and returns the CMake generator name, instance path, and the exact
# MSVC side-by-side version to pin.
#
# Why this exists: the build is squeezed between two _MSC_VER checks and only a
# narrow band of v143 satisfies both.
#
#   * AC29 DevKit  (Support/Modules/GSRoot/Definitions.hpp)  1930 <= _MSC_VER < 1950
#   * bx / bgfx    (bx/include/bx/platform.h)                _MSC_VER >= 1935
#
# So the usable window is MSVC 14.35 .. 14.49 — v143, but NOT the earliest v143.
# VS2022 ships this as its default toolset; VS18 (2026) defaults to v145
# (14.5x, _MSC_VER 1951) and only offers v143 as a side-by-side component.
#
# A machine can have VS18 with a *partial* or *stale* v143 and still configure:
#   - the ARM-only component installs headers but no x64 CRT -> LNK1104 libcmt.lib
#   - the 14.30 component satisfies Archicad but not bx      -> C2338 in platform.h
# Both of those were real failures on this box, so we check three things:
#   1) MSBuild\Microsoft\VC\*\Platforms\x64\PlatformToolsets\v143
#   2) VC\Tools\MSVC\<ver>\lib\x64\libcmt.lib actually present
#   3) <ver> inside the 14.35 .. 14.49 window above
#
# The winning version is pinned explicitly (-T v143,version=...) rather than left
# to MSBuild's Microsoft.VCToolsVersion.v143.default.props, whose <Choose> block
# silently picks whichever SxS toolset happens to be installed.
#
# Dot-sourced by Initialize-Build29.ps1.

# MSVC minor version bounds, inclusive. 14.35 is bx's floor (_MSC_VER 1935);
# 14.49 is the DevKit's ceiling (_MSC_VER < 1950).
$script:V143MinMinor = 35
$script:V143MaxMinor = 49

function Get-VSGeneratorName ([int] $major) {
    switch ($major) {
        16 { "Visual Studio 16 2019" }
        17 { "Visual Studio 17 2022" }
        18 { "Visual Studio 18 2026" }
        default { $null }
    }
}

# Returns @{ Best = <version string or $null>; Rejected = @(<version strings>) }.
# "Rejected" holds v143 toolsets that have an x64 CRT but fall outside the
# window - that is the case worth naming in the error, because everything looks
# installed and the build dies deep inside a vendored header instead.
function Get-V143X64Toolsets ([string] $vsPath) {
    $result = @{ Best = $null; Rejected = @() }

    $hasToolsetProps = Get-ChildItem -Path (Join-Path $vsPath "MSBuild\Microsoft\VC") -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "Platforms\x64\PlatformToolsets\v143\Toolset.props" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $hasToolsetProps) { return $result }

    $usable = @()
    Get-ChildItem -Path (Join-Path $vsPath "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^14\.(\d\d)\.' } |
        ForEach-Object {
            $minor = [int] $Matches[1]
            # No x64 CRT means the ARM-only component - not a candidate at all.
            if (-not (Test-Path (Join-Path $_.FullName "lib\x64\libcmt.lib"))) { return }
            if ($minor -ge $script:V143MinMinor -and $minor -le $script:V143MaxMinor) {
                $usable += [pscustomobject]@{ Minor = $minor; Version = $_.Name }
            } elseif ($minor -lt $script:V143MinMinor) {
                $result.Rejected += $_.Name
            }
        }

    $result.Best = ($usable | Sort-Object Minor -Descending | Select-Object -First 1).Version
    return $result
}

# Returns a hashtable:
#   @{ Generator = "Visual Studio 18 2026"; Instance = "<path>"; Name = "<display>";
#      ToolsVersion = "14.44.35207" }
# Throws with an actionable message if no instance can build v143 x64 in-window.
function Find-VSToolset {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found - no Visual Studio installation detected."
    }

    $instances = & $vswhere -all -prerelease -products * -format json | ConvertFrom-Json
    $candidates = @()
    $nearMisses = @()
    foreach ($i in $instances) {
        $major = [int] ($i.installationVersion -split '\.')[0]
        $gen = Get-VSGeneratorName $major
        if (-not $gen) { continue }
        $tools = Get-V143X64Toolsets $i.installationPath
        if ($tools.Best) {
            $candidates += [pscustomobject]@{
                Major        = $major
                Generator    = $gen
                Instance     = $i.installationPath
                Name         = $i.displayName
                ToolsVersion = $tools.Best
            }
        } elseif ($tools.Rejected.Count -gt 0) {
            $nearMisses += "  - $($i.displayName) at $($i.installationPath)`n      has only: $($tools.Rejected -join ', ')  (too old - bx needs 14.$script:V143MinMinor+)"
        }
    }

    if ($candidates.Count -eq 0) {
        $seen = ($instances | ForEach-Object { "  - $($_.displayName) ($($_.installationVersion)) at $($_.installationPath)" }) -join "`n"
        $near = if ($nearMisses.Count -gt 0) { "`nv143 IS installed here, but out of window:`n" + ($nearMisses -join "`n") + "`n" } else { "" }
        # Printed rather than thrown: PowerShell echoes a thrown multi-line
        # string twice (message + FullyQualifiedErrorId), which buries it.
        Write-Host @"
No Visual Studio instance can build with a usable v143 (VS2022) toolset for x64.

The build needs MSVC 14.$script:V143MinMinor .. 14.$script:V143MaxMinor, squeezed between two checks:
  * the AC29 DevKit rejects _MSC_VER outside 1930-1949 (Definitions.hpp)
  * bx/bgfx rejects _MSC_VER below 1935 (bx/include/bx/platform.h)
VS18's default v145 is 1951 - too new. The 14.30 component is 1930 - too old.

Installed instances:
$seen
$near
Fix: in the Visual Studio Installer, add the individual component
  "MSVC v143 - VS 2022 C++ x64/x86 build tools (v14.44)"
(the x64/x86 one - the ARM variant installs headers but no x64 CRT, which
configures fine and then fails at link with LNK1104 'libcmt.lib'), or from a
shell:

  & "`${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe" modify ``
      --installPath "<one of the paths above>" ``
      --add Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64 --passive --norestart
"@ -ForegroundColor Yellow
        throw "No usable v143 x64 toolset installed - see the instructions above."
    }

    # Prefer the oldest qualifying VS: v143 is that generation's native toolset,
    # so it is the least likely to hit side-by-side gaps.
    $pick = $candidates | Sort-Object Major | Select-Object -First 1
    return @{
        Generator    = $pick.Generator
        Instance     = $pick.Instance
        Name         = $pick.Name
        ToolsVersion = $pick.ToolsVersion
    }
}

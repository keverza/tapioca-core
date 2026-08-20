# Configure the AC29 Visual Studio project for EvP.
# The generator is detected per machine (VS2022 on one dev box, VS18/2026 on the
# other); the v143 toolset is fixed, because the Archicad DevKit checks
# _MSC_VER 1930-1949 in Definitions.hpp and bx checks _MSC_VER >= 1935. The exact
# side-by-side MSVC version is pinned rather than left to MSBuild's default
# <Choose> block. See Find-VSToolset.ps1 for what "has v143" means and what to
# install when it is missing.
# Needs Python 3.10+ on PATH.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$root\Find-CMake.ps1"
. "$root\Find-VSToolset.ps1"
$cmake = Find-CMake
$vs = Find-VSToolset
$toolset = "v143,version=$($vs.ToolsVersion)"
Write-Host "Using cmake: $cmake" -ForegroundColor Cyan
Write-Host "Using $($vs.Generator) [$($vs.Name)] with toolset $toolset" -ForegroundColor Cyan

$build = Join-Path $root "build_29"
$devkit = if (-not [string]::IsNullOrWhiteSpace($env:AC_API_DEVKIT_DIR)) {
    $env:AC_API_DEVKIT_DIR
} else {
    Join-Path $root "..\reference\archicad29-api-devkit"
}

# A cache generated for a different VS instance cannot be reused - drop it so a
# machine switch (or a newly installed toolset) reconfigures cleanly.
$cacheFile = Join-Path $build "CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cache = Get-Content $cacheFile
    # Compare the parsed values, not the raw lines: CMake writes the generator as
    # :INTERNAL= but our -D instance lands as :UNINITIALIZED=, and either may use
    # '\' or '/'.
    $cachedGen = ($cache | Select-String -Pattern '^CMAKE_GENERATOR:[A-Z]+=(.*)$').Matches.Groups[1].Value
    $cachedInst = ($cache | Select-String -Pattern '^CMAKE_GENERATOR_INSTANCE:[A-Z]+=(.*)$').Matches.Groups[1].Value
    # The toolset is pinned to an exact MSVC version, so a newly installed (or
    # removed) side-by-side toolset must also invalidate the cache - CMake will
    # not change CMAKE_GENERATOR_TOOLSET in place.
    $cachedTs = ($cache | Select-String -Pattern '^CMAKE_GENERATOR_TOOLSET:[A-Z]+=(.*)$').Matches.Groups[1].Value
    $genOk = $cachedGen -eq $vs.Generator
    $instOk = ($cachedInst -replace '/', '\').TrimEnd('\') -eq $vs.Instance.TrimEnd('\')
    $tsOk = $cachedTs -eq $toolset
    if (-not ($genOk -and $instOk -and $tsOk)) {
        Write-Host "Stale build_29 cache (different VS instance/generator/toolset) - removing." -ForegroundColor Yellow
        Remove-Item -Recurse -Force $build
    }
}

& $cmake -G $vs.Generator -T $toolset -A x64 `
    -DCMAKE_GENERATOR_INSTANCE="$($vs.Instance)" `
    -DAC_API_DEVKIT_DIR="$devkit" `
    -DAC_ADDON_LANGUAGE="INT" `
    -S "$root" -B "$build"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

Write-Host "Configured. Open build_29\EvP.sln or run Build-AddOn29.ps1." -ForegroundColor Green

# Configure, build and run the L2 geometry tests (docs/guides/testing.md §2).
#
#   .\Invoke-CppTests.ps1              plain build
#   .\Invoke-CppTests.ps1 -Sanitize    MSVC AddressSanitizer build
#
# Needs no Archicad, no DevKit, no Python — that is the point of L2.
param (
    [switch] $Sanitize,
    [string] $Config = "Debug",
    [string] $Filter = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$root\..\..\Find-CMake.ps1"
$cmake = Find-CMake
$ctest = Join-Path (Split-Path $cmake) "ctest.exe"

$buildDir = if ($Sanitize) { "$root\build-asan" } else { "$root\build" }
$sanFlag  = if ($Sanitize) { "-DEVP_SANITIZE=ON" } else { "-DEVP_SANITIZE=OFF" }

& $cmake -S $root -B $buildDir $sanFlag
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
& $cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

if ($Sanitize) {
    # The ASan runtime (clang_rt.asan_dynamic-x86_64.dll) ships inside the MSVC
    # toolchain and is NOT on PATH by default. Without it the test exe dies with
    # 0xC0000135 STATUS_DLL_NOT_FOUND before main() and looks like a crash.
    $asanDll = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio" -Recurse `
        -Filter "clang_rt.asan_dynamic-x86_64.dll" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*Hostx64\x64*" } | Select-Object -First 1
    if ($null -eq $asanDll) { throw "ASan runtime DLL not found in the MSVC toolchain." }
    $env:PATH = "$($asanDll.DirectoryName);$env:PATH"
    Write-Host "ASan runtime: $($asanDll.DirectoryName)" -ForegroundColor DarkGray
}

$exe = Join-Path $buildDir "$Config\EvPGeomTests.exe"
if ($Filter -ne "") {
    & $exe "--gtest_filter=$Filter"
} else {
    & $ctest --test-dir $buildDir -C $Config --output-on-failure
}
if ($LASTEXITCODE -ne 0) { throw "Tests failed (exit $LASTEXITCODE)." }
Write-Host "Geometry tests passed ($Config$(if ($Sanitize) { ', ASan' }))." -ForegroundColor Green

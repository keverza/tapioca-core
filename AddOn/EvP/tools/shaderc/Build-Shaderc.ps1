# Build bgfx's shaderc, HLSL-only, into tools\shaderc\bin\shaderc.exe.
#
# Run this ONCE (and again only when the vendored bgfx tree changes). The .bin.h
# files it produces are checked in, so an ordinary Build-AddOn29.ps1 never needs it —
# which is the point: the add-on's build must not depend on a shader compiler
# being present.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. "$root\..\..\Find-CMake.ps1"
$cmake = Find-CMake

& $cmake -S "$root" -B "$root\build" -A x64
if ($LASTEXITCODE -ne 0) { throw "shaderc configure failed (exit $LASTEXITCODE)." }

& $cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "shaderc build failed (exit $LASTEXITCODE)." }

Write-Host "Built -> $root\bin\shaderc.exe" -ForegroundColor Green

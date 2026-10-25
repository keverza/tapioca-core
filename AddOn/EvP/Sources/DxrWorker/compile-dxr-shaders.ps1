# Regenerate DxrOcclusionDxil.h from DxrOcclusion.hlsl.
#
# The DXIL is committed so ordinary builds need no shader compiler; rerun this
# after editing the HLSL and commit both files. Uses the Windows SDK's dxc.exe
# (its dxil.dll beside it signs the container, which D3D12 requires outside
# developer mode). Pass -Dxc to use another compiler.
param ([string] $Dxc = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrWhiteSpace($Dxc)) {
    $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
    $Dxc = Get-ChildItem -Path $kits -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.' } | Sort-Object { [version] $_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\dxc.exe" } | Where-Object { Test-Path $_ } |
        Select-Object -First 1
}
if (-not $Dxc -or -not (Test-Path $Dxc)) { throw "dxc.exe not found; install the Windows SDK or pass -Dxc." }
if (-not (Test-Path (Join-Path (Split-Path -Parent $Dxc) "dxil.dll"))) {
    throw "dxil.dll is not beside $Dxc; the DXIL would be unsigned."
}

$source = Join-Path $here "DxrOcclusion.hlsl"
$header = Join-Path $here "DxrOcclusionDxil.h"
Write-Host "Using $Dxc" -ForegroundColor Cyan
& $Dxc -nologo -T lib_6_3 -O3 -Zpr -Vn kDxrOcclusionDxil -Fh $header $source
if ($LASTEXITCODE -ne 0) { throw "dxc failed (exit $LASTEXITCODE)." }
Write-Host "Wrote $header" -ForegroundColor Green

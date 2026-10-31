param (
    [string] $BuildDir = "$PSScriptRoot\build",
    [ValidateSet("Debug", "RelWithDebInfo", "Release")] [string] $Config = "RelWithDebInfo",
    [switch] $BuildOnly,
    [switch] $Fresh
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\..\Find-CMake.ps1"
$cmake = Find-CMake
$timer = [Diagnostics.Stopwatch]::StartNew()
& $cmake -S $PSScriptRoot -B $BuildDir -A x64
if ($LASTEXITCODE -ne 0) { throw "Playground configure failed." }
& $cmake --build $BuildDir --config $Config --target TapiocaUIPlayground PlaygroundTests --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Playground build failed." }
Write-Host ("Configure + build: {0:N0} ms" -f $timer.Elapsed.TotalMilliseconds)
& (Join-Path (Split-Path $cmake) "ctest.exe") --test-dir $BuildDir -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Playground smoke tests failed." }
if (!$BuildOnly) {
    $exe = Join-Path $BuildDir "bin\$Config\TapiocaUIPlayground.exe"
    $arguments = if ($Fresh) { @("--fresh") } else { @() }
    Start-Process -FilePath $exe -ArgumentList ($arguments + @("--interactive"))
}

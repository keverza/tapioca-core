# compile-shaders.ps1 was renamed to Build-Shaders.ps1. Keep this shim for existing callers.
& "$PSScriptRoot\Build-Shaders.ps1" @args
exit $LASTEXITCODE

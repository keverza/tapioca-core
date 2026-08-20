# build_29.ps1 was renamed to Build-AddOn29.ps1. Keep this shim for existing callers.
& "$PSScriptRoot\Build-AddOn29.ps1" @args
exit $LASTEXITCODE

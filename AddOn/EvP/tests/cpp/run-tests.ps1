# run-tests.ps1 was renamed to Invoke-CppTests.ps1. Keep this shim for existing callers.
& "$PSScriptRoot\Invoke-CppTests.ps1" @args
exit $LASTEXITCODE

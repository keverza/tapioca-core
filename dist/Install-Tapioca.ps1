# Install prerequisites and starter commands from the extracted release folder.
# The add-on stays here; register Tapioca.apx manually in Archicad's Add-On Manager.
#requires -Version 5.1
[CmdletBinding()]
param(
    [switch] $ForceRuntime,
    [switch] $SkipRuntime,
    [switch] $UpdateExamples
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'Tapioca requires 64-bit Windows.'
}
if (Get-Process -Name 'Archicad*' -ErrorAction SilentlyContinue) {
    throw 'Close every Archicad session before installing Tapioca.'
}
if ($SkipRuntime -and $ForceRuntime) { throw '-SkipRuntime and -ForceRuntime cannot be combined.' }
foreach ($relative in @('Tapioca.apx', 'EvPPy.dll', 'PyPackage\tapioca\__init__.py',
        'Install-Runtime.ps1', 'Commands\HelloCommand\command.py')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $relative) -PathType Leaf)) {
        throw "Incomplete release: $relative missing. Extract the complete Tapioca-AC29.zip and run its Tapioca\Install-Tapioca.ps1, not the flat dist folder."
    }
}

# Remove Windows download blocking only from this release's executable files.
Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File |
    Where-Object { $_.Extension -in '.apx', '.dll', '.exe', '.ps1', '.pyd', '.gha', '.rhp' } |
    Unblock-File

if (-not $SkipRuntime) {
    & (Join-Path $PSScriptRoot 'Install-Runtime.ps1') -Force:$ForceRuntime
} else {
    Write-Warning 'Runtime installation skipped; Python commands need a working x64 CPython 3.12 runtime.'
}

$commands = Join-Path $env:LOCALAPPDATA 'Tapioca\Commands'
New-Item -ItemType Directory -Path $commands -Force | Out-Null
foreach ($folder in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'Commands') -Directory) {
    $destination = Join-Path $commands $folder.Name
    if (Test-Path -LiteralPath $destination) {
        if (-not $UpdateExamples) {
            Write-Host "Keeping existing command folder: $destination"
            continue
        }
        if (-not (Test-Path -LiteralPath $destination -PathType Container) -or
            ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to update a non-directory or junction: $destination"
        }
        Get-ChildItem -LiteralPath $folder.FullName |
            Copy-Item -Destination $destination -Recurse -Force
    } else {
        Copy-Item -LiteralPath $folder.FullName -Destination $commands -Recurse
    }
}

Write-Host "Starter commands installed -> $commands" -ForegroundColor Green
Write-Host "Keep this release folder intact. In Archicad 29 Add-On Manager, add: $(Join-Path $PSScriptRoot 'Tapioca.apx')" -ForegroundColor Cyan
Write-Host 'Open Tapioca Panel from the Tapioca menu, press Rescan, then run Hello Tapioca.'

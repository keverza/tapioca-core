#requires -Version 5.1
<#
.SYNOPSIS
    Install the workspace-pinned Rust toolchain without changing the global default.
.DESCRIPTION
    Rustup itself is downloaded only with -InstallRustup. Its official SHA-256 is
    checked before execution. Neither the user PATH nor the global Rust default is
    changed. No Archicad build, deployment, or production migration is performed.
#>
[CmdletBinding()]
param ([switch] $InstallRustup)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workspace = Join-Path $repo "AddOn\EvP\rust"
$pin = Get-Content -LiteralPath (Join-Path $workspace "rust-toolchain.toml") -Raw
if ($pin -notmatch 'channel\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"') {
    throw "rust-toolchain.toml must pin an exact Rust version."
}
$version = $Matches[1]
$cargoHome = if ($env:CARGO_HOME) { $env:CARGO_HOME } else { Join-Path $env:USERPROFILE ".cargo" }
$rustup = Get-Command rustup -ErrorAction SilentlyContinue
$rustupPath = if ($rustup) { $rustup.Source } else { Join-Path $cargoHome "bin\rustup.exe" }

if (-not (Test-Path -LiteralPath $rustupPath)) {
    if (-not $InstallRustup) {
        throw "Rustup is missing. Re-run tools/build/Setup-Rust.ps1 -InstallRustup to install it."
    }
    $tempRoot = Join-Path $env:LOCALAPPDATA "Temp\opencode\tapioca-rust-setup"
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    # Rustup dispatches by executable name, so the installer must keep this name.
    $installer = Join-Path $tempRoot "rustup-init.exe"
    $url = "https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-msvc/rustup-init.exe"
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $installer
    $response = Invoke-WebRequest -UseBasicParsing -Uri "$url.sha256"
    $hashText = if ($response.Content -is [byte[]]) {
        [Text.Encoding]::UTF8.GetString($response.Content)
    } else { [string] $response.Content }
    $expected = ($hashText.Trim() -split '\s+')[0]
    if ($expected -notmatch '^[0-9a-fA-F]{64}$' -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash -ne $expected) {
        throw "Rustup installer checksum mismatch; installer was not executed."
    }
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $installer -y --no-modify-path --profile minimal --default-toolchain none --default-host x86_64-pc-windows-msvc
        if ($LASTEXITCODE -ne 0) { throw "Rustup installation failed ($LASTEXITCODE)." }
    } finally {
        $ErrorActionPreference = $previousPreference
    }
}

# Native stderr is not a failure in Windows PowerShell 5.1; exit codes are.
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    # Rustup selects the first installed toolchain as default even with `install`.
    # Preserve the previous absence of a default as well as an existing default.
    $defaultBefore = @(& $rustupPath default 2>$null)
    $hadDefault = $LASTEXITCODE -eq 0
    try {
        & $rustupPath toolchain install $version --profile minimal --component rustfmt --component clippy
        if ($LASTEXITCODE -ne 0) { throw "Rust toolchain installation failed ($LASTEXITCODE)." }
    } finally {
        if (-not $hadDefault) {
            & $rustupPath default none
            if ($LASTEXITCODE -ne 0) { throw "Could not restore the absence of a global Rust default." }
        }
    }
    & $rustupPath target add --toolchain $version x86_64-pc-windows-msvc
    if ($LASTEXITCODE -ne 0) { throw "MSVC Rust target installation failed ($LASTEXITCODE)." }
    & $rustupPath run $version rustc --version
    if ($LASTEXITCODE -ne 0) { throw "The installed compiler could not run." }
} finally {
    $ErrorActionPreference = $previousPreference
}
Write-Host "Rust $version ready. No global default/PATH change; check_rust.py locates rustup automatically."
Write-Host "Next: python tools/quality/check_rust.py"

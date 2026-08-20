# Resolves a usable cmake.exe: PATH first, then the CMake bundled with Visual
# Studio (via vswhere), then a couple of well-known locations. Dot-sourced by
# the generate/build scripts. Sets $script:CMake to the resolved path.
function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoots = & $vswhere -all -prerelease -property installationPath 2>$null
        foreach ($vs in $vsRoots) {
            $c = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $c) { return $c }
        }
    }

    foreach ($p in @(
        "C:\Program Files\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )) { if (Test-Path $p) { return $p } }

    throw "cmake.exe not found. Install CMake or run from a 'Developer PowerShell for VS'."
}

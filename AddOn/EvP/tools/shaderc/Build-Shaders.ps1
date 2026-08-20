# Compile AddOn/EvP/Shaders/*.sc into AddOn/EvP/ShadersBin/*.bin.h.
#
# ⚠️ THE OUTPUT IS CHECKED IN, AND THAT IS THE DESIGN. Build-AddOn29.ps1 never runs
# this: the add-on may not read files beside the .apx (CLAUDE.md), so shaders
# ship as BGFX_EMBEDDED_SHADER byte arrays compiled INTO the binary, and a
# checked-in .bin.h means an ordinary build needs no shader compiler present.
# Run this by hand after editing a .sc, and commit what it produces.
#
# ⚠️ dxbc ONLY. tools/shaderc is built HLSL-only (see its CMakeLists), and D3D11
# is the only backend cmake/Bgfx.cmake compiles into the .apx. Asking for
# another profile here would produce an empty variant that BGFX_EMBEDDED_SHADER
# would happily select and then fail to create a program from.
$ErrorActionPreference = "Stop"
$tool    = Split-Path -Parent $MyInvocation.MyCommand.Path
$evpRoot = Resolve-Path "$tool\..\.."
$src     = Join-Path $evpRoot "Shaders"
$out     = Join-Path $evpRoot "ShadersBin"
$shaderc = Join-Path $tool "bin\shaderc.exe"
$bgfxSrc = Resolve-Path "$evpRoot\..\reference\bgfx-master\src"

if (-not (Test-Path $shaderc)) {
    throw "shaderc.exe not found at $shaderc. Run .\Build-Shaderc.ps1 first."
}
if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }

# Every .sc except the shared includes: varying.def.sc is a declaration file and
# uniforms.sh is a header, neither is a shader.
$shaders = Get-ChildItem $src -Filter "*.sc" | Where-Object { $_.Name -ne "varying.def.sc" }

foreach ($shader in $shaders) {
    # vs_* / fs_* / cs_* is bgfx's own convention and shaderc needs the type
    # spelled out; deriving it from the prefix keeps one source of truth.
    $type = switch -Regex ($shader.Name) {
        '^vs_' { "vertex" }
        '^fs_' { "fragment" }
        '^cs_' { "compute" }
        default { throw "$($shader.Name): a shader must be named vs_*, fs_* or cs_*." }
    }

    # ⚠️ THE PROFILE IS `s_5_0` FOR BOTH STAGES, not vs_5_0/ps_5_0. shaderc's
    # profile table is generic ("s_4_0", "s_5_0") and it derives vs/ps from
    # --type; asking for vs_5_0 gets "Unknown profile" and a failed build.
    $target = Join-Path $out ($shader.BaseName + ".bin.h")
    Write-Host "  $($shader.Name) -> ShadersBin\$($shader.BaseName).bin.h  ($type, dxbc)"

    # --bin2c emits a C array instead of a raw .bin.
    # ⚠️ THE SYMBOL MUST END IN `_dxbc`. BGFX_EMBEDDED_SHADER(name) expands to
    # BGFX_EMBEDDED_SHADER_DXBC(..., name), which concatenates `name` with
    # `_dxbc` — so an array called plain `vs_archviz_mesh` links against nothing
    # and the error names a symbol that appears nowhere in the source.
    & $shaderc `
        -f $shader.FullName `
        -o $target `
        --varyingdef (Join-Path $src "varying.def.sc") `
        --type $type `
        --platform windows `
        `
        -p s_5_0 `
        -O 3 `
        --bin2c "$($shader.BaseName)_dxbc" `
        -i $bgfxSrc `
        -i $src
    if ($LASTEXITCODE -ne 0) { throw "shaderc failed on $($shader.Name) (exit $LASTEXITCODE)." }
}

Write-Host "Compiled $($shaders.Count) shader(s) -> $out" -ForegroundColor Green
Write-Host "Commit the .bin.h files: Build-AddOn29.ps1 embeds them and never regenerates them."

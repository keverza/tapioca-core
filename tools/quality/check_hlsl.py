"""Compile the ArchViz viewport's embedded HLSL offline, before Archicad sees it.

⚠️ THE C++ BUILD SAYS NOTHING ABOUT THESE SHADERS. `DiligentShaders.hpp` keeps
each stage as a raw string literal and the D3D11 backend compiles them at
DEVICE-INIT time -- deliberately, so there is no offline toolchain and no
checked-in blob that can disagree with the source (see that file's header). The
cost of that trade is that a missing brace, an undeclared identifier or a
swizzle on a scalar is a RUNTIME failure: `Build-AddOn29.ps1` succeeds, the
palette syncs, the viewport opens BLACK, and the only evidence is a line in the
log. That is a full user round trip for a typo, and the viewport's shaders are
edited constantly under PLAT-RE51.

So this runs the same concatenation `ArchVizShaderSource` does -- the shared
cbuffer, then the stage body -- through fxc, which is the compiler the D3D11
backend reaches for anyway.

⚠️ IT CHECKS COMPILATION, NOT CORRECTNESS. A shader that compiles can still
light the model wrongly, read the cbuffer at the wrong offset, or sample a
shadow map that is not bound. The debug views exist for those; this only
guarantees the frame is not black for the one reason nothing else catches.

⚠️ IT SKIPS RATHER THAN FAILS WHEN fxc IS ABSENT. Build-AddOn29.ps1 runs this
before the compiler, but the add-on build itself does not need the Windows SDK
-- so a machine that can produce a working .apx must never be blocked by a
missing SDK. A skip prints and returns 0; only a real compile error returns 1.
"""

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "AddOn/EvP/Sources/AddOn/ArchViz/DiligentShaders.hpp"

# Stage -> shader model. ps_5_0/vs_5_0 is what the D3D11 backend targets at
# feature level 11; a stage compiled here under a different model would prove
# something about a compiler the add-on never uses.
STAGES = {
    "kArchVizMeshVS": "vs_5_0",
    "kArchVizShadowVS": "vs_5_0",
    "kArchVizOutlineVS": "vs_5_0",
    "kArchVizFullScreenVS": "vs_5_0",
    "kArchVizFlatPS": "ps_5_0",
    "kArchVizGBufferPS": "ps_5_0",
    "kArchVizGBufferDebugPS": "ps_5_0",
    "kArchVizAmbientOcclusionDebugPS": "ps_5_0",
    "kArchVizResolvePS": "ps_5_0",
    "kArchVizDepthRangeCS": "cs_5_0",
    "kArchVizDepthRangeSmoothCS": "cs_5_0",
    "kArchVizEnvBackgroundVS": "vs_5_0",
    "kArchVizEnvBackgroundPS": "ps_5_0",
    "kArchVizEnvPrefilterVS": "vs_5_0",
    "kArchVizEnvPrefilterPS": "ps_5_0",
    # ⚠️ THE MESH PS IS THREE LITERALS, concatenated at runtime because MSVC
    # caps a string literal at 16 KB. Compiling only the first piece would
    # check a shader with no entry point and pass on a truncated file.
    "kArchVizMeshPS": "ps_5_0",
}

# Stages whose source is more than one literal, in concatenation order.
CONTINUATIONS = {
    "kArchVizMeshPS": ["kArchVizMeshPSMain", "kArchVizMeshPSMainTail"],
}

# Literals prepended AFTER the cbuffer and BEFORE the stage's own body, matching
# the extra arguments the stage passes to ArchVizShaderSource.
#
# ⚠️ A STAGE THAT NEEDS A PRELUDE AND DOES NOT DECLARE IT HERE FAILS THIS CHECK
# EVEN THOUGH THE ADD-ON WOULD RUN FINE -- fxc sees an undeclared EnvUv. That
# noisy failure is the point: the alternative is this table silently drifting
# from DiligentScene::Init's concatenation, and then the gate compiles something
# the add-on never does.
PRELUDES = {
    "kArchVizMeshPS": ["kArchVizEnvCommonPS"],
    "kArchVizEnvBackgroundVS": ["kArchVizEnvCommonPS"],
    "kArchVizEnvBackgroundPS": ["kArchVizEnvCommonPS"],
    # ⚠️ THE PREFILTER PIXEL SHADER NEEDS THE SAME PRELUDE AS THE BACKGROUND,
    # and that is the point of RE51.B6's design rather than an accident: EnvUv
    # and its inverse EnvDir are the ONE equirectangular convention in the tree,
    # and the prefilter writes the very texture the other two read.
    "kArchVizEnvPrefilterPS": ["kArchVizEnvCommonPS"],
    # ⚠️ THE RESOLVE PS CALLS Grade(), which lives in the common prelude.
    # Without it fxc sees an undeclared identifier and fails -- which is the
    # point of the PRELUDES table: a stage that needs a prelude and does not
    # declare it here fails loudly rather than compiling a half-shader.
    "kArchVizResolvePS": ["kArchVizEnvCommonPS"],
}


def find_fxc():
    """The newest x64 fxc in the Windows SDK, or None."""
    roots = [Path(r"C:\Program Files (x86)\Windows Kits\10\bin"),
             Path(r"C:\Program Files\Windows Kits\10\bin")]
    found = []
    for root in roots:
        if root.is_dir():
            found.extend(root.glob("*/x64/fxc.exe"))
    return max(found, key=lambda p: p.parent.parent.name) if found else None


def main():
    fxc = find_fxc()
    if fxc is None:
        # ⚠️ SKIP, NOT FAIL. A missing SDK is a machine that cannot run this
        # check, not a repository with a broken shader -- and exiting non-zero
        # would make it indistinguishable from the failure it exists to report.
        print("fxc.exe not found (Windows SDK absent) — HLSL check SKIPPED")
        return 0

    source = HEADER.read_text(encoding="utf-8")
    blocks = dict(re.findall(r'(\w+)\s*=\s*R"hlsl\((.*?)\)hlsl";', source, re.S))
    if "kArchVizCBuffer" not in blocks:
        print("FAIL  the shared cbuffer literal was not found in %s" % HEADER.name)
        print("      (the raw-string shape this parses may have changed)")
        return 1
    cbuffer = blocks["kArchVizCBuffer"]

    out = REPO / "AddOn/EvP/build_29/hlsl_check"
    out.mkdir(parents=True, exist_ok=True)

    failed = 0
    for name, profile in sorted(STAGES.items()):
        if name not in blocks:
            print("FAIL  %-22s literal is missing from the header" % name)
            failed += 1
            continue
        path = out / (name + ".hlsl")
        prelude = ""
        missing_prelude = False
        for pre in PRELUDES.get(name, []):
            if pre not in blocks:
                print("FAIL  %-22s prelude %s is missing" % (name, pre))
                failed += 1
                missing_prelude = True
                break
            prelude += blocks[pre]
        if missing_prelude:
            continue
        body = prelude + blocks[name]
        for extra in CONTINUATIONS.get(name, []):
            if extra not in blocks:
                print("FAIL  %-22s continuation %s is missing" % (name, extra))
                failed += 1
                break
            body += blocks[extra]
        path.write_text(cbuffer + body, encoding="utf-8")
        result = subprocess.run(
            [str(fxc), "/T", profile, "/E", "main", "/nologo",
             "/Fo", str(out / (name + ".dxbc")), str(path)],
            capture_output=True, text=True)
        print("%s  %-22s %s" % ("ok  " if result.returncode == 0 else "FAIL",
                                name, profile))
        if result.returncode != 0:
            failed += 1
            # The line numbers fxc reports are into the CONCATENATED file, which
            # is why it is written out rather than piped.
            print((result.stdout + result.stderr).strip())
            print("      source: %s" % path)

    print("\n%d stage(s) compiled, %d failed" % (len(STAGES), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

#pragma once

// ArchViz/StartupAntiAliasing -- when the viewer's start view turns temporal anti-aliasing on (#14).
//
// The viewer opens with TAA off and turns it on once the model has finished arriving. While
// elements stream in, TAA's history is thrown away by every batch: the picture jittered, the
// shadows speckled and the load ran slower (live, 2026-11-21, after a build that opened with TAA
// on). Without TAA at all, the settled start view kept strong moire on a louvred facade, while
// every preview but Wireframe turns it on. So: off while loading, on once the scene is quiet.
//
// One-shot. Anything that turns TAA on first (a preview, the checkbox) disarms it, and so does a
// render mode other than Shaded; a later edit that streams elements in never turns it back off.

#include <cstddef>

namespace geomsrv {
namespace archviz {

struct StartupAntiAliasing {
    // The scene must hold elements, with none queued and the count unchanged, this long.
    static constexpr double kQuietSeconds = 1.5;

    bool armed = true;
    size_t lastElements = 0;
    double quietSince = -1.0;

    // True on the one frame TAA should be turned on. `taaOn` is the HUD's state this frame.
    bool Step (bool taaOn, bool shaded, size_t elements, size_t pending, double nowSeconds)
    {
        if (!armed)
            return false;
        if (taaOn || !shaded) {
            armed = false;
            return false;
        }
        if (elements == 0 || pending > 0 || elements != lastElements) {
            lastElements = elements;
            quietSince = -1.0;
            return false;
        }
        if (quietSince < 0.0)
            quietSince = nowSeconds;
        if (nowSeconds - quietSince < kQuietSeconds)
            return false;
        armed = false;
        return true;
    }
};

} // namespace archviz
} // namespace geomsrv

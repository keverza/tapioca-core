#ifndef EVP_SUNSTUDY_SUNSTUDYSOLARCUT_HPP
#define EVP_SUNSTUDY_SUNSTUDYSOLARCUT_HPP

// SunStudy/SunStudySolarCut -- what of the blockers must go for a study point
// to have direct sun through a chosen time slice (#152 follow-up).
//
// THE CUTTER IS ONE SURFACE. The point's rays toward consecutive suns of the
// slice span a flat triangle through the point; the slice's triangles, side
// by side, are the solar envelope -- a fan of planes, one per azimuth sector.
// The cut is that surface extruded upward: inside each sector, everything
// above its plane goes. Because each sector is bounded by planes (two
// vertical, through the point, and the envelope's), every blocker triangle is
// clipped EXACTLY (Sutherland-Hodgman against linear half-spaces): no voxels,
// no sampling, and the result does not depend on a grid.
//
// What comes back:
//   removed   -- the blockers' surfaces above the envelope (what goes);
//   caps      -- the cut faces where the envelope passes through a blocker,
//                from the blocker's own section loops (trapezoid fill), so
//                the removed part reads as a closed solid;
//   envelope  -- the merged cutting surface, for the preview;
//   kept      -- each cut blocker as it would remain (its surfaces below the
//                envelope plus the caps), for re-tracing the shadows it casts.
// The cut clears the rays rather than touching them: 5 cm under the surface,
// one degree beyond the first and last sun (an exact cut leaves what remains
// grazing every edge ray). Where the slice's wedge ends, the removed part is
// closed by vertical side faces.
// Volumes come from the removed part's closed boundary (divergence theorem).
// An open mesh has no closed section loops, so its cap -- and its volume --
// is skipped rather than invented.
//
// Pure CPU geometry on an immutable snapshot; cancellable; no host, no store.

#include "Geometry/Mesh.hpp"
#include "SunStudy/SunSeries.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace evp::sunstudy {

struct SolarCutInput {
    double point[3] = {};
    // The receiving surface's normal: steps with the sun behind it are left
    // out (no removal can light it then). Null for a point in the air.
    const double* normal = nullptr;
    std::vector<SunStep> steps; // the slice's measured suns, in time order
    double hoursPerStep = 0.25;
    // One per snapshot mesh: whether it can block (analysis or context, not
    // ignored), and the mesh the point stands on, which never blocks itself.
    std::vector<uint8_t> blocks;
    size_t ownMesh = SIZE_MAX;
    // The study point's radius, metres. The cut is swept along a horizontal
    // line through the point, `radius` each way across the slice's middle
    // direction: each plane drops by the line's reach along its normal, the
    // outer sides stand at the line's ends, and nothing behind the line is
    // cut. 0: the point alone.
    double radius = 0.0;
};

struct SolarCutTriangle {
    double v[3][3] = {};
    uint32_t mesh = 0;
};

struct SolarCutObject {
    size_t mesh = 0;
    std::string guid;
    int32_t elemType = 0;
    double removed = 0.0; // cubic metres above the envelope
    double total = 0.0;   // the element's own volume
    bool closed = true;   // its section loops closed: cap and volume are exact
};

struct SolarCutResult {
    std::vector<SolarCutTriangle> removed, caps, envelope;
    // Preview boundary emitted by the sheet generator, never inferred from
    // sector seams (swept sector planes do not share bit-identical vertices).
    std::vector<std::array<double, 6>> envelopeEdges;
    size_t sectors = 0; // the envelope's sectors; a radius makes the outer two quads
    // The cut elements as they would remain, by snapshot mesh index.
    std::vector<std::pair<size_t, geomsrv::Mesh>> kept;
    std::vector<SolarCutObject> objects; // most removed first
    double removedVolume = 0.0;
    // The slice at the point: hours the sun is in front of it, and of those,
    // hours already lit (a clear ray now). The cut lights the rest.
    double facingHours = 0.0, litHours = 0.0;
    std::vector<SunStep> facingSteps; // the suns the envelope was built from
    std::string error;
};

// The cutting surface alone (facing suns, hours, the envelope `reach` long),
// with no blocker clipped: cheap, so a moved point shows its surface at once.
bool ComputeSolarCutSurface (const SolarCutInput& input, SolarCutResult& result,
                             const std::function<bool (const double direction[3])>& lit, double reach);

// The cut. `lit` answers whether a sun direction is already clear from the
// point (the study's occluders); null counts nothing as lit. `reach` is how
// far the drawn envelope extends when nothing is cut.
bool ComputeSolarCut (const geomsrv::Snapshot& snapshot, const SolarCutInput& input, SolarCutResult& result,
                      const std::function<bool (const double direction[3])>& lit = {},
                      const std::function<bool ()>& cancelled = {}, double reach = 20.0);

} // namespace evp::sunstudy

#endif

#ifndef EVP_SUNSTUDY_VIEWPOINTSTUDY_HPP
#define EVP_SUNSTUDY_VIEWPOINTSTUDY_HPP

#include "Geometry/Mesh.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace evp::sunstudy {

// Plan: the horizontal isovist, all round, in the XY plane at point.z.
// FieldOfView: the same within a horizontal view angle about a bearing.
// Sphere: the spherical (3D) isovist, rays over a band of elevations.
enum class ViewpointMode : uint8_t { Plan = 0, FieldOfView = 1, Sphere = 2 };

struct ViewpointOptions {
    std::array<double, 3> point { 0.0, 0.0, 1.6 };
    double radius = 20.0; // metres
    size_t rays = 720;    // plan: all round; field of view: pro rata to its angle
    ViewpointMode mode = ViewpointMode::Plan;
    double bearingDegrees = 0.0;      // field of view: its centre, counterclockwise from +X
    double fieldOfViewDegrees = 90.0; // field of view: the full horizontal angle, 1..359
    // Sphere: elevations seen, -90 straight down .. 90 the zenith, and the grid.
    double minElevationDegrees = -90.0;
    double maxElevationDegrees = 90.0;
    size_t sphereColumns = 96; // azimuth steps all round
    size_t sphereRows = 33;    // elevation steps across the band, both ends included
    // Sphere: the side of the cells seen surfaces are cut into, metres; 0 picks
    // one from the radius. Coarsened when the cells would exceed a fixed budget.
    double surfaceCellMetres = 0.0;
};

struct ViewpointStudyResult {
    bool valid = false;
    std::string error;
    uint64_t snapshotId = 0, captureStamp = 0;
    ViewpointOptions options;
    // Plan: counterclockwise, without duplicate closing vertex. Fan triangulation
    // about options.point preserves concave isovists without a convex hull.
    // Field of view: the same from one edge of the angle to the other, OPEN --
    // the fan closes through the point, not from the last ray to the first.
    // Sphere: options.sphereRows x sphereColumns ray ends, row-major from the
    // lowest elevation; each row wraps all round.
    std::vector<std::array<double, 3>> perimeter;
    // Per ray, aligned with `perimeter`: the snapshot mesh index of the Context
    // element that stopped it, or -1 when it reached the radius.
    std::vector<int64_t> rayMesh;
    // Sphere: what the point sees of the capture's surfaces within the radius and
    // band, as triangles (three corners each). Every capture element receives;
    // only Context occludes. A triangle seen whole is kept whole; a partly seen
    // one is cut along where the radius, band or a shadow crosses its cells.
    std::vector<std::array<double, 3>> seenSurface;
    double seenArea = 0.0;          // square metres of `seenSurface`
    double surfaceCellMetres = 0.0; // the cell side used, after any coarsening
    size_t collisionCount = 0;
    double area = 0.0;         // plan and field of view: square metres seen
    double volume = 0.0;       // sphere: cubic metres seen
    double openFraction = 0.0; // the share of rays (sphere: of solid angle) that reach the radius

    bool Closed () const
    {
        return options.mode == ViewpointMode::Plan;
    }
};

// One Context element that stops viewpoint rays.
struct ViewpointBlocker {
    size_t mesh = 0;      // snapshot mesh index
    std::string guid;     // its element GUID as captured
    size_t rays = 0;      // rays it stopped
    double nearest = 0.0; // horizontal distance of its nearest stop, metres
};

// The elements that stopped rays, most rays first (ties: nearer, then mesh
// order). Empty when nothing was hit or `result` is not valid for `snapshot`.
std::vector<ViewpointBlocker> ViewpointBlockers (const ViewpointStudyResult& result, const geomsrv::Snapshot& snapshot);

// A run of consecutive rays with the same outcome: stopped by `mesh`, or open
// (`mesh == -1`). Runs wrap: a run crossing ray 0 is reported once, starting
// at its first ray, so `first + count` may exceed the ray count.
struct ViewpointArc {
    size_t first = 0;
    size_t count = 0;
    int64_t mesh = -1;
};
std::vector<ViewpointArc> ViewpointArcs (const std::vector<int64_t>& rayMesh);

// Add `guid` to `context`, or remove it when present (GUIDs compared in
// canonical form; order otherwise kept). Returns true when it was added.
bool ToggleViewpointContext (std::vector<std::string>& context, const std::string& guid);

ViewpointStudyResult RunViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                        const std::vector<std::string>& context, const ViewpointOptions& options,
                                        const std::function<bool ()>& isCancelled = {});

// RunViewpointStudy on the core TAPIOCA_SUNSTUDY_CORE selects (P5); the
// Rust policy returns the same result through the same C++ query index.
ViewpointStudyResult RunSelectedViewpointStudy (const std::shared_ptr<const geomsrv::Snapshot>& snapshot,
                                                const std::vector<std::string>& context,
                                                const ViewpointOptions& options,
                                                const std::function<bool ()>& isCancelled = {});

} // namespace evp::sunstudy
#endif

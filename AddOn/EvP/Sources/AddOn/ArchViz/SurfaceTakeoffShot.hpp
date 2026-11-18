#ifndef TAPIOCA_ARCHVIZ_SURFACETAKEOFFSHOT_HPP
#define TAPIOCA_ARCHVIZ_SURFACETAKEOFFSHOT_HPP

// ArchViz/SurfaceTakeoffShot -- the area table under an Exposed surfaces screenshot
// (issue #193 S7). The PNG is one report: the live viewer's still above, this
// table below, both from the SAME pinned result and highlight. The table is
// export content, not a picture of the HUD.
//
// PURE except DrawShotTable (ImGui, render thread, inside DiligentHud's capture
// frame). The pin is taken when the screenshot is requested; ViewerScreenshot
// refuses the image if the published highlight stops matching it before readback,
// so a new table is never written under old geometry or the reverse.

#include "ArchViz/SurfaceTakeoffPage.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace geomsrv::surfacetakeoff {
struct Result;
}

namespace geomsrv::archviz::surfacetakeoff {

struct ShotRow {
    int32_t surface = 0;
    std::string name;
    uint32_t rgba = 0;
    size_t elements = 0; // distinct contributing elements
    double areaM2 = 0;   // full precision; rounded only when drawn
};

struct ShotTable {
    // What the image must still show at readback.
    uint64_t generation = 0;
    uint64_t snapshotId = 0;
    std::optional<int32_t> material;
    std::set<int32_t> materials;
    ContextStyle contextStyle = ContextStyle::Desaturated;
    float contextOpacity = kDefaultContextOpacity;

    std::string title;    // "EXPOSED SURFACE AREAS" or "ISOLATED MATERIAL - SUBTOTAL"
    std::string subtitle; // measure, scope, filter, completeness
    std::vector<ShotRow> rows;
    double totalM2 = 0;  // summed before rounding
    size_t elements = 0; // distinct union, not the sum of the rows
    bool roundingNote = false;
    std::vector<std::string> footer;
};

// The rows the page currently highlights: every row with highlighted area, or
// the isolated one alone. False with a reason when there is nothing to export.
bool BuildShotTable (const geomsrv::surfacetakeoff::Result& result, const HighlightSummary& highlight,
                     uint64_t snapshotId, const std::string& timestamp, ShotTable& table, std::string& error);

// "<0.001" for small nonzero values, otherwise three decimals.
std::string ShotArea (double areaM2);

// True while the published highlight still is the one the table was pinned from.
bool ShotMatches (const ShotTable& table, const HighlightSummary& highlight, uint64_t snapshotId);

// RENDER THREAD, inside an ImGui frame sized width x height and cleared white.
// Returns the height the table needed, in pixels, which may exceed `height`:
// the caller then draws it again taller, never truncates.
float DrawShotTable (const ShotTable& table, float width, float height);

} // namespace geomsrv::archviz::surfacetakeoff

#endif

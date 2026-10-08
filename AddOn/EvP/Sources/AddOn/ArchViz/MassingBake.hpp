#ifndef EVP_ARCHVIZ_MASSINGBAKE_HPP
#define EVP_ARCHVIZ_MASSINGBAKE_HPP
#include "ArchViz/MassingSlices.hpp"
#include "NodeGraph/Json.hpp"

namespace geomsrv::archviz::massingbake {
enum class Kind { Slices, Envelope, Collapse };
// Pure bounded topology snapshot, independent of visibility/overlay styling.
bool Geometry (Kind kind, const massingslices::Result* slices, const massingcalculation::Result* envelope,
               const std::vector<SliceChain>& collapse, evp::nodegraph::json::JsonValue& result, std::string& error);
// Versioned UTF-8 JSON fixture: the same counted polygons used by native baking.
bool Export2DJson (const massingslices::Result& slices, std::string& text, std::string& error);
std::string Inputs (const evp::nodegraph::json::JsonValue& geometry, const evp::nodegraph::json::JsonValue& settings,
                    uint64_t token);
// One home story per physical floor of each building, clamped at the highest story.
std::vector<size_t> HomeStoreys (const ProjectStoreys& storeys, const std::vector<double>& elevations,
                                 const std::vector<std::string>& groups);
// Recover only runs of sampled circular edges; mixed/straight edges remain lines.
slabslices::Ring CircularRing (const slabslices::Ring& sampled);
// Bake-only straight-ring cleanup. SDK general EPS is 10 micrometres;
// leave a 20-micrometre edge margin without changing previews/export fixtures.
constexpr double kSlabEdgeTolerance = 0.00002;
bool CleanSlabRing (const slabslices::Ring& input, slabslices::Ring& output, std::string& error);
// Main-thread facade; DG dialog and Python/API writes are deferred out of HUD layout.
void RejectedSlab (const std::vector<SliceChain>& contours, const std::string& group, double z, uint64_t token,
                   size_t sourceIndex);
void Request (Kind kind);
bool Current (uint64_t token);
void Forget ();
void Shutdown ();
} // namespace geomsrv::archviz::massingbake
#endif

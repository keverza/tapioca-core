#ifndef EVP_ARCHVIZ_MASSINGRULES_HPP
#define EVP_ARCHVIZ_MASSINGRULES_HPP

// Authored per-edge offsets, independent of the HUD and Archicad. The fingerprint
// format is shared with the retained Python command; no derived envelope is stored.
#include "Metadata/TapiocaMetadata.hpp"

#include <string>
#include <vector>

namespace geomsrv::archviz::massingrules {
enum class Mode { Default, Custom, None };
struct Edge {
    double ax = 0, ay = 0, bx = 0, by = 0;
    double arcAngle = 0; // signed radians, Archicad polygon convention
};
struct Assignment {
    Mode mode = Mode::Default;
    double distance = 3; // metres; None is zero, not an unregulated height rule
    bool review = false;
};
struct Page {
    std::string guid;
    std::vector<Edge> edges;
    std::vector<Assignment> assignments;
    bool known = false;
    bool hasStored = false;
    metadata::Value stored;
    std::string note;
};
struct Edit {
    Page before; // captured GUID, geometry and saved value; stale writes refuse
    std::vector<Assignment> assignments;
};
const char* ModeName (Mode mode);
std::string Fingerprint (const Edge& edge);
bool ValidEdges (const std::vector<Edge>& edges);
bool SameGeometry (const std::vector<Edge>& a, const std::vector<Edge>& b);
// New edge -> previous edge, preserving exact/reordered edges, straight splits,
// cyclic correspondence for vertex moves, then nearest-edge inheritance for
// merged/new segments. Invalid rings produce -1 entries.
std::vector<int> SegmentMap (const std::vector<Edge>& before, const std::vector<Edge>& after);
bool CheckSource (const Edit& edit, const std::vector<Edge>& current, const metadata::EntityMetadata& entity,
                  std::string& error);
// Invalid records refuse atomically. Unique unchanged hashes remap across winding
// and ordering; same-count vertex edits retain segmentIndex values. Genuinely
// unmatched topology remains marked, while live drafts also remap straight splits.
bool Restore (Page& page, const metadata::EntityMetadata& entity);
bool Encode (const std::vector<Edge>& edges, const std::vector<Assignment>& assignments, metadata::Property& property,
             std::string& error);
} // namespace geomsrv::archviz::massingrules
#endif

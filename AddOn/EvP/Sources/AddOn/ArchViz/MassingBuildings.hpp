#ifndef EVP_ARCHVIZ_MASSINGBUILDINGS_HPP
#define EVP_ARCHVIZ_MASSINGBUILDINGS_HPP
#include "ArchViz/HudSection.hpp"
#include "Geometry/Mesh.hpp"
#include <memory>

namespace geomsrv::archviz::massingbuildings {
constexpr char kUniqueLayer[] = "tapioca.massing.uniqueBuildings";
constexpr char kSelectedLayer[] = "tapioca.massing.selectedBuilding";
constexpr char kFloorsLayer[] = "tapioca.massing.selectedFloors";
struct Record {
    std::string guid, id;
};
struct Group {
    std::string key, id;
    std::vector<std::string> guids;
};
struct Surface {
    Record record;
    std::shared_ptr<const Mesh> body;
};
struct Preview {
    Group building;
    hudsection::Section section;
    std::vector<hudmeta::Page> heights;
};
// Exact, case-sensitive authored IDs identify buildings (one stairwell each).
// Missing IDs remain separate slabs; element IDs, geometry and adjacency are not identity.
std::string Id (const metadata::EntityMetadata& entity);
std::vector<Group> Groups (const std::vector<Record>& records);
std::vector<std::string> Members (const std::vector<Record>& records, const std::vector<std::string>& seeds);
std::vector<Preview> Previews (const hudsection::Section& section, const std::vector<Record>& records,
                               const std::vector<hudmeta::Page>& heights, const std::vector<std::string>& selected);
// Empty key inspects all buildings in distinct colours. Otherwise highlight one
// complete group. Requires every current operated body, never prism substitutes.
bool Inspect (const std::vector<Surface>& surfaces, const std::string& key, overlaylayers::Layer& layer,
              std::string& error);
} // namespace geomsrv::archviz::massingbuildings
#endif

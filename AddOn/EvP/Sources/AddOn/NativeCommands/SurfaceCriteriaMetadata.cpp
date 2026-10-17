#include "APIEnvir.h"
#include "ACAPinc.h"
#include "SurfaceCriteriaMetadata.hpp"
#include "Metadata/MetadataExtractor.hpp"
#include <algorithm>
#include <cctype>

namespace geomsrv {
namespace {
std::string Key (std::string value)
{
    if (value.size () == 38 && value.front () == '{' && value.back () == '}')
        value = value.substr (1, 36);
    std::transform (value.begin (), value.end (), value.begin (),
                    [] (unsigned char c) { return char (std::toupper (c)); });
    return value;
}
} // namespace
void ReadSurfaceCriteriaMetadata (const Snapshot& snapshot, const std::vector<size_t>& targets,
                                  std::map<std::string, surfacecriteria::Element>& elements)
{
    std::vector<std::string> guids;
    for (size_t index : targets)
        guids.push_back (snapshot.meshes[index].guid);
    const auto metadata = ExtractMetadataFor (guids, MetaLevel::Basic);
    if (!metadata)
        return;
    for (const auto& row : metadata->elems)
        for (auto& [guid, element] : elements) {
            if (Key (guid) != Key (row.guid))
                continue;
            element.layer = row.layer;
            element.story = row.story;
            element.id = row.elemId;
            element.detailsAvailable = true;
        }
}
} // namespace geomsrv

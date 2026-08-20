#ifndef GEOMETRYSERVER_ELEMENTMETA_HPP
#define GEOMETRYSERVER_ELEMENTMETA_HPP

#include <string>
#include <vector>
#include <utility>

// Plain, ACAPI-free metadata for one Archicad element, gathered on the main
// thread by MetadataExtractor and served to HTTP workers via MetadataStore.
// All strings are UTF-8.
namespace geomsrv {

// A project story. `elevation` is the story level in world Z (meters) — the same
// frame as the geometry vertices. `height` is the distance to the story above;
// the topmost story has no story above it, so its height is unknown (hasHeight
// = false) and is serialized as null rather than faked.
struct Story {
    int         index = 0;
    std::string name;
    double      elevation = 0.0;
    double      height = 0.0;
    bool        hasHeight = false;
};

struct ElementMeta {
    std::string guid;        // API element GUID (uppercase, dashed)
    std::string typeName;    // localized element type name (e.g. "Wall")
    std::string elemId;      // element ID / info string (e.g. "W-001")
    std::string layer;       // layer name
    std::string story;       // home story name
    // "classification code" -> "classification name"
    std::vector<std::pair<std::string, std::string>> classifications;
    // user-defined property name -> value string
    std::vector<std::pair<std::string, std::string>> properties;
};

} // namespace geomsrv

#endif

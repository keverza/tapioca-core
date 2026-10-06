#ifndef EVP_ARCHVIZ_GRAPHICSSETTINGS_HPP
#define EVP_ARCHVIZ_GRAPHICSSETTINGS_HPP

#include "ArchViz/OverlayLayers.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace geomsrv::archviz::graphicssettings {
enum class Kind { Number, Colour, Boolean };
struct Definition {
    std::string key, group, units;
    Kind kind = Kind::Number;
    double initial = 0, min = 0, max = 1;
};
struct Value {
    double number = 0;
    bool enabled = false; // Untouched properties retain each producer's authored style.
};
struct Snapshot {
    std::map<std::string, Definition> definitions;
    std::map<std::string, Value> values;
    uint64_t revision = 0;
    uint64_t sceneRevision = 0; // UI-only tuning must not upload model geometry again.
};
struct Swatch {
    std::string name;
    uint32_t rgba;
};
const std::vector<Swatch>& Palette ();
std::shared_ptr<const Snapshot> Current ();
void Register (const Definition& definition);
bool Set (const std::string& key, double value);
bool Reset (const std::string& key = {});
double Number (const Snapshot& settings, const std::string& key, double fallback);
double Number (const std::string& key, double fallback);
uint32_t Colour (const std::string& key, uint32_t fallback);
uint32_t FunctionColour (const std::string& function, uint32_t fallback);
bool Affects (const overlaylayers::Layer& layer, const Snapshot& settings);
// MAIN THREAD store consumers: copy only when overrides apply, never mutate authored layers.
overlaylayers::Layer Apply (const overlaylayers::Layer& layer, const Snapshot& settings);
std::string Encode (const Snapshot& settings);
struct SaveResult {
    bool saved = false;
    std::filesystem::path path;
    std::string error;
};
SaveResult Save (const Snapshot& settings, const std::filesystem::path& logs);
SaveResult SaveToLogs (const Snapshot& settings);
} // namespace geomsrv::archviz::graphicssettings
#endif

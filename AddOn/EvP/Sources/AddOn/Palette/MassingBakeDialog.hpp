#ifndef EVP_ARCHVIZ_MASSINGBAKEDIALOG_HPP
#define EVP_ARCHVIZ_MASSINGBAKEDIALOG_HPP
#include "ArchViz/MassingBake.hpp"
#include "UniString.hpp"
namespace evp::massingbakeui {
enum class Action { Cancel, Bake, Export2D };
Action AskSettings (geomsrv::archviz::massingbake::Kind kind, evp::nodegraph::json::JsonValue& settings);
bool AskExportPath (GS::UniString& path);
} // namespace evp::massingbakeui
#endif

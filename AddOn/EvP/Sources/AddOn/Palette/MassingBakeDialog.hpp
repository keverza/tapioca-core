#ifndef EVP_ARCHVIZ_MASSINGBAKEDIALOG_HPP
#define EVP_ARCHVIZ_MASSINGBAKEDIALOG_HPP
#include "ArchViz/MassingBake.hpp"
namespace evp::massingbakeui {
bool AskSettings (geomsrv::archviz::massingbake::Kind kind, evp::nodegraph::json::JsonValue& settings);
}
#endif

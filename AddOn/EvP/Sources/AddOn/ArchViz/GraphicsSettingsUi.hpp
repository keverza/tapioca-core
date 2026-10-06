#ifndef EVP_ARCHVIZ_GRAPHICSSETTINGSUI_HPP
#define EVP_ARCHVIZ_GRAPHICSSETTINGSUI_HPP
#include <imgui.h>

namespace geomsrv::archviz::graphicssettingsui {
void ApplyStyle (ImGuiStyle& style, float scale);
ImVec4 StyleColour (ImGuiCol index, ImVec4 fallback);
void Draw ();
} // namespace geomsrv::archviz::graphicssettingsui
#endif

#ifndef EVP_ARCHVIZ_GRAPHICSSETTINGSUI_HPP
#define EVP_ARCHVIZ_GRAPHICSSETTINGSUI_HPP
#include <imgui.h>
#include <functional>

namespace geomsrv::archviz::graphicssettingsui {
void ApplyStyle (ImGuiStyle& style, float scale);
ImVec4 StyleColour (ImGuiCol index, ImVec4 fallback);
void Draw (const std::function<void ()>& displayStyles = {});
} // namespace geomsrv::archviz::graphicssettingsui
#endif

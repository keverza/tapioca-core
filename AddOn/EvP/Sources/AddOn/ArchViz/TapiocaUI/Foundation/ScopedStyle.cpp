#include "ArchViz/TapiocaUI/Foundation/ScopedStyle.hpp"

namespace geomsrv::archviz::ui {

ScopedStyle::~ScopedStyle ()
{
    Pop ();
}

ScopedStyle& ScopedStyle::Colour (ImGuiCol index, ImU32 colour)
{
    ImGui::PushStyleColor (index, colour);
    ++colours_;
    return *this;
}

ScopedStyle& ScopedStyle::Colour (ImGuiCol index, const ImVec4& colour)
{
    ImGui::PushStyleColor (index, colour);
    ++colours_;
    return *this;
}

ScopedStyle& ScopedStyle::Var (ImGuiStyleVar index, float value)
{
    ImGui::PushStyleVar (index, value);
    ++vars_;
    return *this;
}

ScopedStyle& ScopedStyle::Var (ImGuiStyleVar index, const ImVec2& value)
{
    ImGui::PushStyleVar (index, value);
    ++vars_;
    return *this;
}

ScopedStyle& ScopedStyle::Font (ImFont* font, float size)
{
    ImGui::PushFont (font, size);
    ++fonts_;
    return *this;
}

ScopedStyle& ScopedStyle::Adopt (int colours, int vars)
{
    colours_ += colours > 0 ? colours : 0;
    vars_ += vars > 0 ? vars : 0;
    return *this;
}

void ScopedStyle::Pop ()
{
    // Fonts were pushed last in every caller here, so they come off first; ImGui keeps the
    // colour, variable and font stacks apart, so the order between them is not load-bearing.
    for (; fonts_ > 0; --fonts_)
        ImGui::PopFont ();
    if (vars_ > 0)
        ImGui::PopStyleVar (vars_);
    if (colours_ > 0)
        ImGui::PopStyleColor (colours_);
    vars_ = 0;
    colours_ = 0;
}

} // namespace geomsrv::archviz::ui

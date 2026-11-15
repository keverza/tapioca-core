// #191 U4: the Inspector shows one context at a time -- a path's profile, a region's settings, a
// drawing tool's options or, with nothing selected, the terrain -- and never another context's.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditPathEdit.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <string>

namespace {
namespace eh = geomsrv::archviz::edithud;
namespace edit = geomsrv::edit;
namespace lg = tapioca::geometry;

struct Gui {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    Gui ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 400, 800 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~Gui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    // One frame of `draw` in a 320 x 700 window: the narrowest panel the native shell docks.
    std::string Frame (const std::function<void ()>& draw)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (399, 799);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 320, 700 });
        ImGui::Begin ("inspector", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::LogToBuffer ();
        draw ();
        std::string text = ImGui::GetCurrentContext ()->LogBuffer.c_str ();
        ImGui::LogFinish ();
        ImGui::End ();
        ImGui::Render ();
        return text;
    }
};

struct Site {
    edit::Document doc;
    edit::Id path = edit::kNoId, region = edit::kNoId, reference = edit::kNoId;
};

edit::Id Role (edit::Document& doc, edit::Id entity, lg::SourceRole role)
{
    std::string error;
    EXPECT_TRUE (lg::SetLandscapeSourceRole (doc, entity, role, error)) << error;
    return entity;
}

// A landscape with a 100 m ramp (a break at 40 m), a region and a reference line.
Site MakeSite ()
{
    Site s;
    std::string error;
    EXPECT_TRUE (lg::StartLandscape (s.doc, { 0x19, 0x1, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x2 }, error))
        << error;
    s.path = edit::AddPath (s.doc, { { 0, -20, 100.0 }, { 100, -20, 103.0 } }, 1.8);
    edit::Id mid = 0;
    EXPECT_TRUE (edit::InsertPathBreak (s.doc, s.path, 40.0, error, &mid)) << error;
    EXPECT_TRUE (edit::SetPathElevation (s.doc, s.path, mid, 102.0, error)) << error;
    s.region = Role (s.doc, edit::AddPolyline (s.doc, { { 0, 0, 0 }, { 8, 0, 0 }, { 8, 8, 0 }, { 0, 8, 0 } }, true),
                     lg::SourceRole::RegionBoundary);
    s.reference =
        Role (s.doc, edit::AddPolyline (s.doc, { { -5, -2, 0 }, { -5, 10, 0 } }, false), lg::SourceRole::ReferenceLine);
    return s;
}

bool Has (const std::string& text, const char* what)
{
    return text.find (what) != std::string::npos;
}

// What marks each context in the panel text: the profile, the selected lines' role, the next
// line's role, the reference line's style and, with nothing selected, the terrain.
struct Marks {
    bool profile, selectedRole, nextRole, lineStyle, terrain;
};
Marks Read (const std::string& text)
{
    return { Has (text, "Path profile"), Has (text, "Use selected lines as:"), Has (text, "Next line's role:"),
             Has (text, "Width (pixels)"), Has (text, "Rebuild terrain (3D)") };
}
} // namespace

TEST (HudVirtualEditorInspector, InspectorShowsOneContext191)
{
    const auto site = MakeSite ();
    Gui gui;
    eh::Session s (site.doc);
    const auto draw = [&] { return Read (gui.Frame ([&] { eh::detail::Inspector (s); })); };

    auto marks = draw ();
    EXPECT_EQ (eh::ResolveContext (s), eh::InspectorContext::Nothing);
    EXPECT_TRUE (marks.terrain);
    EXPECT_FALSE (marks.profile) << "the only path's profile is not shown with nothing selected";
    EXPECT_FALSE (marks.selectedRole);
    EXPECT_FALSE (marks.nextRole);

    s.selection.Set ({ { site.path, edit::Part::Entity, 0 } });
    marks = draw ();
    EXPECT_TRUE (marks.profile);
    EXPECT_FALSE (marks.terrain);
    EXPECT_FALSE (marks.selectedRole);

    s.selection.Set ({ { site.region, edit::Part::Entity, 0 } });
    marks = draw ();
    EXPECT_EQ (eh::ResolveContext (s), eh::InspectorContext::Region);
    EXPECT_TRUE (marks.selectedRole);
    EXPECT_FALSE (marks.profile);
    EXPECT_FALSE (marks.terrain);
    EXPECT_FALSE (marks.nextRole) << "one role picker per context";

    s.selection.Set ({ { site.reference, edit::Part::Entity, 0 } });
    marks = draw ();
    EXPECT_TRUE (marks.selectedRole);
    EXPECT_TRUE (marks.lineStyle) << "the selected reference line's style, without opening a header";
    EXPECT_FALSE (marks.profile);

    s.selection.Clear ();
    eh::detail::SetTool (s, eh::Tool::Polyline);
    s.landscapeMode = eh::LandscapeMode::DrawSource;
    marks = draw ();
    EXPECT_TRUE (marks.nextRole);
    EXPECT_FALSE (marks.selectedRole);
    EXPECT_FALSE (marks.terrain);
}

// The action row sits under every selection context, with Delete in it, and no context without a
// selection shows it; the offset distance appears only where Offset is offered.
TEST (HudVirtualEditorInspector, TheActionRowFollowsTheSelection191)
{
    const auto site = MakeSite ();
    Gui gui;
    eh::Session s (site.doc);
    const auto text = [&] { return gui.Frame ([&] { eh::detail::Inspector (s); }); };
    EXPECT_FALSE (Has (text (), "Delete"));
    s.selection.Set ({ { site.region, edit::Part::Entity, 0 } });
    auto shown = text ();
    EXPECT_TRUE (Has (shown, "Delete"));
    EXPECT_TRUE (Has (shown, "More"));
    EXPECT_FALSE (Has (shown, "Duplicate offset")) << "Duplicate's offset is under More";
    s.selection.Set ({ { site.path, edit::Part::Entity, 0 } });
    EXPECT_TRUE (Has (text (), "Delete"));
}

// A selected path's profile is laid out inside a 320 x 700 panel under the pinned header: its
// labels are placed above the panel's bottom without scrolling.
TEST (HudVirtualEditorInspector, PathProfileVisibleWithoutScroll191)
{
    const auto site = MakeSite ();
    Gui gui;
    eh::Session s (site.doc);
    s.selection.Set ({ { site.path, edit::Part::Entity, 0 } });
    for (int i = 0; i < 3; ++i)
        gui.Frame ([&] { eh::Panel (s); });
    ASSERT_FALSE (s.profilePlaced.empty ());
    float top = 1e9f;
    for (const auto& label : s.profilePlaced)
        top = std::min (top, label.box.y0);
    EXPECT_LT (top, 700.0f) << "the profile starts below the panel: the user must scroll to reach it";
}

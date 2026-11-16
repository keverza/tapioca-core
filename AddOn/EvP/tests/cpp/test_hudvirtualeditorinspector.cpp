// #191 U4: the Inspector shows one context at a time -- a path's profile, a region's settings, a
// drawing tool's options or, with nothing selected, the terrain -- and never another context's.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPrimitives.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>
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

namespace {
// The inventory's Inspector rows (Area C), counted: a new C row fails NoInventoryControlUnreachable
// until it is added to the table there.
size_t InventoryInspectorRows ()
{
    std::ifstream in (std::filesystem::path (__FILE__).parent_path () /
                      "../../tools/UIPlayground/fixtures/EDITOR-CONTROLS.md");
    size_t rows = 0;
    for (std::string line; std::getline (in, line);) {
        if (line.empty () || line[0] != '|')
            continue;
        std::vector<std::string> cells;
        std::stringstream split (line.substr (1));
        for (std::string cell; std::getline (split, cell, '|');)
            cells.push_back (cell);
        if (cells.size () >= 8) {
            const auto area = cells[6].find_first_not_of (' ');
            rows += area != std::string::npos && cells[6][area] == 'C';
        }
    }
    return rows;
}
} // namespace

// #191 U4: every Inspector control of EDITOR-CONTROLS.md is drawn in its context -- or under that
// context's More, Expert or own header, opened here as a user would open it.
TEST (HudVirtualEditorInspector, NoInventoryControlUnreachable191)
{
    EXPECT_EQ (InventoryInspectorRows (), 44u) << "EDITOR-CONTROLS.md changed its Inspector rows: update this table";
    auto site = MakeSite ();
    const auto plainA = edit::AddPolyline (site.doc, { { 30, 0, 0 }, { 34, 0, 0 }, { 34, 4, 0 }, { 30, 4, 0 } }, true);
    const auto plainB = edit::AddPolyline (site.doc, { { 32, 2, 0 }, { 36, 2, 0 }, { 36, 6, 0 }, { 32, 6, 0 } }, true);
    std::string error;
    edit::PrimitiveInput input;
    input.kind = edit::Primitive::Ellipse;
    input.first = { -12, 4, 0 };
    input.second = { -9, 6, 0 };
    const auto ellipse = edit::AddPrimitive (site.doc, input, "ellipse", error);
    ASSERT_NE (ellipse, edit::kNoId) << error;
    const auto whole = [] (edit::Id id) { return edit::Ref { id, edit::Part::Entity, 0 }; };
    const auto* path = edit::FindPath (site.doc, site.path);
    ASSERT_NE (path, nullptr);
    ASSERT_GE (path->profile.size (), 3u);
    const edit::Ref brk { site.path, edit::Part::Break, path->profile[1].id };
    const auto* region = edit::Find (site.doc, site.region);
    const edit::Ref vertex { site.region, edit::Part::Vertex, region->vertices[0].id };

    struct Case {
        const char* name;
        std::function<void (eh::Session&)> setup;
        std::vector<const char*> labels;
    };
    const std::vector<Case> cases {
        { "close prompt",
          [] (eh::Session& s) { s.closePrompt = true; },
          { "Save and close", "Discard and close", "Cancel" } },
        { "recovery offer",
          [] (eh::Session& s) { s.recoveries.push_back ({ "r.json", "2026-11-15 10:00", "", 3, 2 }); },
          { "Recover", "Discard" } },
        { "draft list",
          [] (eh::Session& s) {
              s.draftFilesListed = true;
              s.draftFiles = { "a.json" };
          },
          { "a.json", "Refresh drafts", "Hide drafts" } },
        { "mirror", [] (eh::Session& s) { eh::detail::SetTool (s, eh::Tool::Mirror); }, { "Keep original" } },
        { "array draft",
          [&] (eh::Session& s) {
              s.selection.Set ({ whole (plainA) });
              ASSERT_TRUE (eh::detail::BeginRectangularArray (s)) << s.message;
          },
          { "Array count X (total)", "Array spacing X (m)", "Apply array", "Cancel array" } },
        { "boolean draft",
          [&] (eh::Session& s) {
              s.selection.Set ({ whole (plainA), whole (plainB) });
              ASSERT_TRUE (eh::detail::BeginBoolean (s, edit::BooleanKind::Union)) << s.message;
          },
          { "Apply boolean", "Cancel boolean" } },
        { "polygon tool", [] (eh::Session& s) { eh::detail::SetTool (s, eh::Tool::Polygon); }, { "Sides (Up/Down)" } },
        { "polyline tool",
          [] (eh::Session& s) {
              eh::detail::SetTool (s, eh::Tool::Polyline);
              s.landscapeMode = eh::LandscapeMode::DrawSource;
          },
          { "Next line's role:", "Region", "boundary", "Cutter", "Property" } },
        { "path",
          [&] (eh::Session& s) { s.selection.Set ({ whole (site.path) }); },
          { "Path profile", "Kerb radius", "Crown", "Falls left", "Falls right", "Path to entrance" } },
        { "path break", [&] (eh::Session& s) { s.selection.Set ({ brk }); }, { "Smoothing", "Fix elevation" } },
        { "region",
          [&] (eh::Session& s) { s.selection.Set ({ whole (site.region) }); },
          { "Use selected lines as:", "Material", "Building", "outline", "Elevation", "Terrain interaction",
            "Material preset metadata", "Choose Archicad fill / surface...", "Delete", "More", "Expert",
            "Duplicate offset XYZ (m)", "Rectangular array...", "object ", "Layer", "Group", "Ungroup" } },
        { "offset",
          [&] (eh::Session& s) { s.selection.Set ({ whole (plainA) }); },
          { "Offset distance", "Offset join", "Offset miter limit" } },
        { "reference line",
          [&] (eh::Session& s) { s.selection.Set ({ whole (site.reference) }); },
          { "Visible", "Locked", "Width (pixels)", "Line colour" } },
        { "ellipse",
          [&] (eh::Session& s) { s.selection.Set ({ whole (ellipse) }); },
          { "Circle (use major radius)", "Convert to curve" } },
        { "fixed heights",
          [&] (eh::Session& s) { s.selection.Set ({ vertex }); },
          { "Fix selected heights", "Release selected heights" } },
        { "several",
          [&] (eh::Session& s) { s.selection.Set ({ whole (site.path), whole (ellipse) }); },
          { "1 path ", "Use selected lines as:" } },
        { "nothing",
          [] (eh::Session&) {},
          { "Rebuild terrain (3D)", "Place entrances", "Keep recovery snapshots", "Reference line display",
            "Add selected Archicad lines and fills", "As property line", "As building outline",
            "Doors as entrances" } },
    };
    for (const auto& c : cases) {
        Gui gui;
        eh::Session s (site.doc);
        c.setup (s);
        if (::testing::Test::HasFatalFailure ())
            return;
        gui.Frame ([&] { eh::detail::Inspector (s); });
        auto* window = ImGui::FindWindowByName ("inspector");
        ASSERT_NE (window, nullptr);
        for (const char* header : { "More", "Expert", "Fixed terrain heights" })
            window->StateStorage.SetInt (ImHashStr (header, 0, window->ID), 1);
        const auto text = gui.Frame ([&] { eh::detail::Inspector (s); });
        for (const char* label : c.labels)
            EXPECT_TRUE (Has (text, label)) << c.name << ": \"" << label << "\" is not drawn ("
                                            << eh::InspectorContextName (eh::ResolveContext (s)) << ")";
    }
}

// #189: after the editor's own Boolean, the picked regions are construction inputs. Picking, the
// panel's region and the Inspector all follow the result: an input's line stands for it.
TEST (HudVirtualEditorInspector, ABooleansInputsStandForItsResult189)
{
    auto site = MakeSite ();
    const auto second =
        Role (site.doc, edit::AddPolyline (site.doc, { { 5, 3, 0 }, { 13, 3, 0 }, { 13, 11, 0 }, { 5, 11, 0 } }, true),
              lg::SourceRole::RegionBoundary);
    std::string error;
    for (const auto& object : std::vector<lg::HardscapeObject> (site.doc.landscape->objects)) {
        lg::RegionSurface surface;
        surface.elevation = lg::RegionElevationMode::Constant;
        surface.first.z = 2;
        ASSERT_TRUE (lg::SetRegionSurface (site.doc, object.id, surface, {}, error)) << error;
        lg::RegionTerrain terrain;
        terrain.effect = lg::RegionTerrainEffect::Grade;
        ASSERT_TRUE (lg::SetRegionTerrain (site.doc, object.id, terrain, error)) << error;
    }
    eh::Session s (site.doc);
    ASSERT_TRUE (eh::BeginLandscapeCommand (s, lg::OperationKind::Intersection));
    ASSERT_TRUE (eh::PickLandscapeShape (s, { 1, 1, 0 }, true));
    ASSERT_TRUE (eh::PickLandscapeShape (s, { 12, 10, 0 }, true));
    ASSERT_TRUE (eh::AdvanceLandscapeCommand (s)) << s.message;
    EXPECT_NE (s.message.find ("construction inputs"), std::string::npos) << s.message;
    const auto result = s.landscapeObject;
    ASSERT_NE (result, edit::kNoId);
    const auto& landscape = *s.edits.Doc ().landscape;
    ASSERT_EQ (lg::LandscapeConstructionInputs (landscape).size (), 2u);

    // One input line, or both: the Boolean result's context, and the panel's region is the result.
    for (const auto& selection : std::vector<std::vector<edit::Ref>> {
             { { site.region, edit::Part::Entity, 0 } },
             { { site.region, edit::Part::Entity, 0 }, { second, edit::Part::Entity, 0 } } }) {
        s.selection.Set (selection);
        EXPECT_EQ (eh::ResolveContext (s), eh::InspectorContext::BooleanResult) << selection.size ();
        EXPECT_EQ (eh::detail::SelectedRegionObject (s), result) << selection.size ();
    }
    Gui gui;
    const auto text = gui.Frame ([&] { eh::detail::Inspector (s); });
    EXPECT_TRUE (Has (text, "Construction inputs (dashed)")) << text;
    EXPECT_TRUE (Has (text, "Terrain interaction")) << "the result's settings are edited here";
    EXPECT_TRUE (Has (text, "Use selected lines as:"));

    // The next command picks the result as one shape; an input is not a shape of its own.
    ASSERT_TRUE (eh::BeginLandscapeCommand (s, lg::OperationKind::Union));
    size_t regions = 0;
    for (const auto& shape : eh::LandscapeCommandShapes (s))
        if (shape.pick.object) {
            ++regions;
            EXPECT_EQ (shape.pick.object, result);
        }
    EXPECT_EQ (regions, 1u);
    eh::CancelLandscapeCommand (s);

    // Undo: the inputs are ordinary regions again, each with the effect it had.
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_TRUE (lg::LandscapeConstructionInputs (*s.edits.Doc ().landscape).empty ());
    EXPECT_EQ (lg::TerrainRegionObjects (s.edits.Doc ()).size (), 2u);
    s.selection.Set ({ { site.region, edit::Part::Entity, 0 } });
    EXPECT_EQ (eh::ResolveContext (s), eh::InspectorContext::Region);
}

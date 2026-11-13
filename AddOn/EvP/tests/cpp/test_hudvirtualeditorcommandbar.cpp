// #191 U2: the virtual editor's command bar -- the build state chip, a refusal that stays until a
// newer build succeeds, a Cancel for every running command, and Exit apart from Delete.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/HudVirtualEditorTerrain.hpp"
#include "Edit/EditLandscape.hpp"
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
        io.DisplaySize = { 400, 600 };
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
    // One frame of `draw` in a 320 px window, the pointer at `at`; its logged text.
    std::string Frame (const std::function<void ()>& draw, ImVec2 at = { 399, 599 }, bool down = false)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (at.x, at.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 320, 600 });
        ImGui::Begin ("bar", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::LogToBuffer ();
        draw ();
        std::string text = ImGui::GetCurrentContext ()->LogBuffer.c_str ();
        ImGui::LogFinish ();
        ImGui::End ();
        ImGui::Render ();
        return text;
    }
};

eh::TerrainDesignStatus Status (bool current, bool built, uint64_t revision, std::string error = {})
{
    eh::TerrainDesignStatus status;
    status.active = true;
    status.current = current;
    status.built = built;
    status.revision = revision;
    status.error = std::move (error);
    return status;
}

edit::Id Square (edit::Document& doc, double x, double y, double side)
{
    return edit::AddPolyline (doc, { { x, y, 0 }, { x + side, y, 0 }, { x + side, y + side, 0 }, { x, y + side, 0 } },
                              true);
}
} // namespace

TEST (HudVirtualEditorCommandBar, ChipStateTable191)
{
    EXPECT_EQ (eh::ChipState ({}, {}).kind, eh::ChipKind::None) << "no terrain design";
    EXPECT_EQ (eh::ChipState (Status (true, true, 7), {}).kind, eh::ChipKind::Current);
    EXPECT_EQ (eh::ChipState (Status (true, true, 7), {}).text, "Current r7");
    auto building = Status (false, false, 0);
    building.pending = true;
    EXPECT_EQ (eh::ChipState (building, {}).kind, eh::ChipKind::Building);
    building.pending = false, building.previewPending = true;
    EXPECT_EQ (eh::ChipState (building, {}).kind, eh::ChipKind::Building);
    EXPECT_EQ (eh::ChipState (Status (false, false, 0), {}).kind, eh::ChipKind::Stale) << "the document moved on";
    auto previewed = Status (true, false, 9);
    previewed.previewed = true;
    EXPECT_EQ (eh::ChipState (previewed, {}).kind, eh::ChipKind::Stale) << "a preview stands in for the mesh";
    const auto refused = eh::ChipState (Status (true, true, 7), "PropertyLineOutside: no ring");
    EXPECT_EQ (refused.kind, eh::ChipKind::Refused) << "a standing refusal wins over everything";
    EXPECT_EQ (refused.text, "Refused");
    EXPECT_EQ (refused.detail, "PropertyLineOutside: no ring") << "its hover and row 2 say what";
}

// #188: the refusal is what the user did not see -- it stays until a build of a newer revision
// succeeds, through edits (no current outcome) and new messages.
TEST (HudVirtualEditorCommandBar, RefusalStaysUntilANewerBuildSucceeds188)
{
    eh::Session s;
    eh::TrackBuildRefusal (s, Status (true, false, 5, "PropertyLineOutside: no ring"));
    EXPECT_EQ (s.buildRefusal, "PropertyLineOutside: no ring");
    eh::TrackBuildRefusal (s, Status (false, false, 0)); // an edit: nothing describes the document yet
    s.message = "Intersection complete.";
    EXPECT_FALSE (s.buildRefusal.empty ()) << "an edit or a message does not clear it";
    eh::TrackBuildRefusal (s, Status (true, true, 5));
    EXPECT_FALSE (s.buildRefusal.empty ()) << "a build of the refused revision itself does not clear it";
    Gui gui;
    const auto bar = gui.Frame ([&] { eh::detail::CommandBar (s); });
    EXPECT_NE (bar.find ("Refused"), std::string::npos) << bar;
    EXPECT_NE (bar.find ("PropertyLineOutside"), std::string::npos) << "row 2 names it" << bar;
    EXPECT_EQ (bar.find ("Intersection complete."), std::string::npos) << "over the last message" << bar;
    eh::TrackBuildRefusal (s, Status (true, true, 6));
    EXPECT_TRUE (s.buildRefusal.empty ()) << "a newer good build does";
}

// Every running command has its Cancel in the prompt row, and pressing it ends the command.
TEST (HudVirtualEditorCommandBar, CancelIsReachableDuringEveryCommand191)
{
    const auto run = [] (eh::Session& s, const char* name) {
        SCOPED_TRACE (name);
        const auto command = eh::detail::Running (s);
        ASSERT_TRUE (command);
        EXPECT_STREQ (command->name, name);
        Gui gui;
        ImVec2 cancel {};
        const auto bar = [&] {
            eh::detail::CommandBar (s);
            const ImVec2 min = ImGui::GetItemRectMin (), max = ImGui::GetItemRectMax ();
            cancel = { (min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f }; // the row's last item
        };
        const auto text = gui.Frame (bar);
        EXPECT_NE (text.find ("Cancel (Esc)"), std::string::npos) << text;
        gui.Frame (bar, cancel);
        gui.Frame (bar, cancel, true);
        gui.Frame (bar, cancel, false);
        EXPECT_FALSE (eh::detail::Running (s)) << "Cancel ended it";
    };
    {
        edit::Document doc;
        std::string error;
        ASSERT_TRUE (lg::SeedLandscapeDemo (doc, 0, error)) << error;
        eh::Session s (doc);
        ASSERT_TRUE (eh::BeginLandscapeCommand (s, lg::OperationKind::Union));
        run (s, "Union");
    }
    {
        edit::Document doc;
        const auto a = Square (doc, 0, 0, 4), b = Square (doc, 2, 2, 4);
        eh::Session s (doc);
        s.selection.Set ({ { a, edit::Part::Entity, 0 }, { b, edit::Part::Entity, 0 } });
        ASSERT_TRUE (eh::detail::BeginBoolean (s, edit::BooleanKind::Union)) << s.message;
        run (s, "Boolean");
    }
    {
        edit::Document doc;
        const auto a = Square (doc, 0, 0, 4);
        eh::Session s (doc);
        s.selection.Set ({ { a, edit::Part::Entity, 0 } });
        ASSERT_TRUE (eh::detail::BeginRectangularArray (s)) << s.message;
        run (s, "Array");
    }
    for (const auto tool : { eh::Tool::Mirror, eh::Tool::Trim }) {
        eh::Session s;
        eh::detail::SetTool (s, tool);
        run (s, tool == eh::Tool::Mirror ? "Mirror" : "Trim");
        EXPECT_EQ (s.tool, eh::Tool::Select);
    }
}

// The destructive action is not beside the exit: Exit is in the command bar, Delete is not.
TEST (HudVirtualEditorCommandBar, ExitAndDeleteAreNotAdjacent191)
{
    edit::Document doc;
    const auto a = Square (doc, 0, 0, 4);
    eh::Session s (doc);
    s.selection.Set ({ { a, edit::Part::Entity, 0 } });
    Gui gui;
    const auto bar = gui.Frame ([&] { eh::detail::CommandBar (s); });
    EXPECT_NE (bar.find ("Exit"), std::string::npos) << bar;
    EXPECT_EQ (bar.find ("Delete"), std::string::npos) << bar;
    const auto inspector = gui.Frame ([&] { eh::detail::Inspector (s); });
    EXPECT_NE (inspector.find ("Delete"), std::string::npos) << "Delete is in the Inspector's action row";
}

namespace {
// The centre of row 1's `index`-th glyph: the toolbar drawn first in the window.
ImVec2 ToolCentre (int index)
{
    const auto& style = ImGui::GetStyle ();
    const float side = ImGui::GetFontSize () + 2.0f * style.FramePadding.y;
    return { style.WindowPadding.x + index * (side + style.ItemSpacing.x) + side * 0.5f,
             style.WindowPadding.y + side * 0.5f };
}

// Presses row 1's `index`-th glyph; its place is measured in a first frame, where the font is set.
void Press (Gui& gui, const std::function<void ()>& draw, int index)
{
    ImVec2 at {};
    gui.Frame ([&] {
        at = ToolCentre (index);
        draw ();
    });
    gui.Frame (draw, at);
    gui.Frame (draw, at, true);
    gui.Frame (draw, at, false);
}
} // namespace

// #191 U3: row 1's glyphs, in order, set the same tool and landscape mode their radios did.
TEST (HudVirtualEditorCommandBar, ToolbarSelectsEveryTool191)
{
    struct Expected {
        eh::Tool tool;
        bool drawsSource;
    };
    const Expected expected[] = { { eh::Tool::Select, false },   { eh::Tool::Polyline, true },
                                  { eh::Tool::Path, true },      { eh::Tool::Curve, true },
                                  { eh::Tool::Rectangle, true }, { eh::Tool::Polygon, true },
                                  { eh::Tool::Ellipse, true },   { eh::Tool::Mirror, false },
                                  { eh::Tool::Trim, false } };
    for (int i = 0; i < int (std::size (expected)); ++i) {
        SCOPED_TRACE (i);
        eh::Session s;
        if (i == 0)
            eh::detail::SetTool (s, eh::Tool::Polyline); // Select must be a change
        Gui gui;
        const auto bar = [&] { eh::detail::Toolbar (s); };
        Press (gui, bar, i);
        EXPECT_EQ (s.tool, expected[i].tool);
        if (expected[i].drawsSource)
            EXPECT_EQ (s.landscapeMode, eh::LandscapeMode::DrawSource);
        else if (i == 0)
            EXPECT_EQ (s.landscapeMode, eh::LandscapeMode::Select);
    }
}

// The tool block keeps its enable condition: a running landscape command holds the tool.
TEST (HudVirtualEditorCommandBar, ToolbarDisabledWhileCommandRuns191)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (lg::SeedLandscapeDemo (doc, 0, error)) << error;
    eh::Session s (doc);
    ASSERT_TRUE (eh::BeginLandscapeCommand (s, lg::OperationKind::Union));
    const auto before = s.tool;
    Gui gui;
    Press (gui, [&] { eh::detail::Toolbar (s); }, 1);
    EXPECT_EQ (s.tool, before) << "Polyline is not reachable while Union runs";
    EXPECT_TRUE (eh::detail::Running (s));
}

// The pinned A + B stays within #191's budget at 320 px: four rows, at most 120 logical px.
TEST (HudVirtualEditorCommandBar, PinnedHeaderFitsTheBudget191)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (lg::SeedLandscapeDemo (doc, 0, error)) << error;
    eh::Session s (doc);
    s.message = "Intersection complete. Sources remain editable. A long message that has to be elided here.";
    Gui gui;
    float height = 0, left = 0, after = -1, frame = 0;
    gui.Frame ([&] {
        const float top = ImGui::GetCursorPosY ();
        left = ImGui::GetCursorPosX ();
        eh::PanelHeader (s);
        after = ImGui::GetCursorPosX ();
        height = ImGui::GetCursorPosY () - top - ImGui::GetStyle ().ItemSpacing.y;
        frame = ImGui::GetFrameHeight ();
    });
    // The page starts under the header, never beside its last control (a helper once resolved to
    // detail::Contours, which draws nothing, and left row 2 open for the page).
    EXPECT_EQ (after, left) << "the header ends on a fresh line";
    EXPECT_GE (height, 4.0f * frame) << "A + B are four rows";
    EXPECT_LE (height, 120.0f) << "A + B: " << height << " px";
}

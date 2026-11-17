// #191 U5: the Outliner -- Objects, Recipe and Issues views of one document under the Inspector,
// capped with its own scroll, collapsible to one line; rows and the view's selection follow each
// other, a row under the pointer is what the view highlights, and the issue count is on its tab.
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditLandscape.hpp"
#include "Edit/EditOrganize.hpp"
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
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~Gui ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    // One frame of the Outliner in a 320 x 700 window, the pointer at `at`; its logged text.
    std::string Frame (eh::Session& s, ImVec2 at = { 399, 799 }, bool down = false, int button = 0)
    {
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (at.x, at.y);
        io.AddMouseButtonEvent (0, down && button == 0);
        io.AddMouseButtonEvent (1, down && button == 1);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 320, 700 });
        ImGui::Begin ("outliner", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::LogToBuffer ();
        eh::detail::Outliner (s);
        std::string text = context->LogBuffer.c_str ();
        ImGui::LogFinish ();
        ImGui::End ();
        ImGui::Render ();
        return text;
    }
    // The tab labels the frame laid out (ImGui does not log a tab's label).
    std::string Tabs () const
    {
        std::string names;
        for (int i = 0; i < context->TabBars.GetMapSize (); ++i)
            if (auto* bar = context->TabBars.TryGetMapData (i))
                for (auto& tab : bar->Tabs)
                    names += std::string (ImGui::TabBarGetTabName (bar, &tab)) + "\n";
        return names;
    }
    ImGuiWindow* Child (const char* id) const
    {
        for (auto* window : context->Windows)
            if (std::string (window->Name).find (id) != std::string::npos)
                return window;
        return nullptr;
    }
};

edit::Document Squares (int count, bool landscape = false)
{
    edit::Document doc;
    std::string error;
    if (landscape)
        EXPECT_TRUE (lg::StartLandscape (doc, { 0x19, 0x1, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0x3 }, error))
            << error;
    for (int i = 0; i < count; ++i) {
        const double x = 6.0 * i;
        edit::AddPolyline (doc, { { x, 0, 0 }, { x + 4, 0, 0 }, { x + 4, 4, 0 }, { x, 4, 0 } }, true);
    }
    return doc;
}

bool Has (const std::string& text, const char* what)
{
    return text.find (what) != std::string::npos;
}
} // namespace

// A row press selects its object, and only it; the row under the pointer is the view's hover.
TEST (HudVirtualEditorOutliner, OutlinerRowSelectsObject191)
{
    eh::Session s (Squares (3));
    Gui gui;
    gui.Frame (s);
    bool hovered = false, selected = false;
    for (float y = 20; y < 690 && !selected; y += 3) {
        gui.Frame (s, { 200, y });
        hovered = hovered || s.outlinerHover.entity != edit::kNoId;
        if (s.outlinerHover.entity == edit::kNoId)
            continue;
        const auto expected = s.outlinerHover;
        gui.Frame (s, { 200, y }, true);
        gui.Frame (s, { 200, y }, false);
        if (s.selection.Items ().empty ())
            continue;
        ASSERT_EQ (s.selection.Items ().size (), 1u);
        EXPECT_EQ (s.selection.Items ()[0].entity, expected.entity);
        EXPECT_EQ (s.selection.Items ()[0].part, edit::Part::Entity);
        selected = true;
    }
    EXPECT_TRUE (hovered) << "a row under the pointer is what the view highlights";
    EXPECT_TRUE (selected);
    EXPECT_EQ (s.edits.UndoDepth (), 0u) << "a row press only selects";
    // Off the rows, nothing is hovered.
    gui.Frame (s, { 399, 799 });
    EXPECT_EQ (s.outlinerHover.entity, edit::kNoId);
}

// A selection made in the view scrolls its row into the Objects view, which holds about twelve
// rows and scrolls on its own.
TEST (HudVirtualEditorOutliner, ViewSelectionMarksRow191)
{
    auto doc = Squares (40);
    const auto last = doc.entities.back ().id;
    eh::Session s (std::move (doc));
    Gui gui;
    gui.Frame (s);
    gui.Frame (s);
    auto* objects = gui.Child ("##objects");
    ASSERT_NE (objects, nullptr);
    EXPECT_LT (objects->Size.y, 400.0f) << "the view is capped, not as tall as its forty rows";
    EXPECT_EQ (objects->Scroll.y, 0.0f);
    s.selection.Set ({ { last, edit::Part::Entity, 0 } }); // as a click in the view does
    gui.Frame (s);
    gui.Frame (s);
    EXPECT_GT (objects->Scroll.y, 0.0f) << "the selected row was scrolled into sight";
    EXPECT_EQ (s.outlinerSeen, s.selection.Items ());
}

// The Issues tab carries the count; collapsed, the Outliner is one line; search filters the rows.
TEST (HudVirtualEditorOutliner, IssueCountCollapseAndSearch191)
{
    eh::Session s (Squares (2, true));
    Gui gui;
    eh::Landscape (s);
    lg::LandscapeDiagnostic warning;
    warning.message = "fixture warning";
    s.landscape.diagnostics.push_back (warning);
    EXPECT_EQ (eh::detail::IssueCount (s), 1u);
    auto text = gui.Frame (s);
    const auto tabs = gui.Tabs ();
    EXPECT_TRUE (Has (tabs, "Issues 1###issues\n")) << tabs; // ### keeps the tab as its count changes
    EXPECT_TRUE (Has (tabs, "Recipe\n")) << tabs;
    EXPECT_TRUE (Has (tabs, "Objects\n")) << tabs;

    // Rows read "Outline  #<id>": a whole line, ended by IM_NEWLINE or by the end of the log.
    const auto row = [&] (const std::string& logged, size_t i) {
        const std::string id = "#" + std::to_string (s.edits.Doc ().entities[i].id);
        return Has (logged, (id + IM_NEWLINE).c_str ()) ||
               (logged.size () >= id.size () && logged.compare (logged.size () - id.size (), id.size (), id) == 0);
    };
    EXPECT_TRUE (row (text, 0)) << text;
    EXPECT_TRUE (row (text, 1)) << text;
    s.outlinerFilter = "#" + std::to_string (s.edits.Doc ().entities.back ().id);
    text = gui.Frame (s);
    EXPECT_TRUE (row (text, 1)) << text;
    EXPECT_FALSE (row (text, 0)) << "a row whose name lacks the search text is filtered out";

    s.outlinerCollapsed = true;
    text = gui.Frame (s);
    EXPECT_TRUE (Has (text, "objects"));
    EXPECT_FALSE (Has (text, "Issues"));
    EXPECT_FALSE (Has (text, "Search"));
    // A click on the state chip asks for the Issues view: the Outliner opens on it.
    s.outlinerShow = 2;
    text = gui.Frame (s);
    EXPECT_FALSE (s.outlinerCollapsed);
    EXPECT_EQ (s.outlinerShow, -1);
}

// #124: a group is renamed from its row's right-click box, one undo step, keeping its id and
// members; an empty name or one a group beside it has is refused with the reason and the box stays.
TEST (HudVirtualEditorOutliner, AGroupRenamesFromItsRowAsOneUndo124)
{
    auto doc = Squares (4);
    std::string error;
    const edit::Id beds = edit::GroupObjects (
        doc, { { doc.entities[0].id, edit::Part::Entity, 0 }, { doc.entities[1].id, edit::Part::Entity, 0 } }, "beds",
        error);
    ASSERT_NE (beds, edit::kNoId) << error;
    ASSERT_NE (edit::GroupObjects (
                   doc,
                   { { doc.entities[2].id, edit::Part::Entity, 0 }, { doc.entities[3].id, edit::Part::Entity, 0 } },
                   "lawn", error),
               edit::kNoId)
        << error;
    const auto members = edit::GroupMembers (doc, beds);
    eh::Session s (doc);
    Gui gui;
    gui.Frame (s);
    const auto popupOpen = [&] { return gui.context->OpenPopupStack.Size > 0; };
    // Right-click down the rows until a box opens: the first group row is "beds".
    ImVec2 at {};
    for (float y = 20; y < 690 && !popupOpen (); y += 3) {
        at = { 120, y };
        gui.Frame (s, at);
        gui.Frame (s, at, true, 1);
        gui.Frame (s, at, false, 1);
        gui.Frame (s, at);
    }
    ASSERT_TRUE (popupOpen ()) << "a group row opens its rename box on right-click";
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
    const auto type = [&] (const char* text) {
        ImGui::GetIO ().AddInputCharactersUTF8 (text);
        gui.Frame (s, at);
        ImGui::GetIO ().AddKeyEvent (ImGuiKey_Enter, true);
        gui.Frame (s, at);
        ImGui::GetIO ().AddKeyEvent (ImGuiKey_Enter, false);
        gui.Frame (s, at);
    };
    type ("lawn");
    EXPECT_TRUE (popupOpen ()) << "a refused name keeps the box open";
    EXPECT_NE (s.message.find ("already called"), std::string::npos) << s.message;
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
    EXPECT_EQ (edit::FindGroup (s.edits.Doc (), beds)->name, "beds");

    type ("  planting ");
    EXPECT_FALSE (popupOpen ()) << s.message;
    ASSERT_NE (edit::FindGroup (s.edits.Doc (), beds), nullptr);
    EXPECT_EQ (edit::FindGroup (s.edits.Doc (), beds)->name, "planting");
    EXPECT_EQ (edit::GroupMembers (s.edits.Doc (), beds), members) << "a rename moves nothing";
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_EQ (edit::FindGroup (s.edits.Doc (), beds)->name, "beds");

    // The wrapper: an empty name is refused with a reason; the same name, quietly.
    EXPECT_FALSE (eh::detail::RenameGroup (s, beds, "   "));
    EXPECT_NE (s.message.find ("needs a name"), std::string::npos) << s.message;
    EXPECT_FALSE (eh::detail::RenameGroup (s, beds, "beds"));
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
}

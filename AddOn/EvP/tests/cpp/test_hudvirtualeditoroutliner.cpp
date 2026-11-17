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
#include <cstring>
#include <functional>
#include <map>
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

// #124: Focus in view frames what is shown of a layer, group or object, and is a view change
// only: the plan asks for the model box with a margin, 3D for the pixel rectangle the box
// projects to. Nothing shown (an empty or hidden target) or nothing in front is refused with a
// reason, and the document's revision and undo never move.
TEST (HudVirtualEditorOutliner, FocusFramesWhatIsShownAndNeverEditsTheDocument124)
{
    auto doc = Squares (3); // 4 m squares at x = 0, 6, 12
    std::string error;
    const edit::Id a = doc.entities[0].id, b = doc.entities[1].id, c = doc.entities[2].id;
    ASSERT_TRUE (edit::SetHidden (doc, { { c, edit::Part::Entity, 0 } }, true, error)) << error;
    eh::Session s (doc);
    const auto revision = s.edits.Doc ().revision;

    ASSERT_TRUE (eh::detail::FocusOn (s, { a, b, c }, "group beds")) << s.message;
    ASSERT_TRUE (s.focusPending.has_value ());
    const auto& pending = *s.focusPending;
    // Only the shown two (x 0..10, y 0..4), with a margin; the hidden third (x 12..16) is not framed.
    EXPECT_LT (pending.lo.x, 0.0);
    EXPECT_GT (pending.hi.x, 10.0);
    EXPECT_LT (pending.hi.x, 12.0);
    EXPECT_LT (pending.lo.y, 0.0);
    EXPECT_GT (pending.hi.y, 4.0);

    geomsrv::archviz::overlayhud::Input plan;
    plan.planar = true;
    plan.plan[0] = 10, plan.plan[2] = 100, plan.plan[4] = -10, plan.plan[5] = 400;
    eh::detail::TakeFocus (s, eh::detail::Map { plan });
    EXPECT_FALSE (s.focusPending.has_value ());
    ASSERT_TRUE (s.focusRequest.has_value ());
    EXPECT_TRUE (s.focusRequest->planar);
    EXPECT_EQ (s.focusRequest->what, "group beds");

    // 3D: the rectangle the box's corners project to (here a plain scale, y down).
    ASSERT_TRUE (eh::detail::FocusOn (s, { a }, "Outline  #1"));
    geomsrv::archviz::overlayhud::Input view;
    view.planar = false;
    view.project = [] (double x, double y, double, float& px, float& py) {
        px = float (100 + 10 * x), py = float (400 - 10 * y);
        return true;
    };
    eh::detail::TakeFocus (s, eh::detail::Map { view });
    ASSERT_TRUE (s.focusRequest.has_value ());
    EXPECT_FALSE (s.focusRequest->planar);
    EXPECT_LT (s.focusRequest->rect[0], 100.0f); // left of x = 0
    EXPECT_GT (s.focusRequest->rect[2], 140.0f); // right of x = 4
    EXPECT_LT (s.focusRequest->rect[1], 360.0f); // above y = 4
    EXPECT_GT (s.focusRequest->rect[3], 400.0f); // below y = 0

    EXPECT_TRUE (s.focusRequest->onScreen);

    // Behind the camera: nothing projects. The box is still asked for -- the Tapioca viewer frames
    // it -- with no rectangle, which Archicad's 3D zoom needs and refuses without (FollowFocus).
    s.focusRequest.reset ();
    ASSERT_TRUE (eh::detail::FocusOn (s, { a }, "Outline  #1"));
    view.project = [] (double, double, double, float&, float&) { return false; };
    eh::detail::TakeFocus (s, eh::detail::Map { view });
    ASSERT_TRUE (s.focusRequest.has_value ());
    EXPECT_FALSE (s.focusRequest->planar);
    EXPECT_FALSE (s.focusRequest->onScreen);
    EXPECT_LT (s.focusRequest->lo.x, s.focusRequest->hi.x);

    // Hidden-only and empty targets.
    EXPECT_FALSE (eh::detail::FocusOn (s, { c }, "layer Hidden"));
    EXPECT_NE (s.message.find ("hidden"), std::string::npos) << s.message;
    EXPECT_FALSE (eh::detail::FocusOn (s, {}, "layer Empty"));
    EXPECT_NE (s.message.find ("empty"), std::string::npos) << s.message;
    EXPECT_FALSE (s.focusPending.has_value ());

    EXPECT_EQ (s.edits.Doc ().revision, revision) << "focus is a view change";
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
}

// #124: dropping a layer or group row is one undo step; a refused drop says why and records nothing.
TEST (HudVirtualEditorOutliner, DroppingALayerOrGroupRowIsOneUndo124)
{
    auto doc = Squares (4);
    const edit::Id l1 = edit::AddLayer (doc, "A"), l2 = edit::AddLayer (doc, "B");
    std::string error;
    const auto R = [&] (size_t i) { return edit::Ref { doc.entities[i].id, edit::Part::Entity, 0 }; };
    const edit::Id g1 = edit::GroupObjects (doc, { R (0), R (1) }, "g1", error);
    const edit::Id g2 = edit::GroupObjects (doc, { R (2), R (3) }, "g2", error);
    ASSERT_NE (g2, edit::kNoId) << error;
    eh::Session s (doc);
    ASSERT_TRUE (eh::detail::DropLayer (s, l2, l1));
    EXPECT_EQ (s.edits.Doc ().layers.front ().id, l2);
    ASSERT_TRUE (eh::detail::DropGroup (s, g2, g1));
    EXPECT_EQ (s.edits.Doc ().groups.front ().id, g2);
    EXPECT_EQ (s.edits.UndoDepth (), 2u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_EQ (s.edits.Doc ().groups.front ().id, g1);
    EXPECT_FALSE (eh::detail::DropLayer (s, l1, edit::kNoId));
    EXPECT_NE (s.message.find ("layer 0"), std::string::npos) << s.message;
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    // The panel draws with the grip and the drag targets in place.
    Gui gui;
    gui.Frame (s);
}

namespace {
// The Objects view's Layers header open, as a click on it leaves it. A probe that lands on the
// header toggles it, so each one opens it again first.
void OpenLayers (Gui& gui, eh::Session& s)
{
    if (ImGuiWindow* objects = gui.Child ("##objects"); objects != nullptr)
        objects->StateStorage.SetInt (objects->GetID ("Layers"), 1);
    gui.Frame (s);
}

// Press at `at` and drag down past ImGui's threshold: the id the row drag carries as `type`, if
// the press was on such a row's handle. The drag is then let go far from any row.
edit::Id Grab (Gui& gui, eh::Session& s, ImVec2 at, const char* type)
{
    OpenLayers (gui, s);
    gui.Frame (s, at);
    gui.Frame (s, at, true);
    gui.Frame (s, { at.x, at.y + 4 }, true);
    gui.Frame (s, { at.x, at.y + 12 }, true);
    edit::Id id = edit::kNoId;
    if (const ImGuiPayload* p = ImGui::GetDragDropPayload (); p != nullptr && p->IsDataType (type))
        std::memcpy (&id, p->Data, sizeof id);
    gui.Frame (s, { 399, 799 }, true);
    gui.Frame (s, { 399, 799 });
    gui.Frame (s, { 399, 799 });
    return id;
}

// Each row handle found down the panel at `x`: the y it was grabbed at, by the id it carries.
std::map<edit::Id, float> Handles (Gui& gui, eh::Session& s, float x, const char* type)
{
    std::map<edit::Id, float> at;
    for (float y = 20; y < 690; y += 4)
        if (const edit::Id id = Grab (gui, s, { x, y }, type); id != edit::kNoId && !at.count (id))
            at[id] = y;
    return at;
}

// Press on one handle, carry it over another, let go there.
void DragOnto (Gui& gui, eh::Session& s, float x, float from, float to)
{
    OpenLayers (gui, s);
    gui.Frame (s, { x, from });
    gui.Frame (s, { x, from }, true);
    for (int step = 1; step <= 6; ++step)
        gui.Frame (s, { x, from + (to - from) * float (step) / 6.0f }, true);
    gui.Frame (s, { x, to }, true);
    gui.Frame (s, { x, to });
    gui.Frame (s, { 399, 799 });
}
} // namespace

// #124, through the panel as a user does it: a layer dragged by its grip onto another layer, and a
// group row onto a sibling group, each one undo step; layer 0 cannot be picked up.
TEST (HudVirtualEditorOutliner, LayersAndGroupsReorderByDraggingTheirRows124)
{
    auto doc = Squares (4);
    const edit::Id a = edit::AddLayer (doc, "A"), b = edit::AddLayer (doc, "B");
    std::string error;
    const auto R = [&] (size_t i) { return edit::Ref { doc.entities[i].id, edit::Part::Entity, 0 }; };
    const edit::Id g1 = edit::GroupObjects (doc, { R (0), R (1) }, "g1", error);
    const edit::Id g2 = edit::GroupObjects (doc, { R (2), R (3) }, "g2", error);
    ASSERT_NE (g2, edit::kNoId) << error;
    eh::Session s (doc);
    Gui gui;
    gui.Frame (s);
    ASSERT_NE (gui.Child ("##objects"), nullptr);

    // The grip is the row's first item, just inside the Objects view's border.
    constexpr float kGrip = 24.0f;
    const auto layers = Handles (gui, s, kGrip, "TAPIOCA_LAYER_ROW");
    ASSERT_EQ (layers.size (), 2u) << "A and B have grips; layer 0's is disabled";
    ASSERT_TRUE (layers.count (a) && layers.count (b));
    EXPECT_LT (layers.at (a), layers.at (b));
    EXPECT_EQ (s.edits.UndoDepth (), 0u) << "a drag let go away from the rows moves nothing";

    DragOnto (gui, s, kGrip, layers.at (b), layers.at (a));
    ASSERT_EQ (s.edits.Doc ().layers.size (), 2u);
    EXPECT_EQ (s.edits.Doc ().layers[0].id, b) << s.message;
    EXPECT_EQ (s.edits.Doc ().layers[1].id, a);
    EXPECT_EQ (s.edits.UndoDepth (), 1u);

    const auto groups = Handles (gui, s, 150.0f, "TAPIOCA_GROUP_ROW");
    ASSERT_TRUE (groups.count (g1) && groups.count (g2)) << groups.size ();
    EXPECT_EQ (s.edits.UndoDepth (), 1u);
    DragOnto (gui, s, 150.0f, groups.at (g2), groups.at (g1));
    EXPECT_EQ (s.edits.Doc ().groups.front ().id, g2) << s.message;
    EXPECT_EQ (s.edits.UndoDepth (), 2u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_EQ (s.edits.Doc ().groups.front ().id, g1);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_EQ (s.edits.Doc ().layers.front ().id, a);
}

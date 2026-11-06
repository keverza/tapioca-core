#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "ArchViz/OverlayHud.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPrimitives.hpp"
#include "hud_fixture.hpp"
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <string>

// A panel widget's drag is one undoable gesture (#24, #157 G1): the path panel's kerb radius,
// picked-corner radius and break smoothing used to apply a whole history entry every frame.
// These drive real multi-frame ImGui drags through the same controls the panel draws.
namespace eh = geomsrv::archviz::edithud;
namespace ed = geomsrv::edit;
namespace hud = geomsrv::archviz::overlayhud;

namespace {
struct WidgetDrive {
    ImGuiContext* previous = ImGui::GetCurrentContext ();
    ImGuiContext* context = ImGui::CreateContext ();
    eh::Session session;
    std::function<void (eh::Session&)> controls;
    bool drawn = true;
    ImVec2 itemMin {}, itemMax {};

    WidgetDrive (ed::Document doc, std::function<void (eh::Session&)> draw)
        : session (std::move (doc)), controls (std::move (draw))
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { 500, 300 };
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    WidgetDrive (const WidgetDrive&) = delete;
    WidgetDrive& operator= (const WidgetDrive&) = delete;
    ~WidgetDrive ()
    {
        ImGui::DestroyContext (context);
        ImGui::SetCurrentContext (previous);
    }
    void Frame (ImVec2 pointer, bool down)
    {
        auto& io = ImGui::GetIO ();
        io.DeltaTime = 1.0f / 60;
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 450, 250 });
        ImGui::Begin ("path widgets");
        eh::detail::SettleWidgetGesture (session); // as Panel does, before its widgets
        if (drawn) {
            controls (session);
            itemMin = ImGui::GetItemRectMin ();
            itemMax = ImGui::GetItemRectMax ();
        }
        ImGui::End ();
        ImGui::Render ();
    }
    ImVec2 Centre () const
    {
        return { (itemMin.x + itemMax.x) * 0.5f, (itemMin.y + itemMax.y) * 0.5f };
    }
    // The pointer is over the widget a frame before it presses there, as the input layer delivers it.
    void Press ()
    {
        Frame ({ 440, 240 }, false);
        Frame (Centre (), false);
        Frame (Centre (), true);
    }
    void DragBy (float dx)
    {
        const ImVec2 at = ImGui::GetIO ().MousePos;
        Frame ({ at.x + dx, at.y }, true);
    }
    void Release ()
    {
        Frame (ImGui::GetIO ().MousePos, false);
        Frame (ImGui::GetIO ().MousePos, false);
    }
    void Escape ()
    {
        // The host runs the editor's keys before the ImGui frames of the same layout.
        hud::Input input = hudtest::At (0, 0);
        input.viewActive = true;
        input.keys = { { 0x1B, true } };
        eh::Keys (session, input, false);
    }
    const ed::Document& D () const
    {
        return session.edits.Doc ();
    }
};

ed::Document OnePath ()
{
    ed::Document doc;
    ed::AddPath (doc, { { 0, 0, 0 }, { 20, 0, 0 }, { 20, 20, 0 } }, 4.0);
    return doc;
}

// The path panel's radius controls, on the document's one path.
void KerbControls (eh::Session& s)
{
    if (!s.edits.Doc ().paths.empty ())
        eh::detail::PathRadiusControls (s, s.edits.Doc ().paths.front ());
}

struct KerbDrive : WidgetDrive {
    ed::Id path;
    KerbDrive () : WidgetDrive (OnePath (), KerbControls), path (D ().paths.front ().id)
    {
        session.selection.Set ({ { path, ed::Part::Entity, 0 } });
    }
};
} // namespace

TEST (HudPathWidgetGesture, AKerbRadiusDragAcrossFramesIsOneUndoEntry)
{
    KerbDrive d;
    const ed::Id path = d.path;
    const ed::Document original = d.D ();
    d.Press ();
    ASSERT_NE (d.session.widgetGesture, 0u);
    ASSERT_TRUE (d.session.edits.InGesture ());
    double previous = 0.0;
    int changes = 0;
    for (int i = 0; i < 4; ++i) {
        d.DragBy (12.0f);
        const double now = ed::FindPath (d.D (), path)->edgeRadius;
        changes += now > previous ? 1 : 0;
        previous = now;
        EXPECT_EQ (d.session.edits.UndoDepth (), 0u) << "a frame of the drag wrote history";
    }
    EXPECT_GE (changes, 2) << "the preview follows the drag";
    EXPECT_GT (previous, 0.0);
    d.Release ();
    EXPECT_FALSE (d.session.edits.InGesture ());
    EXPECT_EQ (d.session.widgetGesture, 0u);
    EXPECT_EQ (d.session.edits.UndoDepth (), 1u);
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), path)->edgeRadius, previous);
    ASSERT_TRUE (d.session.edits.Undo ());
    EXPECT_EQ (d.D ().paths, original.paths);
    EXPECT_FALSE (d.session.edits.CanUndo ());
}

TEST (HudPathWidgetGesture, EscapeMidDragRestoresAndTheRestOfTheDragChangesNothing)
{
    KerbDrive d;
    const ed::Id path = d.path;
    const ed::Document original = d.D ();
    d.Press ();
    d.DragBy (15.0f);
    d.DragBy (15.0f);
    ASSERT_GT (ed::FindPath (d.D (), path)->edgeRadius, 0.0);
    d.Escape ();
    EXPECT_FALSE (d.session.edits.InGesture ());
    EXPECT_EQ (d.D ().paths, original.paths);
    // Esc gave up only the gesture: the selection and the editor stay.
    EXPECT_EQ (d.session.selection.Items ().size (), 1u);
    EXPECT_FALSE (d.session.close);
    d.DragBy (20.0f); // the button is still held over the widget
    EXPECT_EQ (d.D ().paths, original.paths);
    d.Release ();
    EXPECT_EQ (d.session.widgetGesture, 0u);
    EXPECT_EQ (d.session.edits.UndoDepth (), 0u);
    EXPECT_EQ (d.D ().paths, original.paths);
    // A second Esc is the ladder's next step again, not a stale widget gesture.
    d.Escape ();
    EXPECT_TRUE (d.session.selection.Items ().empty ());
}

TEST (HudPathWidgetGesture, AClickWithoutChangeLeavesNoHistoryOrRevision)
{
    KerbDrive d;
    const uint64_t revision = d.D ().revision;
    d.Press ();
    d.Release ();
    EXPECT_FALSE (d.session.edits.InGesture ());
    EXPECT_EQ (d.session.edits.UndoDepth (), 0u);
    EXPECT_EQ (d.D ().revision, revision);
}

TEST (HudPathWidgetGesture, AWidgetThatStopsBeingDrawnMidDragCancelsItsGesture)
{
    KerbDrive d;
    const ed::Id path = d.path;
    const ed::Document original = d.D ();
    d.Press ();
    d.DragBy (20.0f);
    ASSERT_GT (ed::FindPath (d.D (), path)->edgeRadius, 0.0);
    d.drawn = false; // the selection changed: the path's panel is gone
    d.DragBy (5.0f);
    d.DragBy (5.0f);
    EXPECT_FALSE (d.session.edits.InGesture ()) << "stranded gesture";
    EXPECT_EQ (d.session.widgetGesture, 0u);
    EXPECT_EQ (d.D ().paths, original.paths);
    EXPECT_EQ (d.session.edits.UndoDepth (), 0u);
    // The next gesture starts cleanly (past the double-click time, which would type instead).
    d.drawn = true;
    d.Release ();
    for (int i = 0; i < 40; ++i)
        d.Frame ({ 440, 240 }, false);
    d.Press ();
    d.DragBy (20.0f);
    d.Release ();
    EXPECT_EQ (d.session.edits.UndoDepth (), 1u);
}

TEST (HudPathWidgetGesture, AnotherOwnersGestureIsNeitherMovedNorEndedByTheWidget)
{
    KerbDrive d;
    const ed::Document original = d.D ();
    ASSERT_TRUE (d.session.edits.Begin ()); // e.g. a canvas drag already owns the edit
    d.Press ();
    d.DragBy (20.0f);
    d.DragBy (20.0f);
    EXPECT_EQ (d.D ().paths, original.paths);
    d.Release ();
    EXPECT_TRUE (d.session.edits.InGesture ()) << "the widget ended a gesture it did not own";
    EXPECT_EQ (d.session.widgetGesture, 0u);
    d.session.edits.Cancel ();
    EXPECT_EQ (d.session.edits.UndoDepth (), 0u);
}

TEST (HudPathWidgetGesture, ARefusedValueKeepsTheLastValidPreviewAndSaysWhy)
{
    ed::Document doc = OnePath ();
    const ed::Id path = doc.paths.front ().id, id = path;
    // A test operation refusing radii past 0.5 m: the drag runs past it.
    WidgetDrive d (std::move (doc), [id] (eh::Session& s) {
        float r = float (ed::FindPath (s.edits.Doc (), id)->edgeRadius);
        ImGui::SetNextItemWidth (140.0f);
        ImGui::DragFloat ("Limited radius", &r, 0.05f, 0.0f, 20.0f, "%.2f m");
        const double v = double (r);
        eh::detail::WidgetGesture (s, [id, v] (ed::Document& doc, std::string& error) {
            if (v > 0.5) {
                error = "too round";
                return false;
            }
            return ed::SetPathEdgeRadius (doc, id, v, error);
        });
    });
    d.Press ();
    d.DragBy (8.0f); // 0.05 m/px past the lock threshold: within the limit
    const double valid = ed::FindPath (d.D (), path)->edgeRadius;
    ASSERT_GT (valid, 0.0);
    ASSERT_LE (valid, 0.5);
    d.DragBy (40.0f);
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), path)->edgeRadius, valid);
    EXPECT_EQ (d.session.message, "too round");
    EXPECT_TRUE (d.session.edits.InGesture ());
    d.Release ();
    EXPECT_EQ (d.session.edits.UndoDepth (), 1u);
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), path)->edgeRadius, valid);
}

TEST (HudPathWidgetGesture, ASmoothingSliderDragIsOneUndoEntry)
{
    ed::Document doc;
    const ed::Id path = ed::AddPath (doc, { { 0, 0, 0 }, { 30, 0, 0 } }, 2.0);
    std::string error;
    const ed::Id gradeBreak = ed::NewId (doc);
    ASSERT_TRUE (ed::InsertBreak (ed::FindPath (doc, path)->profile, 15.0, gradeBreak, error)) << error;
    ed::FindPath (doc, path)->profile[1].z = 2.0;
    const auto original = doc.paths;
    WidgetDrive d (std::move (doc), [path] (eh::Session& s) {
        if (const ed::Path* p = ed::FindPath (s.edits.Doc (), path))
            eh::detail::BreakSmoothingControl (s, *p, 1);
    });
    d.session.selection.Set ({ { path, ed::Part::Break, gradeBreak } });
    d.Frame ({ 440, 240 }, false);
    ASSERT_GT (d.itemMax.x, d.itemMin.x) << "no room for a vertical curve";
    // A slider press sets the value under the pointer; then the drag moves it.
    d.Frame ({ d.itemMin.x + 4, d.Centre ().y }, false);
    d.Frame ({ d.itemMin.x + 4, d.Centre ().y }, true);
    for (int i = 0; i < 4; ++i) {
        d.DragBy (15.0f);
        EXPECT_EQ (d.session.edits.UndoDepth (), 0u);
    }
    const double curve = ed::FindPath (d.D (), path)->profile[1].curve;
    EXPECT_GT (curve, 0.0);
    d.Release ();
    EXPECT_EQ (d.session.edits.UndoDepth (), 1u);
    EXPECT_DOUBLE_EQ (ed::FindPath (d.D (), path)->profile[1].curve, curve);
    ASSERT_TRUE (d.session.edits.Undo ());
    EXPECT_EQ (d.D ().paths, original);
}

#include "ArchViz/HudVirtualEditorDetail.hpp"
#include "Edit/EditEllipse.hpp"
#include "Edit/EditOrganize.hpp"
#include "Edit/EditShape.hpp"
#include <gtest/gtest.h>
#include <limits>

namespace ed = geomsrv::edit;
namespace eh = geomsrv::archviz::edithud;
namespace detail = eh::detail;

namespace {
ed::Id MakeEllipse (eh::Session& s, bool circle = false)
{
    ed::Document doc;
    ed::PrimitiveInput input;
    input.kind = ed::Primitive::Ellipse;
    input.second = { 5, 2, 0 };
    input.uniform = circle;
    std::string error;
    const ed::Id id = ed::AddPrimitive (doc, input, "", error);
    EXPECT_NE (id, ed::kNoId) << error;
    s.edits = ed::Session (doc);
    s.selection.Set ({ { id, ed::Part::Entity, 0 } });
    return id;
}

ed::EllipseParameters Parameters (const eh::Session& s, ed::Id id)
{
    ed::EllipseParameters out;
    std::string error;
    EXPECT_TRUE (ed::GetEllipseParameters (*ed::Find (s.edits.Doc (), id), out, error)) << error;
    return out;
}
} // namespace

TEST (HudVirtualEditorEllipse, TypedValuesKeepDoublePrecisionAndUndoOneFieldAtATime)
{
    eh::Session s;
    const auto id = MakeEllipse (s);
    const double exact = 1.2345678901234567;
    ASSERT_TRUE (detail::SetSelectedEllipseValue (s, detail::EllipseField::CenterX, exact));
    EXPECT_DOUBLE_EQ (Parameters (s, id).center.x, exact);
    ASSERT_TRUE (detail::SetSelectedEllipseValue (s, detail::EllipseField::MajorRadius, exact + 5));
    EXPECT_NEAR (Parameters (s, id).majorRadius, exact + 5, 2e-15);
    ASSERT_TRUE (detail::SetSelectedEllipseValue (s, detail::EllipseField::MinorRadius, exact));
    EXPECT_NEAR (Parameters (s, id).minorRadius, exact, 2e-15);
    ASSERT_TRUE (detail::SetSelectedEllipseValue (s, detail::EllipseField::Rotation, 37.1234567890123));
    EXPECT_NEAR (Parameters (s, id).rotation, 37.1234567890123 * 3.14159265358979323846 / 180, 2e-15);
    EXPECT_EQ (s.edits.UndoDepth (), 4u);
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_NEAR (Parameters (s, id).rotation, 0, 1e-15);
    EXPECT_DOUBLE_EQ (Parameters (s, id).center.x, exact);
    ASSERT_TRUE (s.edits.Redo ());
    EXPECT_TRUE (ed::Find (s.edits.Doc (), id)->ellipse.has_value ());
}

TEST (HudVirtualEditorEllipse, CircleModeUsesMajorRadiusAndOneUndoRestoresEllipse)
{
    eh::Session s;
    const auto id = MakeEllipse (s);
    ASSERT_TRUE (detail::SetSelectedEllipseMode (s, ed::EllipseMode::Circle));
    EXPECT_EQ (Parameters (s, id).mode, ed::EllipseMode::Circle);
    EXPECT_DOUBLE_EQ (Parameters (s, id).minorRadius, 5);
    ASSERT_TRUE (detail::SetSelectedEllipseValue (s, detail::EllipseField::MajorRadius, 3.123456789012345));
    EXPECT_DOUBLE_EQ (Parameters (s, id).majorRadius, Parameters (s, id).minorRadius);
    ASSERT_TRUE (s.edits.Undo ());
    ASSERT_TRUE (s.edits.Undo ());
    EXPECT_EQ (Parameters (s, id).mode, ed::EllipseMode::Ellipse);
    EXPECT_DOUBLE_EQ (Parameters (s, id).minorRadius, 2);
    EXPECT_FALSE (detail::SetSelectedEllipseMode (s, ed::EllipseMode::Ellipse));
}

TEST (HudVirtualEditorEllipse, PartSelectionOffersExplicitUndoableConversion)
{
    eh::Session s;
    const auto id = MakeEllipse (s);
    const auto vertex = ed::Find (s.edits.Doc (), id)->vertices.front ().id;
    s.selection.Set ({ { id, ed::Part::Vertex, vertex } });
    ASSERT_NE (detail::SelectedEllipse (s), nullptr);
    ASSERT_TRUE (detail::ConvertSelectedEllipse (s));
    EXPECT_EQ (detail::SelectedEllipse (s), nullptr);
    ASSERT_TRUE (s.edits.Apply ([id, vertex] (ed::Document& doc, std::string& error) {
        return ed::SetHandle (doc, id, vertex, true, { 1, 1, 0 }, false, error);
    }));
    ASSERT_TRUE (s.edits.Undo ());
    ASSERT_TRUE (s.edits.Undo ());
    ASSERT_NE (detail::SelectedEllipse (s), nullptr);
    EXPECT_EQ (s.selection.Items ().front ().item, vertex);
    ASSERT_TRUE (s.edits.Redo ());
    EXPECT_EQ (detail::SelectedEllipse (s), nullptr);
}

TEST (HudVirtualEditorEllipse, CurrentModeIsANoOpAndCenterEditsPreserveConjugateAxes)
{
    eh::Session s;
    const auto id = MakeEllipse (s);
    ed::Document doc = s.edits.Doc ();
    auto* entity = ed::Find (doc, id);
    entity->ellipse->axisU = { 4, 1, 0 };
    entity->ellipse->axisV = { 1, 2, 0 };
    std::string error;
    ASSERT_TRUE (ed::RegenerateEllipse (*entity, error));
    s.edits = ed::Session (doc);
    EXPECT_FALSE (detail::SetSelectedEllipseMode (s, ed::EllipseMode::Ellipse));
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
    ASSERT_TRUE (detail::SetSelectedEllipseValue (s, detail::EllipseField::CenterY, 1.2345678901234567));
    ASSERT_TRUE (detail::SetSelectedEllipseValue (s, detail::EllipseField::CenterZ, 3.2345678901234567));
    EXPECT_DOUBLE_EQ (Parameters (s, id).center.y, 1.2345678901234567);
    EXPECT_DOUBLE_EQ (Parameters (s, id).center.z, 3.2345678901234567);
    EXPECT_DOUBLE_EQ (ed::Find (s.edits.Doc (), id)->ellipse->axisU.y, 1);
    EXPECT_DOUBLE_EQ (ed::Find (s.edits.Doc (), id)->ellipse->axisV.x, 1);
}

TEST (HudVirtualEditorEllipse, RefusesBusyLockedInvalidAndAmbiguousEditsWithoutHistory)
{
    eh::Session s;
    const auto id = MakeEllipse (s);
    for (int busy = 0; busy < 8; ++busy) {
        s.profileTyping = busy == 0;
        s.planeTyping = busy == 1;
        s.labelOpen = busy == 2;
        s.typedHandle = busy == 3 ? 0 : -1;
        s.drag = busy == 4 ? eh::Drag::Part : eh::Drag::None;
        s.draftingImport = busy == 5;
        s.debugExportRequested = busy == 6;
        if (busy == 7)
            ASSERT_TRUE (s.edits.Begin ());
        EXPECT_FALSE (detail::SetSelectedEllipseValue (s, detail::EllipseField::CenterY, 10));
        EXPECT_FALSE (detail::ConvertSelectedEllipse (s));
        if (busy == 7)
            s.edits.Cancel ();
    }
    s.draftingImport = s.debugExportRequested = false;
    EXPECT_FALSE (detail::SetSelectedEllipseValue (s, detail::EllipseField::MajorRadius, -1));
    EXPECT_FALSE (
        detail::SetSelectedEllipseValue (s, detail::EllipseField::CenterZ, std::numeric_limits<double>::infinity ()));
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
    std::string error;
    ed::Document locked = s.edits.Doc ();
    ASSERT_TRUE (ed::SetLocked (locked, s.selection.Items (), true, error));
    s.edits = ed::Session (locked);
    EXPECT_FALSE (detail::SetSelectedEllipseValue (s, detail::EllipseField::CenterX, 10));
    EXPECT_FALSE (detail::ConvertSelectedEllipse (s));
    s.selection.Set ({ { id, ed::Part::Entity, 0 }, { id + 100, ed::Part::Entity, 0 } });
    EXPECT_EQ (detail::SelectedEllipse (s), nullptr);
    EXPECT_FALSE (detail::ConvertSelectedEllipse (s));
    EXPECT_EQ (s.edits.UndoDepth (), 0u);
}

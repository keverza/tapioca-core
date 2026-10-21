// Constraint operations: MaterialMask, GradingLimit and ProtectedZone keep their footprint,
// attach their mask only to the regions it reaches, report a mask that reaches nothing,
// and flag a terrain-affecting constraint that has no property line to bound it.
#include "Edit/EditDocument.hpp"
#include "Edit/EditLandscape.hpp"
#include <algorithm>
#include <gtest/gtest.h>

namespace edit = geomsrv::edit;
namespace geo = tapioca::geometry;

namespace {
edit::Document Start ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, error)) << error;
    return doc;
}
edit::Id Square (edit::Document& doc, double x, double y, double size = 10)
{
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (
        doc, { { x, y, 0 }, { x + size, y, 0 }, { x + size, y + size, 0 }, { x, y + size, 0 } }, true);
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    return id;
}
edit::Id Operation (edit::Document& doc, geo::OperationKind kind, std::vector<edit::Id> inputs)
{
    geo::LandscapeOperation op;
    op.kind = kind;
    op.inputs = std::move (inputs);
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    return id;
}
void Object (edit::Document& doc, edit::Id op)
{
    geo::HardscapeObject object;
    object.footprintOperation = op;
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
}
geo::LandscapeResult Evaluate (const edit::Document& doc)
{
    geo::LandscapeResult result;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    return result;
}
size_t Count (const geo::LandscapeResult& result, geo::LandscapeDiagnosticCode code, edit::Id operation)
{
    return size_t (std::count_if (result.diagnostics.begin (), result.diagnostics.end (),
                                  [=] (const auto& d) { return d.code == code && d.operation == operation; }));
}
bool Has (const std::vector<edit::Id>& ids, edit::Id id)
{
    return std::find (ids.begin (), ids.end (), id) != ids.end ();
}

constexpr geo::OperationKind kConstraints[] = { geo::OperationKind::MaterialMask, geo::OperationKind::GradingLimit,
                                                geo::OperationKind::ProtectedZone };
} // namespace

TEST (EditLandscapeConstraints, EveryConstraintKeepsItsFootprintAndAttachesOnlyWhereItsMaskReaches)
{
    for (const auto kind : kConstraints) {
        auto doc = Start ();
        const auto line = Square (doc, -10, -10, 60);
        std::string error;
        ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, line, error)) << error;
        const auto left = Square (doc, 0, 0), right = Square (doc, 20, 0), mask = Square (doc, 3, 3, 4);
        const auto pieces = Operation (doc, geo::OperationKind::Union, { left, right });
        const auto constraint = Operation (doc, kind, { pieces, mask });
        Object (doc, constraint);
        const auto result = Evaluate (doc);
        ASSERT_EQ (result.regions.size (), 2u) << int (kind);
        double area = 0;
        for (const auto& region : result.regions) {
            area += region.area;
            EXPECT_EQ (region.rings.size (), 1u) << "a constraint never makes a hole";
            EXPECT_TRUE (Has (region.operations, constraint)) << int (kind);
            const bool reached = Has (region.sources, left);
            EXPECT_EQ (Has (region.sources, mask), reached) << "only the region the mask overlaps acquires it";
        }
        EXPECT_NEAR (area, 200, 1.0e-6) << int (kind);
        EXPECT_EQ (Count (result, geo::LandscapeDiagnosticCode::ClipEmpty, constraint), 0u);
    }
}

TEST (EditLandscapeConstraints, AMaskThatReachesNoRegionReportsClipEmptyAndKeepsTheFootprint)
{
    for (const auto kind : kConstraints) {
        auto doc = Start ();
        const auto line = Square (doc, -10, -10, 60);
        std::string error;
        ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, line, error)) << error;
        const auto footprint = Square (doc, 0, 0), distant = Square (doc, 30, 30, 5);
        const auto constraint = Operation (doc, kind, { footprint, distant });
        Object (doc, constraint);
        const auto result = Evaluate (doc);
        ASSERT_EQ (result.regions.size (), 1u) << int (kind);
        EXPECT_NEAR (result.regions[0].area, 100, 1.0e-6);
        EXPECT_FALSE (Has (result.regions[0].sources, distant));
        EXPECT_EQ (Count (result, geo::LandscapeDiagnosticCode::ClipEmpty, constraint), 1u) << int (kind);
        // A mask touching the footprint only along an edge reaches no area either.
        auto touching = Start ();
        ASSERT_TRUE (geo::SetLandscapePropertyLine (touching, Square (touching, -10, -10, 60), error)) << error;
        const auto touchingConstraint =
            Operation (touching, kind, { Square (touching, 0, 0), Square (touching, 10, 0, 5) });
        Object (touching, touchingConstraint);
        EXPECT_EQ (Count (Evaluate (touching), geo::LandscapeDiagnosticCode::ClipEmpty, touchingConstraint), 1u)
            << int (kind);
    }
}

TEST (EditLandscapeConstraints, TerrainConstraintsWithoutAPropertyLineAreFlaggedNotRefused)
{
    for (const auto kind : kConstraints) {
        auto doc = Start ();
        const auto constraint = Operation (doc, kind, { Square (doc, 0, 0), Square (doc, 3, 3, 4) });
        Object (doc, constraint);
        const auto before = doc;
        const auto result = Evaluate (doc);
        ASSERT_EQ (result.regions.size (), 1u) << "M1 still evaluates regions without a property line";
        EXPECT_EQ (doc.landscape, before.landscape);
        const bool terrain = kind != geo::OperationKind::MaterialMask;
        EXPECT_EQ (Count (result, geo::LandscapeDiagnosticCode::PropertyLineOutside, constraint), terrain ? 1u : 0u)
            << int (kind);

        // Disabled, it no longer claims a terrain effect.
        auto disabled = doc;
        std::string error;
        ASSERT_TRUE (geo::SetLandscapeOperationEnabled (disabled, constraint, false, error)) << error;
        EXPECT_EQ (Count (Evaluate (disabled), geo::LandscapeDiagnosticCode::PropertyLineOutside, constraint), 0u);

        // A valid property line bounds it: no warning.
        const auto line = Square (doc, -10, -10, 40);
        ASSERT_TRUE (geo::SetLandscapePropertyLine (doc, line, error)) << error;
        EXPECT_EQ (Count (Evaluate (doc), geo::LandscapeDiagnosticCode::PropertyLineOutside, constraint), 0u);
    }
}

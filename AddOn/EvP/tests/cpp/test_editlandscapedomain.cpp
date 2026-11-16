// #112 P1: the one domain-building API every Boolean authoring path calls. The DAG
// stays the single planar authority; these tests pin the command's contract:
// metadata inheritance from the subject only, stable owner identity, atomic refusal
// and derived topology (holes/islands) without a second domain model.
#include "Edit/EditLandscapeDomain.hpp"
#include "Edit/EditLandscapeFieldPlan.hpp"
#include "Edit/EditRegionTerrain.hpp"
#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditDocument.hpp"
#include "Edit/EditPolyline.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;

namespace {
edit::Document Start ()
{
    edit::Document doc;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4 }, error)) << error;
    return doc;
}
edit::Id Source (edit::Document& doc, std::vector<edit::Vec3> points)
{
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (doc, std::move (points), true);
    edit::Id id = 0;
    std::string error;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    return id;
}
// A RegionBoundary square with its auto-owned configured region object.
edit::Id Region (edit::Document& doc, double x, double size = 10)
{
    const auto line =
        edit::AddPolyline (doc, { { x, 0, 0 }, { x + size, 0, 0 }, { x + size, size, 0 }, { x, size, 0 } }, true);
    std::string error;
    EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::RegionBoundary, error)) << error;
    return doc.landscape->objects.back ().id;
}
void Configure (edit::Document& doc, edit::Id object)
{
    geo::RegionSurface surface;
    surface.elevation = geo::RegionElevationMode::Constant;
    surface.first.z = 2;
    geo::MaterialSpec material;
    material.preset = geo::MaterialPreset::HardPaving;
    material.fillGuid = "00112233-4455-6677-8899-AABBCCDDEEFF";
    material.surfaceGuid = "11223344-5566-7788-99AA-BBCCDDEEFF00";
    std::string error;
    ASSERT_TRUE (geo::SetRegionSurface (doc, object, surface, material, error)) << error;
    geo::RegionTerrain terrain;
    terrain.effect = geo::RegionTerrainEffect::Grade;
    ASSERT_TRUE (geo::SetRegionTerrain (doc, object, terrain, error)) << error;
}
const geo::HardscapeObject& Object (const edit::Document& doc, edit::Id id)
{
    const auto object = std::find_if (doc.landscape->objects.begin (), doc.landscape->objects.end (),
                                      [=] (const auto& o) { return o.id == id; });
    EXPECT_NE (object, doc.landscape->objects.end ());
    return *object;
}
std::vector<geo::DerivedRegion> Footprint (const edit::Document& doc, edit::Id object)
{
    geo::LandscapeResult evaluated;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, evaluated)) << evaluated.error;
    std::vector<geo::DerivedRegion> out;
    for (const auto& region : evaluated.regions)
        if (region.objectId == object)
            out.push_back (region);
    return out;
}
} // namespace

TEST (EditLandscapeDomain, CanvasStyleFaceBooleanInheritsSubjectMetadataAndRoundTrips)
{
    auto doc = Start ();
    const auto subject = Region (doc, 0);
    // The merged line is plain closed linework (the #112 repro): only the subject
    // owns a configured region, so the subject metadata source is unambiguous.
    Source (doc, { { 10, 0, 0 }, { 20, 0, 0 }, { 20, 10, 0 }, { 10, 10, 0 } });
    Configure (doc, subject);
    geo::LandscapeResult evaluated;
    ASSERT_TRUE (geo::EvaluateLandscape (doc, evaluated)) << evaluated.error;
    const auto faceA = *geo::LandscapeFaceAt (evaluated, { 5, 5, 0 });
    const auto faceB = *geo::LandscapeFaceAt (evaluated, { 15, 5, 0 });
    ASSERT_EQ (doc.landscape->operations.size (), 1u);

    geo::DomainCommand command;
    command.kind = geo::OperationKind::Union;
    command.name = "Union";
    command.subject.faces = { faceA.descriptor };
    command.operand.faces = { faceB.descriptor };
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    EXPECT_EQ (doc.landscape->operations.size (), 4u); // subject root plus two selectors and the Boolean
    EXPECT_EQ (doc.landscape->objects.size (), 2u);
    const auto& output = Object (doc, applied.objectId);
    EXPECT_EQ (output.name, "Union");
    EXPECT_EQ (output.boundarySource, 0u);
    // A's metadata wins; the operand region never contributes its own.
    ASSERT_TRUE (output.surface.has_value ());
    EXPECT_EQ (output.surface->elevation, geo::RegionElevationMode::Constant);
    EXPECT_DOUBLE_EQ (output.surface->first.z, 2);
    EXPECT_EQ (output.terrain.effect, geo::RegionTerrainEffect::Grade);
    EXPECT_EQ (output.baseMaterial.preset, geo::MaterialPreset::HardPaving);
    // Derived domain covers the merged rectangle and persists/reloads as one edit.
    const auto parts = Footprint (doc, applied.objectId);
    ASSERT_EQ (parts.size (), 1u);
    EXPECT_EQ (parts[0].rings.size (), 1u);
    EXPECT_NEAR (parts[0].area, 200, 1e-6);
    std::string json, error;
    ASSERT_TRUE (geo::WriteLandscape (doc, geo::LandscapeStorage::Virtual, json, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (json, loaded, error)) << error;
    EXPECT_EQ (loaded.landscape, doc.landscape);
}

TEST (EditLandscapeDomain, OwnerPathPreservesIdentityMetadataAndSplitsIslands)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner);
    // A bar across the middle: the difference splits the subject into two islands.
    const auto bar = Source (doc, { { 4, -1, 0 }, { 6, -1, 0 }, { 6, 11, 0 }, { 4, 11, 0 } });
    const auto root = Object (doc, owner).footprintOperation;
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Difference;
    command.owner = owner;
    command.subject.input = root;
    command.operand.input = bar;
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    EXPECT_EQ (applied.objectId, owner);
    const auto& object = Object (doc, owner);
    EXPECT_NE (object.footprintOperation, root);
    ASSERT_TRUE (object.surface.has_value ());
    EXPECT_EQ (object.surface->elevation, geo::RegionElevationMode::Constant);
    EXPECT_EQ (object.terrain.effect, geo::RegionTerrainEffect::Grade);
    // Identity stable, derived topology updated: two islands, each one ring.
    const auto parts = Footprint (doc, owner);
    ASSERT_EQ (parts.size (), 2u);
    for (const auto& part : parts)
        EXPECT_EQ (part.rings.size (), 1u);
}

TEST (EditLandscapeDomain, DifferenceHoleStaysInTheEvaluatedDomain)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner);
    const auto hole = Source (doc, { { 3, 3, 0 }, { 7, 3, 0 }, { 7, 7, 0 }, { 3, 7, 0 } });
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Difference;
    command.owner = owner;
    command.subject.input = Object (doc, owner).footprintOperation;
    command.operand.input = hole;
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    const auto parts = Footprint (doc, owner);
    ASSERT_EQ (parts.size (), 1u);
    EXPECT_EQ (parts[0].rings.size (), 2u); // outer plus the hole
    EXPECT_NEAR (parts[0].area, 84, 1e-6);
}

TEST (EditLandscapeDomain, AdoptWithoutOperandBindsFacesToOneNamedOutput)
{
    auto doc = Start ();
    Source (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } });
    geo::LandscapeResult evaluated;
    ASSERT_TRUE (geo::EvaluateLandscape (doc, evaluated)) << evaluated.error;
    const auto face = *geo::LandscapeFaceAt (evaluated, { 5, 5, 0 });
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Union;
    command.name = "Hardscape";
    command.subject.faces = { face.descriptor };
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    // No Boolean node: the selector itself is the adopted root.
    EXPECT_EQ (doc.landscape->operations.size (), 1u);
    const auto& output = Object (doc, applied.objectId);
    EXPECT_EQ (output.name, "Hardscape");
    EXPECT_EQ (output.footprintOperation, doc.landscape->operations.back ().id);
    EXPECT_FALSE (output.surface.has_value ());
    EXPECT_EQ (Footprint (doc, applied.objectId).size (), 1u);
}

TEST (EditLandscapeDomain, AmbiguousFreshFaceSubjectRefusesAtomically)
{
    auto doc = Start ();
    const auto a = Region (doc, 0);
    const auto b = Region (doc, 10);
    Configure (doc, a);
    Configure (doc, b);
    geo::LandscapeResult evaluated;
    ASSERT_TRUE (geo::EvaluateLandscape (doc, evaluated)) << evaluated.error;
    const auto faceA = *geo::LandscapeFaceAt (evaluated, { 5, 5, 0 });
    const auto faceB = *geo::LandscapeFaceAt (evaluated, { 15, 5, 0 });
    const auto before = doc;
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Union;
    command.name = "Union";
    command.subject.faces = { faceA.descriptor, faceB.descriptor };
    command.operand.faces = { faceA.descriptor };
    const auto applied = geo::ApplyDomainCommand (doc, command);
    EXPECT_FALSE (applied.ok);
    EXPECT_NE (applied.error.find ("one configured region"), std::string::npos) << applied.error;
    EXPECT_EQ (doc.landscape, before.landscape);
    EXPECT_EQ (doc.nextId, before.nextId);
}

TEST (EditLandscapeDomain, UnknownOperandAndMissingOwnerRefuseAtomically)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner);
    const auto root = Object (doc, owner).footprintOperation;
    const auto before = doc;
    {
        geo::DomainCommand command;
        command.kind = geo::OperationKind::Difference;
        command.owner = owner;
        command.subject.input = root;
        command.operand.input = before.nextId + 10;
        const auto applied = geo::ApplyDomainCommand (doc, command);
        EXPECT_FALSE (applied.ok);
        EXPECT_EQ (doc.landscape, before.landscape);
        EXPECT_EQ (doc.nextId, before.nextId);
    }
    {
        geo::DomainCommand command;
        command.kind = geo::OperationKind::Difference;
        command.owner = before.nextId + 10;
        command.subject.input = root;
        const auto bar = Source (doc, { { 4, -1, 0 }, { 6, -1, 0 }, { 6, 11, 0 }, { 4, 11, 0 } });
        command.operand.input = bar;
        const auto applied = geo::ApplyDomainCommand (doc, command);
        EXPECT_FALSE (applied.ok);
        EXPECT_NE (applied.error.find ("no longer exists"), std::string::npos) << applied.error;
    }
}

// #188: Boundary curve Z resolves through an owned boundary line only, and a derived output owns none,
// so a Boolean of Boundary regions used to leave a result whose terrain build could never make its
// surface target. The output takes Constant elevation: the subject line's height when it is level,
// else its mean height, which the note says.
TEST (EditLandscapeDomain, ABooleanOfBoundaryRegionsTakesConstantElevation188)
{
    const auto boundaryRegion = [] (edit::Document& doc, double x, std::vector<double> heights) {
        const auto line = edit::AddPolyline (
            doc, { { x, 0, heights[0] }, { x + 10, 0, heights[1] }, { x + 10, 10, heights[2] }, { x, 10, heights[3] } },
            true);
        std::string error;
        EXPECT_TRUE (geo::SetLandscapeSourceRole (doc, line, geo::SourceRole::RegionBoundary, error)) << error;
        const auto object = doc.landscape->objects.back ().id;
        geo::RegionSurface surface;
        surface.elevation = geo::RegionElevationMode::Boundary;
        EXPECT_TRUE (geo::SetRegionSurface (doc, object, surface, {}, error)) << error;
        geo::RegionTerrain terrain;
        terrain.effect = geo::RegionTerrainEffect::Grade;
        EXPECT_TRUE (geo::SetRegionTerrain (doc, object, terrain, error)) << error;
        return object;
    };
    const auto intersect = [] (edit::Document& doc, edit::Id a, edit::Id b) {
        geo::DomainCommand command;
        command.kind = geo::OperationKind::Intersection;
        command.name = "Intersection";
        command.subject.input = Object (doc, a).footprintOperation;
        command.operand.input = Object (doc, b).footprintOperation;
        return geo::ApplyDomainCommand (doc, command);
    };
    {
        auto doc = Start ();
        const auto a = boundaryRegion (doc, 0, { 1.5, 1.5, 1.5, 1.5 });
        const auto b = boundaryRegion (doc, 5, { 0, 0, 0, 0 });
        const auto applied = intersect (doc, a, b);
        ASSERT_TRUE (applied.ok) << applied.error;
        const auto& output = Object (doc, applied.objectId);
        EXPECT_EQ (output.boundarySource, 0u);
        ASSERT_TRUE (output.surface.has_value ());
        EXPECT_EQ (output.surface->elevation, geo::RegionElevationMode::Constant);
        EXPECT_DOUBLE_EQ (output.surface->first.z, 1.5) << "the subject's level line, not the operand's";
        EXPECT_TRUE (applied.note.empty ()) << applied.note;
        EXPECT_EQ (Object (doc, a).surface->elevation, geo::RegionElevationMode::Boundary) << "operands unchanged";
    }
    {
        auto doc = Start ();
        const auto a = boundaryRegion (doc, 0, { 0, 1, 2, 1 });
        const auto b = boundaryRegion (doc, 5, { 0, 0, 0, 0 });
        const auto applied = intersect (doc, a, b);
        ASSERT_TRUE (applied.ok) << applied.error;
        const auto& output = Object (doc, applied.objectId);
        EXPECT_EQ (output.surface->elevation, geo::RegionElevationMode::Constant);
        EXPECT_NEAR (output.surface->first.z, 1.0, 1e-9);
        EXPECT_NE (applied.note.find ("mean"), std::string::npos) << applied.note;
    }
}

// #210: Dissolve takes a Boolean apart. A result the Boolean authored goes, with its steps; the
// inputs are owners again exactly as they were. A chained result goes alone, leaving the earlier
// result whole; that earlier one refuses while the later is built from it.
TEST (EditLandscapeDomain, DissolveRemovesAnAuthoredResultAndItsInputsOwnAgain210)
{
    auto doc = Start ();
    const auto a = Region (doc, 0), b = Region (doc, 5), c = Region (doc, 12);
    for (const auto id : { a, b, c })
        Configure (doc, id);
    const auto objects = doc.landscape->objects;
    const auto operations = doc.landscape->operations;
    const auto combine = [] (edit::Document& d, geo::OperationKind kind, edit::Id subject, edit::Id operand) {
        geo::DomainCommand command;
        command.kind = kind;
        command.name = "Result";
        command.subject.input = Object (d, subject).footprintOperation;
        command.operand.input = Object (d, operand).footprintOperation;
        const auto applied = geo::ApplyDomainCommand (d, command);
        EXPECT_TRUE (applied.ok) << applied.error;
        return applied.objectId;
    };
    const auto result = combine (doc, geo::OperationKind::Intersection, a, b);
    const auto afterFirst = doc;
    const auto last = combine (doc, geo::OperationKind::Union, result, c);
    ASSERT_EQ (geo::TerrainRegionObjects (doc), (std::vector<edit::Id> { last }));

    // The earlier result refuses while the later one is built from it, changing nothing.
    const auto before = doc;
    const auto refused = geo::DissolveBoolean (doc, result);
    EXPECT_FALSE (refused.ok);
    EXPECT_NE (refused.error.find ("dissolve that one first"), std::string::npos) << refused.error;
    EXPECT_EQ (doc.landscape, before.landscape);
    EXPECT_EQ (doc.revision, before.revision);

    // The later one goes alone: the earlier result is whole and owns its area again.
    const auto outer = geo::DissolveBoolean (doc, last);
    ASSERT_TRUE (outer.ok) << outer.error;
    EXPECT_EQ (outer.objectId, edit::kNoId);
    EXPECT_EQ (doc.landscape->objects, afterFirst.landscape->objects);
    EXPECT_EQ (doc.landscape->operations, afterFirst.landscape->operations);
    EXPECT_EQ (geo::TerrainRegionObjects (doc), (std::vector<edit::Id> { c, result }));

    // Then the first: every input is an owner again, settings untouched, no step left behind.
    const auto inner = geo::DissolveBoolean (doc, result);
    ASSERT_TRUE (inner.ok) << inner.error;
    EXPECT_EQ (doc.landscape->objects, objects);
    EXPECT_EQ (doc.landscape->operations, operations);
    EXPECT_TRUE (geo::LandscapeConstructionInputs (*doc.landscape).empty ());
    EXPECT_EQ (geo::TerrainRegionObjects (doc), (std::vector<edit::Id> { a, b, c }));
    geo::LandscapeResult evaluated;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, evaluated)) << evaluated.error;

    // A region that is no Boolean result refuses.
    const auto plain = geo::DissolveBoolean (doc, a);
    EXPECT_FALSE (plain.ok);
    EXPECT_NE (plain.error.find ("not a Boolean result"), std::string::npos) << plain.error;
}

// #210: the case Dissolve is for. A reopened draft has no undo history; the result read back
// dissolves to exactly the regions saved before the Boolean.
TEST (EditLandscapeDomain, AReopenedResultDissolves210)
{
    auto doc = Start ();
    const auto a = Region (doc, 0), b = Region (doc, 5);
    Configure (doc, a);
    Configure (doc, b);
    const auto objects = doc.landscape->objects;
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Union;
    command.name = "Union";
    command.subject.input = Object (doc, a).footprintOperation;
    command.operand.input = Object (doc, b).footprintOperation;
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    std::string text, error;
    ASSERT_TRUE (geo::WriteLandscape (doc, geo::LandscapeStorage::Virtual, text, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, error)) << error;
    const auto dissolved = geo::DissolveBoolean (loaded, applied.objectId);
    ASSERT_TRUE (dissolved.ok) << dissolved.error;
    EXPECT_EQ (loaded.landscape->objects, objects);
    EXPECT_EQ (geo::TerrainRegionObjects (loaded), (std::vector<edit::Id> { a, b }));
}

// #210: a region a line cut in place (the cutter's owner) keeps its identity and gets its own
// boundary back; the cutter line takes no part any more.
TEST (EditLandscapeDomain, DissolveRestoresARegionCutInPlace210)
{
    auto doc = Start ();
    const auto owner = Region (doc, 0);
    Configure (doc, owner);
    const auto root = Object (doc, owner).footprintOperation;
    const auto settings = Object (doc, owner);
    const auto operations = doc.landscape->operations;
    const auto bar = Source (doc, { { 4, -1, 0 }, { 6, -1, 0 }, { 6, 11, 0 }, { 4, 11, 0 } });
    geo::DomainCommand command;
    command.kind = geo::OperationKind::Difference;
    command.owner = owner;
    command.subject.input = root;
    command.operand.input = bar;
    const auto applied = geo::ApplyDomainCommand (doc, command);
    ASSERT_TRUE (applied.ok) << applied.error;
    ASSERT_EQ (Footprint (doc, owner).size (), 2u) << "the bar splits it in two";
    EXPECT_EQ (geo::LandscapeSourceResult (*doc.landscape, bar), owner);

    const auto dissolved = geo::DissolveBoolean (doc, owner);
    ASSERT_TRUE (dissolved.ok) << dissolved.error;
    EXPECT_EQ (dissolved.objectId, owner);
    EXPECT_EQ (Object (doc, owner), settings) << "identity, surface, effect and material kept";
    EXPECT_EQ (doc.landscape->operations, operations);
    EXPECT_EQ (Footprint (doc, owner).size (), 1u);
    EXPECT_EQ (geo::LandscapeSourceResult (*doc.landscape, bar), edit::kNoId);
}

// #189: a Boolean's inputs become construction geometry. They stay authored, with their own
// settings, and only the result owns that area: one terrain owner, one field-plan entry. The rule
// is read from the operation DAG, so nothing is stored and a chain, a bypass and a reload all
// follow from it.
TEST (EditLandscapeDomain, BooleanInputsAreConstructionAndOnlyTheResultOwnsTerrain189)
{
    auto doc = Start ();
    const auto a = Region (doc, 0), b = Region (doc, 5), c = Region (doc, 12);
    for (const auto id : { a, b, c })
        Configure (doc, id);
    EXPECT_TRUE (geo::LandscapeConstructionInputs (*doc.landscape).empty ());
    EXPECT_EQ (geo::TerrainRegionObjects (doc), (std::vector<edit::Id> { a, b, c }));

    const auto combine = [] (edit::Document& d, geo::OperationKind kind, edit::Id subject, edit::Id operand) {
        geo::DomainCommand command;
        command.kind = kind;
        command.name = "Result";
        command.subject.input = Object (d, subject).footprintOperation;
        command.operand.input = Object (d, operand).footprintOperation;
        const auto applied = geo::ApplyDomainCommand (d, command);
        EXPECT_TRUE (applied.ok) << applied.error;
        return applied.objectId;
    };
    const auto settingsA = Object (doc, a);
    const auto result = combine (doc, geo::OperationKind::Intersection, a, b);
    using Input = geo::ConstructionInput;
    EXPECT_EQ (geo::LandscapeConstructionInputs (*doc.landscape),
               (std::vector<Input> { { a, result, result }, { b, result, result } }));
    EXPECT_EQ (geo::LandscapeTerrainOwner (*doc.landscape, a), result);
    EXPECT_EQ (geo::LandscapeTerrainOwner (*doc.landscape, result), result);
    EXPECT_EQ (geo::LandscapeTerrainOwner (*doc.landscape, c), c);
    EXPECT_EQ (geo::TerrainRegionObjects (doc), (std::vector<edit::Id> { c, result }));
    EXPECT_EQ (Object (doc, a), settingsA) << "an input keeps its surface, effect and material";

    geo::LandscapeResult evaluated;
    ASSERT_TRUE (geo::EvaluateLandscape (doc, evaluated)) << evaluated.error;
    for (const auto& region : evaluated.regions)
        EXPECT_EQ (region.constructionOf, region.objectId == a || region.objectId == b ? result : edit::kNoId)
            << "region of object " << region.objectId;

    geo::LandscapeSnapshot snapshot;
    snapshot.doc = &doc;
    snapshot.revision = doc.revision;
    const auto plan = geo::CompileLandscapeFieldPlan (snapshot);
    ASSERT_TRUE (plan.ok) << plan.error;
    std::vector<edit::Id> owners;
    for (const auto& entry : plan.entries)
        owners.push_back (entry.object);
    std::sort (owners.begin (), owners.end ());
    EXPECT_EQ (owners, (std::vector<edit::Id> { c, result })) << "the overlap has one owner, not three";

    // The same answer after a save and reload: nothing about it is stored.
    {
        std::string text, error;
        ASSERT_TRUE (geo::WriteLandscape (doc, geo::LandscapeStorage::Virtual, text, error)) << error;
        edit::Document loaded;
        ASSERT_TRUE (geo::ReadLandscape (text, loaded, error)) << error;
        EXPECT_EQ (geo::LandscapeConstructionInputs (*loaded.landscape),
                   geo::LandscapeConstructionInputs (*doc.landscape));
    }

    // A result that is itself combined is an input too; the last result owns everything under it.
    const auto last = combine (doc, geo::OperationKind::Union, result, c);
    EXPECT_EQ (
        geo::LandscapeConstructionInputs (*doc.landscape),
        (std::vector<Input> { { a, result, last }, { b, result, last }, { c, last, last }, { result, last, last } }));
    EXPECT_EQ (geo::TerrainRegionObjects (doc), (std::vector<edit::Id> { last }));

    // A bypassed step passes its first input through: the other input is no longer part of the
    // footprint, so it owns its own area again.
    std::string error;
    ASSERT_TRUE (geo::SetLandscapeOperationEnabled (doc, Object (doc, last).footprintOperation, false, error)) << error;
    EXPECT_EQ (geo::LandscapeTerrainOwner (*doc.landscape, c), c);
    EXPECT_EQ (geo::LandscapeTerrainOwner (*doc.landscape, a), last);
    EXPECT_EQ (geo::TerrainRegionObjects (doc), (std::vector<edit::Id> { c, last }));
}

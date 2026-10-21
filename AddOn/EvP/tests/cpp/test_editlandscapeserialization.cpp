#include "Edit/EditLandscapeSerialization.hpp"
#include "Edit/EditPathEdit.hpp"
#include "Edit/EditPolyline.hpp"
#include "Edit/EditSession.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace geo = tapioca::geometry;
namespace edit = geomsrv::edit;
namespace {
constexpr char kGuid[] = "12345678-1234-4234-9234-123456789ABC";
constexpr char kSecondGuid[] = "22345678-1234-4234-9234-123456789ABC";
constexpr char kMetadata[] = "32345678-1234-4234-9234-123456789ABC";

edit::Document Full (edit::Id first = 1)
{
    edit::Document doc;
    doc.nextId = first;
    std::string error;
    EXPECT_TRUE (geo::StartLandscape (doc, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, error));
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } }, true);
    auto* entity = edit::Find (doc, source.entityId);
    entity->vertices[0].in = { 0, 1, 0 };
    entity->vertices[0].smooth = true;
    entity->vertices[0].lockZ = true;
    entity->segments[0].tag = std::numeric_limits<uint32_t>::max ();
    source.archicadGuid = kGuid;
    edit::Id id = 0;
    EXPECT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::RegionSelect;
    op.inputs = { id };
    EXPECT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    geo::HardscapeObject object;
    object.footprintOperation = id;
    object.name = "Plaza \"A\" / žolė";
    object.baseMaterial.preset = geo::MaterialPreset::HardPaving;
    object.baseMaterial.fillGuid = kGuid;
    object.baseMaterial.fillName = "Stone";
    object.baseMaterial.surfaceGuid = kSecondGuid;
    object.baseMaterial.surfaceName = "Finish";
    object.baseMaterial.hatchDirection = -23.5;
    object.baseMaterial.hatchDensity = 3.25;
    object.thickness = 0.125;
    EXPECT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
    geo::LandscapeResult result;
    EXPECT_TRUE (geo::EvaluateLandscape (doc, result)) << result.error;
    geo::MaterialAssignment assignment;
    assignment.id = edit::NewId (doc);
    assignment.region = result.regions.front ().regionId;
    assignment.material = object.baseMaterial;
    assignment.priority = 7;
    doc.landscape->assignments.push_back (assignment);
    geo::LandscapePatch patch;
    patch.key = edit::NewId (doc);
    patch.object = id;
    patch.assignment = assignment.id;
    patch.role = "surface";
    patch.representation = "mesh";
    patch.sources = { doc.landscape->sources.front ().id };
    patch.predecessorRegions = { assignment.region };
    doc.landscape->patches.push_back (patch);
    patch.predecessorPatches = { patch.key };
    patch.key = edit::NewId (doc);
    doc.landscape->patches.push_back (patch);
    doc.landscape->bindings.push_back (
        { geo::BindingKind::Object, id, { kGuid, kSecondGuid }, kMetadata, "outputs", 0 });
    doc.revision = 0;
    EXPECT_TRUE (edit::Validate (doc, error)) << error;
    return doc;
}
std::string Write (const edit::Document& doc, geo::LandscapeStorage mode = geo::LandscapeStorage::Virtual)
{
    std::string text, error;
    EXPECT_TRUE (geo::WriteLandscape (doc, mode, text, error)) << error;
    return text;
}
std::string Replace (std::string text, const std::string& from, const std::string& to)
{
    const auto at = text.find (from);
    EXPECT_NE (at, std::string::npos);
    if (at != std::string::npos)
        text.replace (at, from.size (), to);
    return text;
}
void Same (const edit::Document& a, const edit::Document& b)
{
    EXPECT_EQ (a.entities, b.entities);
    EXPECT_EQ (a.paths, b.paths);
    EXPECT_EQ (a.landscape, b.landscape);
    EXPECT_EQ (a.nextId, b.nextId);
    EXPECT_EQ (a.revision, b.revision);
}
geo::LandscapeCapture Capture (const edit::Document& original)
{
    return [original] (const geo::LandscapeSource& source, std::map<edit::Id, edit::Vec3>& points, std::string&) {
        if (source.entityId)
            for (const auto& vertex : edit::Find (original, source.entityId)->vertices)
                points.emplace (vertex.id, vertex.p);
        if (source.pathId)
            for (const auto& point : edit::FindPath (original, source.pathId)->points)
                points.emplace (point.id, point.p);
        return true;
    };
}
} // namespace

TEST (EditLandscapeSerialization, MinimalRevisionZeroAndUnsignedIdsRoundTrip)
{
    for (edit::Id first : { edit::Id { 1 }, (edit::Id { 1 } << 60) + 7 }) {
        const auto original = Full (first);
        const auto text = Write (original);
        edit::Document loaded;
        std::string error;
        ASSERT_TRUE (geo::ReadLandscape (text, loaded, error)) << error;
        Same (original, loaded);
        EXPECT_EQ (loaded.revision, 0u);
        EXPECT_EQ (Write (loaded), text);
        EXPECT_NE (text.find ("\"nextId\":\"" + std::to_string (original.nextId) + "\""), std::string::npos);
    }
}

TEST (EditLandscapeSerialization, CompletePathDefinitionKeepsControlsProfileSectionsAndKerbs)
{
    auto original = Full ();
    std::string error;
    const auto path = edit::AddPath (original, { { 20, 0, 2 }, { 30, 0, 3 }, { 30, 10, 3.5 } }, 2.0, "walk");
    const auto bend = edit::FindPath (original, path)->points[1].id;
    ASSERT_TRUE (edit::SetPathRadius (original, path, bend, 2, error)) << error;
    auto* p = edit::FindPath (original, path);
    p->points[1].lockXY = true;
    p->edgeRadius = 0.2;
    p->kerbs.push_back ({ p->points[0].id, 0, edit::KerbSide::Left, edit::KerbSide::Right, 0.3 });
    geo::LandscapeSource source;
    source.pathId = path;
    source.role = geo::SourceRole::PathCenterline;
    source.archicadGuid = kSecondGuid;
    edit::Id id = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (original, source, id, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (Write (original), loaded, error)) << error;
    Same (original, loaded);
}

TEST (EditLandscapeSerialization, NativeModeOmitsAnchorCoordinatesAndRequiresIdKeyedCapture)
{
    const auto original = Full ();
    const auto text = Write (original, geo::LandscapeStorage::Native);
    EXPECT_EQ (text.find ("\"point\":{"), std::string::npos);
    edit::Document loaded;
    std::string error;
    EXPECT_FALSE (geo::ReadLandscape (text, loaded, error));
    EXPECT_NE (error.find ("capture"), std::string::npos);
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, error, Capture (original))) << error;
    Same (original, loaded);
}

TEST (EditLandscapeSerialization, NativeModeRefusesMissingGuidAndGuessedPointOrder)
{
    auto original = Full ();
    original.landscape->sources[0].archicadGuid.clear ();
    std::string text = "unchanged", error;
    EXPECT_FALSE (geo::WriteLandscape (original, geo::LandscapeStorage::Native, text, error));
    EXPECT_EQ (text, "unchanged");
    original = Full ();
    text = Write (original, geo::LandscapeStorage::Native);
    edit::Document loaded;
    EXPECT_FALSE (geo::ReadLandscape (text, loaded, error, [] (const auto&, auto& points, auto&) {
        points.emplace (1, edit::Vec3 {});
        points.emplace (2, edit::Vec3 { 10, 0, 0 });
        return true;
    }));
    EXPECT_NE (error.find ("authored point"), std::string::npos);
}

TEST (EditLandscapeSerialization, ReloadRebuildAndSourceEditRetainDurableAncestryWithoutFaceKeys)
{
    const auto original = Full ();
    const auto text = Write (original, geo::LandscapeStorage::Native);
    EXPECT_EQ (text.find ("faceId"), std::string::npos);
    EXPECT_EQ (text.find ("tableId"), std::string::npos);
    EXPECT_EQ (text.find ("triangles"), std::string::npos);
    edit::Document loaded;
    std::string error;
    ASSERT_TRUE (geo::ReadLandscape (text, loaded, error, Capture (original))) << error;
    geo::LandscapeResult rebuilt;
    ASSERT_TRUE (geo::RebuildLandscape (loaded, rebuilt, error)) << error;
    EXPECT_EQ (loaded.landscape, original.landscape);
    edit::Session session (loaded);
    const auto entity = loaded.entities[0].id, vertex = loaded.entities[0].vertices[1].id;
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, entity, vertex, { 12, 0, 0 }, e);
    })) << session.Message ();
    EXPECT_EQ (session.Doc ().landscape->regions, original.landscape->regions);
    EXPECT_EQ (session.Doc ().landscape->assignments, original.landscape->assignments);
    EXPECT_EQ (session.Doc ().landscape->patches, original.landscape->patches);
    EXPECT_EQ (session.Doc ().landscape->bindings, original.landscape->bindings);
    EXPECT_EQ (session.Doc ().landscape->operations, original.landscape->operations);
    edit::Document second;
    const auto saved = Write (session.Doc ());
    ASSERT_TRUE (geo::ReadLandscape (saved, second, error)) << error;
    Same (session.Doc (), second);
    EXPECT_EQ (Write (second), saved);
}

TEST (EditLandscapeSerialization, MalformedUnknownDuplicateAndMissingFieldsNeverMutateDestination)
{
    const auto original = Full ();
    const auto valid = Write (original);
    for (const auto& text :
         { std::string { "{" }, Replace (valid, "\"version\":1", "\"version\":2"),
           Replace (valid, "\"mode\":\"virtual\",", ""), Replace (valid, "\"nextId\":", "\"extra\":0,\"nextId\":"),
           Replace (valid, "\"revision\":\"0\"", "\"revision\":0"),
           Replace (valid, "\"revision\":\"0\"", "\"revision\":\"00\""),
           Replace (valid, "\"revision\":\"0\"", "\"revision\":\"18446744073709551616\""),
           Replace (valid, "\"version\":1", "\"version\":1,\"version\":1"),
           Replace (valid, "\"hatchDensity\":3.25", "\"hatchDensity\":-1"),
           Replace (valid, "\"kind\":7", "\"kind\":99") }) {
        auto destination = original;
        std::string error;
        EXPECT_FALSE (geo::ReadLandscape (text, destination, error)) << text;
        EXPECT_FALSE (error.empty ());
        Same (original, destination);
    }
}

TEST (EditLandscapeSerialization, StrictNumbersUnicodeAndBoundedJson)
{
    const auto original = Full ();
    const auto valid = Write (original);
    for (const auto* number : { "+1", "01", "1.", ".1", "1e", "1e999" }) {
        auto destination = original;
        std::string error;
        EXPECT_FALSE (geo::ReadLandscape (Replace (valid, "\"version\":1", std::string ("\"version\":") + number),
                                          destination, error));
        Same (original, destination);
    }
    edit::Document loaded;
    std::string error;
    EXPECT_FALSE (geo::ReadLandscape (Replace (valid, "Plaza", "\\uD800"), loaded, error));
    ASSERT_TRUE (geo::ReadLandscape (Replace (valid, "Plaza", "\\uD83C\\uDF33"), loaded, error)) << error;
    EXPECT_EQ (loaded.landscape->objects[0].name.substr (0, 4), "\xF0\x9F\x8C\xB3");
    EXPECT_FALSE (geo::ReadLandscape (std::string (8 * 1024 * 1024 + 1, ' '), loaded, error));
}

TEST (EditLandscapeSerialization, DuplicateAuthoredIdsAndDagCyclesRefuseOnRead)
{
    const auto original = Full ();
    const auto valid = Write (original);
    const auto source = original.landscape->sources[0].id, op = original.landscape->operations[0].id;
    auto duplicate =
        Replace (valid, "\"id\":\"" + std::to_string (source) + "\"", "\"id\":\"" + std::to_string (op) + "\"");
    auto cycle = Replace (valid, "\"inputs\":[\"" + std::to_string (source) + "\"]",
                          "\"inputs\":[\"" + std::to_string (op) + "\"]");
    edit::Document loaded;
    std::string error;
    EXPECT_FALSE (geo::ReadLandscape (duplicate, loaded, error));
    EXPECT_FALSE (geo::ReadLandscape (cycle, loaded, error));
    EXPECT_NE (error.find ("DagCycle"), std::string::npos);
}

TEST (EditLandscapeSerialization, UnknownAndWrongDomainOperationInputsNeverMutateDestination)
{
    const auto original = Full ();
    const auto valid = Write (original);
    const auto source = original.landscape->sources[0].id;
    for (const auto input : { original.nextId + 1, original.entities[0].id }) {
        auto destination = original;
        std::string error;
        const auto text = Replace (valid, "\"inputs\":[\"" + std::to_string (source) + "\"]",
                                   "\"inputs\":[\"" + std::to_string (input) + "\"]");
        EXPECT_FALSE (geo::ReadLandscape (text, destination, error));
        EXPECT_NE (error.find ("operations.inputs"), std::string::npos) << error;
        Same (original, destination);
    }
}

TEST (EditLandscapeSerialization, ExtraActiveIdentityRefusesInsteadOfBeingRetiredOnFirstRebuild)
{
    const auto original = Full ();
    auto malformed = original;
    auto extra = malformed.landscape->regions[0];
    extra.id = edit::NewId (malformed);
    // A structurally valid but nonmatching descriptor must not masquerade as an
    // active footprint at ingress. Historical inactive records remain legal.
    extra.descriptor.boundaries[0][0].reversed = !extra.descriptor.boundaries[0][0].reversed;
    malformed.landscape->regions.push_back (extra);
    std::string text = "unchanged", error;
    EXPECT_FALSE (geo::WriteLandscape (malformed, geo::LandscapeStorage::Virtual, text, error));
    EXPECT_EQ (text, "unchanged");
    malformed.landscape->regions.back ().active = false;
    const auto inactive = Write (malformed);
    const auto corrupt = Replace (inactive, "\"active\":false", "\"active\":true");
    auto destination = original;
    EXPECT_FALSE (geo::ReadLandscape (corrupt, destination, error));
    EXPECT_NE (error.find ("landscape.regions"), std::string::npos) << error;
    Same (original, destination);
    ASSERT_TRUE (geo::ReadLandscape (inactive, destination, error)) << error;
    Same (malformed, destination);
}

TEST (EditLandscapeSerialization, MissingAndDuplicateActiveFootprintsRefuseOnWriteAndRead)
{
    const auto original = Full ();
    for (bool duplicate : { false, true }) {
        auto invalid = original;
        if (duplicate) {
            auto object = invalid.landscape->objects[0];
            object.id = edit::NewId (invalid);
            invalid.landscape->objects.push_back (object);
        }
        else {
            invalid.landscape->assignments[0].valid = false;
            invalid.landscape->regions[0].active = false;
        }
        std::string text = "unchanged", error;
        EXPECT_FALSE (geo::WriteLandscape (invalid, geo::LandscapeStorage::Virtual, text, error));
        EXPECT_EQ (text, "unchanged");
        EXPECT_NE (error.find ("landscape.regions"), std::string::npos) << error;
    }
    const auto valid = Write (original);
    auto destination = original;
    std::string error;
    auto missing = Replace (valid, "\"active\":true", "\"active\":false");
    missing = Replace (missing, "\"valid\":true", "\"valid\":false");
    EXPECT_FALSE (geo::ReadLandscape (missing, destination, error));
    EXPECT_NE (error.find ("landscape.regions"), std::string::npos) << error;
    Same (original, destination);
}

TEST (EditLandscapeSerialization, EmptySessionAndDeletedSourceHistoryRemainValidCheckpoints)
{
    edit::Document empty;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (empty, { 1 }, error)) << error;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (Write (empty), loaded, error)) << error;
    Same (empty, loaded);
    auto orphaned = Full ();
    ASSERT_TRUE (geo::DeleteLandscapeSource (orphaned, orphaned.landscape->sources[0].id, error)) << error;
    ASSERT_FALSE (orphaned.landscape->regions[0].active);
    ASSERT_FALSE (orphaned.landscape->assignments[0].valid);
    ASSERT_TRUE (
        geo::ReadLandscape (Write (orphaned, geo::LandscapeStorage::Native), loaded, error, Capture (orphaned)))
        << error;
    Same (orphaned, loaded);
    geo::LandscapeResult result;
    ASSERT_TRUE (geo::RebuildLandscape (loaded, result, error)) << error;
    Same (orphaned, loaded);
    EXPECT_TRUE (result.regions.empty ());
    EXPECT_FALSE (result.diagnostics.empty ());
}

TEST (EditLandscapeSerialization, ReloadSplitMergeRetainsInvalidAssignmentsPatchesAndBindings)
{
    auto original = Full ();
    original.entities[0].vertices[0].in = {};
    geo::LandscapeResult result;
    std::string error;
    ASSERT_TRUE (geo::RebuildLandscape (original, result, error)) << error;
    const auto predecessor = result.regions[0].regionId;
    edit::Document loaded;
    ASSERT_TRUE (
        geo::ReadLandscape (Write (original, geo::LandscapeStorage::Native), loaded, error, Capture (original)))
        << error;
    edit::Session session (loaded);
    const auto entity = loaded.entities[0].id, vertex = loaded.entities[0].vertices[1].id;
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, entity, vertex, { 5, 15, 0 }, e);
    })) << session.Message ();
    ASSERT_TRUE (geo::EvaluateLandscape (session.Doc (), result)) << result.error;
    ASSERT_EQ (result.regions.size (), 2u);
    std::vector<edit::Id> children;
    for (const auto& region : result.regions) {
        EXPECT_EQ (region.predecessorRegions, (std::vector<edit::Id> { predecessor }));
        children.push_back (region.regionId);
        EXPECT_EQ (region.material, original.landscape->objects[0].baseMaterial);
    }
    EXPECT_FALSE (session.Doc ().landscape->assignments[0].valid);
    EXPECT_EQ (session.Doc ().landscape->assignments[0].region, predecessor);
    EXPECT_EQ (session.Doc ().landscape->patches, original.landscape->patches);
    EXPECT_EQ (session.Doc ().landscape->bindings, original.landscape->bindings);
    edit::Document split;
    ASSERT_TRUE (geo::ReadLandscape (Write (session.Doc (), geo::LandscapeStorage::Native), split, error,
                                     Capture (session.Doc ())))
        << error;
    Same (session.Doc (), split);
    ASSERT_TRUE (geo::RebuildLandscape (split, result, error)) << error;
    Same (session.Doc (), split);
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, entity, vertex, { 10, 0, 0 }, e);
    })) << session.Message ();
    ASSERT_TRUE (geo::EvaluateLandscape (session.Doc (), result)) << result.error;
    ASSERT_EQ (result.regions.size (), 1u);
    std::sort (children.begin (), children.end ());
    EXPECT_EQ (result.regions[0].predecessorRegions, children);
    EXPECT_NE (result.regions[0].regionId, predecessor);
    EXPECT_FALSE (session.Doc ().landscape->assignments[0].valid);
    const auto merged = session.Doc ();
    ASSERT_TRUE (session.Undo ());
    EXPECT_EQ (session.Doc ().landscape, split.landscape);
    ASSERT_TRUE (session.Redo ());
    EXPECT_EQ (session.Doc ().landscape, merged.landscape);
    edit::Document second;
    ASSERT_TRUE (geo::ReadLandscape (Write (session.Doc ()), second, error)) << error;
    Same (session.Doc (), second);
}

TEST (EditLandscapeSerialization, SingleSegmentGeneratedStrokeReloadEditKeepsIdentityWithoutClaimingExactSubdomains)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (geo::StartLandscape (doc, { 1 }, error)) << error;
    geo::LandscapeSource source;
    source.entityId = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 } }, false);
    source.archicadGuid = kGuid;
    edit::Id id = 0;
    ASSERT_TRUE (geo::AddLandscapeSource (doc, source, id, error)) << error;
    geo::LandscapeOperation op;
    op.kind = geo::OperationKind::Stroke;
    op.inputs = { id };
    ASSERT_TRUE (geo::AddLandscapeOperation (doc, op, id, error)) << error;
    geo::HardscapeObject object;
    object.footprintOperation = id;
    ASSERT_TRUE (geo::AddHardscapeObject (doc, object, id, error)) << error;
    const auto identity = doc.landscape->regions[0].id;
    edit::Document loaded;
    ASSERT_TRUE (geo::ReadLandscape (Write (doc, geo::LandscapeStorage::Native), loaded, error, Capture (doc)))
        << error;
    edit::Session session (loaded);
    const auto vertex = loaded.entities[0].vertices[1].id;
    ASSERT_TRUE (session.Apply ([=] (edit::Document& d, std::string& e) {
        return edit::MoveVertex (d, source.entityId, vertex, { 12, 0, 0 }, e);
    })) << session.Message ();
    geo::LandscapeResult result;
    ASSERT_TRUE (geo::EvaluateLandscape (session.Doc (), result)) << result.error;
    ASSERT_EQ (result.regions.size (), 1u);
    EXPECT_EQ (result.regions[0].regionId, identity);
    EXPECT_NEAR (result.regions[0].area, 18 + 3.141592653589793 * 0.75 * 0.75, 0.01);
    for (const auto& cycle : result.regions[0].boundary)
        for (const auto& piece : cycle) {
            EXPECT_TRUE (piece.generated);
            EXPECT_TRUE (piece.parameters.empty ());
            ASSERT_EQ (piece.support.size (), 1u);
            EXPECT_EQ (piece.support[0].source, session.Doc ().landscape->sources[0].id);
            EXPECT_EQ (piece.support[0].segment, loaded.entities[0].segments[0].id);
        }
    edit::Document second;
    ASSERT_TRUE (geo::ReadLandscape (Write (session.Doc ()), second, error)) << error;
    Same (session.Doc (), second);
}

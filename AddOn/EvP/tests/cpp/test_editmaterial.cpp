#include "Edit/EditMaterial.hpp"
#include "Edit/EditDocument.hpp"
#include "ArchViz/HudVirtualEditor.hpp"
#include "ArchViz/HudVirtualEditorMaterial.hpp"
#include <gtest/gtest.h>

namespace lg = tapioca::geometry;
namespace eh = geomsrv::archviz::edithud;
namespace edit = geomsrv::edit;
namespace {
constexpr const char* kFill = "00112233-4455-6677-8899-AABBCCDDEEFF";
constexpr const char* kSurface = "11223344-5566-7788-99AA-BBCCDDEEFF00";
lg::MaterialCatalog Catalog ()
{
    return { { { lg::MaterialAttributeKind::Fill, 7, kFill, "Renamed fill" },
               { lg::MaterialAttributeKind::Surface, 12, kSurface, "Surface" } } };
}
} // namespace

TEST (EditMaterial, GuidWinsOverRenamesAndProjectIndexChanges)
{
    lg::MaterialSpec material;
    material.fillGuid = kFill;
    material.fillName = "Old fill name";
    const auto resolution = lg::ResolveMaterialAttribute (Catalog (), lg::MaterialAttributeKind::Fill,
                                                          material.fillGuid, material.fillName);
    EXPECT_EQ (resolution.state, lg::MaterialResolutionState::Resolved);
    ASSERT_TRUE (resolution.attribute);
    EXPECT_EQ (resolution.attribute->index, 7);
    EXPECT_EQ (resolution.attribute->name, "Renamed fill");
    std::string error;
    EXPECT_TRUE (lg::ValidateMaterialAvailability (material, Catalog (), error));
    EXPECT_EQ (material.fillName, "Old fill name") << "Read-only resolution does not rewrite authored metadata.";
}

TEST (EditMaterial, MissingGuidNeverFallsBackToSameNameOrPreset)
{
    lg::MaterialSpec material;
    material.preset = lg::MaterialPreset::Grass;
    material.fillGuid = kSurface; // Valid GUID, but this project uses it for a surface, not a fill.
    material.fillName = "Renamed fill";
    std::string error;
    EXPECT_FALSE (lg::ValidateMaterialAvailability (material, Catalog (), error));
    EXPECT_NE (error.find ("Missing"), std::string::npos);
    auto resolution = lg::ResolveMaterialAttribute (Catalog (), lg::MaterialAttributeKind::Fill, "", material.fillName);
    EXPECT_EQ (resolution.state, lg::MaterialResolutionState::NeedsConfirmation);
    auto catalog = Catalog ();
    catalog.attributes.push_back (catalog.attributes[0]);
    resolution = lg::ResolveMaterialAttribute (catalog, lg::MaterialAttributeKind::Fill, kFill, "");
    EXPECT_EQ (resolution.state, lg::MaterialResolutionState::Ambiguous);
}

TEST (EditMaterial, ExplicitNativeChoiceStoresGuidAndNameNotIndexAndKeepsOtherFields)
{
    lg::MaterialSpec material;
    material.surfaceGuid = kSurface;
    material.surfaceName = "Surface";
    material.hatchDensity = 5;
    std::string error;
    ASSERT_TRUE (lg::PickMaterialAttribute (material, Catalog ().attributes[0], error));
    EXPECT_EQ (material.fillGuid, kFill);
    EXPECT_EQ (material.fillName, "Renamed fill");
    EXPECT_EQ (material.surfaceGuid, kSurface);
    EXPECT_EQ (material.hatchDensity, 5);
    auto invalid = Catalog ().attributes[0];
    invalid.index = 0;
    EXPECT_FALSE (lg::PickMaterialAttribute (material, invalid, error));
}

TEST (EditMaterial, QueuedPickerAppliesOneUndoableEditAndRejectsAStaleDialog)
{
    edit::Document doc;
    std::string error;
    ASSERT_TRUE (lg::StartLandscape (doc, { 1 }, error));
    const auto line = edit::AddPolyline (doc, { { 0, 0, 0 }, { 10, 0, 0 }, { 0, 10, 0 } }, true);
    ASSERT_TRUE (lg::SetLandscapeSourceRole (doc, line, lg::SourceRole::RegionBoundary, error));
    const auto object = doc.landscape->objects[0].id;
    eh::Session session (doc);
    ASSERT_TRUE (eh::RequestMaterialPicker (session, object));
    const auto request = session.materialPicker;
    EXPECT_EQ (session.edits.UndoDepth (), 0u);
    lg::MaterialSpec picked;
    ASSERT_TRUE (lg::PickMaterialAttribute (picked, Catalog ().attributes[0], error));
    ASSERT_TRUE (eh::ApplyPickedMaterial (session, request, picked)) << session.message;
    EXPECT_EQ (session.edits.UndoDepth (), 1u);
    EXPECT_EQ (session.edits.Doc ().landscape->objects[0].baseMaterial.fillGuid, kFill);
    EXPECT_FALSE (eh::ApplyPickedMaterial (session, request, picked));
    ASSERT_TRUE (session.edits.Undo ());
    EXPECT_EQ (session.edits.Doc ().landscape, doc.landscape);
    session.materialPicker.reset ();
    ASSERT_TRUE (eh::RequestMaterialPicker (session, object));
    const auto cancelled = session.materialPicker;
    session.materialPicker.reset ();
    EXPECT_FALSE (eh::ApplyPickedMaterial (session, cancelled, picked));
    EXPECT_EQ (session.edits.UndoDepth (), 0u);
}

#include "ArchViz/HudMassingRules.hpp"
#include "ArchViz/HudMassing.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/OverlayHudEngine.hpp"
#include "ArchViz/HudMassingStats.hpp"
#include "Geometry/Primitives.hpp"
#include "hud_fixture.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

namespace calc = geomsrv::archviz::massingcalculation;
namespace rules = geomsrv::archviz::massingrules;
namespace widgets = geomsrv::archviz::hudmassingrules;
namespace layers = geomsrv::archviz::overlaylayers;
namespace slices = geomsrv::archviz::massingslices;

namespace {
rules::Page Parcel (std::string guid, double x = 0, double y = 0)
{
    rules::Page page;
    page.guid = std::move (guid);
    page.edges = {
        { x, y, x + 10, y }, { x + 10, y, x + 10, y + 10 }, { x + 10, y + 10, x, y + 10 }, { x, y + 10, x, y }
    };
    EXPECT_TRUE (rules::Restore (page, {}));
    return page;
}
calc::Result Envelope (double x, double y, double width = 10, double height = 10)
{
    geomsrv::Mesh box;
    std::string error;
    EXPECT_TRUE (geomsrv::engine::MakeBox ({ x + width / 2, y + height / 2, 5 }, width, height, 12, box, error));
    calc::Result result;
    result.layer.name = "tapioca.massing.envelope";
    result.site.name = "tapioca.massing.lines";
    layers::Mesh mesh;
    mesh.points = box.vertices;
    mesh.indices = box.triangles;
    result.layer.meshes.push_back (std::move (mesh));
    result.parcelArea = result.allowedArea = width * height;
    result.offsetXY = { x, y, x + width, y, x + width, y + height, x, y + height };
    result.hasEnvelope = true;
    result.faces = 6;
    return result;
}
calc::Request Site (const std::vector<rules::Page>& pages)
{
    widgets::Draft active;
    widgets::SiteDraft site;
    widgets::SyncSite (pages, active, site);
    return widgets::SiteInputs (pages, active, site);
}
slices::Input Slab ()
{
    slices::Input input;
    input.slab.guid = "slab";
    input.slab.id = "Building";
    input.slab.top = 0.3;
    input.slab.outer.xy = { 0, 0, 10, 0, 10, 10, 0, 10 };
    return input;
}
double Area (const layers::Mesh& mesh)
{
    double area = 0;
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        const auto a = mesh.indices[i] * 3, b = mesh.indices[i + 1] * 3, c = mesh.indices[i + 2] * 3;
        area += std::abs ((mesh.points[b] - mesh.points[a]) * (mesh.points[c + 1] - mesh.points[a + 1]) -
                          (mesh.points[b + 1] - mesh.points[a + 1]) * (mesh.points[c] - mesh.points[a])) *
                0.5;
    }
    return area;
}
} // namespace

TEST (MassingParcels, DefineAddUnionsMultiplePropertyLinesWithoutReplacingExistingRoles)
{
    namespace hud = geomsrv::archviz::hudmassing;
    const auto plan = hud::Plan ({ hud::Group::PropertyLine, hud::Action::Add }, { "first" }, { "second", "third" });
    ASSERT_EQ (plan.size (), 2u);
    EXPECT_TRUE (plan[0].assign);
    EXPECT_TRUE (plan[1].assign);
    EXPECT_NE (plan[0].guid, "first");
    EXPECT_NE (plan[1].guid, "first");
}

TEST (MassingParcels, DraftsKeepIndependentOffsetsAndEveryParcelIsIncludedInPreview)
{
    const std::vector<rules::Page> pages { Parcel ("first"), Parcel ("second", 20, 15) };
    widgets::Draft active;
    widgets::SiteDraft site;
    widgets::SyncSite (pages, active, site);
    active.assignments[0].distance = 1;
    active.endpoints[1] = false;
    active.dirty = true;
    active.calculation.capZ = 30;
    widgets::SyncSite (pages, active, site);
    active = site.parcels.at ("second");
    active.assignments[0].distance = 2;
    widgets::SyncSite (pages, active, site);
    auto request = widgets::SiteInputs (pages, active, site);
    ASSERT_EQ (request.parcels.size (), 2u);
    EXPECT_EQ (request.before.guid, "first"); // independent of which parcel is selected
    EXPECT_EQ (request.parcels[0].assignments[0].distance, 1);
    EXPECT_FALSE (request.parcels[0].endpoints[1]);
    EXPECT_EQ (request.parcels[1].assignments[0].distance, 2);
    auto expanded = calc::Expand (request);
    ASSERT_EQ (expanded.size (), 2u);
    EXPECT_EQ (expanded[0].before.guid, "first");
    EXPECT_EQ (expanded[1].before.guid, "second");
    EXPECT_TRUE (expanded[0].parcels.empty ());
    EXPECT_TRUE (expanded[1].parcels.empty ());
    const auto old = request;
    request.parcels[1].assignments[2].distance = 0;
    EXPECT_FALSE (calc::SameRequest (old, request));
    request = old;
    request.parcels[1].before.edges[0].ax += 1e-8;
    EXPECT_FALSE (calc::SameRequest (old, request));
    std::string encoded, error;
    EXPECT_FALSE (calc::Encode (request, {}, false, 0, encoded, error));
    EXPECT_TRUE (calc::Encode (expanded[1], {}, false, 0, encoded, error));
}

TEST (MassingParcels, SourceChangesKeepEachParcelsOffsetsAndRemovedParcelsArePruned)
{
    std::vector<rules::Page> pages { Parcel ("first"), Parcel ("second", 20) };
    widgets::Draft active;
    widgets::SiteDraft site;
    widgets::SyncSite (pages, active, site);
    active.assignments[0].distance = 1;
    active.dirty = true;
    site.parcels["second"].assignments[0].distance = 2;
    site.parcels["second"].dirty = true;
    pages[1].edges[0].ax = pages[1].edges[3].bx = 20.1;
    widgets::SyncSite (pages, active, site);
    EXPECT_TRUE (active.dirty);
    EXPECT_EQ (active.assignments[0].distance, 1);
    EXPECT_TRUE (site.parcels["second"].dirty);
    EXPECT_EQ (site.parcels["second"].assignments[0].distance, 2);
    EXPECT_FALSE (site.parcels["second"].note.empty ());
    widgets::SyncSite ({ pages[1] }, active, site);
    EXPECT_EQ (active.source.guid, "second");
    EXPECT_EQ (site.parcels.count ("first"), 0u);
    EXPECT_EQ (widgets::SiteInputs ({ pages[1] }, active, site).before.guid, "second");
}

TEST (MassingParcels, CombinedPreviewKeepsSeparateEnvelopesAndSumsParcelAreasEvenIfTheyOverlap)
{
    const auto request = Site ({ Parcel ("first"), Parcel ("second", 5) });
    const auto singles = calc::Expand (request);
    auto first = Envelope (0, 0), second = Envelope (5, 0);
    calc::Preview combined;
    std::string error;
    ASSERT_TRUE (calc::Combine (request, { { singles[0], first }, { singles[1], second } }, combined, error)) << error;
    EXPECT_TRUE (combined.result.hasEnvelope);
    EXPECT_EQ (combined.result.parcelArea, 200); // not the 150 m2 union
    EXPECT_EQ (combined.result.allowedArea, 200);
    EXPECT_EQ (combined.result.layer.meshes.size (), 2u);
    EXPECT_EQ (combined.parcels.size (), 2u);
    EXPECT_EQ (combined.parcels[1].result.offsetXY.front (), 5);
    EXPECT_EQ (combined.result.layer.meshes[1].hoverTitle, "Parcel 2 envelope");
    second.hasEnvelope = false;
    second.layer.meshes.clear ();
    ASSERT_TRUE (calc::Combine (request, { { singles[0], first }, { singles[1], second } }, combined, error)) << error;
    EXPECT_FALSE (combined.result.hasEnvelope) << "partial site must not fabricate allowed figures";
    slices::Result floors;
    ASSERT_TRUE (slices::Build ({ Slab () }, {}, &combined.result, floors, error)) << error;
    EXPECT_FALSE (floors.clipped);
    EXPECT_EQ (floors.allowedArea, 0);
    EXPECT_EQ (floors.layer.meshes.front ().rgba, 0xF2C14E59);
    combined.result.parcelArea = 123;
    EXPECT_FALSE (calc::Combine (request, { { singles[0], first } }, combined, error));
    EXPECT_EQ (combined.result.parcelArea, 123);
    EXPECT_FALSE (calc::Combine (request, { { singles[0], first }, { singles[0], first } }, combined, error));
    EXPECT_EQ (combined.result.parcelArea, 123);
}

TEST (MassingParcels, OverlappingAllowedEnvelopesUseAUnionAndNeverXorOrDoubleCountSlabAreas)
{
    auto envelope = Envelope (0, 0, 6, 10);
    const auto second = Envelope (4, 0, 6, 10);
    envelope.layer.meshes.push_back (second.layer.meshes.front ());
    slices::Result result;
    std::string error;
    ASSERT_TRUE (slices::Build ({ Slab () }, {}, &envelope, result, error)) << error;
    EXPECT_TRUE (result.clipped);
    EXPECT_NEAR (result.allowedArea, 100, 1e-6);
    ASSERT_EQ (result.layer.meshes.size (), 1u);
    EXPECT_NEAR (Area (result.layer.meshes[0]), 100, 1e-6);
    envelope.layer.meshes[1] = Envelope (8, 0, 2, 10).layer.meshes.front ();
    ASSERT_TRUE (slices::Build ({ Slab () }, {}, &envelope, result, error)) << error;
    EXPECT_NEAR (result.allowedArea, 80, 1e-6);
    ASSERT_EQ (result.layer.meshes.size (), 2u);
    EXPECT_NEAR (Area (result.layer.meshes[0]), 80, 1e-6);
    EXPECT_NEAR (Area (result.layer.meshes[1]), 20, 1e-6);
    EXPECT_EQ (result.layer.meshes[1].rgba, 0xAA4465FF);
}

TEST (MassingParcels, ProtrudingRegionIsPropertyRedAtHalfOpacityAndExcludedFromLabelsAndFeasibility)
{
    auto input = Slab ();
    input.slab.holes.push_back ({ { 4, 4, 6, 4, 6, 6, 4, 6 }, {} });
    const auto envelope = Envelope (2, 2, 6, 6);
    slices::Result result;
    std::string error;
    ASSERT_TRUE (slices::Build ({ input }, {}, &envelope, result, error)) << error;
    ASSERT_EQ (result.layer.meshes.size (), 2u);
    EXPECT_NEAR (Area (result.layer.meshes[0]), 32, 1e-6);
    EXPECT_NEAR (Area (result.layer.meshes[1]), 64, 1e-6);
    EXPECT_EQ (result.layer.meshes[0].rgba, 0xF2C14E59);
    EXPECT_EQ (result.layer.meshes[1].rgba, 0xAA4465FF);
    EXPECT_FLOAT_EQ (result.layer.meshes[1].style.opacity, 0.5f);
    EXPECT_EQ (result.allowedArea, 32);
    EXPECT_EQ (result.rawArea, 96);
    ASSERT_EQ (result.layer.texts.size (), 1u);
    EXPECT_EQ (result.layer.texts[0].text, "32.0 m\xC2\xB2");
    EXPECT_EQ (result.layer.meshes[1].hoverRows[0].second, "64.0 m\xC2\xB2");
    geomsrv::archviz::storysliceoverlay::Controls display;
    display.fillRgba &= 0xFFFFFF00;
    ASSERT_TRUE (slices::Build ({ input }, {}, &envelope, result, error, display)) << error;
    EXPECT_TRUE (result.layer.meshes.empty ());
    EXPECT_EQ (result.allowedArea, 32);
}

TEST (MassingParcels, InvalidSecondEnvelopeRefusesTheWholeSlicePublication)
{
    auto envelope = Envelope (0, 0);
    envelope.layer.meshes.push_back (Envelope (20, 0).layer.meshes.front ());
    envelope.layer.meshes[1].indices[0] = 999999;
    slices::Result result;
    result.note = "unchanged";
    std::string error;
    EXPECT_FALSE (slices::Build ({ Slab () }, {}, &envelope, result, error));
    EXPECT_EQ (result.note, "unchanged");
}

TEST (MassingParcels, NativeHudRequestsBothParcelsAndClearDropsEveryDraft)
{
    using namespace hudtest;
    Watched hud;
    hud::OwnPages pages;
    pages.standalone = true;
    pages.massing.known = true;
    pages.massing.parcels = { Parcel ("first"), Parcel ("second", 20, 10) };
    pages.massing.rules = pages.massing.parcels[0];
    hud.engine.SetOwnPages (pages);
    hud::SelectKey (*hud.state, geomsrv::archviz::hudmassing::kTabKey);
    hud.Lay ({}, At (600, 600));
    hud.Lay ({}, At (600, 600));
    const auto requests = hud::TakeMassingCalculations (*hud.state);
    ASSERT_EQ (requests.size (), 1u);
    ASSERT_EQ (requests[0].parcels.size (), 2u);
    EXPECT_EQ (requests[0].parcels[0].before.guid, "first");
    EXPECT_EQ (requests[0].parcels[1].before.guid, "second");
    EXPECT_EQ (hud.state->massingSite.parcels.size (), 2u);
    hud::ClearState (*hud.state);
    EXPECT_TRUE (hud.state->massingSite.parcels.empty ());
    EXPECT_FALSE (hud.state->massingSite.lastRequested);
}

TEST (MassingParcels, EverySetAnswerQueuesACompleteSiteWithoutNeedingAnotherFrame)
{
    using namespace hudtest;
    auto state = hud::NewState ();
    const std::vector<rules::Page> pages { Parcel ("first"), Parcel ("second", 20) };
    widgets::SyncSite (pages, state->massingRules, state->massingSite);
    for (const auto& key :
         { "Cap Project Z", "STR base height", "Run per 1 m rise", "Flat base depth", "Default setback", "Offset" }) {
        const auto before = calc::Expand (widgets::SiteInputs (pages, state->massingRules, state->massingSite))[0];
        widgets::NumberEdit edit { before, key, 0, 0, 100, 0 };
        ASSERT_TRUE (hud::AnswerMassingNumber (*state, edit, 4.25)) << key;
        const auto queued = hud::TakeMassingCalculations (*state);
        ASSERT_EQ (queued.size (), 1u) << key;
        EXPECT_EQ (queued[0].parcels.size (), 2u);
        EXPECT_TRUE (
            calc::SameRequest (queued[0], widgets::SiteInputs (pages, state->massingRules, state->massingSite)));
        EXPECT_FALSE (hud::AnswerMassingNumber (*state, edit, 999));
        EXPECT_TRUE (hud::TakeMassingCalculations (*state).empty ());
    }
}

TEST (MassingParcels, StatsMixVolumeAndCoverageUseRealContoursAndCapTheLastFloorAtSlabTop)
{
    auto input = Slab ();
    input.slab.top = 8;
    input.slab.holes.push_back ({ { 4, 4, 6, 4, 6, 6, 4, 6 }, {} });
    slices::Result result;
    std::string error;
    ASSERT_TRUE (slices::Build ({ input }, {}, nullptr, result, error)) << error;
    EXPECT_EQ (result.rawVolume, 96 * 8);
    ASSERT_EQ (result.rows.size (), 2u); // short top remainder belongs to the last floor
    EXPECT_EQ (result.rows.back ().floorHeight, 5);
    result.rows[0].function = "commercial";
    const auto mix = slices::UsageMix (result);
    ASSERT_EQ (mix.size (), 2u);
    EXPECT_EQ (mix[0].function, "commercial");
    EXPECT_EQ (mix[0].percent, 50);
    EXPECT_EQ (mix[0].volume, 96 * 3);
    EXPECT_EQ (mix[1].volume, 96 * 5);
    calc::Preview preview;
    preview.inputs = Site ({ Parcel ("first"), Parcel ("second", 5) });
    ASSERT_TRUE (slices::Coverage (result, preview, error)) << error;
    EXPECT_EQ (result.parcelArea, 200);
    EXPECT_EQ (result.builtArea, 96 + 48); // hole + overlapping parcels, not two stacked floors
    EXPECT_EQ (result.unbuiltArea, 56);
    layers::Layer highlight;
    ASSERT_TRUE (slices::Highlight (result, "commercial", highlight, error)) << error;
    ASSERT_EQ (highlight.meshes.size (), 1u);
    EXPECT_FLOAT_EQ (highlight.meshes[0].style.opacity, 1);
    double low = 100, high = -100;
    for (size_t i = 2; i < highlight.meshes[0].points.size (); i += 3) {
        low = (std::min) (low, highlight.meshes[0].points[i]);
        high = (std::max) (high, highlight.meshes[0].points[i]);
    }
    EXPECT_EQ (low, 0);
    EXPECT_EQ (high, 3);
    double signedVolume = 0;
    const auto& mesh = highlight.meshes[0];
    for (size_t i = 0; i < mesh.indices.size (); i += 3) {
        const size_t a = mesh.indices[i] * 3, b = mesh.indices[i + 1] * 3, c = mesh.indices[i + 2] * 3;
        signedVolume +=
            (mesh.points[a] * (mesh.points[b + 1] * mesh.points[c + 2] - mesh.points[b + 2] * mesh.points[c + 1]) +
             mesh.points[a + 1] * (mesh.points[b + 2] * mesh.points[c] - mesh.points[b] * mesh.points[c + 2]) +
             mesh.points[a + 2] * (mesh.points[b] * mesh.points[c + 1] - mesh.points[b + 1] * mesh.points[c])) /
            6;
    }
    EXPECT_NEAR (signedVolume, 96 * 3, 1e-6) << "courtyard remains a hole with inward-facing walls";
    ASSERT_TRUE (slices::Highlight (result, "", highlight, error));
    EXPECT_TRUE (highlight.meshes.empty ());
}

TEST (MassingParcels, ExtrudedHighlightsUseSliceFillOpacityAndAFunctionColouredWireframe)
{
    slices::Result result;
    std::string error;
    geomsrv::archviz::storysliceoverlay::Controls display;
    display.fillOpacity = 0.4f;
    display.outlineWidthPixels = 1.25f;
    ASSERT_TRUE (slices::Build ({ Slab () }, {}, nullptr, result, error, display)) << error;
    layers::Layer highlight;
    ASSERT_TRUE (slices::Highlight (result, "residential", highlight, error)) << error;
    ASSERT_FALSE (highlight.meshes.empty ());
    for (const auto& mesh : highlight.meshes) {
        EXPECT_EQ (mesh.rgba & 0xFF, 0x59u);
        EXPECT_FLOAT_EQ (mesh.style.opacity, 0.4f);
        EXPECT_EQ (mesh.style.edgeRgba, result.rows[0].rgba | 0xFFu);
        EXPECT_FLOAT_EQ (mesh.style.edgeWidthPixels, 1.25f);
    }
    display.fillRgba &= 0xFFFFFF00;
    ASSERT_TRUE (slices::Build ({ Slab () }, {}, nullptr, result, error, display));
    ASSERT_TRUE (slices::Highlight (result, "residential", highlight, error));
    EXPECT_EQ (highlight.meshes[0].rgba & 0xFF, 0u);
    EXPECT_NE (highlight.meshes[0].style.edgeRgba & 0xFF, 0u);
}

TEST (MassingParcels, StatsPercentageDiagramHoverReturnsItsFunctionAndClearsOnLeave)
{
    ImGuiContext* context = ImGui::CreateContext ();
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = { 1000, 1000 };
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &w, &h);
    slices::Result result;
    std::string error;
    ASSERT_TRUE (slices::Build ({ Slab () }, {}, nullptr, result, error));
    ImRect lastRow;
    const auto frame = [&] (ImVec2 mouse) {
        io.AddMousePosEvent (mouse.x, mouse.y);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 500, 600 });
        ImGui::Begin ("stats-test");
        const auto function = geomsrv::archviz::hudmassingstats::Draw (result);
        lastRow = context->LastItemData.Rect;
        ImGui::End ();
        ImGui::Render ();
        return function;
    };
    EXPECT_TRUE (frame ({ 800, 800 }).empty ());
    EXPECT_EQ (frame (lastRow.GetCenter ()), "residential");
    EXPECT_TRUE (frame ({ 800, 800 }).empty ());
    ImGui::DestroyContext (context);
}

TEST (MassingParcels, SharedPlanCanvasKeepsRelativePositionAndClickSwitchesWithoutLosingDrafts)
{
    ImGuiContext* context = ImGui::CreateContext ();
    auto& io = ImGui::GetIO ();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = { 1200, 1200 };
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    const std::vector<rules::Page> pages { Parcel ("first"), Parcel ("second", 20, 10) };
    widgets::Draft active;
    widgets::SiteDraft site;
    ImRect canvas;
    const auto frame = [&] (ImVec2 pointer, bool down = false) {
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 500, 750 });
        ImGui::Begin ("parcels-test");
        widgets::DrawSite (pages, active, site, false, {}, {});
        canvas = context->LastItemData.Rect;
        ImGui::End ();
        ImGui::Render ();
    };
    frame ({ 850, 850 });
    frame ({ 850, 850 });
    active.calculations.clear ();
    active.assignments[0].distance = 2;
    active.dirty = true;
    active.calculation.capZ = 30;
    frame ({ 850, 850 });
    active.calculations.clear ();
    const auto before = widgets::SiteInputs (pages, active, site);
    // Site bounds 0..30, 0..20 fit at 6.8 px/m. The second parcel's
    // bottom edge midpoint (25,10) is east of the first, not re-centred over it.
    const auto center = canvas.GetCenter ();
    const ImVec2 secondMidpoint { center.x + 68, center.y };
    frame (secondMidpoint, true);
    frame (secondMidpoint);
    EXPECT_EQ (active.source.guid, "second");
    EXPECT_TRUE (active.calculations.empty ()) << "selection alone must not recalculate";
    EXPECT_TRUE (calc::SameRequest (before, widgets::SiteInputs (pages, active, site)));
    EXPECT_EQ (site.parcels.at ("first").assignments[0].distance, 2);
    EXPECT_TRUE (site.parcels.at ("first").dirty);
    EXPECT_EQ (active.calculation.capZ, 30);
    const widgets::NumberEdit prompt { calc::Expand (before)[0], "Cap Project Z", 25, 5, 50 };
    EXPECT_FALSE (widgets::AnswerNumber (active, prompt, 25)) << "never retarget a prompt to another parcel";
    ImGui::DestroyContext (context);
}

TEST (MassingParcels, DimensionsAndMeanElevationUseEachParcelsOwnGeometryAndReferenceCount)
{
    const auto request = Site ({ Parcel ("first"), Parcel ("second", 20) });
    const auto singles = calc::Expand (request);
    auto first = Envelope (3, 3, 4, 4), second = Envelope (23, 3, 4, 4);
    first.parcelArea = second.parcelArea = 100;
    first.hasMeanZ = second.hasMeanZ = first.hasMeanASL = second.hasMeanASL = true;
    first.meanZ = 2;
    second.meanZ = 6;
    first.meanASL = 102;
    second.meanASL = 106;
    layers::PointSet a, b;
    a.points = { 0, 0, 2 };
    b.points = { 20, 0, 6, 30, 0, 6, 30, 10, 6 };
    first.site.points.push_back (a);
    second.site.points.push_back (b);
    calc::Preview combined;
    std::string error;
    ASSERT_TRUE (calc::Combine (request, { { singles[0], first }, { singles[1], second } }, combined, error)) << error;
    EXPECT_EQ (combined.result.meanZ, 5);
    EXPECT_EQ (combined.result.meanASL, 105);
    const auto dimensions = calc::OffsetDimensions (combined);
    ASSERT_EQ (dimensions.dimensions.size (), 8u);
    EXPECT_EQ (dimensions.dimensions[0].from[2], 2);
    EXPECT_EQ (dimensions.dimensions[4].from[2], 6);
    EXPECT_GE (dimensions.dimensions[4].from[0], 20);
    EXPECT_TRUE (layers::Validate (dimensions).empty ());
}

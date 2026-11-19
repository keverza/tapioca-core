#include "ArchViz/ViewerSceneLayers.hpp"
#include "ArchViz/EditLayers.hpp"
#include "ArchViz/MassingCalculation.hpp"
#include "ArchViz/MassingSlices.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/SurfaceTakeoffLayer.hpp"
#include <gtest/gtest.h>
#include <set>

namespace av = geomsrv::archviz;
namespace ol = geomsrv::archviz::overlaylayers;
namespace vs = geomsrv::archviz::viewerscene;

namespace {
std::shared_ptr<const ol::Layer> RawLayer (const std::string& name, ol::Behind occlusion = ol::Behind::Hide)
{
    auto layer = std::make_shared<ol::Layer> ();
    layer->name = name;
    layer->occlusion = occlusion;
    ol::Mesh mesh;
    mesh.points = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    mesh.indices = { 0, 1, 2 };
    mesh.rgba = 0x11223344u;
    layer->meshes.push_back (mesh);
    ol::Polyline line;
    line.points = { 0, 0, 0, 1, 1, 1 };
    layer->polylines.push_back (line);
    return layer;
}
std::set<std::string> Names (const std::vector<vs::LayerPtr>& layers)
{
    std::set<std::string> out;
    for (const auto& layer : layers)
        out.insert (layer->name);
    return out;
}
} // namespace

// The envelope and slices are unstyled meshes on a Hide layer: the injected overlay's raw
// pipeline draws them, PrepareScene skips them, and the viewer drew nothing (#201).
TEST (ViewerSceneLayers, RawItemsBecomeGuestItemsWithTheSameLookAndOcclusion)
{
    const auto raw = RawLayer (av::massingcalculation::kEnvelopeLayer);
    ASSERT_FALSE (ol::DrawnByGuest (raw->meshes[0], *raw));
    ASSERT_FALSE (ol::DrawnByGuest (raw->polylines[0], *raw));
    EXPECT_TRUE (av::overlayscene::PrepareScene ({ raw }, nullptr).fills.empty ());

    const auto guest = vs::GuestDrawn (raw);
    ASSERT_NE (guest, raw);
    EXPECT_TRUE (ol::DrawnByGuest (guest->meshes[0], *guest));
    EXPECT_TRUE (ol::DrawnByGuest (guest->polylines[0], *guest));
    EXPECT_EQ (guest->meshes[0].style.shading, ol::Shading::Flat);
    EXPECT_EQ (guest->meshes[0].style.behind, ol::Behind::Hide);
    EXPECT_EQ (guest->polylines[0].behind, ol::Behind::Hide);
    EXPECT_EQ (guest->meshes[0].rgba, raw->meshes[0].rgba);
    const auto scene = av::overlayscene::PrepareScene ({ guest }, nullptr);
    EXPECT_EQ (scene.fills.size (), 3u);
    EXPECT_FALSE (scene.lines.empty ());

    EXPECT_EQ (vs::GuestDrawn (RawLayer ("x", ol::Behind::Show))->meshes[0].style.behind, ol::Behind::Show);
    // A layer with nothing raw is the same object.
    EXPECT_EQ (vs::GuestDrawn (guest), guest);
}

TEST (ViewerSceneLayers, EditorAloneDrawsItsLayerTheMassingWorkflowDrawsTheShownOnes)
{
    auto plan = std::make_shared<ol::Layer> ();
    plan->name = "plan.only";
    plan->views = ol::Views::TwoD;
    const std::vector<vs::LayerPtr> all = {
        RawLayer (av::editlayers::kLayer),    RawLayer (av::massingcalculation::kEnvelopeLayer),
        RawLayer (av::massingslices::kLayer), RawLayer (av::surfacetakeoff::kLayer),
        RawLayer ("hidden.by.user"),          plan
    };
    const auto shown = [] (const std::string& name) { return name != "hidden.by.user"; };
    EXPECT_EQ (Names (vs::Select (all, { false, false, shown })), std::set<std::string> { av::editlayers::kLayer });
    EXPECT_EQ (Names (vs::Select (all, { true, false, shown })),
               (std::set<std::string> { av::editlayers::kLayer, av::massingcalculation::kEnvelopeLayer,
                                        av::massingslices::kLayer }));
    // The exploded diagram stands in for the storeys: their in-place layer and the envelope go.
    EXPECT_EQ (Names (vs::Select (all, { true, true, shown })), std::set<std::string> { av::editlayers::kLayer });
    EXPECT_TRUE (vs::DrawsStoreysInPlace (av::massingslices::kHighlightLayer));
    EXPECT_TRUE (vs::DrawsStoreysInPlace (av::buildingplan::kUnitsLayer)); // the planned flats too
    EXPECT_TRUE (vs::DrawsStoreysInPlace (av::massingcalculation::kEnvelopeLayer));
    EXPECT_FALSE (vs::DrawsStoreysInPlace ("tapioca.massing.lines")); // the property lines stay
}

// PrepareScene caches by layer identity: an unchanged source must convert to the same copy.
TEST (ViewerSceneLayers, ConverterKeepsTheConvertedIdentityOfAnUnchangedLayer)
{
    vs::Converter converter;
    const auto a = RawLayer ("a"), b = RawLayer ("b");
    const auto first = converter.Convert ({ a, b });
    const auto second = converter.Convert ({ a, b });
    ASSERT_EQ (first.size (), 2u);
    EXPECT_EQ (first, second);
    const auto b2 = RawLayer ("b");
    const auto third = converter.Convert ({ a, b2 });
    EXPECT_EQ (third[0], first[0]);
    EXPECT_NE (third[1], first[1]);
    converter.Clear ();
    EXPECT_NE (converter.Convert ({ a })[0], first[0]);
}

// Behind the diagram a property line is hidden, not dashed through the storeys (#201).
TEST (ViewerSceneLayers, BehindTheDiagramFaintAndDashedBecomeHidden)
{
    auto site = std::make_shared<ol::Layer> ();
    site->name = "tapioca.massing.lines";
    site->occlusion = ol::Behind::Dash;
    ol::Polyline line;
    line.points = { 0, 0, 0, 10, 0, 0 };
    line.dashMetres = { 1.0f, 0.5f };
    line.hidden.widthPixels = 1.0f;
    site->polylines.push_back (line);
    line.behind = ol::Behind::Show; // an "always" item keeps its rule
    site->polylines.push_back (line);
    const vs::LayerPtr layer = site;
    EXPECT_EQ (vs::GuestDrawn (layer, false), layer); // nothing raw: as it is
    const auto hidden = vs::GuestDrawn (layer, true);
    ASSERT_NE (hidden, layer);
    EXPECT_EQ (hidden->occlusion, ol::Behind::Hide);
    EXPECT_EQ (hidden->polylines[0].behind, ol::Behind::Layer);
    EXPECT_EQ (ol::Resolve (hidden->polylines[0].behind, *hidden), ol::Behind::Hide);
    EXPECT_EQ (hidden->polylines[0].hidden.widthPixels, 0.0f);
    EXPECT_EQ (hidden->polylines[1].behind, ol::Behind::Show);
    EXPECT_EQ (hidden->polylines[0].dashMetres, layer->polylines[0].dashMetres); // its own dash stays

    // The converter prepares again when the stand-in changes.
    vs::Converter converter;
    const auto off = converter.Convert ({ layer }, false);
    const auto on = converter.Convert ({ layer }, true);
    EXPECT_EQ (off[0], layer);
    EXPECT_NE (on[0], layer);
    EXPECT_EQ (converter.Convert ({ layer }, true)[0], on[0]);
}

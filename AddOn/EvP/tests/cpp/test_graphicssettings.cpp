#include "ArchViz/GraphicsSettings.hpp"
#include "ArchViz/GraphicsSettingsUi.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/OverlayScene.hpp"
#include "ArchViz/TraceAnnotationLayer.hpp"
#include "NodeGraph/Json.hpp"
#include "GuestKernel.hpp"
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>

namespace gs = geomsrv::archviz::graphicssettings;
namespace annotation = geomsrv::annotation;
namespace gui = geomsrv::archviz::graphicssettingsui;
namespace layers = geomsrv::archviz::overlaylayers;
namespace scene = geomsrv::archviz::overlayscene;
namespace shell = geomsrv::archviz::hudshell;
namespace json = evp::nodegraph::json;

namespace {
struct GraphicsSettings : testing::Test {
    void SetUp () override
    {
        gs::Reset ();
    }
    void TearDown () override
    {
        gs::Reset ();
        layers::ClearEverything ();
    }
};
layers::Layer Example (const std::string& name)
{
    layers::Layer layer;
    layer.name = name;
    layers::Mesh mesh;
    mesh.points = { 0, 0, 0, 4, 0, 0, 0, 4, 0 };
    mesh.indices = { 0, 1, 2 };
    mesh.rgba = 0xDCF3FAFFu;
    mesh.style.opacity = 0.25f;
    mesh.style.behind = layers::Behind::Fade;
    mesh.styled = true;
    layer.meshes.push_back (mesh);
    layers::Polyline line;
    line.points = { 0, 0, 0, 4, 0, 0 };
    line.rgba = 0xAA4465FFu;
    line.dashMetres = { 3, 1, 0.1f, 1 };
    layer.polylines.push_back (line);
    return layer;
}
struct Context {
    ImGuiContext* context = ImGui::CreateContext ();
    Context ()
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = { 1600, 2000 };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
    }
    ~Context ()
    {
        ImGui::DestroyContext (context);
    }
    ImGuiID checkbox = 0;
    ImGuiID palette = 0;
    void Frame (ImVec2 pointer = { 1500, 1900 }, bool down = false)
    {
        shell::BaseStyle (1);
        auto& io = ImGui::GetIO ();
        io.AddMousePosEvent (pointer.x, pointer.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        ImGui::SetNextWindowPos ({ 0, 0 });
        ImGui::SetNextWindowSize ({ 360, 1800 });
        ImGui::Begin ("style-lab-test", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("Graphics style lab"), 1);
        ImGui::PushID ("graphics-style-lab");
        ImGui::GetStateStorage ()->SetInt (ImGui::GetID ("envelope"), 1);
        ImGui::PushID ("envelope");
        ImGui::PushID ("envelope.surfaceOpacity");
        checkbox = ImGui::GetID ("##override");
        ImGui::PopID ();
        ImGui::PushID ("envelope.surfaceColor");
        palette = ImGui::GetID ("##palette");
        ImGui::PopID ();
        ImGui::PopID ();
        ImGui::PopID ();
        gui::Draw ();
        ImGui::End ();
        ImGui::Render ();
    }
};
} // namespace

TEST_F (GraphicsSettings, UntouchedOverridesLeaveAuthoredGeometryAndStylesAlone)
{
    auto source = Example ("tapioca.massing.envelope");
    EXPECT_FALSE (gs::Affects (source, *gs::Current ()));
    const auto styled = gs::Apply (source, *gs::Current ());
    EXPECT_EQ (styled.meshes[0].points, source.meshes[0].points);
    EXPECT_EQ (styled.meshes[0].indices, source.meshes[0].indices);
    EXPECT_EQ (styled.meshes[0].rgba, source.meshes[0].rgba);
    EXPECT_FLOAT_EQ (styled.meshes[0].style.opacity, source.meshes[0].style.opacity);
    EXPECT_EQ (styled.polylines[0].dashMetres, source.polylines[0].dashMetres);
    layers::Set (source);
    const auto first = layers::Layers ();
    const auto second = layers::Layers ();
    EXPECT_EQ (first[0], second[0]);
}

TEST_F (GraphicsSettings, BoundsNonfiniteAndUnknownKeysCannotCorruptThePublishedSnapshot)
{
    const auto before = gs::Current ();
    EXPECT_FALSE (gs::Set ("envelope.surfaceOpacity", -0.1));
    EXPECT_FALSE (gs::Set ("envelope.surfaceOpacity", 1.1));
    EXPECT_FALSE (gs::Set ("envelope.surfaceOpacity", std::numeric_limits<double>::quiet_NaN ()));
    EXPECT_FALSE (gs::Set ("annotation.textHalo", std::numeric_limits<double>::infinity ()));
    EXPECT_FALSE (gs::Set ("annotation.textColor", 123.5));
    EXPECT_FALSE (gs::Set ("envelope.surfaceIsHatched", 0.5));
    EXPECT_FALSE (gs::Set ("unknown", 0.5));
    EXPECT_EQ (gs::Current (), before);
    ASSERT_TRUE (gs::Set ("envelope.surfaceOpacity", 0.6));
    EXPECT_EQ (gs::Current ()->revision, before->revision + 1);
    EXPECT_FALSE (before->values.at ("envelope.surfaceOpacity").enabled);
    EXPECT_FALSE (gs::Set ("envelope.surfaceOpacity", 0.6));
    EXPECT_TRUE (gs::Reset ("envelope.surfaceOpacity"));
    EXPECT_FALSE (gs::Reset ("envelope.surfaceOpacity"));
}

TEST_F (GraphicsSettings, UiOnlyChangesDoNotInvalidateSceneUploads)
{
    layers::Set (Example ("tapioca.massing.envelope"));
    const auto before = layers::Generation ();
    const auto original = layers::Layers ()[0];
    ASSERT_TRUE (gs::Set ("ui.panelWidth", 360));
    ASSERT_TRUE (gs::Set ("diagram.height", 300));
    EXPECT_EQ (layers::Generation (), before);
    EXPECT_EQ (layers::Layers ()[0], original);
    gs::Reset ();
    EXPECT_EQ (layers::Generation (), before);
}

TEST_F (GraphicsSettings, EnvelopeOverridesReachTheGuestFillAndRefreshWithoutReauthoringGeometry)
{
    const auto authored = Example ("tapioca.massing.envelope");
    layers::Set (authored);
    const auto first = layers::Layers ()[0];
    const auto generation = layers::Generation ();
    ASSERT_TRUE (gs::Set ("envelope.surfaceOpacity", 0.6));
    ASSERT_TRUE (gs::Set ("envelope.surfaceOcclusionIntensity", 0.12));
    ASSERT_TRUE (gs::Set ("envelope.surfaceIsHatched", 1));
    ASSERT_TRUE (gs::Set ("envelope.surfaceHatchDirection", 75));
    ASSERT_TRUE (gs::Set ("envelope.surfaceHatchDensity", 2.5));
    ASSERT_TRUE (gs::Set ("envelope.surfaceColor", 0x5A7F62FFu));
    EXPECT_GT (layers::Generation (), generation);
    const auto styled = layers::Layers ()[0];
    EXPECT_NE (first, styled);
    EXPECT_EQ (styled->meshes[0].points, authored.meshes[0].points);
    EXPECT_FLOAT_EQ (first->meshes[0].style.opacity, 0.25f);
    const auto plan = scene::PreparePlan ({ styled }, nullptr);
    ASSERT_EQ (plan.fillDraws.size (), 1u);
    EXPECT_FLOAT_EQ (plan.fillDraws[0].opacity, 0.6f);
    EXPECT_FLOAT_EQ (plan.fillDraws[0].occludedOpacity, 0.12f);
    EXPECT_TRUE (plan.fillDraws[0].hatched);
    EXPECT_FLOAT_EQ (plan.fillDraws[0].hatchDirection, 75);
    EXPECT_FLOAT_EQ (plan.fillDraws[0].hatchDensity, 2.5f);
    gs::Reset ();
    EXPECT_EQ (layers::Layers ()[0]->meshes[0].rgba, authored.meshes[0].rgba);
    EXPECT_FLOAT_EQ (layers::Layers ()[0]->meshes[0].style.opacity, 0.25f);
}

TEST_F (GraphicsSettings, ParcelAndOffsetCategoriesRemainSeparateWithinTheSiteLayer)
{
    auto source = Example ("tapioca.massing.lines");
    auto offset = source.polylines[0];
    offset.rgba = 0xA66226FFu;
    source.polylines.push_back (offset);
    gs::Set ("parcel.lineThickness", 4);
    gs::Set ("parcel.lineDashLength", 2);
    gs::Set ("parcel.lineGapLength", 0.5);
    gs::Set ("offset.lineColor", 0x5A7F62FFu);
    gs::Set ("offset.lineOpacity", 0.5);
    const auto styled = gs::Apply (source, *gs::Current ());
    EXPECT_FLOAT_EQ (styled.polylines[0].widthPixels, 4);
    EXPECT_EQ (styled.polylines[0].dashMetres, (std::vector<float> { 2, 0.5f }));
    EXPECT_EQ (styled.polylines[1].rgba, 0x5A7F6280u);
    EXPECT_EQ (styled.polylines[1].dashMetres, offset.dashMetres);
    gs::Set ("parcel.lineDashLength", 0);
    EXPECT_TRUE (gs::Apply (source, *gs::Current ()).polylines[0].dashMetres.empty ());
}

TEST_F (GraphicsSettings, ThicknessOverrideRoutesSolidThreeDLinesThroughThePixelWidthRenderer)
{
    auto source = Example ("custom-solid-lines");
    source.polylines[0].dashMetres.clear ();
    ASSERT_FALSE (layers::DrawnByGuest (source.polylines[0], source));
    gs::Set ("otherOverlay.lineThickness", 4);
    const auto styled = gs::Apply (source, *gs::Current ());
    EXPECT_TRUE (layers::DrawnByGuest (styled.polylines[0], styled));
    EXPECT_EQ (layers::Resolve (styled.polylines[0].behind, styled), layers::Behind::Hide);
    EXPECT_FLOAT_EQ (styled.polylines[0].widthPixels, 4);
}

TEST_F (GraphicsSettings, AnnotationTextSizePreservesModelSizedTextAndHideThresholdIsProjectedPixels)
{
    auto source = Example ("tapioca.massing.offsetDimensions");
    layers::Text text;
    text.planar = true;
    text.sizePixels = 13;
    text.sizeMetres = 0.2;
    source.texts.push_back (text);
    layers::Dimension dimension;
    dimension.textSizePixels = 13;
    dimension.textSizeMetres = 0.2;
    source.dimensions.push_back (dimension);
    gs::Set ("annotation.textSize", 26);
    gs::Set ("annotation.textHideDistance", 12);
    gs::Set ("annotation.textHalo", 2);
    gs::Set ("annotation.textColor", 0x1C211DFFu);
    const auto styled = gs::Apply (source, *gs::Current ());
    EXPECT_DOUBLE_EQ (styled.texts[0].sizeMetres, 0.4);
    EXPECT_DOUBLE_EQ (styled.dimensions[0].textSizeMetres, 0.4);
    EXPECT_FLOAT_EQ (styled.texts[0].minProjectedPixels, 12);
    EXPECT_FLOAT_EQ (styled.dimensions[0].textMinProjectedPixels, 12);
    EXPECT_FLOAT_EQ (styled.texts[0].haloPixels, 2);
    EXPECT_EQ (styled.texts[0].rgba, 0x1C211DFFu);
}

TEST_F (GraphicsSettings, FunctionColoursAndCoverageUseTheirOwnRolesEvenWhenTheyShareALayerName)
{
    auto source = Example ("tapioca.massing.storySlices");
    source.meshes[0].rgba = 0xF2C14E59u;
    gs::Set ("function.residential", 0x5A7F62FFu);
    EXPECT_TRUE (gs::Affects (source, *gs::Current ()));
    EXPECT_EQ (gs::Apply (source, *gs::Current ()).meshes[0].rgba, 0x5A7F6259u);
    source.meshes[0].graphicsFunction = "residential";
    source.meshes[0].rgba = 0x12345659u; // Custom project palette, not the bundled RGB.
    EXPECT_EQ (gs::Apply (source, *gs::Current ()).meshes[0].rgba, 0x5A7F6259u);
    source.name = "tapioca.massing.functionVolumes";
    source.graphicsCategory = "coverageHighlight";
    gs::Set ("coverageHighlight.surfaceOpacity", 0.8);
    gs::Set ("functionHighlight.surfaceOpacity", 0.4);
    EXPECT_FLOAT_EQ (gs::Apply (source, *gs::Current ()).meshes[0].style.opacity, 0.8f);
    source.graphicsCategory = "functionHighlight";
    EXPECT_FLOAT_EQ (gs::Apply (source, *gs::Current ()).meshes[0].style.opacity, 0.4f);
}

TEST_F (GraphicsSettings, CollapseHatchCanBeDisabledReorientedAndResetWithoutChangingTerrainGeometry)
{
    auto source = Example ("tapioca.massing.collapseZone.terrain");
    gs::Set ("collapseZone.surfaceHatchDirection", 20);
    auto styled = gs::Apply (source, *gs::Current ());
    EXPECT_TRUE (styled.polylines.empty ());
    EXPECT_TRUE (styled.meshes[0].style.hatched);
    EXPECT_FLOAT_EQ (styled.meshes[0].style.hatchDirection, 20);
    gs::Set ("collapseZone.surfaceIsHatched", 0);
    EXPECT_FALSE (gs::Apply (source, *gs::Current ()).meshes[0].style.hatched);
    gs::Reset ();
    EXPECT_EQ (gs::Apply (source, *gs::Current ()).polylines.size (), source.polylines.size ());
    EXPECT_EQ (styled.meshes[0].points, source.meshes[0].points);
}

TEST_F (GraphicsSettings, UiSizesAndInteractionColoursSurvivePanelLookPushesWithoutCompoundingDpi)
{
    Context context;
    shell::BaseStyle (1);
    gs::Set ("ui.size.ScrollbarSize", 3);
    gs::Set ("ui.size.FramePadding.x", 8);
    gs::Set ("ui.size.WindowPadding.y", 12);
    gs::Set ("ui.color.ButtonHovered", 0x5A7F62FFu);
    for (float scale : { 1.0f, 2.0f, 1.0f }) {
        shell::BaseStyle (scale);
        EXPECT_FLOAT_EQ (ImGui::GetStyle ().ScrollbarSize, 3 * scale);
        EXPECT_FLOAT_EQ (ImGui::GetStyle ().FramePadding.x, 8 * scale);
        const int colours = shell::PushLook (shell::PlainLook (), scale);
        EXPECT_FLOAT_EQ (ImGui::GetStyle ().WindowPadding.y, 12 * scale);
        EXPECT_EQ (shell::Unpacked (ImGui::GetColorU32 (ImGuiCol_ButtonHovered)), 0x5A7F62FFu);
        ImGui::PopStyleColor (colours);
        ImGui::PopStyleVar (shell::kLookVars);
    }
}

TEST_F (GraphicsSettings, ViewerTraceLabelsFollowTheSameTextSizeColourHaloAndProjectedHideControls)
{
    using namespace geomsrv::archviz;
    annotation::Frame frame;
    annotation::Primitive label;
    label.kind = annotation::PrimitiveKind::Label;
    label.points = { { 0, 0, 0.5 } };
    label.text = "trace label";
    frame.primitives.push_back (label);
    const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    annotation::DimensionStyle style;
    style.textHeightModel = 0.03;
    style.hideBelowPixels = 0;
    style.capAbovePixels = 96;
    const auto baseline = BuildTraceAnnotations (frame, identity, 400, 400, 1, false, {}, nullptr, style);
    ASSERT_EQ (baseline.labels.size (), 1u);
    gs::Set ("annotation.textSize", 26);
    gs::Set ("annotation.textColor", 0x5A7F62FFu);
    gs::Set ("annotation.textHaloColor", 0xF9FCF9FFu);
    gs::Set ("annotation.textHalo", 2);
    auto edited = BuildTraceAnnotations (frame, identity, 400, 400, 1, false, {}, nullptr, style);
    ASSERT_EQ (edited.labels.size (), 1u);
    EXPECT_NEAR (edited.labels[0].fontSize, 2 * baseline.labels[0].fontSize, 1e-5f);
    EXPECT_EQ (edited.labels[0].rgba, 0x5A7F62FFu);
    EXPECT_EQ (edited.labels[0].haloRgba, 0xF9FCF9FFu);
    EXPECT_FLOAT_EQ (edited.labels[0].haloWidthPixels, 2);
    gs::Set ("annotation.textHideDistance", 96);
    edited = BuildTraceAnnotations (frame, identity, 400, 400, 1, false, {}, nullptr, style);
    EXPECT_TRUE (edited.labels.empty ());
}

TEST_F (GraphicsSettings, CategoryOverrideCanBeEnabledAndResetWithMouseOnlyInTheSettingsEditor)
{
    Context context;
    context.Frame ();
    context.Frame ();
    ImVec2 point { -1, -1 };
    for (float y = 40; y < 1780 && point.y < 0; y += 2) {
        context.Frame ({ 38, y });
        if (context.context->HoveredId == context.checkbox)
            point = { 38, y };
    }
    ASSERT_GT (point.y, 0);
    EXPECT_FALSE (gs::Current ()->values.at ("envelope.surfaceOpacity").enabled);
    context.Frame (point, true);
    context.Frame (point);
    EXPECT_TRUE (gs::Current ()->values.at ("envelope.surfaceOpacity").enabled);
    context.Frame (point, true);
    context.Frame (point);
    EXPECT_FALSE (gs::Current ()->values.at ("envelope.surfaceOpacity").enabled);
}

TEST_F (GraphicsSettings, PaletteSwatchesAndLabelsHaveUniqueIdsAndBothSelectWithoutConflictWarnings)
{
    Context context;
    context.Frame ();
    context.Frame ();
    ImVec2 combo { -1, -1 };
    for (float y = 40; y < 1780 && combo.y < 0; y += 2) {
        context.Frame ({ 100, y });
        if (context.context->HoveredId == context.palette)
            combo = { 100, y };
    }
    ASSERT_GT (combo.y, 0);
    const auto open = [&] {
        context.Frame (combo, true);
        context.Frame (combo);
        context.Frame ();
    };
    open ();
    ASSERT_EQ (context.context->OpenPopupStack.Size, 1);
    auto* popup = context.context->OpenPopupStack[0].Window;
    ASSERT_NE (popup, nullptr);
    const auto& style = ImGui::GetStyle ();
    const float row = ImGui::GetFontSize () + style.ItemSpacing.y;
    const float y = popup->Pos.y + style.WindowPadding.y + 2 * row + 6;
    const ImVec2 swatch { popup->Pos.x + style.WindowPadding.x + 6, y };
    const ImVec2 label { swatch.x + 12 + style.ItemSpacing.x + 8, y };
    const auto hover = [&] (ImVec2 at) {
        for (int frame = 0; frame < 3; ++frame)
            context.Frame (at);
        EXPECT_NE (context.context->HoveredId, 0u);
        EXPECT_EQ (context.context->HoveredIdPreviousFrameItemCount, 1);
        EXPECT_EQ (context.context->DebugDrawIdConflictsId, 0u);
        return context.context->HoveredId;
    };
    const auto labelId = hover (label);
    const auto swatchId = hover (swatch);
    EXPECT_NE (labelId, swatchId);
    context.Frame (label, true);
    context.Frame (label);
    EXPECT_TRUE (gs::Current ()->values.at ("envelope.surfaceColor").enabled);
    EXPECT_EQ (gs::Colour ("envelope.surfaceColor", 0), gs::Palette ()[2].rgba);
    EXPECT_TRUE (context.context->OpenPopupStack.empty ());
    gs::Reset ("envelope.surfaceColor");
    open ();
    ASSERT_EQ (context.context->OpenPopupStack.Size, 1);
    hover (swatch);
    context.Frame (swatch, true);
    context.Frame (swatch);
    EXPECT_TRUE (gs::Current ()->values.at ("envelope.surfaceColor").enabled);
    EXPECT_EQ (gs::Colour ("envelope.surfaceColor", 0), gs::Palette ()[2].rgba);
    EXPECT_TRUE (context.context->OpenPopupStack.empty ());
}

TEST_F (GraphicsSettings, ExportIsTypedVersionedJsonAndEscapesCustomFunctionNames)
{
    gs::FunctionColour ("custom\"use\n", 0x123456FFu);
    gs::Set ("function.custom\"use\n", 0x5A7F62FFu);
    gs::Set ("envelope.surfaceIsHatched", 1);
    gs::Set ("envelope.surfaceOpacity", 0.6);
    const auto parsed = json::Parse (gs::Encode (*gs::Current ()));
    ASSERT_TRUE (parsed.ok) << parsed.error;
    int64_t version = 0;
    ASSERT_TRUE (parsed.value.Find ("version")->AsInteger (version));
    EXPECT_EQ (version, 1);
    const auto* values = parsed.value.Find ("settings");
    std::string colour;
    ASSERT_TRUE (values->Find ("function.custom\"use\n")->Find ("value")->AsString (colour));
    EXPECT_EQ (colour, "#5A7F62FF");
    bool checked = false;
    ASSERT_TRUE (values->Find ("envelope.surfaceIsHatched")->Find ("value")->AsBool (checked));
    EXPECT_TRUE (checked);
    double opacity = 0;
    ASSERT_TRUE (values->Find ("envelope.surfaceOpacity")->Find ("value")->AsDouble (opacity));
    EXPECT_DOUBLE_EQ (opacity, 0.6);
}

TEST_F (GraphicsSettings, SaveUsesUniqueUtf8FilesAndReportsFilesystemFailuresWithoutChangingSettings)
{
    const auto stamp = std::chrono::steady_clock::now ().time_since_epoch ().count ();
    auto leaf = std::filesystem::path (u8"graphics-settings-test-\u017E-");
    leaf += std::to_string (stamp);
    const auto folder = std::filesystem::temp_directory_path () / "opencode" / leaf;
    const auto snapshot = gs::Current ();
    const auto first = gs::Save (*snapshot, folder);
    ASSERT_TRUE (first.saved) << first.error;
    const auto second = gs::Save (*snapshot, folder);
    EXPECT_TRUE (second.saved) << second.error;
    EXPECT_NE (first.path, second.path);
    std::ifstream file (first.path, std::ios::binary);
    const std::string bytes { std::istreambuf_iterator<char> (file), std::istreambuf_iterator<char> () };
    EXPECT_EQ (bytes, gs::Encode (*snapshot));
    file.close ();
    const auto failed = gs::Save (*snapshot, first.path); // A file is not a directory.
    EXPECT_FALSE (failed.saved);
    EXPECT_FALSE (failed.error.empty ());
    EXPECT_FALSE (gs::Save (*snapshot, {}).saved);
    EXPECT_EQ (gs::Current (), snapshot);
    std::filesystem::remove (first.path);
    std::filesystem::remove (second.path);
    std::filesystem::remove (folder);
}

TEST_F (GraphicsSettings, TastePaletteUsesConvertedTokensAndAdditionalFunctionColours)
{
    const auto& palette = gs::Palette ();
    EXPECT_EQ (palette[0].name, "background");
    EXPECT_EQ (palette[0].rgba, 0xF9FCF9FFu);
    EXPECT_EQ (palette[4].rgba, 0x5A7F62FFu);
    EXPECT_GT (palette.size (), 20u);
}

TEST_F (GraphicsSettings, GuestHatchesAreModelPeriodicAndBehindOpacityDoesNotFadeThePlanOrFrontPass)
{
    constexpr const char* kernel = R"hlsl(
StructuredBuffer<float4> Input : register (t8);
RWStructuredBuffer<float4> Output : register (u0);
[numthreads (1, 1, 1)]
void CSMain (uint3 id : SV_DispatchThreadID) {
    float4 v = Input[id.x];
    Output[id.x] = float4 (HatchCoverage (v.x, v.y), SurfaceAlpha (0.8, v.z, v.w), 0, 0);
}
)hlsl";
    std::string error;
    const auto result = guestkernel::Run (
        kernel, {},
        { { 0, 0.1f, 0, 0.3f }, { 4, 0.1f, 1, 0.3f }, { 0.5f, 0.1f, 2, 0 }, { 0.1f, 1, 1, 0 }, { 0, 0.1f, 1, 1 } }, 1,
        error);
    ASSERT_EQ (result.size (), 5u) << error;
    EXPECT_FLOAT_EQ (result[0][0], 1);
    EXPECT_FLOAT_EQ (result[1][0], 1);
    EXPECT_FLOAT_EQ (result[2][0], 0);
    EXPECT_FLOAT_EQ (result[3][0], 0.25f);
    EXPECT_NEAR (result[0][1], 0.8f, 1e-6f);
    EXPECT_NEAR (result[1][1], 0.24f, 1e-6f);
    EXPECT_NEAR (result[2][1], 0.8f, 1e-6f);
    EXPECT_FLOAT_EQ (result[3][1], 0);
    EXPECT_NEAR (result[4][1], 0.8f, 1e-6f);
}

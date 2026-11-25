#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudCaptureFrame.hpp"
#include "ArchViz/HudShell.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "SunStudyHudTestServices.hpp"
#include "ArchViz/ViewerScreenshotGeometry.hpp"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>

#include "ArchViz/ShadowCaptureArea.hpp"

namespace {

using namespace geomsrv::archviz;
using namespace geomsrv::archviz::viewerhud;
using hudshell::DockEdge;

TEST (ViewerScreenshot, ShadedFractionUsesMappedAreaAndTheCorrectStepWord)
{
    const auto measure = [] (unsigned step) {
        return MeasureShadowArea ({ 1, 9, 4 }, { 1, 0, 0, 1 }, 2, step, [] (size_t i) { return int (i); });
    };
    auto morning = measure (0), evening = measure (32);
    ASSERT_TRUE (morning.valid);
    EXPECT_EQ (morning.measured, 10);
    EXPECT_EQ (morning.shaded, 9);
    EXPECT_EQ (morning.missing, 4);
    EXPECT_EQ (evening.shaded, 1);
    EXPECT_FALSE (measure (64).valid);
}

TEST (ViewerScreenshot, DaylightIntervalStartsAtFirstSampleAndIncludesLastOnce)
{
    EXPECT_EQ (DaylightPanelSteps ({ 390, 420, 450, 480, 510, 540, 565 }, 1), (std::vector<int> { 0, 2, 4, 6 }));
    EXPECT_EQ (DaylightPanelSteps ({ 390, 450, 510, 570 }, 2), (std::vector<int> { 0, 2, 3 }));
    EXPECT_EQ (DaylightPanelSteps ({ 420 }, 1), (std::vector<int> { 0 }));
    EXPECT_TRUE (DaylightPanelSteps ({ 450, 390 }, 1).empty ());
}

TEST (ViewerScreenshot, FiveTimesUseTwoRowsOnOneCanvasWithSpaceForCaptions)
{
    const ScreenshotSize size { 24, 16 };
    const auto grid = TimeGrid (5, size);
    ASSERT_EQ (grid.width, 3u);
    ASSERT_EQ (grid.height, 2u);
    std::vector<uint8_t> layout (24 * 16 * 4, 255);
    for (unsigned i = 0; i < 5; ++i)
        ASSERT_TRUE (PlaceTimePanel (layout, size, 5, i, 2, std::vector<uint8_t> (layout.size (), i + 1)));
    for (unsigned i = 0; i < 6; ++i) {
        auto at = [&] (unsigned x, unsigned y) { return layout[((i / 3 * 8 + y) * 24 + i % 3 * 8 + x) * 4]; };
        EXPECT_EQ (at (4, 3), i < 5 ? i + 1 : 255);
        EXPECT_EQ (at (4, 7), 255);
    }
    EXPECT_EQ (layout.size (), 24 * 16 * 4u);
}

TEST (ViewerScreenshot, SeasonalMatrixKeepsHeadingsGuttersAndMetricOutsideEachImage)
{
    for (ScreenshotSize canvas : { ScreenshotSize { 503, 401 }, ScreenshotSize { 307, 521 } }) {
        std::vector<uint8_t> sheet (size_t (canvas.width) * canvas.height * 4, 255);
        for (unsigned i = 0; i < 15; ++i) {
            const auto tile = SeasonalTileOf (canvas, 5, i, 1.5f);
            const auto b = tile.image;
            ASSERT_GT (b.left, tile.gutter);
            ASSERT_GT (b.top, tile.header);
            ASSERT_LT (b.bottom + tile.gap + tile.caption,
                       tile.header + (canvas.height - tile.header) * (i / 5 + 1) / 3);
            const std::vector<uint8_t> pixels (sheet.size (), uint8_t (i));
            ASSERT_TRUE (PlaceTimePanel (sheet, canvas, 15, i, 0, pixels, { 5, 3 }, b));
            const auto at = [&] (unsigned x, unsigned y) { return sheet[(size_t (y) * canvas.width + x) * 4]; };
            EXPECT_EQ (at (b.left, b.top), i);
            EXPECT_EQ (at (b.right - 1, b.bottom - 1), i);
            EXPECT_EQ (at (b.left - 1, b.top), 255);
            EXPECT_EQ (at (b.left, b.bottom + tile.gap), 255);
        }
    }
}

constexpr ScreenshotSize kView { 800, 600 };
constexpr CaptureDock kDock { { 200, 0, 600, 52 }, kView, DockEdge::Top, true };

TEST (ViewerScreenshot, ConvertsClampsAndRoundsPixelEdges)
{
    const auto size = ViewerScreenshotTargetSize (801, 601, 1.5f);
    EXPECT_EQ (size.width, 1202u);
    EXPECT_EQ (size.height, 902u);
    const auto crop = ViewerScreenshotCrop ({ -0.05f, 0.125f, 0.5f, 1.1f }, size);
    EXPECT_EQ (crop.left, 0u);
    EXPECT_EQ (crop.top, 113u);
    EXPECT_EQ (crop.right, 601u);
    EXPECT_EQ (crop.bottom, 902u);
    EXPECT_EQ (ViewerScreenshotCrop ({ 0.6f, 0.2f, 0.4f, 0.8f }, size).Width (), 0u);
    EXPECT_EQ (ViewerScreenshotCrop ({ 0.2f, 0.2f, 0.20001f, 0.8f }, size).Width (), 0u);
    EXPECT_EQ (ViewerScreenshotCrop ({ 0, 0, std::numeric_limits<float>::quiet_NaN (), 1 }, size).Width (), 0u);

    const auto limited = ViewerScreenshotTargetSize (9000, 4500, 3.0f);
    EXPECT_EQ (limited.width, kViewerScreenshotMaxDimension);
    EXPECT_EQ (limited.height, kViewerScreenshotMaxDimension / 2);
    EXPECT_EQ (ViewerScreenshotTargetSize (0, 601, 2.0f).height, 0u);
}

TEST (ViewerScreenshot, ReadsAsymmetricCropFromTopLeftRgbaRows)
{
    std::vector<uint8_t> pixels;
    for (uint8_t y = 0; y < 4; ++y)
        for (uint8_t x = 0; x < 5; ++x)
            pixels.insert (pixels.end (), { x, y, 90, 255 });
    EXPECT_FALSE (CropScreenshotRgba (pixels, { 5, 4 }, { 2, 1, 6, 3 }));
    ASSERT_TRUE (CropScreenshotRgba (pixels, { 5, 4 }, { 2, 1, 5, 3 }));
    EXPECT_EQ (pixels, (std::vector<uint8_t> { 2, 1, 90, 255, 3, 1, 90, 255, 4, 1, 90, 255,
                                               2, 2, 90, 255, 3, 2, 90, 255, 4, 2, 90, 255 }));
}

TEST (ViewerScreenshot, VisionPathsUseTheNextTimestampCollisionSuffix)
{
    namespace fs = std::filesystem;
    const auto now = std::chrono::steady_clock::now ().time_since_epoch ().count ();
    const fs::path directory = fs::temp_directory_path () / ("tapioca-shot-test-" + std::to_string (now));
    ASSERT_TRUE (fs::create_directory (directory));
    constexpr char stamp[] = "20261122-142205";
    std::error_code error;
    const auto first = ViewerScreenshotPath (directory, kShotVision, 1.5f, stamp, error);
    EXPECT_FALSE (error);
    EXPECT_EQ (first.filename ().string (), "vision-20261122-142205-1.5x.png");
    std::ofstream (first).put ('1');
    const auto second = ViewerScreenshotPath (directory, kShotVision, 1.5f, stamp, error);
    EXPECT_FALSE (error);
    EXPECT_EQ (second.filename ().string (), "vision-20261122-142205-1.5x-2.png");
    std::ofstream (second).put ('2');
    EXPECT_EQ (ViewerScreenshotPath (directory, kShotVision, 1.5f, stamp, error).filename ().string (),
               "vision-20261122-142205-1.5x-3.png");
    EXPECT_EQ (ViewerScreenshotPath (directory, kShotExposedSurfaces, 2.0f, stamp, error).filename ().string (),
               "exposed-surfaces-20261122-142205-2x.png");
    EXPECT_EQ (ViewerScreenshotPath (directory, kShotSunStudy, 3.0f, stamp, error).filename ().string (),
               "sunstudy-20261122-142205-3x.png");
    fs::remove (first);
    fs::remove (second);
    fs::remove (directory);
}

TEST (ViewerScreenshot, DefaultsExcludeTheDockAtEveryEdgeAndAfterResize)
{
    EXPECT_FLOAT_EQ (DefaultCaptureRect (kDock, kView, 8).top, 60);
    EXPECT_FLOAT_EQ (DefaultCaptureRect (kDock, { 1600, 1200 }, 8).top, 112);
    EXPECT_FLOAT_EQ (DefaultCaptureRect ({ { 200, 555, 600, 600 }, kView, DockEdge::Bottom, true }, kView, 8).bottom,
                     547);
    EXPECT_FLOAT_EQ (DefaultCaptureRect ({ { 0, 85, 60, 400 }, kView, DockEdge::Left, false }, kView, 8).left, 68);
    EXPECT_FLOAT_EQ (DefaultCaptureRect ({ { 745, 85, 800, 400 }, kView, DockEdge::Right, false }, kView, 8).right,
                     737);
    EXPECT_FLOAT_EQ (DefaultCaptureRect ({ { 500, 168, 552, 400 }, kView, DockEdge::Floating, false }, kView, 8).right,
                     492);
}

TEST (ViewerScreenshot, AllEdgesAndCornersRespectBoundsAndMinimum)
{
    constexpr ScreenshotRect start { 100, 150, 500, 450 };
    constexpr float minimum = 48;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (!x && !y)
                continue;
            const auto smaller = EditCaptureRect (start, kView, -1000.0f * x, -1000.0f * y, x, y, minimum, 0);
            EXPECT_FLOAT_EQ (smaller.Width (), x ? minimum : start.Width ());
            EXPECT_FLOAT_EQ (smaller.Height (), y ? minimum : start.Height ());
            const auto larger = EditCaptureRect (start, kView, 1000.0f * x, 1000.0f * y, x, y, minimum, 0);
            EXPECT_FLOAT_EQ (larger.left, x < 0 ? 0.0f : start.left);
            EXPECT_FLOAT_EQ (larger.right, x > 0 ? float (kView.width) : start.right);
            EXPECT_FLOAT_EQ (larger.top, y < 0 ? 0.0f : start.top);
            EXPECT_FLOAT_EQ (larger.bottom, y > 0 ? float (kView.height) : start.bottom);
        }
    }
    const auto moved = EditCaptureRect (start, kView, -1000, 1000, 0, 0, minimum, 0);
    EXPECT_FLOAT_EQ (moved.left, 0);
    EXPECT_FLOAT_EQ (moved.bottom, float (kView.height));
    EXPECT_FLOAT_EQ (moved.Width (), start.Width ());
    EXPECT_FLOAT_EQ (moved.Height (), start.Height ());
}

TEST (ViewerScreenshot, AspectSelectionAndResizeKeepTheChosenRatio)
{
    constexpr ScreenshotRect start { 100, 150, 500, 450 };
    for (const float aspect : { 16.0f / 9.0f, 4.0f / 3.0f, 1.0f, float (kView.width) / kView.height }) {
        const auto fitted = FitCaptureAspect (start, kView, 48, aspect);
        EXPECT_NEAR (fitted.Width () / fitted.Height (), aspect, 0.00001f);
        EXPECT_FLOAT_EQ (fitted.left + fitted.right, start.left + start.right);
        EXPECT_FLOAT_EQ (fitted.top + fitted.bottom, start.top + start.bottom);
        const auto edge = EditCaptureRect (fitted, kView, 500, 0, 1, 0, 48, aspect);
        EXPECT_NEAR (edge.Width () / edge.Height (), aspect, 0.00001f);
        EXPECT_FLOAT_EQ (edge.top + edge.bottom, fitted.top + fitted.bottom);
        const auto corner = EditCaptureRect (fitted, kView, 25, 50, -1, -1, 48, aspect);
        EXPECT_NEAR (corner.Width () / corner.Height (), aspect, 0.00001f);
        EXPECT_FLOAT_EQ (corner.right, fitted.right);
        EXPECT_FLOAT_EQ (corner.bottom, fitted.bottom);
    }
    const auto small = EditCaptureRect ({ 0, 41, 32, 59 }, { 40, 100 }, 50, 0, 1, 0, 48, 16.0f / 9.0f);
    EXPECT_NEAR (small.Width () / small.Height (), 16.0f / 9.0f, 0.00001f);
    EXPECT_GE (small.top, 0.0f);
    EXPECT_LE (small.right, 40.0f);
}

class CaptureGui : public testing::Test {
  protected:
    ImGuiContext* previous_ = ImGui::GetCurrentContext ();
    ImGuiContext* context_ = ImGui::CreateContext ();
    HudState state_;
    ImVec2 entry_;
    ImVec2 capture_;
    ImVec2 cancel_;
    bool ownsMouse_ = false;
    bool sunRow_ = false;
    SunStudyOverlayStatus shown_;

    void SetUp () override
    {
        auto& io = ImGui::GetIO ();
        io.IniFilename = nullptr;
        io.DisplaySize = { float (kView.width), float (kView.height) };
        io.DeltaTime = 1.0f / 60;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32 (&pixels, &width, &height);
        Frame ();
        Frame ();
    }

    void TearDown () override
    {
        ImGui::DestroyContext (context_);
        ImGui::SetCurrentContext (previous_);
    }

    void Frame (ImVec2 point = { -1, -1 }, bool down = false, std::string* labels = nullptr)
    {
        ImGuiIO& io = ImGui::GetIO ();
        io.AddMousePosEvent (point.x, point.y);
        io.AddMouseButtonEvent (0, down);
        ImGui::NewFrame ();
        if (state_.captureFrame.active) {
            if (labels)
                ImGui::LogToBuffer ();
            ownsMouse_ = DrawCaptureFrame (state_, kDock, kView, 1.0f, &shown_);
            if (labels) {
                *labels = ImGui::GetCurrentContext ()->LogBuffer.c_str ();
                ImGui::LogFinish ();
            }
            if (const ImGuiWindow* bar = ImGui::FindWindowByName ("##capture-bar")) {
                const auto& style = ImGui::GetStyle ();
                const float cancelWidth = ImGui::CalcTextSize ("Cancel").x + 2.0f * style.FramePadding.x;
                const float captureWidth = ImGui::CalcTextSize ("Capture").x + 2.0f * style.FramePadding.x;
                const ImVec2 end = bar->DC.CursorPosPrevLine;
                const float y = end.y + (ImGui::GetFontSize () * 0.5f) + style.FramePadding.y;
                cancel_ = { end.x - cancelWidth * 0.5f, y };
                capture_ = { end.x - cancelWidth - style.ItemSpacing.x - captureWidth * 0.5f, y };
            }
        }
        else {
            ImGui::SetNextWindowPos ({ 10, 10 });
            ImGui::SetNextWindowSize ({ 400, 175 });
            ImGui::Begin ("Sun study row");
            DrawCaptureRow (state_, sunRow_ ? kShotSunStudy : kShotVision);
            const auto min = ImGui::GetItemRectMin ();
            const auto max = ImGui::GetItemRectMax ();
            entry_ = { sunRow_ ? ImGui::GetWindowPos ().x + ImGui::GetWindowSize ().x * .25f : (min.x + max.x) * 0.5f,
                       (min.y + max.y) * 0.5f };
            ImGui::End ();
        }
        ImGui::Render ();
    }

    void Click (ImVec2 point)
    {
        Frame (point);
        Frame (point, true);
        Frame (point);
    }

    static bool WindowDrawsColour (const char* name, ImU32 colour)
    {
        const auto* window = ImGui::FindWindowByName (name);
        if (!window || !window->Active)
            return false;
        const auto& vertices = window->DrawList->VtxBuffer;
        return std::any_of (vertices.begin (), vertices.end (),
                            [colour] (const ImDrawVert& vertex) { return vertex.col == colour; });
    }
};

TEST_F (CaptureGui, CaptureWindowsUseLightHudColoursEvenOverTheDarkBaseStyle)
{
    const auto& look = hudshell::PlainLook ();
    const auto background = hudshell::Packed (look.backgroundRgba);
    const auto neutralHover = hudshell::Packed (hudshell::WithAlpha (0x4178CBFF, 0.30f));
    const auto baseBackground = ImGui::GetColorU32 (ImGuiCol_WindowBg);
    ASSERT_NE (baseBackground, background);
    Click (entry_);
    ASSERT_TRUE (state_.captureFrame.active);
    Frame ();
    Frame ();
    EXPECT_TRUE (WindowDrawsColour ("##capture-bar", background));
    Frame (capture_);
    EXPECT_TRUE (WindowDrawsColour ("##capture-bar", neutralHover));
    EXPECT_EQ (ImGui::GetColorU32 (ImGuiCol_WindowBg), baseBackground);
}

TEST_F (CaptureGui, SeasonalColumnsRetainUnmeasuredClocksAndCanStartWithoutAnExistingDay)
{
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotSunStudy));
    auto& f = state_.captureFrame;
    f.builder = f.studyDay = f.seasonal = true;
    f.shadowStyle = 1;
    shown_.stepMinutes = { 660, 780 };
    Frame ();
    Frame ();
    EXPECT_EQ (f.clocks, (std::vector<int> { 540, 660, 780, 900 }));
    EXPECT_EQ (f.panelSteps, (std::vector<int> { -1, 0, 1, -1 }));
    f.startMinute = 600;
    f.endMinute = 720;
    f.divisions = 4;
    Frame ();
    EXPECT_EQ (f.clocks, (std::vector<int> { 600, 630, 660, 690, 720 }));
    Click (capture_);
    EXPECT_TRUE (state_.sunShotRequested);
}

TEST_F (CaptureGui, EntryMovesFrameAndCancelKeepsPreviousResult)
{
    state_.sunShotResult = "prior.png";
    Click (entry_);
    ASSERT_TRUE (state_.captureFrame.active);
    Frame ();
    Frame ();
    EXPECT_FLOAT_EQ (state_.captureFrame.rect.top, 60.0f / kView.height);
    Frame ({ 350, 300 }, true);
    EXPECT_TRUE (ownsMouse_);
    Frame ({ 356, 280 }, true);
    Frame ({ 356, 280 });
    EXPECT_NEAR (state_.captureFrame.rect.left * kView.width, 14, 0.001f);
    EXPECT_NEAR (state_.captureFrame.rect.top * kView.height, 40, 0.001f);

    Frame ({ 798, 40 }, true);
    Frame ({ 738, 60 }, true);
    Frame ({ 738, 60 });
    EXPECT_NEAR (state_.captureFrame.rect.right * kView.width, 738, 0.001f);
    EXPECT_NEAR (state_.captureFrame.rect.top * kView.height, 60, 0.001f);
    Click (cancel_);
    EXPECT_FALSE (state_.captureFrame.active);
    EXPECT_TRUE (state_.captureFrame.cancelled);
    EXPECT_FALSE (state_.sunShotRequested);
    EXPECT_EQ (state_.sunShotResult, "prior.png");
}

TEST_F (CaptureGui, SunStudyRowFramesBeforeCaptureAndBusyBlocksAnotherShot)
{
    sunRow_ = true;
    Frame ();
    Frame ();
    Click (entry_);
    ASSERT_TRUE (state_.captureFrame.active);
    EXPECT_EQ (state_.captureFrame.kind, kShotSunStudy);
    EXPECT_FALSE (state_.sunShotRequested);
    Frame ();
    Frame ();
    Click (capture_);
    EXPECT_FALSE (state_.captureFrame.active);
    EXPECT_TRUE (state_.sunShotRequested);
    EXPECT_EQ (state_.shotKind, kShotSunStudy);
    EXPECT_FALSE (BeginCaptureFrame (state_, kShotExposedSurfaces));
    state_.sunShotRequested = false;
    state_.sunShotBusy = true;
    sunRow_ = false;
    Frame ();
    Frame ();
    Click (entry_);
    EXPECT_FALSE (state_.captureFrame.active);
    EXPECT_FALSE (state_.sunShotRequested);
}

TEST_F (CaptureGui, ScreenshotKeepsActiveShadowViewAndNeverSubmitsAStudy)
{
    tapioca::test::ResetSunStudyHudServices ();
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotSunStudy));
    state_.sunView = 2;
    state_.sunStep = 4;
    auto& status = tapioca::test::SunStudyTestStatus ();
    status.running = true;
    status.dayDate = "2026-03-21";
    Frame ();
    Frame ();
    Click (capture_);
    EXPECT_TRUE (state_.sunShotRequested);
    EXPECT_FALSE (state_.captureFrame.studyDay);
    EXPECT_EQ (SunStudyViewOf (state_).step, 4u);
    EXPECT_EQ (SunStudyViewOf (state_).viewOverride, 5);
    EXPECT_EQ (tapioca::test::SunStudyTestSubmissions (), 0);
    tapioca::test::ResetSunStudyHudServices ();
}

TEST_F (CaptureGui, BuilderChangesQuantityButKeepsAnalysisColoursAndBands)
{
    state_.sunGradientStepped = true;
    state_.sunSingleShadowedRgba = 0x9933aaff;
    state_.sunAmPmEveningRgba = 0xc7aa34ff;
    const auto original = SunStudyViewOf (state_);
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotSunStudy));
    state_.captureFrame.builder = state_.captureFrame.studyDay = true;
    for (int i = 0; i < 3; ++i) {
        state_.captureFrame.reportType = i;
        const auto view = SunStudyViewOf (state_);
        EXPECT_EQ (view.viewOverride, i == 0 ? 5 : i == 1 ? 9 : 0);
        state_.captureFrame.active = false;
        state_.sunShotBusy = true;
        EXPECT_EQ (SunStudyViewOf (state_).viewOverride, view.viewOverride);
        state_.captureFrame.active = true;
        state_.sunShotBusy = false;
        EXPECT_EQ (view.singleShadowedRgba, original.singleShadowedRgba);
        EXPECT_EQ (view.amPmEveningRgba, original.amPmEveningRgba);
        EXPECT_EQ (view.bandCount, original.bandCount);
        ASSERT_EQ (view.rampCount, original.rampCount);
        for (int j = 0; j < view.rampCount; ++j)
            EXPECT_EQ (view.ramp[j].rgba, original.ramp[j].rgba);
    }
}

TEST_F (CaptureGui, PaperShowsOnlyTheControlsForItsQuantity)
{
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotSunStudy));
    auto& f = state_.captureFrame;
    f.builder = f.studyDay = f.seasonal = true;
    for (int workflow = 0; workflow < 3; ++workflow) {
        f.reportType = workflow;
        Frame ();
        std::string labels;
        Frame ({ -1, -1 }, false, &labels);
        const auto has = [&] (const char* s) { return labels.find (s) != std::string::npos; };
        EXPECT_EQ (has ("Stepped gradient"), workflow == 2) << labels;
        EXPECT_EQ (has ("PM only"), workflow == 1) << labels;
        EXPECT_EQ (has ("Shaded"), workflow == 0) << labels;
    }
}

TEST_F (CaptureGui, CameraEditingReleasesOnlyThePaperTileForNavigation)
{
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotSunStudy));
    auto& f = state_.captureFrame;
    f.builder = f.editCamera = true;
    f.paperTiles = { { .25f, .25f, .5f, .5f } };
    Frame ();
    const auto* bar = ImGui::FindWindowByName ("##capture-bar");
    ASSERT_NE (bar, nullptr);
    const ImVec2 middle { (bar->Size.x + kView.width) * .5f, kView.height * .5f };
    Frame (middle);
    EXPECT_FALSE (ownsMouse_);
    EXPECT_GT (f.cameraTileSource.Width (), 0.f);
    Frame ({ float (kView.width) - 2, 2 });
    EXPECT_TRUE (ownsMouse_);
    Frame ({ 10, 10 });
    EXPECT_TRUE (ownsMouse_);
    EXPECT_TRUE (f.active);
}

TEST_F (CaptureGui, ATopViewCaptureCannotReplaceThePerspectiveRegion)
{
    state_.captureRegions[0] = { .1f, .2f, .6f, .8f };
    state_.captureTopView = true;
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotVision));
    state_.captureFrame.rect = { .2f, .1f, .7f, .9f };
    Frame ();
    Frame ();
    Click (capture_);
    ASSERT_TRUE (state_.sunShotRequested);
    state_.sunShotRequested = false;
    state_.captureTopView = false;
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotVision));
    EXPECT_FLOAT_EQ (state_.captureFrame.rect.left, .1f);
    state_.captureFrame.active = false;
    state_.captureTopView = true;
    ASSERT_TRUE (BeginCaptureFrame (state_, kShotVision));
    EXPECT_FLOAT_EQ (state_.captureFrame.rect.left, .2f);
}

} // namespace

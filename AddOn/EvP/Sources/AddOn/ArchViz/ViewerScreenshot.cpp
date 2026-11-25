// The live viewer's screenshot: see ViewerScreenshot.hpp for why it borrows the
// frame loop rather than the headless capture.
//
// ⚠️ ITS OWN TRANSLATION UNIT because DiligentViewport.cpp is a frozen-size
// exception: what stays there is the ORDER (which target the frame draws into,
// where the legend replaces the HUD, which frames are not presented); what a
// screenshot does with the pixels lives here, like DiligentCapture.cpp.
//
// RENDER THREAD. It never touches ACAPI or DG, which is what lets it write the
// file directly.

#include "ArchViz/ViewerScreenshot.hpp"

#include "ArchViz/ArchVizLog.hpp"
#include "ArchViz/Camera.hpp"
#include "ArchViz/ViewPresets.hpp"
#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentNativeInterop.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/SunStudyOverlay.hpp"
#include "ArchViz/ShadowCaptureArea.hpp"
#include "ArchViz/LayoutReportStudy.hpp"
#include "ArchViz/LayoutPageCamera.hpp"
#include "ArchViz/ReportPaperPreview.hpp"
#include "ArchViz/SurfaceTakeoffShot.hpp"
#include "ArchViz/ViewerEditor.hpp"
#include "Geometry/MeshStore.hpp"
#include "Geometry/SurfaceTakeoffController.hpp"
#include "SunStudy/SunStudyStore.hpp"

#include <windows.h>
#include <DeviceContext.h>
#include <GraphicsTypes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <vector>

namespace geomsrv::archviz {

namespace {

// UTF-8 for the log and the HUD. ⚠️ NOT path::u8string, which is
// std::u8string under C++20 and concatenates with nothing here.
std::string Utf8 (const std::filesystem::path& path)
{
    const std::u8string text = path.u8string ();
    return std::string (text.begin (), text.end ());
}

static std::filesystem::path RegionFile (int slot)
{
    const wchar_t* root = _wgetenv (L"LOCALAPPDATA");
    if (!root || !*root || slot < 0 || slot > 2)
        return {};
    const wchar_t* names[] = { L"perspective.rect", L"parallel.rect", L"top.rect" };
    return std::filesystem::path (root) / L"Tapioca" / L"Capture" / names[slot];
}

// %LOCALAPPDATA%\Tapioca\Export, created if missing: a screenshot is a
// deliverable, not a diagnostic, so it does not go in the logs.
bool ExportDirectory (std::filesystem::path& out, std::string& error)
{
    const wchar_t* root = _wgetenv (L"LOCALAPPDATA");
    if (root == nullptr || *root == 0) {
        error = "LOCALAPPDATA is unavailable";
        return false;
    }
    out = std::filesystem::path (root) / L"Tapioca" / L"Export";
    std::error_code code;
    std::filesystem::create_directories (out, code);
    if (code) {
        error = "could not create " + Utf8 (out) + ": " + code.message ();
        return false;
    }
    return true;
}

std::filesystem::path ShotPath (const std::filesystem::path& directory, int kind, float scale, std::error_code& error)
{
    const std::time_t now = std::chrono::system_clock::to_time_t (std::chrono::system_clock::now ());
    std::tm local = {};
    localtime_s (&local, &now);
    char stamp[32] = {};
    std::strftime (stamp, sizeof (stamp), "%Y%m%d-%H%M%S", &local);
    return ViewerScreenshotPath (directory, kind, scale, stamp, error);
}

// Exposed surfaces: is the published highlight still the one the table was
// pinned from? An unknown controller state (its lock held) is not a change.
bool ExposedShotCurrent (const surfacetakeoff::ShotTable& table)
{
    const auto highlight = vieweredit::ReadSurfaceHighlight ();
    if (!highlight || !highlight->capture || !MeshStore::Get ().IsCurrentCapture (highlight->capture, true) ||
        !surfacetakeoff::ShotMatches (table, highlight->summary, highlight->capture->id))
        return false;
    const auto view = geomsrv::surfacetakeoff::SharedSurfaceTakeoffController ().TryPoll ();
    return !view ||
           (view->state == geomsrv::surfacetakeoff::ControllerState::Current && view->generation == table.generation);
}

void Fail (HudState& state, const std::string& reason)
{
    if (state.captureFrame.exportPage >= 0) {
        state.captureFrame.SelectWorkflow (state.captureFrame.returnPage);
        state.captureFrame.exportPage = -1;
    }
    if (state.captureFrame.builder || state.captureFrame.previewOnly) {
        state.captureFrame.previewOnly = false;
        state.captureFrame.active = true;
    }
    state.sunShotFailed = true;
    state.sunShotBusy = false;
    state.sunShotResult = reason;
    ArchVizLog ("viewer screenshot: not saved -- " + reason);
}

bool SaveImage (const std::vector<uint8_t>& image, uint32_t width, uint32_t height, int kind, float scale,
                std::filesystem::path& path, std::string& error)
{
    std::string png;
    std::filesystem::path directory;
    if (!EncodeRgbaPng (image, width, height, png, error) || !ExportDirectory (directory, error))
        return false;
    std::error_code ec;
    if (path.empty ())
        path = ShotPath (directory, kind, scale, ec);
    if (ec) {
        error = "could not choose an export filename: " + ec.message ();
        return false;
    }
    std::ofstream file (path, std::ios::binary);
    file.write (png.data (), static_cast<std::streamsize> (png.size ()));
    file.close ();
    if (!file) {
        error = "could not write " + Utf8 (path);
        return false;
    }
    return true;
}

// Use the same per-sample lit mask as the single-shadow shader. Padding and
// duplicated border texels have no receiver area and never enter the sum.
std::string ShadeMetric (const viewerhud::CaptureFrame::Season& day, int step, double& percent)
{
    const auto& rec = day.record;
    const auto& payload = *day.payload;
    const size_t plane = size_t (payload.width) * payload.height;
    if (!rec || !plane || step < 0 || uint32_t (step) >= payload.stepCount || !payload.stepMasks ||
        payload.stepMasks->size () / plane <= size_t (step / 32))
        return "No area data";
    const bool patch = rec->IsPatchDomain ();
    const auto& areas = patch ? rec->patchGrid.areas : rec->sampleGrid.areas;
    if (areas.size () != rec->positions.size () / 3 || (!patch && areas.size () != rec->atlas.texels.size ()))
        return "No area data";
    const auto area = MeasureShadowArea (areas, *payload.stepMasks, plane, unsigned (step), [&] (size_t i) {
        return patch ? rec->patchAtlas.TexelOf (rec->patchGrid, i) : int64_t (rec->atlas.texels[i]);
    });
    if (!area.valid || area.measured <= 0)
        return "No measured area";
    char text[96];
    percent = 100 * area.shaded / area.measured;
    std::snprintf (text, sizeof (text), "%.1f%% shaded", percent);
    if (area.missing > 0)
        std::snprintf (text, sizeof (text), "%.1f%% shaded | %.1f m2 missing", 100 * area.shaded / area.measured,
                       area.missing);
    return text;
}

int ClockStep (const SunStudyAtlasUpload& p, int minute)
{
    const auto it = std::find (p.stepMinutes.begin (), p.stepMinutes.end (), minute);
    return it == p.stepMinutes.end () ? -1 : int (it - p.stepMinutes.begin ());
}

void DescribeSeason (ReportPanel& panel, const viewerhud::CaptureFrame::Season& day, ReportTemplate type, int step,
                     double& percent)
{
    const auto& p = *day.payload;
    if (!p.stepMinutes.empty ()) {
        int a = p.stepMinutes.front (), b = p.stepMinutes.back ();
        char text[64];
        std::snprintf (text, sizeof (text), "Measured %02d:%02d - %02d:%02d", a / 60, a % 60, b / 60, b % 60);
        panel.window = text;
    }
    if (type == ReportTemplate::Analysis) {
        panel.metric = step < 0 ? "Not measured" : ShadeMetric (day, step, percent);
        return;
    }
    panel.metric = "No measured area";
    if (const auto& rec = day.record) {
        bool patch = rec->IsPatchDomain ();
        const auto& areas = patch ? rec->patchGrid.areas : rec->sampleGrid.areas;
        if (patch || rec->atlas.texels.size () == areas.size ())
            panel.metric = SeasonalMetric (
                p, areas,
                [&] (size_t i) {
                    return patch ? rec->patchAtlas.TexelOf (rec->patchGrid, i) : int64_t (rec->atlas.texels[i]);
                },
                type);
    }
}

void StartSeason (HudState& state, size_t row, bool south)
{
    auto& f = state.captureFrame;
    f.month = row == 0 ? 3 : (row == 1) != south ? 6 : 12;
    f.day = row == 0 ? 20 : 21; // same seasonal dates as the Sun Path arcs
    f.requestedRun = 0;
    if (f.seasons[row].payload)
        return;
    // The form is restored immediately. All columns are on the five-minute
    // clock; trace the inclusive last clock even at a whole hour.
    const int from = state.sunRunHourFrom, to = state.sunRunHourTo, step = state.sunRunTimestep;
    state.sunRunHourFrom = 0;
    state.sunRunHourTo = 24;
    state.sunRunTimestep = 0;
    RunSunStudyDay (state, f.month, f.day, &f.requestedRun);
    state.sunRunHourFrom = from;
    state.sunRunHourTo = to;
    state.sunRunTimestep = step;
    f.progressText = "Preparing seasonal study " + std::to_string (row + 1) + " / 3";
}

} // namespace

ViewerScreenshot::ViewerScreenshot () = default;
ViewerScreenshot::~ViewerScreenshot () = default;

bool ViewerScreenshot::Active () const
{
    return target_ != nullptr;
}

Camera& ViewerScreenshot::CameraFor (Camera& live)
{
    return Active () && comparisonCamera_ ? *comparisonCamera_ : live;
}

void ViewerScreenshot::ServiceFramingCamera (Camera& camera, DiligentScene& scene, HudState& state,
                                             bool& userHasNavigated)
{
    state.viewpointOwnsMouse = state.captureFrame.active ? false : state.viewpointOwnsMouse;
    state.captureTopView = camera.IsTopDown ();
    auto& frame = state.captureFrame;
    if (frame.builder && (frame.active || state.sunShotRequested) && viewerhud::SwitchPageCamera (frame, camera)) {
        scene.ResetTemporalAntiAliasingHistory ();
    }
    if ((frame.active || state.sunShotRequested) && frame.builder && frame.paperCache && frame.seasons[2].payload) {
        auto& cache = *frame.paperCache;
        std::array<float, 32> matrices {};
        camera.GetViewMatrix (matrices.data ());
        camera.GetProjMatrix (matrices.data () + 16,
                              float ((std::max) (1u, frame.view.width)) / (std::max) (1u, frame.view.height));
        if (cache.camera != matrices) {
            // Keep the measured sheet visible underneath the live tiles. Moving
            // the camera changes no caption, legend or seasonal statistic.
            if (cache.camera != std::array<float, 32> {} && !cache.page.pending && frame.clockImages.empty ()) {
                frame.clockImages.resize (cache.page.panels.size ());
                cache.clockTextures.resize (frame.clockImages.size ());
            }
            cache.camera = matrices;
            cache.tilesToRefresh = frame.clockImages.size ();
            if (frame.editCamera)
                cache.navigationUntil = std::chrono::steady_clock::now () + std::chrono::milliseconds (150);
        }
    }
    if (state.sunShotRequested && state.captureFrame.studyDay)
        comparisonCamera_ = std::make_unique<Camera> (camera);
    else if (!Active ())
        comparisonCamera_.reset ();
    if (!regionsRead_) {
        regionsRead_ = true;
        for (int slot = 0; slot < 3; ++slot) {
            ScreenshotRect r;
            std::ifstream file (RegionFile (slot));
            if (file >> r.left >> r.top >> r.right >> r.bottom && r.left >= 0 && r.top >= 0 && r.right <= 1 &&
                r.bottom <= 1 && r.Width () > 0 && r.Height () > 0)
                state.captureRegions[slot] = r;
        }
    }
    if (state.captureFrame.active || state.captureFrame.previewOnly || frame.exportPage >= 0) {
        if (!framingCamera_) {
            framingCamera_ = std::make_unique<Camera> (camera);
            framingUserNavigated_ = userHasNavigated;
            framingOrbitAnchor_ = state.orbitAnchor;
        }
        return;
    }
    if (state.captureFrame.cancelled && framingCamera_) {
        camera = *framingCamera_;
        userHasNavigated = framingUserNavigated_;
        state.orbitAnchor = framingOrbitAnchor_;
        scene.ResetTemporalAntiAliasingHistory ();
    }
    if (frame.cancelled && frame.builder)
        frame = {};
    state.captureFrame.cancelled = false;
    framingCamera_.reset ();
}

static bool RenderPaperLegend (Diligent::IRenderDevice*, Diligent::IDeviceContext*, DiligentHud&,
                               const DiligentSceneStats&, const HudState&, LayoutReportPage&, std::string&);

DiligentViewportTarget& ViewerScreenshot::TargetFor (DiligentViewportTarget& surface, Diligent::IRenderDevice* device,
                                                     Diligent::IDeviceContext* context, DiligentScene& scene,
                                                     HudState& state)
{
    if (target_ != nullptr)
        return *target_;
    if (state.captureFrame.active && state.captureFrame.builder) {
        auto& f = state.captureFrame;
        const auto stats = scene.Stats ();
        f.measuredDraft = f.seasons[0].payload && f.seasons[1].payload && f.seasons[2].payload &&
                          f.seasonStamp == MeshStore::Get ().CaptureStamp ();
        if (!f.measuredDraft && f.paperCache && f.paperCache->page.pending &&
            (f.paperCache->study != stats.sunStudy.studyId || f.paperCache->version != stats.sunStudy.version))
            f.paperDirty = true;
        viewerhud::UpdateReportPaper (device, f, stats.latitudeDegrees < 0, [&] (LayoutReportPage& page) {
            const auto payload = f.measuredDraft ? f.seasons[0].payload : scene.ShownAnalysisPayload ();
            if (f.measuredDraft && !f.clocks.empty ()) {
                double previous = -1;
                for (size_t i = 0; i < page.panels.size (); ++i) {
                    const auto& day = f.seasons[i / f.clocks.size ()];
                    double percent = -1;
                    DescribeSeason (page.panels[i], day, page.type,
                                    ClockStep (*day.payload, f.clocks[i % f.clocks.size ()]), percent);
                    if (i % f.clocks.size () && percent >= 0 && previous >= 0) {
                        char delta[32];
                        std::snprintf (delta, sizeof (delta), " (%+.1f pp)", percent - previous);
                        page.panels[i].metric += delta;
                    }
                    previous = percent;
                }
            }
            state.captureFrame.legendPlan = payload ? payload->capturePlan : nullptr;
            std::string error;
            // An export frame submitted to the interactive ImGui context would cancel active drags/popups.
            if (!paperLegendHud_)
                paperLegendHud_ = std::make_unique<DiligentHud> ();
            if ((!paperLegendHud_->IsReady () &&
                 !paperLegendHud_->Init (device, surface.ColorFormat (), surface.DepthFormat (), error)) ||
                !RenderPaperLegend (device, context, *paperLegendHud_, stats, state, page, error))
                ArchVizLog ("paper preview legend: " + error);
        });
        f.paperCache->study = stats.sunStudy.studyId;
        f.paperCache->version = stats.sunStudy.version;
        if (f.measuredDraft && !f.clockImages.empty () && !f.clocks.empty ()) {
            f.liveStep = -1;
            if (f.paperCache->tilesToRefresh) {
                if (f.editCamera && std::chrono::steady_clock::now () < f.paperCache->navigationUntil)
                    f.liveColumn = (std::min) (f.navigationTile, f.clockImages.size () - 1);
                else {
                    --f.paperCache->tilesToRefresh;
                    f.liveColumn = (f.liveColumn + 1) % f.clockImages.size ();
                }
                const auto& p = *f.seasons[f.liveColumn / f.clocks.size ()].payload;
                const bool switched = scene.ShownSunStudyId () != p.studyId;
                f.liveStep = scene.RestoreAnalysisPayload (device, context, p)
                                 ? (f.reportType == 0 ? ClockStep (p, f.clocks[f.liveColumn % f.clocks.size ()]) : 0)
                                 : -1;
                const int previous = f.shadowStep;
                f.shadowStep = (std::max) (0, f.liveStep);
                state.sunStepMinutes = p.stepMinutes;
                if (switched || previous != f.shadowStep)
                    scene.ResetTemporalAntiAliasingHistory ();
            }
        }
        else if (f.paperCache->page.pending && f.reportType == int (ReportTemplate::Analysis) &&
                 !f.panelSteps.empty ()) {
            f.liveColumn = (f.liveColumn + 1) % f.panelSteps.size ();
            f.shadowStep = (std::max) (0, f.panelSteps[f.liveColumn]);
        }
    }
    if (!state.sunShotRequested)
        return surface;
    state.sunShotRequested = false;
    kind_ = state.shotKind;
    if (kind_ == kShotSunStudy) {
        const auto payload = scene.ShownAnalysisPayload ();
        state.captureFrame.legendPlan = payload ? payload->capturePlan : nullptr;
    }
    table_ = kind_ == kShotExposedSurfaces ? state.exposedShot : nullptr;
    state.exposedShot.reset ();
    if (kind_ == kShotExposedSurfaces && (table_ == nullptr || !ExposedShotCurrent (*table_))) {
        table_.reset ();
        Fail (state, "the calculation changed; take the screenshot again");
        return surface;
    }
    if (surface.Mode () != SurfaceMode::PaletteChild ||
        surface.ColorFormat () != uint32_t (Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB)) {
        Fail (state, "only the palette viewer can take one, not the overlay");
        return surface;
    }
    const int preset = std::clamp (state.sunShotPreset, 0, kViewerScreenshotScaleCount - 1);
    scale_ = kViewerScreenshotScales[preset];
    auto& paper = state.captureFrame;
    if (paper.builder && paper.seasons[0].payload && paper.seasonStamp != MeshStore::Get ().CaptureStamp ()) {
        paper.active = true;
        Fail (state, "Model changed; reopen Layout Builder for a new seasonal study");
        return surface;
    }
    const ScreenshotSize size = ViewerScreenshotTargetSize (surface.Width (), surface.Height (), scale_);
    crop_ = ViewerScreenshotCrop (state.captureFrame.rect, size);
    if (!crop_.Width () || !crop_.Height ()) {
        Fail (state, "the capture rectangle has no pixels");
        return surface;
    }
    auto target = std::make_unique<DiligentViewportTarget> ();
    const auto& frame = state.captureFrame;
    reporting_ = kind_ == kShotSunStudy && frame.builder;
    if (reporting_) {
        report_ = {};
        report_.format = frame.pageFormat;
        report_.type = ReportTemplate (frame.reportType);
        report_.clocks = frame.clocks;
        report_.tileAspect = frame.tileAspect;
        panelIndex_ = 0;
        state.captureFrame.shadowStep = (std::max) (0, frame.panelSteps.front ());
        state.captureFrame.captions.clear ();
        state.captureFrame.progress = 0;
        state.captureFrame.progressText = "Preparing shadow analysis";
        progressSurface_ = &surface;
        sourceStamp_ = MeshStore::Get ().CaptureStamp ();
    }
    std::string error;
    if (!target->Create (device, context, SurfaceMode::Offscreen, nullptr, size.width, size.height, error)) {
        target->Destroy (context);
        report_ = {};
        reporting_ = false;
        Fail (state, error.empty () ? "the viewer has no size yet" : error);
        return surface;
    }
    target_ = std::move (target);
    device_ = device;
    frames_ = 0;
    state.sunShotBusy = true;
    if (reporting_ && frame.seasonal) {
        waitingDay_ = true;
        StartSeason (state, 0, scene.Stats ().latitudeDegrees < 0);
    }
    // The window's history is the wrong size and, at worst, the wrong pixels.
    scene.ResetTemporalAntiAliasingHistory ();
    ArchVizLog ("viewer screenshot: rendering " + std::to_string (size.width) + "x" + std::to_string (size.height));
    return *target_;
}

bool ViewerScreenshot::HidesSurfaceFocus (const HudState& state)
{
    return state.ScreenshotCleanView () && state.ScreenshotKind () != kShotExposedSurfaces;
}

float ViewerScreenshot::EditorDrawScale (const HudState& state) const
{
    return Active () ? scale_ : state.captureFrame.active ? 1.0f : 0.0f;
}

uint32_t ViewerScreenshot::FrameIndex (uint32_t presented) const
{
    return presented + uint32_t (frames_);
}

// The legend as its own white strip, the image's width, read back for stacking
// under the view. ⚠️ ITS OWN TARGET, NOT ROWS RESERVED IN THE VIEW'S: the
// scene draws into the whole of whatever it is given, so a taller target would
// stretch the camera instead of leaving room. `draw` lays out the strip's ImGui frame.
static bool RenderStrip (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, uint32_t width,
                         uint32_t height, const std::function<void ()>& draw, std::vector<uint8_t>& rgba,
                         std::string& error)
{
    DiligentViewportTarget strip;
    if (!strip.Create (device, context, SurfaceMode::Offscreen, nullptr, width, height, error))
        return false;
    Diligent::ITextureView* rtv = nullptr;
    Diligent::ITextureView* dsv = nullptr;
    bool ok = strip.BeginFrame (rtv, dsv);
    if (ok) {
        constexpr float kWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->ClearRenderTarget (rtv, kWhite, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        draw ();
        context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
        ok = strip.CaptureRgba (context, rgba, error);
    }
    else
        error = "the legend strip returned no render target";
    strip.Destroy (context);
    return ok;
}

static bool RenderPaperLegend (Diligent::IRenderDevice* device, Diligent::IDeviceContext* context, DiligentHud& hud,
                               const DiligentSceneStats& stats, const HudState& state, LayoutReportPage& report,
                               std::string& error)
{
    const auto page = ReportPageFormat (report.format);
    const float scale = page.width / 1500.f;
    const auto height = hud.CaptureLegendHeight (scale);
    if (!RenderStrip (
            device, context, page.width, height,
            [&] { hud.DrawCaptureLegend (context, page.width, height, scale, stats, state); }, report.legend, error))
        return false;
    report.legendHeight = height;
    return true;
}

bool ViewerScreenshot::Finish (Diligent::IDeviceContext* context, DiligentScene& scene, DiligentHud& hud,
                               HudState& state)
{
    if (target_ == nullptr)
        return false;
    EndUnpresentedFrame (context); // a screenshot frame is never presented, so it ends (and is paced) here
    auto& f = state.captureFrame;
    if (reporting_) {
        const size_t columns = f.panelSteps.size (), total = columns * (f.seasonal ? 3 : 1);
        const auto abort = [&] (const std::string& why) {
            if (waitingDay_)
                viewerhud::CancelStudyDay (f);
            Fail (state, why);
            Destroy (context);
            scene.ResetTemporalAntiAliasingHistory ();
            return true;
        };
        if (sourceStamp_ != MeshStore::Get ().CaptureStamp ())
            return abort ("model changed during shadow comparison");
        if (waitingDay_) {
            auto& day = f.seasons[panelIndex_ / columns];
            if (day.payload && !scene.RestoreAnalysisPayload (device_, context, *day.payload))
                return abort ("Could not display the retained seasonal study");
            const auto& shown = scene.Stats ().sunStudy;
            std::string detail;
            const auto phase =
                day.payload ? viewerhud::StudyDayState::Ready : viewerhud::PollStudyDay (state, shown, detail);
            if (phase == viewerhud::StudyDayState::Publishing) {
                const auto now = std::chrono::steady_clock::now ();
                if (publishingSince_ == std::chrono::steady_clock::time_point {})
                    publishingSince_ = now;
                if (now - publishingSince_ > std::chrono::seconds (30))
                    return abort ("seasonal study completed but was not displayed");
            }
            else
                publishingSince_ = {};
            if (phase == viewerhud::StudyDayState::Failed)
                return abort (detail);
            if (phase == viewerhud::StudyDayState::Ready) {
                if (!day.payload) {
                    day.payload = scene.ShownAnalysisPayload ();
                    if (!day.payload)
                        return abort ("Seasonal study has no display payload");
                    day.record = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (shown.studyId);
                    f.seasonStamp = sourceStamp_;
                }
                if (panelIndex_ == 0) {
                    const auto first = scene.ShownAnalysisPayload ();
                    f.legendPlan = first ? first->capturePlan : nullptr;
                }
                f.layoutMinutes = shown.stepMinutes;
                for (size_t i = 0; i < columns; ++i) {
                    auto it = std::find (f.layoutMinutes.begin (), f.layoutMinutes.end (), f.clocks[i]);
                    f.panelSteps[i] = it == f.layoutMinutes.end () ? -1 : int (it - f.layoutMinutes.begin ());
                }
                f.pinnedStudy = shown.studyId;
                f.pinnedVersion = shown.version;
                f.shadowStep = (std::max) (0, f.panelSteps[0]);
                state.sunStepMinutes = f.layoutMinutes;
                waitingDay_ = false;
                frames_ = 0;
                scene.ResetTemporalAntiAliasingHistory ();
                return true;
            }
            f.progressText = "Study " + std::to_string (panelIndex_ / columns + 1) + " / 3 | " + detail;
        }
        else {
            f.progressText = "Rendering panel " + std::to_string (panelIndex_ + 1) + " / " + std::to_string (total);
        }
        f.progress =
            float (panelIndex_ + (!waitingDay_ ? float (frames_) / kViewerScreenshotSettleFrames : 0)) / float (total);
        if (f.exportPage >= 0) {
            f.progressText = std::string (ReportTitle (report_.type)) + " | " + f.progressText;
            f.progress = (f.exportPage + f.progress) / 3;
        }
        if (frames_ % 6 == 0 && progressSurface_) {
            Diligent::ITextureView *rtv = nullptr, *dsv = nullptr;
            if (progressSurface_->BeginFrame (rtv, dsv)) {
                context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                hud.DrawCaptureLegend (context, progressSurface_->Width (), progressSurface_->Height (), 1,
                                       scene.Stats (), state, -2, target_->CapturePreviewView ());
                context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
                progressSurface_->Present ();
            }
        }
        if (waitingDay_) {
            ++frames_;
            return true;
        }
    }
    if (kind_ == kShotSunStudy && !state.captureFrame.builder && state.captureFrame.entryMode >= 0) {
        const auto& study = scene.Stats ().sunStudy;
        if (study.studyId != f.pinnedStudy || study.version != f.pinnedVersion) {
            Fail (state, "the analysis changed after Screenshot was opened; frame the new result");
            Destroy (context);
            scene.ResetTemporalAntiAliasingHistory ();
            return true;
        }
    }
    if (kind_ == kShotSunStudy && state.captureFrame.studyDay) {
        const auto& study = scene.Stats ().sunStudy;
        if (study.studyId != state.captureFrame.pinnedStudy || study.version != state.captureFrame.pinnedVersion ||
            study.analysisKind != 0 || study.preview || !study.elementsAttached) {
            Fail (state, "the selected day's study changed while rendering; take the screenshot again");
            Destroy (context);
            scene.ResetTemporalAntiAliasingHistory ();
            return true;
        }
    }
    // Every frame, not only the last: frames drawn after a change would mix the
    // old table with new geometry, and settling would hide that in the average.
    if (table_ != nullptr && !ExposedShotCurrent (*table_)) {
        Fail (state, "the highlight or calculation changed while rendering; take the screenshot again");
        Destroy (context);
        scene.ResetTemporalAntiAliasingHistory ();
        return true;
    }
    if (++frames_ < kViewerScreenshotSettleFrames)
        return true;

    uint32_t width = crop_.Width ();
    uint32_t viewHeight = crop_.Height ();
    uint32_t stripHeight = table_ != nullptr
                               ? hud.CaptureTableHeight (scale_, table_->rows.size (), table_->footer.size ())
                           : kind_ == kShotSunStudy ? hud.CaptureLegendHeight (scale_)
                                                    : 0;
    std::vector<uint8_t> image;
    std::vector<uint8_t> strip;
    std::string error;
    bool ok = target_->CaptureRgba (context, image, error);
    if (ok && !CropScreenshotRgba (image, { target_->Width (), target_->Height () }, crop_)) {
        ok = false;
        error = "could not crop the captured pixels";
    }
    if (ok && reporting_) {
        auto& frame = state.captureFrame;
        const size_t cols = frame.panelSteps.size (), total = cols * (frame.seasonal ? 3 : 1),
                     column = panelIndex_ % cols, rowIndex = panelIndex_ / cols;
        const int step = frame.panelSteps[column];
        ReportPanel panel;
        panel.season = rowIndex == 0 ? "Equinox" : rowIndex == 1 ? "Summer solstice" : "Winter solstice";
        char date[48];
        std::snprintf (date, sizeof (date), "%02d %s", frame.day,
                       frame.month == 3   ? "March"
                       : frame.month == 6 ? "June"
                                          : "December");
        panel.date = date;
        double percent = -1;
        if (frame.seasons[rowIndex].payload)
            DescribeSeason (panel, frame.seasons[rowIndex], report_.type, step, percent);
        else
            panel.metric = "No data";
        if (report_.type == ReportTemplate::Analysis) {
            if (column && percent >= 0 && previousShaded_ >= 0) {
                char delta[32];
                std::snprintf (delta, sizeof (delta), " (%+.1f pp)", percent - previousShaded_);
                panel.metric += delta;
            }
            previousShaded_ = percent;
        }
        panel.width = width;
        panel.height = viewHeight;
        if (report_.type != ReportTemplate::Analysis || step >= 0)
            StoreReportTile (panel, image);
        report_.panels.push_back (std::move (panel));
        if (ok && ++panelIndex_ < total) {
            if (frame.seasonal && panelIndex_ % cols == 0) {
                StartSeason (state, panelIndex_ / cols, scene.Stats ().latitudeDegrees < 0);
                waitingDay_ = true;
            }
            else
                frame.shadowStep = (std::max) (0, frame.panelSteps[panelIndex_ % cols]);
            frames_ = 0;
            scene.ResetTemporalAntiAliasingHistory ();
            return true;
        }
        const auto page = ReportPageFormat (report_.format);
        ok = RenderPaperLegend (device_, context, hud, scene.Stats (), state, report_, error);
        if (ok && frame.previewOnly) {
            frame.clockImages.clear ();
            frame.liveImage = 0;
            frame.paperCache = std::make_shared<viewerhud::ReportPaperPreview> ();
            frame.paperCache->page = std::move (report_);
            frame.paperDirty = false;
            frame.active = true;
            frame.previewOnly = false;
            state.sunShotBusy = false;
            Destroy (context);
            scene.ResetTemporalAntiAliasingHistory ();
            return true;
        }
        ok = ok && ComposeReportPage (report_, image);
        if (!ok)
            error = "could not compose the analysis page";
        width = page.width;
        viewHeight = page.height;
        stripHeight = 0;
    }
    if (ok && table_ != nullptr) {
        // Drawn at the estimate; drawn again at what the layout needed when names
        // wrapped; then cut to exactly that. Never truncated: too tall is refused.
        uint32_t needed = 0;
        const auto draw = [&] () { needed = hud.DrawCaptureTable (context, width, stripHeight, scale_, *table_); };
        ok = RenderStrip (device_, context, width, stripHeight, draw, strip, error);
        if (ok && needed > stripHeight && viewHeight + needed <= kViewerScreenshotMaxDimension) {
            stripHeight = needed;
            ok = RenderStrip (device_, context, width, stripHeight, draw, strip, error);
        }
        if (ok && (needed == 0 || viewHeight + needed > kViewerScreenshotMaxDimension)) {
            ok = false;
            error = needed == 0
                        ? "the area table could not be drawn"
                        : "the area table has too many rows for one image (" + std::to_string (table_->rows.size ()) +
                              "); filter the materials and take it again";
        }
        if (ok) {
            stripHeight = (std::min) (stripHeight, needed);
            strip.resize (size_t (width) * 4 * stripHeight);
            image.insert (image.end (), strip.begin (), strip.end ());
        }
    }
    else if (ok && stripHeight > kViewerScreenshotMaxDimension - viewHeight) {
        ok = false;
        error = "the image and legend are too tall; reduce the capture scale or height";
    }
    else if (ok && stripHeight > 0) {
        const auto draw = [&] () {
            hud.DrawCaptureLegend (context, width, stripHeight, scale_, scene.Stats (), state);
        };
        ok = RenderStrip (device_, context, width, stripHeight, draw, strip, error);
        image.insert (image.end (), strip.begin (), strip.end ());
    }
    std::filesystem::path path;
    if (ok && reporting_ && f.exportPage >= 0) {
        if (f.exportPage == 0) {
            std::filesystem::path directory;
            ok = ExportDirectory (directory, error);
            if (ok) {
                std::error_code ec;
                const auto stem = ShotPath (directory, kind_, scale_, ec).stem ().wstring ();
                for (unsigned i = 0; !ec; ++i) {
                    exportFolder_ = directory / (stem + L"-layouts" + (i ? L"-" + std::to_wstring (i) : L""));
                    if (std::filesystem::create_directory (exportFolder_, ec))
                        break;
                }
                if (ec) {
                    error = "Could not create layout export folder: " + ec.message ();
                    ok = false;
                }
            }
        }
        path = exportFolder_ /
               (std::string ("0") + std::to_string (f.exportPage + 1) + " - " + ReportTitle (report_.type) + ".png");
    }
    ok = ok && SaveImage (image, width, viewHeight + (strip.empty () ? 0u : stripHeight), kind_, scale_, path, error);
    if (!ok)
        Fail (state, error);
    else {
        state.sunShotFailed = false;
        state.sunShotBusy = false;
        state.sunShotResult = Utf8 (path);
        const auto region = RegionFile (state.captureFrame.viewSlot);
        if (!region.empty ()) {
            std::error_code ec;
            std::filesystem::create_directories (region.parent_path (), ec);
            const auto r = state.captureFrame.rect;
            std::ofstream saved (region);
            saved << r.left << ' ' << r.top << ' ' << r.right << ' ' << r.bottom;
            if (!saved)
                ArchVizLog ("viewer screenshot: could not remember the view's capture region");
        }
        ArchVizLog ("viewer screenshot: saved " + state.sunShotResult);
    }

    if (reporting_) {
        f.active = true;
        if (ok && f.exportPage >= 0) {
            if (f.AdvancePageExport ()) {
                state.sunShotRequested = true;
                f.active = false;
            }
            else {
                state.sunShotResult = "Saved 3 layouts: " + Utf8 (exportFolder_);
            }
        }
        else if (ok) {
            f.clockImages.clear ();
            f.liveImage = 0;
            f.paperCache = std::make_shared<viewerhud::ReportPaperPreview> ();
            f.paperCache->page = std::move (report_);
            f.paperDirty = false;
        }
    }
    Destroy (context);
    // Back to the window: its history is now the screenshot's size.
    scene.ResetTemporalAntiAliasingHistory ();
    return true;
}

void ViewerScreenshot::Destroy (Diligent::IDeviceContext* context)
{
    paperLegendHud_.reset ();
    waitingDay_ = false;
    publishingSince_ = {};
    progressSurface_ = nullptr;
    frames_ = 0;
    report_ = {};
    reporting_ = false;
    table_.reset ();
    if (target_ == nullptr)
        return;
    target_->Destroy (context);
    target_.reset ();
}

} // namespace geomsrv::archviz

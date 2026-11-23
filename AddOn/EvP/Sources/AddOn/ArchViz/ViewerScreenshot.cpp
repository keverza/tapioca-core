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
    state.sunShotFailed = true;
    state.sunShotBusy = false;
    state.sunShotResult = reason;
    ArchVizLog ("viewer screenshot: not saved -- " + reason);
}

// Use the same per-sample lit mask as the single-shadow shader. Padding and
// duplicated border texels have no receiver area and never enter the sum.
std::string ShadeMetric (const SunStudyAtlasUpload& payload, int step, double& percent)
{
    const auto rec = evp::sunstudy::SunStudyStore::Get ().CompletedRecord (payload.studyId);
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

void StartSeason (HudState& state, size_t row, bool south)
{
    auto& f = state.captureFrame;
    f.month = row == 0 ? 3 : (row == 1) != south ? 6 : 12;
    f.day = row == 0 ? 20 : 21; // same seasonal dates as the Sun Path arcs
    f.requestedRun = 0;
    // The form is restored immediately. All columns are on the five-minute
    // clock; trace the inclusive last clock even at a whole hour.
    const int from = state.sunRunHourFrom, to = state.sunRunHourTo, step = state.sunRunTimestep;
    state.sunRunHourFrom = f.startMinute / 60;
    state.sunRunHourTo = f.endMinute / 60 + 1;
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
    if (state.captureFrame.active) {
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
    state.captureFrame.cancelled = false;
    framingCamera_.reset ();
}

DiligentViewportTarget& ViewerScreenshot::TargetFor (DiligentViewportTarget& surface, Diligent::IRenderDevice* device,
                                                     Diligent::IDeviceContext* context, DiligentScene& scene,
                                                     HudState& state)
{
    if (target_ != nullptr)
        return *target_;
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
    const ScreenshotSize size = ViewerScreenshotTargetSize (surface.Width (), surface.Height (), scale_);
    crop_ = ViewerScreenshotCrop (state.captureFrame.rect, size);
    if (!crop_.Width () || !crop_.Height ()) {
        Fail (state, "the capture rectangle has no pixels");
        return surface;
    }
    auto target = std::make_unique<DiligentViewportTarget> ();
    const auto& frame = state.captureFrame;
    if (kind_ == kShotSunStudy && frame.studyDay && frame.shadowStyle == 1) {
        const size_t count = frame.panelSteps.size ();
        const auto grid = frame.seasonal ? ScreenshotSize { uint32_t (count), 3 }
                                         : TimeGrid (count, { crop_.Width (), crop_.Height () });
        if (!grid.width || crop_.Width () / grid.width < 32 || crop_.Height () / grid.height < 16 ||
            uint64_t (crop_.Width ()) * crop_.Height () > 64ull * 1024 * 1024) {
            Fail (state, "the grid does not fit this capture size; adjust the interval, scale or crop");
            return surface;
        }
        panelIndex_ = 0;
        layoutImage_.assign (size_t (crop_.Width ()) * crop_.Height () * 4, 255);
        state.captureFrame.gridHeight = crop_.Height ();
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
        std::vector<uint8_t> ().swap (layoutImage_);
        Fail (state, error.empty () ? "the viewer has no size yet" : error);
        return surface;
    }
    target_ = std::move (target);
    device_ = device;
    frames_ = 0;
    state.sunShotBusy = true;
    if (!layoutImage_.empty () && frame.seasonal) {
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

bool ViewerScreenshot::Finish (Diligent::IDeviceContext* context, DiligentScene& scene, DiligentHud& hud,
                               HudState& state)
{
    if (target_ == nullptr)
        return false;
    EndUnpresentedFrame (context); // a screenshot frame is never presented, so it ends (and is paced) here
    auto& f = state.captureFrame;
    if (!layoutImage_.empty ()) {
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
            const auto& shown = scene.Stats ().sunStudy;
            std::string detail;
            const auto phase = viewerhud::PollStudyDay (state, shown, detail);
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
        if (frames_ % 6 == 0 && progressSurface_) {
            Diligent::ITextureView *rtv = nullptr, *dsv = nullptr;
            if (progressSurface_->BeginFrame (rtv, dsv)) {
                context->SetRenderTargets (1, &rtv, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
                hud.DrawCaptureLegend (context, progressSurface_->Width (), progressSurface_->Height (), 1,
                                       scene.Stats (), state, -2);
                context->SetRenderTargets (0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE);
                progressSurface_->Present ();
            }
        }
        if (waitingDay_) {
            ++frames_;
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
    const uint32_t viewHeight = crop_.Height ();
    uint32_t stripHeight = table_ != nullptr
                               ? hud.CaptureTableHeight (scale_, table_->rows.size (), table_->footer.size ())
                           : kind_ == kShotSunStudy ? hud.CaptureLegendHeight (scale_)
                                                    : 0;
    std::vector<uint8_t> image;
    std::vector<uint8_t> strip;
    std::string png;
    std::string error;
    std::filesystem::path directory;
    bool ok = target_->CaptureRgba (context, image, error);
    if (ok && !CropScreenshotRgba (image, { target_->Width (), target_->Height () }, crop_)) {
        ok = false;
        error = "could not crop the captured pixels";
    }
    if (ok && !layoutImage_.empty ()) {
        auto& frame = state.captureFrame;
        const size_t cols = frame.panelSteps.size (), total = cols * (frame.seasonal ? 3 : 1),
                     column = panelIndex_ % cols, rowIndex = panelIndex_ / cols;
        const auto grid =
            frame.seasonal ? ScreenshotSize { uint32_t (cols), 3 } : TimeGrid (cols, { width, viewHeight });
        const uint32_t caption = std::clamp (stripHeight, 1u, viewHeight / grid.height / 3);
        const int step = frame.panelSteps[column], minute = frame.clocks[column];
        const auto payload = scene.ShownAnalysisPayload ();
        double percent = -1;
        auto metric = step < 0 ? "Not measured" : payload ? ShadeMetric (*payload, step, percent) : "No area data";
        if (column && percent >= 0 && previousShaded_ >= 0) {
            char change[40];
            std::snprintf (change, sizeof (change), " (%+.1f pp)", percent - previousShaded_);
            metric += change;
        }
        previousShaded_ = percent;
        char label[180];
        std::snprintf (label, sizeof (label), "%02d/%02d | %02d:%02d\n%s", frame.month, frame.day, minute / 60,
                       minute % 60, metric.c_str ());
        if (frame.seasonal && !column) {
            char heading[48];
            std::snprintf (heading, sizeof (heading), "%s\n%02d/%02d",
                           rowIndex == 0   ? "Equinox"
                           : rowIndex == 1 ? "Summer solstice"
                                           : "Winter solstice",
                           frame.month, frame.day);
            frame.seasonLabels[rowIndex] = heading;
        }
        frame.captions.emplace_back (frame.seasonal ? metric : label);
        const auto tile = frame.seasonal
                              ? SeasonalTileOf ({ width, viewHeight }, grid.width, unsigned (panelIndex_), scale_)
                              : SeasonalTile {};
        ok = step < 0 ||
             PlaceTimePanel (layoutImage_, { width, viewHeight }, total, panelIndex_, caption, image, grid, tile.image);
        if (!ok)
            error = "invalid time panel dimensions";
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
        image.swap (layoutImage_);
        const auto stats = scene.Stats ();
        for (unsigned row = 0; ok && row < grid.height; ++row) {
            const auto boundary = [&] (unsigned r) {
                return r ? tile.header + (viewHeight - tile.header) * r / 3 : 0u;
            };
            const unsigned top = frame.seasonal ? boundary (row) : (row + 1) * viewHeight / grid.height - caption;
            const unsigned band = frame.seasonal ? boundary (row + 1) - top : caption;
            const auto draw = [&] { hud.DrawCaptureLegend (context, width, band, scale_, stats, state, int (row)); };
            std::vector<uint8_t> labels;
            ok = RenderStrip (device_, context, width, band, draw, labels, error);
            if (ok) {
                if (frame.seasonal)
                    for (unsigned col = 0; col < grid.width; ++col) {
                        const auto box =
                            SeasonalTileOf ({ width, viewHeight }, grid.width, row * grid.width + col, scale_).image;
                        for (unsigned y = box.top; y < box.bottom; ++y)
                            std::copy_n (image.data () + (size_t (y) * width + box.left) * 4, box.Width () * 4,
                                         labels.data () + (size_t (y - top) * width + box.left) * 4);
                    }
                std::copy (labels.begin (), labels.end (), image.begin () + size_t (top) * width * 4);
            }
        }
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
    ok = ok && EncodeRgbaPng (image, width, viewHeight + (strip.empty () ? 0u : stripHeight), png, error) &&
         ExportDirectory (directory, error);
    std::filesystem::path path;
    if (ok) {
        std::error_code pathError;
        path = ShotPath (directory, kind_, scale_, pathError);
        if (pathError) {
            ok = false;
            error = "could not choose an export filename: " + pathError.message ();
        }
    }
    if (!ok)
        Fail (state, error);
    else {
        std::ofstream file (path, std::ios::binary);
        file.write (png.data (), static_cast<std::streamsize> (png.size ()));
        file.close ();
        if (!file)
            Fail (state, "could not write " + Utf8 (path));
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
            ArchVizLog ("viewer screenshot: saved " + state.sunShotResult + " (" + std::to_string (png.size ()) +
                        " bytes)");
        }
    }

    Destroy (context);
    // Back to the window: its history is now the screenshot's size.
    scene.ResetTemporalAntiAliasingHistory ();
    return true;
}

void ViewerScreenshot::Destroy (Diligent::IDeviceContext* context)
{
    waitingDay_ = false;
    publishingSince_ = {};
    progressSurface_ = nullptr;
    frames_ = 0;
    std::vector<uint8_t> ().swap (layoutImage_);
    framingCamera_.reset ();
    table_.reset ();
    if (target_ == nullptr)
        return;
    target_->Destroy (context);
    target_.reset ();
}

} // namespace geomsrv::archviz

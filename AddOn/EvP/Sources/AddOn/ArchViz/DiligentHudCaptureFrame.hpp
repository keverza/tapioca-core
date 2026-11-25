#ifndef EVP_ARCHVIZ_DILIGENTHUDCAPTUREFRAME_HPP
#define EVP_ARCHVIZ_DILIGENTHUDCAPTUREFRAME_HPP

#include "ArchViz/ViewerScreenshotGeometry.hpp"
#include "ArchViz/LayoutReportPage.hpp"
#include "SunStudy/AnalysisRoi.hpp"
#include "SunStudy/CapturePlan.hpp"
#include <memory>
#include <string>

namespace evp::sunstudy {
struct StudyRecord;
}
namespace geomsrv::archviz {

struct SunStudyAtlasUpload;
class Camera;
struct HudState;
struct SunStudyOverlayStatus;

namespace hudshell {
enum class DockEdge : uint8_t;
}

namespace viewerhud {
struct ReportPaperPreview;

struct CaptureFrame {
    bool active = false;
    bool cancelled = false;
    int kind = kShotVision;
    int preset = 1;
    int aspect = 0;
    bool studyDay = false;
    std::shared_ptr<const tapioca::sunstudy::CapturePlan> legendPlan;
    int shadowStyle = 0, shadowStep = 0;
    int entryMode = -1, entryStep = -1;
    int startMinute = 540, endMinute = 900, divisions = 3, grip = 0;
    bool seasonal = false;
    bool builder = false;
    bool editCamera = false, previewOnly = false, paperDirty = true;
    float tileAspect = 1.5f;
    uintptr_t paperImage = 0, liveImage = 0;
    size_t liveColumn = 0;
    std::vector<uintptr_t> clockImages;
    ScreenshotRect cameraTilePixels, cameraTileSource;
    bool cameraNavHeld = false;
    std::vector<PaperViewportRect> paperTiles;
    float resizeX = 0, resizeY = 0;
    bool resizingTile = false;
    std::shared_ptr<ReportPaperPreview> paperCache;
    struct Season {
        std::shared_ptr<const SunStudyAtlasUpload> payload;
        std::shared_ptr<const evp::sunstudy::StudyRecord> record;
    };
    Season seasons[3];
    uint64_t seasonStamp = 0;
    bool measuredDraft = false;
    int liveStep = 0;
    int reportType = 0, pageFormat = 0;
    int exportPage = -1, returnPage = 0, cameraPage = -1;
    size_t navigationTile = 0;
    std::shared_ptr<Camera> pageCameras[3];
    int formats[3] = {};
    float aspects[3] = { 1.5f, 1.5f, 1.5f };
    int ClockAtDivision (int i) const
    {
        return startMinute + 5 * ((2 * i * (endMinute - startMinute) + 5 * divisions) / (10 * divisions));
    }
    void BeginPageExport ()
    {
        returnPage = reportType;
        exportPage = 0;
        SelectWorkflow (exportPage);
    }
    bool AdvancePageExport ()
    {
        if (++exportPage < 3) {
            SelectWorkflow (exportPage);
            return true;
        }
        exportPage = -1;
        SelectWorkflow (returnPage);
        return false;
    }
    void SelectWorkflow (int page)
    {
        formats[reportType] = pageFormat;
        aspects[reportType] = tileAspect;
        reportType = page;
        pageFormat = formats[page];
        tileAspect = aspects[page];
        clocks.clear ();
        if (page == 0) {
            for (int i = 0; i <= divisions; ++i)
                clocks.push_back (ClockAtDivision (i));
        }
        else
            clocks.push_back (720);
        panelSteps.assign (clocks.size (), -1);
        paperDirty = true;
        navigationTile = 0;
    }
    std::vector<int> clocks;
    std::vector<std::string> captions;
    std::string seasonLabels[3];
    std::string progressText;
    float progress = 0;
    int hotX = 0, hotY = 0, viewSlot = 0;
    std::vector<int> panelSteps;
    uint32_t gridHeight = 0;
    std::vector<uint16_t> layoutMinutes;
    int month = 1, day = 1;
    uint64_t requestedRun = 0;
    std::string pinnedStudy;
    uint64_t pinnedVersion = 0;
    ScreenshotRect rect;
    ScreenshotSize view;
    ScreenshotRect dragRect;
    float dragX = 0.0f;
    float dragY = 0.0f;
    int dragSideX = 0;
    int dragSideY = 0;
};

struct CaptureDock {
    ScreenshotRect rect;
    ScreenshotSize view;
    hudshell::DockEdge edge {};
    bool horizontal = true;
    float fontScale = 1.f;
};

ScreenshotRect DefaultCaptureRect (const CaptureDock& dock, ScreenshotSize view, float margin);
ScreenshotRect EditCaptureRect (ScreenshotRect rect, ScreenshotSize view, float dx, float dy, int sideX, int sideY,
                                float minimum, float aspect);
ScreenshotRect FitCaptureAspect (ScreenshotRect rect, ScreenshotSize view, float minimum, float aspect);
bool DrawStudyDay (HudState& state, const SunStudyOverlayStatus* shown);
void CancelStudyDay (const CaptureFrame& frame);
enum class StudyDayState { Waiting, Publishing, Ready, Failed };
StudyDayState PollStudyDay (const HudState& state, const SunStudyOverlayStatus& shown, std::string& status);
bool BeginCaptureFrame (HudState& state, int kind);
bool DrawCaptureFrame (HudState& state, const CaptureDock& dock, ScreenshotSize view, float dpiScale,
                       const SunStudyOverlayStatus* shown = nullptr);
// Screenshot and Layout Builder. `builder` false (the Planting tab, #278) draws
// the Layout Builder disabled: its planting page is not built yet.
void DrawCaptureRow (HudState& state, int kind, bool builder = true);

} // namespace viewerhud
} // namespace geomsrv::archviz

#endif

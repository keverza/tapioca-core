#ifndef EVP_ARCHVIZ_DILIGENTHUDCAPTUREFRAME_HPP
#define EVP_ARCHVIZ_DILIGENTHUDCAPTUREFRAME_HPP

#include "ArchViz/ViewerScreenshotGeometry.hpp"
#include "SunStudy/AnalysisRoi.hpp"
#include "SunStudy/CapturePlan.hpp"
#include <memory>
#include <string>

namespace geomsrv::archviz {

struct HudState;
struct SunStudyOverlayStatus;

namespace hudshell {
enum class DockEdge : uint8_t;
}

namespace viewerhud {

struct CaptureFrame {
    bool active = false;
    bool cancelled = false;
    int kind = kShotVision;
    int preset = 1;
    int aspect = 0;
    bool studyDay = false;
    std::shared_ptr<const tapioca::sunstudy::CapturePlan> legendPlan;
    int shadowStyle = 0, shadowStep = 0, hourStep = 1;
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
};

ScreenshotRect DefaultCaptureRect (const CaptureDock& dock, ScreenshotSize view, float margin);
ScreenshotRect EditCaptureRect (ScreenshotRect rect, ScreenshotSize view, float dx, float dy, int sideX, int sideY,
                                float minimum, float aspect);
ScreenshotRect FitCaptureAspect (ScreenshotRect rect, ScreenshotSize view, float minimum, float aspect);
bool DrawStudyDay (HudState& state, const SunStudyOverlayStatus* shown);
void CancelStudyDay (const CaptureFrame& frame);
bool BeginCaptureFrame (HudState& state, int kind);
bool DrawCaptureFrame (HudState& state, const CaptureDock& dock, ScreenshotSize view, float dpiScale,
                       const SunStudyOverlayStatus* shown = nullptr);
void DrawCaptureRow (HudState& state, int kind);

} // namespace viewerhud
} // namespace geomsrv::archviz

#endif

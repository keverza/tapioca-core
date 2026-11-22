#ifndef EVP_ARCHVIZ_DILIGENTHUDCAPTUREFRAME_HPP
#define EVP_ARCHVIZ_DILIGENTHUDCAPTUREFRAME_HPP

#include "ArchViz/ViewerScreenshotGeometry.hpp"

namespace geomsrv::archviz {

struct HudState;

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
bool BeginCaptureFrame (HudState& state, int kind);
bool DrawCaptureFrame (HudState& state, const CaptureDock& dock, ScreenshotSize view, float dpiScale);
void DrawCaptureEntry (HudState& state, const CaptureDock& dock, float dpiScale);
void DrawCaptureRow (HudState& state, int kind);

} // namespace viewerhud
} // namespace geomsrv::archviz

#endif

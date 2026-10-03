#include "ArchViz/DiligentHud.hpp"
#include "ArchViz/DiligentHudSections.hpp"
#include "ArchViz/AnnotationHudControls.hpp"
#include "ArchViz/DiligentScene.hpp"
#include "ArchViz/DiligentShaders.hpp"
#include "ArchViz/ImGuiContextLock.hpp" // ImGui's one global context: every use under its lock
#include "ArchViz/InputRingBuffer.hpp"
#include "ArchViz/ImGuiGraphInteractionLab.hpp"
#include "ArchViz/SceneTextLiveCheck.hpp"

#include <windows.h>
#include <d3d11.h> // Must precede any Diligent D3D11 interop header (Probe 1a).

#include <imgui.h>

#include <ImGuiDiligentRenderer.hpp>
#include <ImGuiImplDiligent.hpp> // for ImGuiDiligentCreateInfo

#include <DeviceContext.h>
#include <RenderDevice.h>
#include <SwapChain.h>

#include <cmath>
#include <memory>

namespace geomsrv {
namespace archviz {

namespace {

// The debug views, by the names the user already knows from the command's
// `debug_view` parameter. ⚠️ THE ORDER IS THE DiligentDebugView ENUM'S ORDER --
// the combo's index IS the value, so an insertion here silently renumbers them.
const char* const kDebugViewNames[] = {
    "final",
    "normals",
    "lit",
    "base color",
    "sun vector",
    "shadow",
    "roughness",
    "G-buffer normals",
    "G-buffer depth",
    "ambient occlusion",
    "G-buffer albedo",
    "G-buffer roughness",
    "G-buffer material",
    "motion vectors",
};
constexpr int kDebugViewCount = int (sizeof (kDebugViewNames) / sizeof (kDebugViewNames[0]));

static_assert (kDebugViewCount == int (DiligentDebugView::MotionVectors) + 1,
               "the HUD's combo and DiligentDebugView have drifted apart");

// The render modes, in SceneRenderMode's order -- the combo's index IS the
// value, exactly as for the debug views above.
const char* const kRenderModeNames[] = { "shaded", "wireframe", "shaded + wireframe" };
constexpr int kRenderModeCount = int (sizeof (kRenderModeNames) / sizeof (kRenderModeNames[0]));

static_assert (kRenderModeCount == int (SceneRenderMode::ShadedWireframe) + 1,
               "the HUD's combo and SceneRenderMode have drifted apart");

// The render qualities, in RenderQuality's order -- same rule again: the combo's
// index IS the enum value.
const char* const kRenderQualityNames[] = { "fast", "realistic" };
constexpr int kRenderQualityCount = int (sizeof (kRenderQualityNames) / sizeof (kRenderQualityNames[0]));

static_assert (kRenderQualityCount == int (RenderQuality::Realistic) + 1,
               "the HUD's combo and RenderQuality have drifted apart");

// ImGui's mouse button numbering, against InputRingBuffer's bitmask.
int ToImGuiButton (uint8_t button)
{
    if (button == kMouseLeft)
        return 0;
    if (button == kMouseRight)
        return 1;
    return -1;
}

} // namespace

struct DiligentHud::Impl {
    ImGuiContext* context = nullptr;
    std::unique_ptr<Diligent::ImGuiDiligentRenderer> renderer;
    bool ready = false;
    double lastTimeSeconds = 0.0;

    // ---- the frame-cost badge ----------------------------------------------
    // ⚠️ THE WORST FRAME MATTERS MORE THAN THE AVERAGE, and an average is what an
    // fps counter shows. A viewer that runs at 60 fps and stalls for 200 ms when
    // an extraction batch lands has taken 200 ms out of ARCHICAD's UI thread's
    // neighbourhood, and the mean over the same second still reads ~55 fps. So
    // the badge carries both, and the peak is held long enough to be read.
    float worstMsInWindow = 0.0f;
    float heldWorstMs = 0.0f;
    double heldUntilSeconds = 0.0;
    ImGuiGraphInteractionLab graphInteractionLab;
};

namespace {

// How long a spike stays on the badge after it happens. Long enough to notice
// and read, short enough that the number still describes the recent past.
constexpr double kWorstHoldSeconds = 3.0;

// Above this, a frame is slow enough that the user would feel it. 16.7 ms is one
// vsynced frame; a viewer that misses it occasionally is fine, one that misses it
// by a lot is the thing this badge exists to catch.
constexpr float kSlowFrameMs = 33.0f;

double QpcSeconds ()
{
    LARGE_INTEGER counter = {};
    LARGE_INTEGER frequency = {};
    if (!::QueryPerformanceCounter (&counter) || !::QueryPerformanceFrequency (&frequency) || frequency.QuadPart <= 0)
        return 0.0;
    return double (counter.QuadPart) / double (frequency.QuadPart);
}

} // namespace

DiligentHud::DiligentHud () : impl_ (new Impl ())
{
}
DiligentHud::~DiligentHud ()
{
    Shutdown ();
    delete impl_;
}

bool DiligentHud::IsReady () const
{
    return impl_ != nullptr && impl_->ready;
}

bool DiligentHud::Init (Diligent::IRenderDevice* device, uint32_t colorBufferFormat, uint32_t depthBufferFormat,
                        std::string& error)
{
    if (device == nullptr) {
        error = "DiligentHud::Init got no device";
        return false;
    }
    if (impl_->ready)
        return true;
    std::lock_guard<std::mutex> lock (ImGuiContextMutex ());

    // ⚠️ AN EXPLICIT CONTEXT, NOT THE IMPLICIT GLOBAL. ImGui's default context is
    // a process-wide singleton, and the bgfx viewer creates one of its own. Two
    // renderers sharing one context would share one set of windows and one set
    // of GPU buffers owned by whichever initialised last.
    impl_->context = ImGui::CreateContext ();
    if (impl_->context == nullptr) {
        error = "ImGui::CreateContext returned nothing";
        return false;
    }
    ImGui::SetCurrentContext (impl_->context);

    ImGuiIO& io = ImGui::GetIO ();
    // No .ini and no .log next to the .apx. `evp.paths` owns where files go, and
    // ImGui would otherwise drop imgui.ini into Archicad's working directory --
    // which is inside Program Files.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark ();

    // ⚠️ THE FORMAT PAIR IS SET BY HAND rather than through the
    // SwapChainDesc constructor, because the overlay has no swap chain to ask.
    // The PSO records the formats it renders into, so a mismatch here is a
    // creation-time failure rather than a draw-time surprise.
    Diligent::ImGuiDiligentCreateInfo ci;
    ci.pDevice = device;
    ci.BackBufferFmt = static_cast<Diligent::TEXTURE_FORMAT> (colorBufferFormat);
    ci.DepthBufferFmt = static_cast<Diligent::TEXTURE_FORMAT> (depthBufferFormat);
    try {
        impl_->renderer = std::make_unique<Diligent::ImGuiDiligentRenderer> (ci);
    }
    catch (const std::exception& ex) {
        error = std::string ("ImGuiDiligentRenderer construction failed: ") + ex.what ();
        ImGui::DestroyContext (impl_->context);
        impl_->context = nullptr;
        return false;
    }

    impl_->ready = true;
    return true;
}

void DiligentHud::Shutdown ()
{
    if (impl_ == nullptr)
        return;
    std::lock_guard<std::mutex> lock (ImGuiContextMutex ());
    impl_->renderer.reset ();
    if (impl_->context != nullptr) {
        ImGui::SetCurrentContext (impl_->context);
        ImGui::DestroyContext (impl_->context);
        impl_->context = nullptr;
    }
    impl_->ready = false;
}

void DiligentHud::Draw (Diligent::IDeviceContext* context, uint32_t width, uint32_t height, const InputSnapshot& input,
                        const DiligentSceneStats& scene, const ProjectedDrawList& annotations, HudState& state,
                        bool showControls)
{
    if (context == nullptr || !impl_->ready || width == 0 || height == 0)
        return;
    std::lock_guard<std::mutex> lock (ImGuiContextMutex ());

    ImGui::SetCurrentContext (impl_->context);
    ImGuiIO& io = ImGui::GetIO ();
    io.DisplaySize = ImVec2 (float (width), float (height));

    // ⚠️ A NON-ZERO DELTA, ALWAYS. ImGui asserts on DeltaTime <= 0, and an assert
    // inside a render thread in Archicad's process is a hard crash of the host
    // application rather than a message. 1/60 is the honest default for a
    // vsynced present.
    const double now = QpcSeconds ();
    const double delta = impl_->lastTimeSeconds > 0.0 ? now - impl_->lastTimeSeconds : 0.0;
    impl_->lastTimeSeconds = now;
    io.DeltaTime = delta > 0.0 && delta < 1.0 ? float (delta) : 1.0f / 60.0f;

    // ---- input ------------------------------------------------------------
    // The same snapshot the camera is about to read. Position first, then the
    // TRANSITIONS -- a press and release inside one frame collapses to nothing
    // if only the final held state is carried, and a fast click on a checkbox is
    // exactly that.
    // A captured drag may legitimately leave the item. PollHardwareInput still
    // provides client coordinates there; retaining them while a button is held
    // is what lets ImGui finish the drag instead of snapping or stalling.
    if (input.inside || input.buttons != kMouseNone)
        io.AddMousePosEvent (float (input.x), float (input.y));
    else
        io.AddMousePosEvent (-FLT_MAX, -FLT_MAX);
    for (int i = 0; i < input.transitionCount && i < InputSnapshot::kMaxTransitions; ++i) {
        const int button = ToImGuiButton (input.transitions[i].button);
        if (button >= 0)
            io.AddMouseButtonEvent (button, input.transitions[i].down);
    }
    if (input.wheelDelta != 0)
        io.AddMouseWheelEvent (0.0f, float (input.wheelDelta) / 120.0f);
    io.AddKeyEvent (ImGuiMod_Shift, input.shift);

    impl_->renderer->NewFrame (width, height, Diligent::SURFACE_TRANSFORM_IDENTITY);
    ImGui::NewFrame ();

    ImDrawList* annotationDrawList = ImGui::GetBackgroundDrawList ();
    for (const ScreenTriangle& triangle : annotations.triangles) {
        const uint32_t rgba = triangle.rgba;
        annotationDrawList->AddTriangleFilled (
            { triangle.points[0].x, triangle.points[0].y }, { triangle.points[1].x, triangle.points[1].y },
            { triangle.points[2].x, triangle.points[2].y },
            IM_COL32 ((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu, rgba & 0xFFu));
    }
    for (const ScreenLine& line : annotations.lines) {
        const uint32_t rgba = line.rgba;
        annotationDrawList->AddLine (
            { line.from.x, line.from.y }, { line.to.x, line.to.y },
            IM_COL32 ((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu, rgba & 0xFFu), line.width);
    }
    for (const ScreenLabel& label : annotations.labels) {
        const uint32_t rgba = label.rgba;
        const float fontSize = label.fontSize > 0.0f ? label.fontSize : ImGui::GetFontSize ();
        const ImVec2 extent = ImGui::GetFont ()->CalcTextSizeA (fontSize, FLT_MAX, 0.0f, label.text.c_str ());
        const ImVec2 position = label.centered ? ImVec2 { label.anchor.x - extent.x * 0.5f, label.anchor.y }
                                               : ImVec2 { label.anchor.x + 4.0f, label.anchor.y - extent.y - 3.0f };
        if (label.backgroundPanel) {
            annotationDrawList->AddRectFilled ({ position.x - 3.0f, position.y - 2.0f },
                                               { position.x + extent.x + 3.0f, position.y + extent.y + 2.0f },
                                               IM_COL32 (255, 255, 255, 224), 2.0f);
        }
        annotationDrawList->AddText (
            ImGui::GetFont (), fontSize, position,
            IM_COL32 ((rgba >> 24) & 0xFFu, (rgba >> 16) & 0xFFu, (rgba >> 8) & 0xFFu, rgba & 0xFFu),
            label.text.c_str ());
    }

    if (!showControls) {
        ImGui::Render ();
        impl_->renderer->RenderDrawData (context, ImGui::GetDrawData ());
        impl_->renderer->EndFrame ();
        state.wantsMouse = false;
        return;
    }

    // ---- the instruction banner (PLAT-RE111) -------------------------------
    //
    // ⚠️ FIRST, AND ACROSS THE TOP, BECAUSE IT IS THE ONLY TEXT THE USER CAN
    // READ MID-NAVIGATION. Archicad's DG palette does not repaint during a
    // navigation drag, so the status line `evp.ui.progress` writes to is frozen
    // exactly when a measurement run needs to say what to do next. The overlay
    // renders every frame regardless.
    //
    // ⚠️ NoInputs, LIKE THE CALLOUT AND FOR THE SAME REASON. Without it the
    // banner counts as a hovered ImGui item wherever it sits, `WantCaptureMouse`
    // goes true, and navigation stops working under the very thing telling the
    // user to navigate.
    if (!state.instruction.empty ()) {
        ImGui::SetNextWindowPos (ImVec2 (float (width) * 0.5f, 16.0f), ImGuiCond_Always, ImVec2 (0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha (0.85f);
        const ImGuiWindowFlags bannerFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin ("##instruction", nullptr, bannerFlags)) {
            // Twice the default size: this is read at arm's length, in
            // peripheral vision, while the hand is busy dragging the view.
            ImGui::SetWindowFontScale (2.0f);
            ImGui::TextColored (ImVec4 (1.0f, 0.85f, 0.25f, 1.0f), "%s", state.instruction.c_str ());
            if (state.instructionSecondsRemaining >= 0.0) {
                ImGui::SetWindowFontScale (3.0f);
                ImGui::Text ("%.0f", std::ceil (state.instructionSecondsRemaining));
            }
            ImGui::SetWindowFontScale (1.0f);
        }
        ImGui::End ();
    }

    ImGui::SetNextWindowPos (ImVec2 (12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize (ImVec2 (300.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin ("Tapioca viewport")) {
        ImGui::Text ("%.1f fps   %u x %u", state.fps, width, height);
        if (!state.adapter.empty ())
            ImGui::TextWrapped ("%s", state.adapter.c_str ());

        ImGui::Separator ();

        // ---- CONTROLS: PANEL ONLY -----------------------------------------
        //
        // ⚠️ THE OVERLAY GETS READOUTS, NEVER WIDGETS. `WS_EX_TRANSPARENT` is
        // all-or-nothing per window (PLAT-RE55), so on the overlay every one of
        // these would DRAW and none of them could be clicked. A control that
        // cannot be operated is worse than an absent one: the user tries it,
        // nothing happens, and a window style reads as the viewer having hung.
        // So the overlay shows the same STATE as text, and the panel owns input.
        if (state.readOnly) {
            ImGui::TextDisabled ("overlay: click-through, display only");
            ImGui::Text (
                "view %s   surfaces %s",
                kDebugViewNames[state.debugView >= 0 && state.debugView < kDebugViewCount ? state.debugView : 0],
                kRenderModeNames[state.renderMode >= 0 && state.renderMode < kRenderModeCount ? state.renderMode : 0]);
            ImGui::Text ("quality %s   projection %s",
                         kRenderQualityNames[state.renderQuality >= 0 && state.renderQuality < kRenderQualityCount
                                                 ? state.renderQuality
                                                 : 0],
                         state.orthographic ? "axonometric" : "perspective");
        }
        else {
            // ⚠️ THE COMBO'S INDEX IS THE ENUM VALUE. See kDebugViewNames.
            ImGui::SetNextItemWidth (-1.0f);
            ImGui::Combo ("##debugview", &state.debugView, kDebugViewNames, kDebugViewCount);
            ImGui::TextDisabled ("debug view");

            ImGui::SetNextItemWidth (-1.0f);
            ImGui::Combo ("##rendermode", &state.renderMode, kRenderModeNames, kRenderModeCount);
            ImGui::TextDisabled ("surfaces -- wireframe is what makes the OVERLAY readable");
            if (state.renderMode != int (SceneRenderMode::Shaded)) {
                // 1 = outlines only, and the default. The floor is 1, not 0:
                // a tessellation factor of 0 culls the patch.
                ImGui::SliderInt ("wire subdivisions", &state.wireTessellation, 1, 16);
                ImGui::SliderFloat ("wire width", &state.wireLineWidth, 0.5f, 3.0f, "%.2f px");
            }
            DrawAnnotationHudControls (state);

            // ⚠️ A SEPARATE COMBO FROM THE ONE ABOVE, not more entries in it.
            // Quality and surfaces are independent axes and every pairing is
            // wanted -- see RenderQuality in ViewerSettings.hpp.
            ImGui::SetNextItemWidth (-1.0f);
            ImGui::Combo ("##renderquality", &state.renderQuality, kRenderQualityNames, kRenderQualityCount);
            ImGui::TextDisabled ("quality -- realistic adds specular + tone mapping");

            ImGui::Checkbox ("axonometric (parallel projection)", &state.orthographic);
            ImGui::Checkbox ("graph interaction lab", &state.showGraphInteractionLab);
            DrawSceneTextLiveCheckControls (state);
            DrawSunStudyHudSection (state, scene);

            DrawEnvironmentControls (state, scene);

            DrawPostProcessingControls (state, scene);

            DrawStorySliceControls (state, scene);

            DrawGhPreviewControls (state);
            ImGui::Checkbox ("callout under the cursor", &state.showCallout);
            ImGui::Checkbox ("selected element properties", &state.showProperties);
        }

        ImGui::Separator ();
        ImGui::Text ("elements %llu", (unsigned long long) scene.elements);
        ImGui::Text ("triangles %llu", (unsigned long long) scene.triangles);
        if (scene.pointLayers > 0)
            ImGui::Text ("point clouds %llu   visible %llu / %llu", (unsigned long long) scene.pointLayers,
                         (unsigned long long) scene.visiblePoints, (unsigned long long) scene.points);
        ImGui::Text ("materials %llu   misses %llu", (unsigned long long) scene.materials,
                     (unsigned long long) scene.materialMisses);
        if (scene.pending > 0)
            ImGui::TextColored (ImVec4 (1.0f, 0.8f, 0.2f, 1.0f), "extracting: %llu queued",
                                (unsigned long long) scene.pending);

        ImGui::Separator ();
        DrawLightInspector (state, scene);

        DrawShadowSettings (state, scene);
        ImGui::Text ("draws %llu   materials in pool %llu", (unsigned long long) scene.drawCalls,
                     (unsigned long long) scene.materials);
    }
    ImGui::End ();

    if (state.showGraphInteractionLab)
        impl_->graphInteractionLab.Draw (width, height, input, state.frameLatency, state.showGraphInteractionLab,
                                         state.graphInteractionFastPath);
    state.graphInteractionActive = ImGui::IsAnyItemActive ();
    DrawSunStudyInspectorTooltip (state, input, width, height);

    // ---- the frame-cost badge, ALWAYS ON -----------------------------------
    // ⚠️ A SEPARATE WINDOW FROM THE PANEL ABOVE, DELIBERATELY. The panel is
    // collapsible, movable and scrollable, and on the overlay it is also
    // CLICK-THROUGH -- so once it is collapsed or pushed off the surface there is
    // no way to get the number back without restarting the viewer. The
    // requirement is that the add-on's cost is visible at every moment, and a
    // readout that can be lost does not meet it.
    //
    // NoInputs is load-bearing for the same reason it is on the callout: this
    // must never make ImGui want the mouse, or the camera and the pick stop
    // responding under a corner of the screen.
    if (state.showFpsBadge) {
        const float frameMs = io.DeltaTime * 1000.0f;
        if (frameMs > impl_->worstMsInWindow)
            impl_->worstMsInWindow = frameMs;
        if (impl_->worstMsInWindow > impl_->heldWorstMs || now >= impl_->heldUntilSeconds) {
            impl_->heldWorstMs = impl_->worstMsInWindow;
            impl_->heldUntilSeconds = now + kWorstHoldSeconds;
            impl_->worstMsInWindow = 0.0f;
        }

        // Right-hand edge, top. Pivot (1,0) is what keeps it anchored there as
        // the text width changes rather than growing off the surface.
        ImGui::SetNextWindowPos (ImVec2 (float (width) - 8.0f, 8.0f), ImGuiCond_Always, ImVec2 (1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha (0.55f);
        const ImGuiWindowFlags badgeFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin ("##fps", nullptr, badgeFlags)) {
            // The render thread's own measured rate (a half-second window), not
            // ImGui's -- they agree while the viewer is healthy and diverge
            // exactly when something is wrong, and the renderer's is the true one.
            ImGui::Text ("%.0f fps   %.1f ms", state.fps, frameMs);
            const bool slow = impl_->heldWorstMs > kSlowFrameMs;
            if (slow) {
                ImGui::TextColored (ImVec4 (1.0f, 0.55f, 0.35f, 1.0f), "worst %.0f ms", impl_->heldWorstMs);
            }
            else {
                ImGui::TextDisabled ("worst %.0f ms", impl_->heldWorstMs);
            }
        }
        ImGui::End ();
    }

    DrawHoverCallout (state, input, width, height);

    DrawSelectedElementWindow (state, height);

    ImGui::Render ();
    impl_->renderer->RenderDrawData (context, ImGui::GetDrawData ());
    impl_->renderer->EndFrame ();

    // ⚠️ READ BACK AFTER Render, NOT BEFORE. WantCaptureMouse is only meaningful
    // once the frame's widgets have been submitted; asking at the top of the
    // frame reports the PREVIOUS frame's answer, and the camera then orbits the
    // model on the click that was meant for the combo box.
    state.wantsMouse = io.WantCaptureMouse;
}

} // namespace archviz
} // namespace geomsrv

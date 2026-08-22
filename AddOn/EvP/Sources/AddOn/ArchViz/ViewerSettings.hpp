#ifndef EVP_ARCHVIZ_VIEWERSETTINGS_HPP
#define EVP_ARCHVIZ_VIEWERSETTINGS_HPP

// The knobs the ImGui panel owns and the frame loop obeys.
//
// It exists so a render-state question can be settled by OBSERVATION in one run
// instead of by argument in a review — the user cannot rebuild the add-on, so a
// constant that is only 80% certain has to become a control, not a guess.
//
// Plain C++ only, with no Diligent or DG dependency. The HUD, scene and frame
// loop share these values without pulling graphics or palette types across
// their boundary.
//
// RENDER THREAD ONLY: the settings are a local of the frame loop, handed to the
// panel by reference and read back the same frame. Nothing else may hold one.

#include <cstdint>

namespace geomsrv {
namespace archviz {

// Named after the winding, not after "front"/"back", on purpose. Which winding is the
// FRONT face is not an independent fact: it depends on the handedness of the
// view+projection pair, because a left-handed pair applied to a right-handed
// world mirrors every triangle's screen-space winding along with the image.
// Labelling the options "cull back faces" would have buried that dependency and
// made "CCW is correct" look like an answer instead of a symptom of the mirror
// it actually was. The user sees the flag; the flag is the evidence.
enum class CullMode : uint8_t {
    Ccw  = 0,   // correct only while the matrices were mirrored
    Cw   = 1,   // the default now that Camera is right-handed
    None = 2,   // no culling: both windings drawn, so the shape is at least whole
};

// How the scene's surfaces are drawn.
//
// ⚠️ `Wireframe` EXISTS FOR THE OVERLAY, NOT AS A STYLE. Over Archicad's own 3D
// window the two pictures are the same building, and a shaded viewer simply
// HIDES the one underneath it -- so "do the two agree" becomes unanswerable at
// exactly the moment it is being asked. Lines let both be seen at once, which
// is the whole measurement.
enum class SceneRenderMode : uint8_t {
    Shaded = 0,
    Wireframe = 1,          // lines only, no fill
    ShadedWireframe = 2,    // fill with the lines over it
};

// HOW WELL a surface is lit — a SEPARATE AXIS from SceneRenderMode, which says
// WHAT is drawn (PLAT-RE126).
//
// ⚠️ TWO ENUMS RATHER THAN ONE WITH MORE CASES, ON PURPOSE. Quality and mode are
// independent questions and every combination is wanted: a fast wireframe for
// checking register against Archicad, a realistic shaded view for judging a
// massing scheme, and realistic-with-lines for presenting one. Folding them into
// a single enum means every new quality multiplies the cases, and the switch
// stops describing anything -- which is exactly what happened to CameraSyncMode,
// whose own header now says it "became a set of independent switches wearing an
// enum costume". This starts on the correct side of that lesson.
//
// ⚠️ THE OVERLAY SHOULD STAY ON `Fast`. Its frame budget belongs to Archicad,
// and it exists to be compared AGAINST Archicad's shading rather than to look
// better than it.
enum class RenderQuality : uint8_t {
    // The shipped look: Lambert diffuse, hemispheric sky/ground ambient, one
    // shadow cascade. Cheap and predictable, and the right default for the
    // overlay and for navigating a large model.
    Fast = 0,

    // Closer to a rendered image: GGX material shading, HDR environment IBL,
    // softer shadows, optional DiligentFX AO/SSR/TAA, and one filmic resolve.
    // It remains an interactive raster quality rather than the progressive
    // physically based renderer planned by RE51.
    Realistic = 1,
};

struct ViewerSettings {
    // ⚠️ THIS DEFAULT MOVED WITH THE HANDEDNESS FIX, AND HAD TO. The run said
    // CCW showed the outside — while Camera was still handing bgfx left-handed
    // matrices, which mirrored the screen winding. Making the camera
    // right-handed un-mirrors the image and therefore flips the correct cull
    // back to CW, which is what Phase 5 originally shipped. Phase 5's cull was
    // right all along and its matrices were not; the toggle is what let those
    // two be told apart, and it stays until a run confirms this pairing.
    CullMode cull = CullMode::Cw;

    // One-shot requests. The panel raises them; the frame loop (or the panel
    // itself, for the position) clears them the next time round. A one-shot flag
    // rather than a callback because the panel and the loop are the same thread
    // and a callback would only hide the ordering.
    bool resetPanelPos = false;   // the HUD escaped; put it back at 12,12
    bool frameScene    = false;   // "zoom to fit", same action as double-clicking the wheel

    // ---- THE FLICKER MATRIX ------------------------------------------------
    //
    // THE SYMPTOM (reported 2026-08-07): Archicad's 2D windows flicker while
    // navigating — but ONLY while this viewer is open, and whether it is docked
    // or floating makes no difference. So it is not the palette's window; it is
    // something the renderer DOES, every frame, in Archicad's process.
    //
    // ⚠️ THESE ARE NOT PREFERENCES AND THEY ARE NOT A FIX. They exist so the
    // question can be answered by BISECTION in one sitting instead of by a
    // sequence of single-configuration guesses, each costing a human round trip
    // — which is exactly what §24 exists to avoid for Part III, applied here.
    // The user switches layers off from the TOP DOWN and reports the first row
    // at which the flicker stops; that row is the cause, and none of the rows
    // above it are.
    //
    // The order is deliberate: cheapest-to-disable and most-suspected first.
    //
    //   presentEveryFrame  the DXGI Present itself. A swap chain presenting into
    //                      a process whose 2D canvas is GDI is the leading
    //                      suspect; with this off, bgfx::frame is still called
    //                      but at `presentHz`, so the difference between "we
    //                      present" and "we present OFTEN" is separable.
    //   drawHud            ImGui: a second program, its own textured draws.
    //   drawScene          the geometry: the biggest GPU load by far.
    //   clearView          the clear touch. If the flicker survives even this,
    //                      the renderer is drawing NOTHING and merely existing —
    //                      which points at the device/swap chain, not at us.
    //
    // ⚠️ `vsync` needs a bgfx::reset, so the frame loop treats a change as a
    // resize. It is here because a vsync'd present blocks on the display, and a
    // blocked present in Archicad's process is a plausible cause of a stutter
    // that looks like flicker.
    bool     drawScene        = true;
    bool     drawHud          = true;
    bool     clearView        = true;
    bool     presentEveryFrame = true;
    uint32_t presentHz        = 10;    // used only when presentEveryFrame is off
    bool     vsync            = true;

    // ---- THE SUN, AS A CONTROL ---------------------------------------------
    //
    // ⚠️ THIS IS A BISECTION, NOT A LOOK-DEV KNOB, and it exists because two
    // rounds of fixes did not settle why the model reads flat. Drag the sun:
    //
    //   the shading CHANGES  -> the shader, the normals and the uniform path are
    //                           all fine, and the problem is the DATA (the sun
    //                           Archicad reports, or it never arriving);
    //   the shading does NOT -> nothing downstream of this slider works, and the
    //                           sun data is irrelevant until it does.
    //
    // Two questions that look identical on screen, separated by one drag. When
    // `sunOverride` is off the extraction thread's SetEnvironment wins, which is
    // the normal path.
    bool     sunOverride      = false;
    float    sunAzimuthDeg    = 135.0f;   // CCW from +X, Archicad's own convention
    float    sunAltitudeDeg   = 45.0f;    // above the horizon
    float    ambientOverride  = 0.35f;
};

}   // namespace archviz
}   // namespace geomsrv

#endif

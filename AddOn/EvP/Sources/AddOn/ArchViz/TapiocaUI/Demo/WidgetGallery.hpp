#ifndef EVP_ARCHVIZ_TAPIOCAUI_DEMO_WIDGETGALLERY_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_DEMO_WIDGETGALLERY_HPP

// TapiocaUI/Demo/WidgetGallery -- every HUD control on one page, without an Archicad project:
// the visual reference a change of look is compared against, and a place to press a control
// and read what it said back.
//
// ⚠️ P0 SHOWS TODAY'S CONTROLS, NOT NEW ONES. Before TapiocaUI replaces a control, the gallery
// shows the one in use now -- hudshell's and ImGui's own, as the features call them -- at four
// densities and in both themes, enabled and disabled, with long, UTF-8, huge, tiny and negative
// content. That is the baseline P1's theme work must not visibly change
// (HANDOFF-TapiocaUI-Library.md sections 9 and 10). Each later widget joins it beside the
// control it replaces.
//
// ⚠️ THE ONLY TapiocaUI FILE THAT MAY INCLUDE HudShell.hpp: it draws the baseline. The rest of
// the library depends on nothing outside itself and ImGui (tools/quality/check_cpp.py).
//
// No state of its own: what the user did is the caller's GalleryState, one per ImGui context,
// so the overlay's context and the viewer's never share it. Pure ImGui on the thread that holds
// the context, inside a frame.

#include "ArchViz/TapiocaUI/Foundation/EditResult.hpp"
#include "ArchViz/TapiocaUI/Foundation/Tokens.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace geomsrv {
namespace archviz {
namespace ui {

// The last thing one control said, and in which frame.
struct GalleryEntry {
    std::string control;
    EditResult result;
    int frame = 0;
};

struct GalleryState {
    uint32_t density = tokens::kDensityStepDefault; // index into tokens::kDensitySteps
    bool dark = false;

    // The samples the controls edit.
    double huge = 1.0e9;
    double tiny = 1.0e-6;
    double negative = -273.15;
    double length = 12.5;
    float fraction = 0.25f;
    bool flag = true;
    bool mixed = false;
    int choice = 0;
    int radio = 0;
    uint32_t colour = 0x2F6FD0FFu;
    char typed[64] = "1,5";
    int activeIcon = -1; // the icon sheet's toggled button, -1 for none

    // One entry per control, the latest first.
    std::vector<GalleryEntry> log;
};

// The gallery's page into the current window.
void DrawGallery (GalleryState& state);

// The gallery in a window of its own; its close button sets `open` false.
void GalleryWindow (GalleryState& state, bool& open);

// What the last item said this frame, logged under `control` when it said anything: edited,
// deactivated after an edit, or left by Escape. Every sample control is logged this way.
void ObserveLastItem (GalleryState& state, const char* control);

// `result` logged under `control` when it says anything: a control that answers with a bool
// (hudshell::ColourChoice) reports through this.
void Record (GalleryState& state, const char* control, const EditResult& result);

} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif

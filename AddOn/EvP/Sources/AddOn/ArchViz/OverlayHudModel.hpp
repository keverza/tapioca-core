#ifndef EVP_ARCHVIZ_OVERLAYHUDMODEL_HPP
#define EVP_ARCHVIZ_OVERLAYHUDMODEL_HPP

// ArchViz/OverlayHudModel -- what the overlays' HUD shows of its own (OverlayHud.hpp
// `OwnPages`): the overlay's state on Stats, Archicad's selection on Selection, what the
// overlay costs on Debug. Read from the runtimes whenever a view's HUD is laid out, and
// handed to that view's engine before it lays out.
//
// ⚠️ THE ENGINE READS NOTHING; THIS READS AND DOES NOTHING. The HUD's layout is pure ImGui and
// tested offline; what it is told comes from here, the one place that knows the runtimes.
//
// ⚠️ DELTAS, NOT TOTALS (OVERLAY-INVARIANTS.md §7): a rate is what moved over the last second
// or more, never a cumulative counter that reads the same for "now" and "ten minutes ago".
// What the overlay's composition costs is the COST line's last window, kept by the report --
// taking a window here would take it from the log.
//
// ⚠️ THE SELECTION IS READ WHEN IT CHANGED, NOT AT EVERY LAYOUT. A layout follows every move of
// the pointer over the HUD; Archicad says when its selection changes (`SelectionChanged`,
// from APINotify), and only then is it read again -- a few elements' kind, ID, layer and storey.
//
// MAIN THREAD, every entry point: it reads ACAPI and the runtimes' main-thread state.

#include "ArchViz/OverlayHitMap.hpp" // overlayinput::View
#include "ArchViz/OverlayHud.hpp"

namespace geomsrv {
namespace archviz {
namespace overlayhudmodel {

// What the view's HUD shows of its own now. The overlay runs there: `standalone`.
overlayhud::OwnPages Pages (overlayinput::View view);

// The view's HUD engine with its own pages set -- made if need be -- or null when it cannot
// start (OverlayGuestText.hpp says why, once).
overlayhud::Engine* Prepare (overlayinput::View view);

// Archicad's selection changed: read again at the next layout, and both views' HUDs laid out
// again. From Archicad's notification: cheap, nothing read here.
void SelectionChanged ();
// Completed source geometry changed: reread Selection without dirtying the
// source model again (otherwise each massing publish schedules another rebuild).
void RefreshSelection ();

// The floors the user picked on the building section, after a layout: their slices drawn on
// the 3D overlay when they, or the section, changed (SectionModel.hpp).
void FollowFloors (const hudsection::Run& run);

// The project closed (§8): the selection and the rates are that project's.
void Forget ();

// Settings' displays as the user set them (OverlayHud.hpp `Displays`), applied: the storey slices
// switched on (taking the selection, or asking for the model's cut) or off, or -- on and from the
// same source -- given their new look without anything read again (StorySliceOverlay::Restyle);
// the Watch annotations switched. Then both HUDs laid out again with what it made of it.
void ApplyDisplays (const overlayhud::Displays& displays);

// ⚠️ A NEW CONSOLE ENTRY IS LAID OUT WITHOUT WAITING FOR THE POINTER (HudConsole.hpp): on, every
// entry said -- on any thread -- asks both views' HUDs for a layout from the message loop, one
// for a burst, so the Debug tab's count appears when something fails. Off at unload.
void WakeOnConsole (bool on);

} // namespace overlayhudmodel
} // namespace archviz
} // namespace geomsrv

#endif

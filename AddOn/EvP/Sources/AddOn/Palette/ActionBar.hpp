#ifndef GEOMETRYSERVER_PALETTE_ACTIONBAR_HPP
#define GEOMETRYSERVER_PALETTE_ACTIONBAR_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"

#include <memory>
#include <vector>

namespace evp {

class PaletteScroll;
struct CommandInfo;

// The row of buttons under the results — what the user can DO with what the last
// run produced. Export it, place it on a layout, bake the plan into real elements.
//
// WHY IT IS A BAND AND NOT A MENU: the whole point of the standardised output API
// is that a command NAMES an action instead of implementing one, so the same
// "Export CSV" appears in the same place with the same behaviour everywhere. A
// per-command menu the author populated would put it back where it was.
//
// The buttons come from the scan: `CommandInfo::actions` are the names
// `evp._invoke.run_action` takes and `CommandInfo::actionLabels` is what each
// button says, parallel. The shell owns the DG subscription and the run, so
// pressing a button lands in ControlPalette::ButtonClicked, which asks this object
// WHICH action was pressed and starts a run with that name.
//
// ⚠️ AN ACTION IS NOT A RUN. It acts on the result the last run stored — see
// evp/_planstore.py. That is why the bar is disabled until a run has completed:
// a button that silently re-ran the command would repeat every write it made.
class ActionBar {
  public:
    // The shell stays the sole registered DG observer: every button is attached
    // to `observer` (the palette), never to this object.
    ActionBar (const DG::Panel& panel, DG::ButtonItemObserver& observer);

    // Nothing to Create(): the buttons are built per command in Rebuild, because
    // their number and their text both come from the selected command.

    // Replace the row with the selected command's declared actions.
    void Rebuild (const CommandInfo& info);

    // Hide the row and forget it.
    void Clear ();

    // Whether the buttons are pressable. False until a run has completed, because
    // there is nothing stored for an action to act on before that.
    void SetEnabled (bool value);

    // Position the row starting at `top`; returns the height used — 0 when the
    // command declares no actions, so it costs the layout no space at all.
    short PlaceAt (short top, short left, short right, const PaletteScroll& clip);

    bool IsVisible () const
    {
        return !buttons.empty ();
    }

    // The action name for the button that was clicked, or empty if it was not one
    // of ours. Event routing, for the shell's single ButtonClicked handler.
    GS::UniString ActionOf (const DG::Item* item) const;

  private:
    const DG::Panel& panel;
    DG::ButtonItemObserver& observer;

    std::vector<std::unique_ptr<DG::Button>> buttons;
    // Parallel to `buttons`: what run_action is asked for when button i is
    // pressed. Kept beside rather than inside because a DG::Button has nowhere to
    // carry a payload, and matching on the label would break the moment two
    // actions were given the same text.
    std::vector<GS::UniString> names;

    bool enabled = false;
};

} // namespace evp

#endif

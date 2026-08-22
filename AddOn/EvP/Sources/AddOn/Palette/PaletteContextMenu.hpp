#ifndef GEOMETRYSERVER_PALETTE_PALETTECONTEXTMENU_HPP
#define GEOMETRYSERVER_PALETTE_PALETTECONTEXTMENU_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "DGModule.hpp"
// DGModule.hpp does NOT pull these in — the menu classes are not part of the DG
// item module. See docs/architecture/api/RMB-CONTEXT-MENU-LIMITS.md §3.
#include "DGCommandDescriptor.hpp"
#include "DGMenu.hpp"
#include "DGMenuItem.hpp"

#include "Python/CommandCatalog.hpp" // CommandInfo — the command's declared entries

#include <vector>

namespace evp {

// The palette's right-click menu: the built-in entries every command gets, plus
// whatever the SELECTED command declared with @tapioca.menu, filtered to the
// region of the palette that was clicked.
//
// WHY IT IS A SUB-OBJECT: the shell owns the DG events, so the three handlers stay
// ControlPalette methods (ControlPaletteMenu.cpp). What lives here is the MENU —
// its commands, its labels, the descriptor pool, the region filter, and the one
// place that translates a DG command id back into something the shell can act on.
// The shell resolves WHERE the click landed and asks; this object never drives it.
//
// ⚠️ THREE HAZARDS, all from DG:
//
//   * `Display` is BLOCKING and MODAL. It runs its own event loop and does not
//     return until the user picks or dismisses. Everything Archicad-facing in this
//     add-on goes through MainThreadGate, so a menu left open while a command runs
//     would hold the gate for as long as it is on screen. The shell therefore
//     refuses to open the menu at all while a run is in flight — see the guard in
//     ControlPaletteMenu.cpp. That is the whole of the main-thread story: this
//     object is never reached during a run.
//
//   * `DG::CommandTable` is `GS::HashTable<DG::Command, DG::CommandDescriptor*>` —
//     RAW OWNED POINTERS, and constructing a CommandDescriptor also registers it in
//     a DG-wide static table. So descriptors are POOLED, not rebuilt: one per slot,
//     created on first use and RELABELLED with SetText afterwards. The pen swatches
//     in ParamPanel borrow a pool for the same reason. What IS rebuilt per click is
//     the DG::Menu — a local, cheap, and the only part that actually changes.
//
//   * `DG::Menu` exposes Add/Insert but no clear, which is the second half of why
//     the menu is a local built per display rather than a member mutated in place.
//
// No resource is needed. DG::Command is a plain (id, developerId, productId) value
// and CommandDescriptor takes its label as a GS::UniString, so a label read out of
// a command's Python source becomes a menu entry with nothing compiled in between.
class PaletteContextMenu {
  public:
    // What the user picked, in the shell's terms rather than DG's.
    enum class Choice {
        None,   // dismissed, or nothing to do
        Run,    // built-in: run the selected command
        Rescan, // built-in: re-scan the command folders
        Hide,   // built-in: close the palette
        Action  // the command's own entry — `action` names which
    };

    struct Result {
        Choice choice = Choice::None;
        // Choice::Action only: the name run_action takes, straight from
        // CommandInfo::menuItems. Empty for every built-in.
        GS::UniString action;
    };

    PaletteContextMenu ();
    ~PaletteContextMenu ();

    // Show the menu at `at` — SCREEN coordinates, which is what DG::ContextMenu
    // wants — and return what was chosen.
    //
    // `info` is the selected command, or null when nothing is selected; `region` is
    // where the click landed, in the vocabulary @tapioca.menu declares against
    // ("panel", "params", "param:<name>", "commands", "results"). `runReady` is the
    // shell's own run gate (WhatIsMissing ().IsEmpty ()): a Run entry enabled while
    // the command cannot run would be the Run button's old bug in a new place.
    Result Display (const DG::NativePoint& at, bool runReady, const CommandInfo* info, const GS::UniString& region);

    // Does an entry DECLARED for `declared` appear over a click in `clicked`?
    // Public and static because it is the contract between @tapioca.menu's region
    // vocabulary and what the palette resolves — the one rule worth testing on its
    // own, and worth reading without the rest of this class.
    //
    //   "panel"        everywhere
    //   "params"       over any generated control — so it also matches "param:x"
    //   "param:<name>" over that one control only
    //   "commands" / "results"   exact
    static bool ShowsIn (const GS::UniString& declared, const GS::UniString& clicked);

  private:
    // The pooled descriptor for command entry `index`, relabelled to `label`.
    // Grows the pool on demand and never shrinks it: a handful of descriptors for
    // the lifetime of the palette is cheaper than churning DG's static table.
    DG::Command EntryCommand (USize index, const GS::UniString& label);

    // The built-ins. Created once, in the constructor.
    DG::Command runCommand;
    DG::Command rescanCommand;
    DG::Command hideCommand;

    // Command-declared entries, one slot per menu position ever needed.
    std::vector<DG::Command> entryCommands;

    // The labels and the enabled state for all of the above. Owns its
    // DG::CommandDescriptor* values.
    DG::CommandTable commands;
};

} // namespace evp

#endif

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

namespace evp {

// The palette's right-click menu — the whole of it, built once and shown on demand.
//
// WHY IT IS A SUB-OBJECT: the shell owns the DG event, so the handler stays a
// ControlPalette method (ControlPaletteMenu.cpp). What lives here is the MENU: its
// commands, its labels, the descriptor table, and the one place that translates a
// DG command id back into something the shell can act on. The shell asks for a
// Choice and acts; this object never drives the shell.
//
// ⚠️ TWO HAZARDS, both from DG:
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
//     RAW OWNED POINTERS. The descriptors are built once in the constructor and
//     deleted in the destructor, which is also why the menu is not rebuilt per
//     click: a per-click rebuild is a per-click new/delete of every entry, for a
//     menu whose shape does not change.
//
// No resource is needed. DG::Command is a plain (id, developerId, productId) value
// and CommandDescriptor takes its label as a GS::UniString, so the labels are
// literals here exactly as every other palette label in this folder is.
class PaletteContextMenu {
  public:
    // What the user picked, in the shell's terms rather than DG's. `None` covers
    // both "dismissed" and "nothing to do".
    enum class Choice { None, Run, Rescan, Hide };

    PaletteContextMenu ();
    ~PaletteContextMenu ();

    // Show the menu at `at` — SCREEN coordinates, which is what DG::ContextMenu
    // wants — and return what was chosen. `runReady` is the shell's own run gate
    // (WhatIsMissing ().IsEmpty ()): a Run entry that is enabled while the command
    // cannot run would be the Run button's old bug in a new place.
    Choice Display (const DG::NativePoint& at, bool runReady);

  private:
    // Built in the constructor, in this order, and never rebuilt. `menu` holds
    // items that reference the commands, so the commands are declared first —
    // members are destroyed in reverse declaration order.
    DG::Command runCommand;
    DG::Command rescanCommand;
    DG::Command hideCommand;

    DG::Menu menu;

    // The labels and the enabled state. Owns its DG::CommandDescriptor* values.
    DG::CommandTable commands;
};

} // namespace evp

#endif

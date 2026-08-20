#ifndef GEOMETRYSERVER_PALETTE_CATALOGPICKER_HPP
#define GEOMETRYSERVER_PALETTE_CATALOGPICKER_HPP

// The evp.LibraryPart / evp.Favourite parameter kinds, end to end: enumerate the
// catalogue, run Palette/CatalogBrowser over it, and turn the row the user chose
// into the control's value.
//
// Its own file rather than another branch inside ParamPanel because it is the
// only parameter kind that has to BUILD A CATALOGUE — thousands of library parts
// or a project's whole favourites tree — and because ParamPanel was already at
// the soft cap. ParamPanel still creates the button and reads the value back;
// this owns everything between the click and the value.
//
// NOT a ParamPanel method: it touches one ParamControl and no panel state.

namespace evp {

struct ParamControl;

// Run the picker for `pc` (which must be Kind::Catalog) and, unless the user
// cancels, replace its value and its button text TOGETHER — a button showing one
// part's name while sending another part's unID is the failure with no symptom.
//
// MAIN THREAD ONLY, and it BLOCKS for human time. Call it straight from a DG
// button handler, never through MainThreadGate — a modal held across the gate
// reports a false timeout (see the gate's contract).
void OpenCatalogBrowser (ParamControl& pc);

} // namespace evp

#endif

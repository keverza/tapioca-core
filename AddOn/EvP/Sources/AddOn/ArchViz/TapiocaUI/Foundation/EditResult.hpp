#ifndef EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_EDITRESULT_HPP
#define EVP_ARCHVIZ_TAPIOCAUI_FOUNDATION_EDITRESULT_HPP

// TapiocaUI/Foundation/EditResult -- what one frame of an editing control says to its caller.
// Every TapiocaUI widget returns it, so a feature handles a slider, a typed number and a
// world-anchored label the same way: preview on `changed`, record the undo step and run the
// expensive work on `committed`, put back the value captured at the edit's start on `canceled`.
//
// ⚠️ A DRAG SAYS `changed` EVERY FRAME AND `committed` ONCE, ON RELEASE; A TEXT FIELD NEVER
// COMMITS PER KEYSTROKE. Today's controls disagree on this (HANDOFF-TapiocaUI-Library.md
// section 2.2); this type is the one answer the library converges them on.

namespace geomsrv {
namespace archviz {
namespace ui {

struct EditResult {
    bool changed = false;   // live preview: update virtual geometry or GPU state
    bool committed = false; // end of the edit: record undo, run ACAPI work
    bool canceled = false;  // restore the value captured when the edit began

    bool Any () const
    {
        return changed || committed || canceled;
    }
};

} // namespace ui
} // namespace archviz
} // namespace geomsrv

#endif

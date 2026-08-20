#ifndef GEOMETRYSERVER_PALETTE_PALETTEPLACEMENT_HPP
#define GEOMETRYSERVER_PALETTE_PALETTEPLACEMENT_HPP

// Where the palette sits and how its bands are divided, persisted to
// %LOCALAPPDATA%\EvP\palette.json.
//
// This is file IO and validation ONLY — no DG, no items, no observer. It is not a
// palette sub-object (nothing here places itself); the shell asks for a struct and
// decides what to do with it. That is the whole reason it is its own unit: the
// schema grows (scroll offset, last filter) and every field it gains would
// otherwise be another line in a shell that has none to spare.
//
// Archicad does not restore an add-on palette's placement for us, and the .grc
// position is a fixed 0,0 — the top-left of monitor 1, wherever Archicad itself
// happens to be.

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace evp {

struct PalettePlacement {
    // 0 means "not saved, or not usable" for every size field, so the caller keeps
    // its own default instead of getting a wedged palette out of a stale file.
    short width         = 0;
    short height        = 0;
    short listHeight    = 0;
    short resultsHeight = 0;
    // PLAT-F13 — the description band. `descriptionHeight` is the TEXT height,
    // excluding the fold header. `descriptionCollapsed` is a real preference,
    // not a size, so unlike every field above it 0/false is a meaningful value
    // and not "unset" — a user who folded the description away meant it.
    short descriptionHeight    = 0;
    bool  descriptionCollapsed = false;

    // Position is trusted verbatim when present: it is where the user last put the
    // palette, on whichever monitor. If that monitor is gone, Archicad clamps it
    // back on screen for us — better than second-guessing a multi-monitor layout.
    short left        = 0;
    short top         = 0;
    bool  hasPosition = false;
};

void SavePalettePlacement (const PalettePlacement& placement);

// Reads AND validates. The minimums are the same ones the splitter drags clamp
// to, so a hand-edited or stale file can never wedge a band open too far or shut
// too small; anything failing them comes back as 0.
PalettePlacement LoadPalettePlacement (short minListHeight, short minResultsHeight,
                                       short minDescriptionHeight);

}   // namespace evp

#endif

#ifndef EVP_PLANOVERLAY_OVERLAYBINDING_HPP
#define EVP_PLANOVERLAY_OVERLAYBINDING_HPP

// WHICH Archicad window an overlay belongs to, and what its tracking tick should
// do when some other window is in front of it.
//
// ⚠️ AN OVERLAY IS BOUND TO A WINDOW, NOT TO "WHEREVER THE USER IS". Binding it
// to whatever is focused would make what the overlay shows depend on where
// someone last clicked -- the same hidden state the preview surface rules
// (GhPreviewProtocol.hpp, PreviewSurface) exist to keep off this path. Archicad's
// floor plan, its 3D window, a section and a schedule are different projections
// with different units and different ideas of what a line's width means; plan
// geometry drawn over a schedule is not a degraded picture, it is a wrong one.
//
// ⚠️ AND THE COST OF GETTING IT WRONG IS NOT ONLY A WRONG PICTURE. The tracking
// tick is a poll: it calls ACAPI_View_PointToCoord four times and repaints a
// layered window. Left running while a schedule, a section or a layout is in
// front, it keeps paying that on Archicad's UI thread for a window nobody can
// see, forever -- which is what "opening a schedule hoards overlay resources"
// is. Suspending is therefore about cost first and correctness second.
//
// Pure, DevKit-free and Win32-free, so the rule can be pinned by a test: it
// decides from two identities and a visibility flag, and OverlayWindow does what
// it is told. That matters because every failure here is silent -- an overlay
// that keeps polling looks exactly like one that does not, until someone
// profiles the UI thread.

#include <cstdint>

namespace geomsrv {
namespace planoverlay {

// One Archicad window/database, reduced to what identifies it.
//
// Mirrors the fields of API_WindowInfo that the DevKit says are enough:
// "In the case of Floor Plan and 3D Model databases, the typeID field is enough
// to identify them"; custom windows add `index`; sections, details, worksheets
// and layouts are identified by `databaseUnId`. All three are carried here so
// ONE comparison serves every window type -- a per-type rule is wrong for
// whichever type nobody thought about, and the two sections that share a typeId
// are exactly that case.
struct OverlayWindowId {
    // API_WindowTypeID, widened. 0 means "no window", which is what a failed
    // read reports and is never equal to a real one.
    uint32_t typeId = 0;
    int32_t index = 0;
    // API_DatabaseUnId is one API_Guid, and API_Guid is sixteen bytes of POD.
    // Carried as two words so this header needs no DevKit type and the
    // comparison is a plain value compare.
    uint64_t databaseGuidHigh = 0;
    uint64_t databaseGuidLow = 0;

    bool Known () const
    {
        return typeId != 0;
    }

    bool operator== (const OverlayWindowId& other) const
    {
        return typeId == other.typeId && index == other.index && databaseGuidHigh == other.databaseGuidHigh &&
               databaseGuidLow == other.databaseGuidLow;
    }

    bool operator!= (const OverlayWindowId& other) const
    {
        return !(*this == other);
    }
};

// What one tracking tick should do.
struct OverlayTrackDecision {
    // Derive the transform and repaint. False means the tick costs nothing --
    // no ACAPI, no projection, no RedrawWindow.
    bool track = false;
    // The visibility the overlay should have after this tick.
    bool visible = false;
    // Whether that differs from the visibility it has now, so the caller can
    // avoid a ShowWindow per tick.
    bool visibilityChanged = false;
};

// `bound` is the window the overlay was opened over; `current` is what Archicad
// reports as active right now. An unreadable current window arrives as a default
// (`Known()` false) and SUSPENDS: failing to read which window is in front is
// exactly the moment not to project geometry into it.
//
// An overlay with no binding at all (`bound.Known()` false) keeps tracking. That
// is the pre-binding behaviour, kept deliberately so that a caller which never
// records a window is not silently blanked -- the failure would look like the
// overlay being broken rather than unbound.
OverlayTrackDecision DecideTrackTick (const OverlayWindowId& bound, const OverlayWindowId& current,
                                      bool currentlyVisible);

} // namespace planoverlay
} // namespace geomsrv

#endif

// The palette's placement: the file it is persisted in, and the shell's two
// methods that read and write its own geometry.
//
// ⚠️ BOTH HALVES OF ONE CONCERN, DELIBERATELY. The struct and its JSON were
// always here; ControlPalette::SavePlacement and RestorePlacement lived in the
// shell and were the only callers, so the shell carried thirty-five lines of
// "which band height goes in which field" that nothing else could read without
// opening this file anyway. Moving them here is the same trade every earlier
// extraction made (check_cpp.py records the list), and it is what paid for the
// Grasshopper band: the shell's line budget only ever goes down.
//
// The definitions stay ControlPalette::* so the header and every caller are
// unchanged; only the file they live in moved.

#include "Palette/ControlPalette.hpp"
#include "Palette/PalettePlacement.hpp"
#include "Python/PathUtils.hpp" // EvpDataDir, ReadTextFile, WriteTextFile

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

namespace {

GS::UniString PlacementFilePath ()
{
    return evp::EvpDataDir () + GS::UniString ("\\palette.json");
}

} // namespace

namespace evp {

void SavePalettePlacement (const PalettePlacement& p)
{
    // Hand-written rather than built through ObjectState: it is six integers, and a
    // failed write is not worth reporting — the palette still works, it just opens
    // where it opened last time.
    GS::UniString error;
    WriteTextFile (PlacementFilePath (),
                   GS::UniString::Printf ("{\"left\":%d,\"top\":%d,\"width\":%d,\"height\":%d,"
                                          "\"listHeight\":%d,\"resultsHeight\":%d,"
                                          "\"descriptionHeight\":%d,\"descriptionCollapsed\":%s,"
                                          "\"previewsEnabled\":%s}",
                                          (int) p.left, (int) p.top, (int) p.width, (int) p.height, (int) p.listHeight,
                                          (int) p.resultsHeight, (int) p.descriptionHeight,
                                          p.descriptionCollapsed ? "true" : "false",
                                          p.previewsEnabled ? "true" : "false"),
                   error);
}

PalettePlacement LoadPalettePlacement (short minListHeight, short minResultsHeight, short minDescriptionHeight)
{
    PalettePlacement p;

    GS::UniString text;
    if (!ReadTextFile (PlacementFilePath (), text))
        return p; // first run — every field stays 0

    GS::ObjectState os;
    if (JSON::ConvertToObjectState (text, os) != NoError)
        return p;

    GS::Int32 left = 0, top = 0, width = 0, height = 0, listH = 0, tableH = 0;

    // 200x200 is the "did someone save a collapsed window" floor: below it the
    // palette would open with no room for the command list at all.
    if (os.Get ("width", width) && os.Get ("height", height) && width > 200 && height > 200) {
        p.width = (short) width;
        p.height = (short) height;
    }
    if (os.Get ("listHeight", listH) && listH >= minListHeight)
        p.listHeight = (short) listH;
    if (os.Get ("resultsHeight", tableH) && tableH >= minResultsHeight)
        p.resultsHeight = (short) tableH;

    GS::Int32 descH = 0;
    if (os.Get ("descriptionHeight", descH) && descH >= minDescriptionHeight)
        p.descriptionHeight = (short) descH;
    // Read WITHOUT a floor: false is the default and a legitimate saved value,
    // so there is nothing here that "failing validation" could mean.
    os.Get ("descriptionCollapsed", p.descriptionCollapsed);
    os.Get ("previewsEnabled", p.previewsEnabled);

    if (os.Get ("left", left) && os.Get ("top", top)) {
        p.left = (short) left;
        p.top = (short) top;
        p.hasPosition = true;
    }

    return p;
}

} // namespace evp

void ControlPalette::SavePlacement () const
{
    const DG::NativePoint position = GetClientPosition ();
    evp::PalettePlacement p;
    p.left = (short) position.GetX ().GetValue ();
    p.top = (short) position.GetY ().GetValue ();
    p.width = GetWidth ();
    p.height = GetHeight ();
    p.listHeight = commandsPanel.Height ();
    p.resultsHeight = results.Height ();
    p.descriptionHeight = description.Height ();
    p.descriptionCollapsed = description.IsCollapsed ();
    p.previewsEnabled = preview.IsEnabled ();
    evp::SavePalettePlacement (p);
}

void ControlPalette::RestorePlacement ()
{
    // 0 means the file had nothing usable for that field, so each default survives.
    const evp::PalettePlacement p = evp::LoadPalettePlacement (
        evp::CommandListPanel::MinHeight, evp::ResultsTable::MinHeight, evp::DescriptionPanel::MinHeight);

    if (p.width > 0)
        SetClientSize (p.width, p.height);
    if (p.listHeight > 0)
        commandsPanel.SetHeight (p.listHeight);
    if (p.resultsHeight > 0)
        results.SetHeight (p.resultsHeight);
    if (p.descriptionHeight > 0)
        description.SetHeight (p.descriptionHeight);
    // No `> 0` guard: false is a real saved value, not "unset".
    description.SetCollapsed (p.descriptionCollapsed);
    preview.SetEnabled (p.previewsEnabled);
    if (p.hasPosition)
        SetClientPosition (DG::NativeUnit (p.left), DG::NativeUnit (p.top));
}

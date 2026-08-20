#include "Palette/PalettePlacement.hpp"
#include "Python/PathUtils.hpp"        // EvpDataDir, ReadTextFile, WriteTextFile

#include "ObjectState.hpp"
#include "ObjectStateJSONConversion.hpp"

namespace {

GS::UniString PlacementFilePath ()
{
    return evp::EvpDataDir () + GS::UniString ("\\palette.json");
}

}   // namespace

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
                                          "\"descriptionHeight\":%d,\"descriptionCollapsed\":%s}",
                                          (int) p.left, (int) p.top,
                                          (int) p.width, (int) p.height,
                                          (int) p.listHeight, (int) p.resultsHeight,
                                          (int) p.descriptionHeight,
                                          p.descriptionCollapsed ? "true" : "false"),
                   error);
}

PalettePlacement LoadPalettePlacement (short minListHeight, short minResultsHeight,
                                       short minDescriptionHeight)
{
    PalettePlacement p;

    GS::UniString text;
    if (!ReadTextFile (PlacementFilePath (), text))
        return p;                       // first run — every field stays 0

    GS::ObjectState os;
    if (JSON::ConvertToObjectState (text, os) != NoError)
        return p;

    GS::Int32 left = 0, top = 0, width = 0, height = 0, listH = 0, tableH = 0;

    // 200x200 is the "did someone save a collapsed window" floor: below it the
    // palette would open with no room for the command list at all.
    if (os.Get ("width", width) && os.Get ("height", height) && width > 200 && height > 200) {
        p.width  = (short) width;
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

    if (os.Get ("left", left) && os.Get ("top", top)) {
        p.left        = (short) left;
        p.top         = (short) top;
        p.hasPosition = true;
    }

    return p;
}

}   // namespace evp

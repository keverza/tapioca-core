#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CommandUtils.hpp"

#include <vector>
#include "Diagnostics/ApiError.hpp" // EVP_ACAPI_FAIL -- see the two lookups below

#include "Geometry/MeshStore.hpp"
#include "Metadata/MetadataStore.hpp"
#include "Screenshot/ScreenshotStore.hpp"

namespace geomsrv {

bool AttributeNameToIndex (API_AttrTypeID type, const GS::UniString& name, API_AttributeIndex& index)
{
    GS::Array<API_Attribute> attributes;
    if (const GSErrCode err = ACAPI_Attribute_GetAttributesByType (type, attributes); err != NoError) {
        EVP_ACAPI_FAIL ("ACAPI_Attribute_GetAttributesByType", err,
                        GS::UniString ("resolving attribute name \"") + name + "\" to an index");
        return false; // the caller reports "not found: <name>"; the REAL reason is now in the trail
    }
    for (const API_Attribute& attribute : attributes) {
        if (GS::UniString (attribute.header.name) == name) {
            index = attribute.header.index;
            return true;
        }
    }
    return false;
}

GS::UniString AttributeIndexToName (API_AttrTypeID type, const API_AttributeIndex& index)
{
    GS::Array<API_Attribute> attributes;
    if (const GSErrCode err = ACAPI_Attribute_GetAttributesByType (type, attributes); err != NoError) {
        EVP_ACAPI_FAIL ("ACAPI_Attribute_GetAttributesByType", err, "resolving an attribute index back to its name");
        return GS::UniString ();
    }
    for (const API_Attribute& attribute : attributes) {
        if (attribute.header.index == index)
            return GS::UniString (attribute.header.name);
    }
    return GS::UniString ();
}

// Is the layer at `index` hidden or locked? Reported as a REFUSAL rather than
// letting the create proceed — see the long note in ResolveLayerParam.
//
// A read failure here is deliberately NOT fatal: if we cannot tell, we let the
// create attempt proceed and Archicad decide. Refusing on a failed *check* would
// turn a diagnostic into an outage.
bool LayerBlocksCreation (const API_AttributeIndex& index, GS::UniString& reason)
{
    API_Attribute attribute = {};
    attribute.header.typeID = API_LayerID;
    attribute.header.index = index;
    if (const GSErrCode err = ACAPI_Attribute_Get (&attribute); err != NoError) {
        EVP_ACAPI_FAIL ("ACAPI_Attribute_Get", err, "checking whether the target layer is hidden or locked");
        return false;
    }

    const GS::UniString name (attribute.header.name);
    const bool hidden = (attribute.header.flags & APILay_Hidden) != 0;
    const bool locked = (attribute.header.flags & APILay_Locked) != 0;

    if (hidden && locked)
        reason = GS::UniString::Printf ("layer \"%T\" is HIDDEN and LOCKED", name.ToPrintf ());
    else if (hidden)
        reason = GS::UniString::Printf ("layer \"%T\" is HIDDEN", name.ToPrintf ());
    else if (locked)
        reason = GS::UniString::Printf ("layer \"%T\" is LOCKED", name.ToPrintf ());
    return hidden || locked;
}

bool ResolveLayerParam (const GS::ObjectState& params, API_Elem_Head& head, GS::UniString& err)
{
    GS::UniString layerName;
    const bool named = params.Get ("layer", layerName) && !layerName.IsEmpty ();

    if (named) {
        API_AttributeIndex index;
        if (!AttributeNameToIndex (API_LayerID, layerName, index)) {
            err = EVP_FAIL (GS::UniString::Printf ("layer not found: %T", layerName.ToPrintf ()),
                            "resolving the `layer` parameter of a create command");
            return false;
        }
        head.layer = index;
    }
    // If the caller named no layer, head.layer is still whatever GetDefaults
    // chose — the user's current default layer, which can just as easily be
    // hidden. So the check below runs EITHER WAY: the failure mode it exists to
    // stop does not care whose choice the layer was.

    // THE REFUSAL THE USER ASKED FOR. Archicad will not place an element on a
    // hidden layer, and a locked layer cannot be edited — but the failure is
    // near-invisible: the create returns an error code with no layer in it, or
    // (worse, on some paths) appears to succeed while nothing shows up. Either
    // way the script reports a number and the user sees an empty drawing, with
    // no hint that a layer is switched off. So refuse HERE, before the create,
    // and name the layer and the reason.
    GS::UniString reason;
    if (LayerBlocksCreation (head.layer, reason)) {
        err = EVP_FAIL (
            reason + (named ? GS::UniString (
                                  " - nothing can be placed on it. Show/unlock the layer, or pass a different `layer`.")
                            : GS::UniString (" - nothing can be placed on it. This is the CURRENT DEFAULT layer (the "
                                             "command named none): "
                                             "show/unlock it, or pass an explicit `layer`.")),
            "checking the target layer before a create command");
        return false;
    }
    return true;
}

bool ParseAnchor (const GS::UniString& name, API_AnchorID& anchor)
{
    if (name == "topLeft") {
        anchor = APIAnc_LT;
        return true;
    }
    if (name == "topCenter") {
        anchor = APIAnc_MT;
        return true;
    }
    if (name == "topRight") {
        anchor = APIAnc_RT;
        return true;
    }
    if (name == "middleLeft") {
        anchor = APIAnc_LM;
        return true;
    }
    if (name == "middleCenter") {
        anchor = APIAnc_MM;
        return true;
    }
    if (name == "middleRight") {
        anchor = APIAnc_RM;
        return true;
    }
    if (name == "bottomLeft") {
        anchor = APIAnc_LB;
        return true;
    }
    if (name == "bottomCenter") {
        anchor = APIAnc_MB;
        return true;
    }
    if (name == "bottomRight") {
        anchor = APIAnc_RB;
        return true;
    }
    return false;
}

bool IsPlaceableViewItem (API_NavigatorItemTypeID itemType)
{
    switch (itemType) {
        case API_StoryNavItem:
        case API_SectionNavItem:
        case API_ElevationNavItem:
        case API_InteriorElevationNavItem:
        case API_DetailDrawingNavItem:
        case API_WorksheetDrawingNavItem:
        // ⚠️ API_DocumentFrom3DNavItem = 23 was MISSING from the first version of
        // this list, and it caused a live failure: EvP.ListViews offered a view
        // called "Perspective" and PlaceDrawingFromView then refused it as "not
        // something a Drawing can source from". A 3D Document is a 2D document —
        // it is one of the most obviously placeable sources there is.
        // The cause is worth remembering because it was a TOOLING failure, not a
        // reading failure: the enum was grepped with `API_[A-Za-z]*NavItem`, which
        // cannot span the `3` in `DocumentFrom3D`, so the value never appeared in
        // the output being read from. When grepping an enum, print the whole block
        // and check the numbering is contiguous — 22 to 24 with no 23 was visible
        // in that output and went unnoticed.
        case API_DocumentFrom3DNavItem:
        case API_PerspectiveNavItem:
        case API_AxonometryNavItem:
        case API_ScheduleNavItem:
        case API_ListNavItem:
        case API_TextListNavItem:
        case API_TocNavItem:
            return true;
        default:
            // Folders, the project root, cameras/camera sets, the layout-book
            // types and anything new default to NOT placeable. Refusing an
            // unknown type is the safe direction: the failure mode for getting
            // this wrong is an empty drawing frame the user has to hunt down.
            return false;
    }
}

// A navigator item type as a name a picker row or a script can read. Only the
// types a Drawing can actually source from are spelled out; the rest report the
// raw enum value, which is enough to diagnose "why is my item not placeable".
//
// ⚠️ API_DocumentFrom3DNavItem is in this list for the reason spelled out on
// IsPlaceableViewItem above: grepping the enum with a character class that cannot
// span the `3` hid it once already, and it cost a live failure.
GS::UniString NavItemTypeName (API_NavigatorItemTypeID itemType)
{
    switch (itemType) {
        case API_StoryNavItem:
            return "story";
        case API_SectionNavItem:
            return "section";
        case API_ElevationNavItem:
            return "elevation";
        case API_InteriorElevationNavItem:
            return "interiorElevation";
        case API_DetailDrawingNavItem:
            return "detail";
        case API_WorksheetDrawingNavItem:
            return "worksheet";
        case API_DocumentFrom3DNavItem:
            return "3dDocument";
        case API_DrawingNavItem:
            return "drawing";
        case API_SubSetNavItem:
            return "subset";
        case API_LayoutNavItem:
            return "layout";
        case API_MasterLayoutNavItem:
            return "masterLayout";
        case API_PerspectiveNavItem:
            return "perspective";
        case API_AxonometryNavItem:
            return "axonometry";
        case API_ScheduleNavItem:
            return "schedule";
        case API_ListNavItem:
            return "list";
        case API_TextListNavItem:
            return "textList";
        case API_TocNavItem:
            return "toc";
        case API_CameraNavItem:
            return "camera";
        case API_CameraSetNavItem:
            return "cameraSet";
        case API_FolderNavItem:
            return "folder";
        case API_ProjectNavItem:
            return "project";
        default:
            return GS::UniString::Printf ("type%d", (int) itemType);
    }
}

bool CollectNavigatorItems (API_NavigatorMapID mapId, GS::Array<NavigatorEntry>& items)
{
    API_NavigatorSet set = {};
    set.mapId = mapId;
    if (const GSErrCode err = ACAPI_Navigator_GetNavigatorSet (&set); err != NoError) {
        EVP_ACAPI_FAIL ("ACAPI_Navigator_GetNavigatorSet", err, "listing a Navigator map to resolve an item name");
        return false;
    }

    // Iterative, not recursive: the depth cap and the visited set are the point.
    // This runs on the main thread, so a cycle in the tree would hang ARCHICAD,
    // not just this command.
    //
    // `visited` is a linear-scanned Array rather than a HashSet on purpose:
    // API_Guid has no GenerateHashValue in the AC29 headers, and a navigator map
    // is tens of items, so O(n^2) here is free and needs no hash to exist.
    struct Pending {
        API_Guid guid;
        GS::UniString path;
        Int32 depth;
    };
    GS::Array<Pending> queue;
    GS::Array<API_Guid> visited;
    const Int32 maxDepth = 32;

    Pending root;
    root.guid = set.rootGuid;
    root.depth = 0;
    queue.Push (root);
    visited.Push (set.rootGuid);

    while (!queue.IsEmpty ()) {
        const Pending current = queue.Pop ();
        if (current.depth >= maxDepth)
            continue;

        API_NavigatorItem parent = {};
        parent.guid = current.guid;
        parent.mapId = mapId; // set for performance, per the header

        GS::Array<API_NavigatorItem> children;
        if (ACAPI_Navigator_GetNavigatorChildrenItems (&parent, &children) != NoError)
            continue; // a childless node answers an error; not fatal

        for (const API_NavigatorItem& child : children) {
            if (visited.Contains (child.guid))
                continue;
            visited.Push (child.guid);

            NavigatorEntry entry;
            entry.guid = child.guid;
            entry.name = GS::UniString (child.uName);
            entry.path = current.path;
            entry.itemType = child.itemType;
            entry.mapId = mapId;
            items.Push (entry);

            Pending next;
            next.guid = child.guid;
            next.path = current.path.IsEmpty () ? entry.name : current.path + "/" + entry.name;
            next.depth = current.depth + 1;
            queue.Push (next);
        }
    }
    return true;
}

bool ResolveStory (double z, bool haveFloor, GS::Int32 requestedFloor, short& floorInd, double& offset,
                   GS::UniString& err)
{
    API_StoryInfo info = {};
    if (const GSErrCode storyErr = ACAPI_ProjectSetting_GetStorySettings (&info);
        storyErr != NoError || info.data == nullptr) {
        err =
            (storyErr != NoError)
                ? EVP_ACAPI_FAIL ("ACAPI_ProjectSetting_GetStorySettings", storyErr, "resolving the story for a create")
                : EVP_FAIL (GS::UniString (
                                "ACAPI_ProjectSetting_GetStorySettings reported success but returned no story data"),
                            "resolving the story for a create");
        return false;
    }
    const short count = info.lastStory - info.firstStory + 1;
    bool found = false;
    double bestLevel = 0.0;
    short bestInd = 0;
    for (short i = 0; i < count; ++i) {
        const API_StoryType& s = (*info.data)[i];
        if (haveFloor) {
            if (s.index == requestedFloor) {
                bestInd = s.index;
                bestLevel = s.level;
                found = true;
                break;
            }
        }
        else if (s.level <= z + 1e-6 && (!found || s.level > bestLevel)) {
            bestInd = s.index;
            bestLevel = s.level;
            found = true;
        }
    }
    if (!found && !haveFloor && count > 0) { // z below every story -> the lowest one
        const API_StoryType& s = (*info.data)[0];
        bestInd = s.index;
        bestLevel = s.level;
        found = true;
    }
    BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    if (!found) {
        err = haveFloor ? GS::UniString::Printf ("no story with index %d", (int) requestedFloor)
                        : GS::UniString ("no stories in project");
        return false;
    }
    floorInd = bestInd;
    offset = z - bestLevel;
    return true;
}

bool WalkPolygonRings (const PolygonHandles& polygon, const double* polyZ, GS::Array<double>& outerCoords,
                       GS::Array<double>& outerArcs, GS::Int32& outerCount, GS::Array<double>& holeCoords,
                       GS::Array<double>& holeArcs, GS::Array<GS::Int32>& holeCounts, GS::Int32& nHoles,
                       bool polylineMode, bool* outerClosed, GS::Array<double>* outerZ, GS::Array<double>* holeZ)
{
    if (outerClosed != nullptr)
        *outerClosed = false;

    outerCount = 0;
    nHoles = 0;

    if (polygon.coords == nullptr || polygon.pends == nullptr)
        return false;

    API_Coord** srcCoords = polygon.coords;
    Int32** srcPends = polygon.pends;
    API_PolyArc** srcParcs = polygon.parcs;

    bool ok = false;

    const Int32 nSub = (Int32) (BMGetHandleSize ((GSHandle) srcPends) / sizeof (Int32)) - 1;
    const Int32 nArcs =
        (srcParcs != nullptr) ? (Int32) (BMGetHandleSize ((GSHandle) srcParcs) / sizeof (API_PolyArc)) : 0;

    // Arc angle of the edge leaving the vertex at coord index `begIdx`, or 0.
    // Polygons carry few arcs, so a linear scan per edge is cheap and allocation-free.
    auto arcAt = [&] (Int32 begIdx) -> double {
        for (Int32 a = 0; a < nArcs; ++a)
            if ((*srcParcs)[a].begIndex == begIdx)
                return (*srcParcs)[a].arcAngle;
        return 0.0;
    };

    for (Int32 sub = 1; sub <= nSub; ++sub) {
        const Int32 start = (*srcPends)[sub - 1] + 1; // pends[0] == 0
        const Int32 end = (*srcPends)[sub];           // last coord of this contour

        // Polygon: `end` IS the closing repeat, always. Polyline: it is a repeat only
        // if it really duplicates the first node — that test is the `closed` answer.
        // The repeat is a literal copy, so compare exactly; an epsilon here would
        // swallow a genuinely short last segment.
        const bool repeats = polylineMode ? (end > start && (*srcCoords)[end].x == (*srcCoords)[start].x &&
                                             (*srcCoords)[end].y == (*srcCoords)[start].y)
                                          : true;
        const Int32 stop = repeats ? end : end + 1; // one past the last kept vertex
        const Int32 distinct = stop - start;

        // >= 2 (not 3): a circle is stored as 2 nodes + two 180-deg arcs, and the
        // Python side reconstructs its area/perimeter from the arc angles.
        if (sub == 1) {
            if (distinct < 2)
                break; // no usable outer -> no holes
            for (Int32 i = start; i < stop; ++i) {
                outerCoords.Push ((*srcCoords)[i].x);
                outerCoords.Push ((*srcCoords)[i].y);
                outerArcs.Push (arcAt (i));
                if (outerZ != nullptr && polyZ != nullptr)
                    outerZ->Push (polyZ[i]);
            }
            outerCount = distinct;
            ok = true;
            if (outerClosed != nullptr)
                *outerClosed = repeats;
            if (polylineMode)
                break; // one contour; no holes
        }
        else if (distinct >= 2) { // skip a degenerate hole
            for (Int32 i = start; i < stop; ++i) {
                holeCoords.Push ((*srcCoords)[i].x);
                holeCoords.Push ((*srcCoords)[i].y);
                holeArcs.Push (arcAt (i));
                if (holeZ != nullptr && polyZ != nullptr)
                    holeZ->Push (polyZ[i]);
            }
            holeCounts.Push (distinct);
            ++nHoles;
        }
    }
    return ok;
}

size_t RetainedBytes ()
{
    return MeshStore::Get ().Bytes () + MetadataStore::Get ().Bytes () + ScreenshotStore::Get ().Bytes ();
}

void AddMemory (GS::ObjectState& os)
{
    os.Add ("retainedBytes", static_cast<GS::Int64> (RetainedBytes ()));
}

namespace {

const char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int Base64Value (GS::UniChar::Layout c)
{
    if (c >= 'A' && c <= 'Z')
        return (int) (c - 'A');
    if (c >= 'a' && c <= 'z')
        return (int) (c - 'a') + 26;
    if (c >= '0' && c <= '9')
        return (int) (c - '0') + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

} // namespace

GS::UniString Base64Encode (const std::vector<unsigned char>& bytes)
{
    GS::String encoded;
    encoded.EnsureCapacity ((USize) ((bytes.size () + 2) / 3 * 4 + 1));
    for (size_t i = 0; i < bytes.size (); i += 3) {
        const size_t remaining = bytes.size () - i;
        const uint32_t triple = (uint32_t) bytes[i] << 16 | (uint32_t) (remaining > 1 ? bytes[i + 1] : 0) << 8 |
                                (uint32_t) (remaining > 2 ? bytes[i + 2] : 0);
        encoded += kBase64Alphabet[(triple >> 18) & 0x3F];
        encoded += kBase64Alphabet[(triple >> 12) & 0x3F];
        encoded += remaining > 1 ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=';
        encoded += remaining > 2 ? kBase64Alphabet[triple & 0x3F] : '=';
    }
    return GS::UniString (encoded);
}

bool Base64Decode (const GS::UniString& text, std::vector<unsigned char>& bytes)
{
    bytes.clear ();
    bytes.reserve (text.GetLength () / 4 * 3);

    uint32_t accumulator = 0;
    int held = 0;
    bool padded = false;
    for (UIndex i = 0; i < text.GetLength (); ++i) {
        const GS::UniChar::Layout c = text[i];
        if (c == '=') {
            padded = true;
            continue;
        }
        // ⚠️ WHITESPACE IS TOLERATED, ANYTHING ELSE IS NOT. Transports wrap long
        // strings; a stray letter means the payload is not what the sender meant.
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (padded)
            return false; // data after padding is a malformed payload

        const int value = Base64Value (c);
        if (value < 0)
            return false;

        accumulator = (accumulator << 6) | (uint32_t) value;
        held += 6;
        if (held >= 8) {
            held -= 8;
            bytes.push_back ((unsigned char) ((accumulator >> held) & 0xFF));
        }
    }
    return true;
}

} // namespace geomsrv

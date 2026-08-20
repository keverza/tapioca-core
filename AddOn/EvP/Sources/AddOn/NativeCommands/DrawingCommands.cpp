#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/DrawingCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"      // ResolveLayerParam, ParseAnchor

namespace geomsrv {

namespace {

// ===========================================================================
// The Drawing crop — E20.
//
// A Drawing placed on a layout clips its source to a polygon. After a source
// rearrange, every placed Drawing still shows the region its block USED to
// occupy, and re-pointing it needs a clip-polygon write.
//
// THE JSON API CANNOT DO THIS BY ANY PATH — all three candidates were ruled out
// from source, and one of them lies about it:
//   * Tapir SetDetailsOfElements — its TypeSpecificSettings `oneOf` is Wall +
//     Zone only. Sending a DrawingSettings object returns SUCCESS and silently
//     discards it (confirmed live: the visible region did not change).
//   * CreateDrawings (delete + recreate) — its schema has no clipPolygon and it
//     creates with an empty memo, so a recreated Drawing is UNCLIPPED.
//   * Tapir UpdateDrawings — ACAPI_Drawing_Update_Drawings, a content refresh;
//     it re-renders from the source and never touches the clip.
//
// Native route, from APIdefs_Elements.h: API_DrawingType.poly is an API_Polygon,
// documented as "the clip polygon if isCutWithFrame is true, else the bounding
// box in polygon format", with the isCutWithFrame flag beside it.
//
// ⚠️ THE CAVEAT THIS FILE MUST CARRY: API_DrawingID has NO ROW in the
// ACAPI_Element_Change support table in ACAPinc.h. That table names, per type,
// which fields and memo handles Change can edit — Slab, Roof, Mesh, Zone,
// Hatch, PolyLine, Detail and Worksheet all explicitly list their polygon memo
// handles; Drawing lists nothing at all. So the polygon write here is
// PLAUSIBLE-BUT-UNPROVEN, not documented-supported.
//
// That is precisely why SetDrawingClipPolygon READS THE POLYGON BACK after the
// write and reports `verified` — a NoError from Change is not evidence, given
// that the JSON path already burned us by returning success for a no-op. Do not
// build anything on top of this until a probe shows verified=true on a real
// layout.
// ===========================================================================

// Read a Drawing's element + polygon memo. `found` distinguishes "no such guid"
// from "that guid is not a Drawing", which are different bugs in a caller.
bool GetDrawing (const GS::UniString& guidString, API_Element& element, GS::UniString& err)
{
    element = {};
    element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
    if (const GSErrCode getErr = ACAPI_Element_Get (&element); getErr != NoError) {
        err = EVP_ACAPI_FAIL ("ACAPI_Element_Get", getErr,
                              GS::UniString ("drawing ") + guidString + " - not found, deleted, or not in the current database (activate the layout first)");
        return false;
    }
    if (element.header.type.typeID != API_DrawingID) {
        GS::UniString typeName;
        ACAPI_Element_GetElemTypeName (element.header.type, typeName);
        err = EVP_FAIL (GS::UniString::Printf ("%T is not a Drawing (it is a %T)", guidString.ToPrintf (), typeName.ToPrintf ()),
                        "EvP drawing clip-polygon command");
        return false;
    }
    return true;
}

// The polygon memo as a flat [x0,y0,x1,y1,…] of DISTINCT points — the closing
// repeat that Archicad's 1-indexed convention stores at [nCoords] is dropped, so
// what comes out is what a caller would send back in.
void ReadClipPolygon (const API_Guid& guid, GS::Array<double>& flat, GS::Array<double>& arcs)
{
    API_ElementMemo memo = {};
    if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_Polygon) != NoError)
        return;

    if (memo.coords != nullptr) {
        const GSSize bytes  = BMGetHandleSize (reinterpret_cast<GSHandle> (memo.coords));
        const Int32  stored = (Int32) (bytes / sizeof (API_Coord));
        // Stored slots are [0]=unused, [1..nCoords] with [nCoords]==[1].
        for (Int32 i = 1; i + 1 < stored; ++i) {
            flat.Push ((*memo.coords)[i].x);
            flat.Push ((*memo.coords)[i].y);
        }
    }
    if (memo.parcs != nullptr) {
        const GSSize bytes = BMGetHandleSize (reinterpret_cast<GSHandle> (memo.parcs));
        const Int32  n     = (Int32) (bytes / sizeof (API_PolyArc));
        for (Int32 i = 0; i < n; ++i)
            arcs.Push ((*memo.parcs)[i].arcAngle);
    }
    ACAPI_DisposeElemMemoHdls (&memo);
}

// Build the polygon memo for a Drawing's clip from a flat [x0,y0,x1,y1,…] of
// DISTINCT points. Fills poly.nCoords/nSubPolys/nArcs on `poly` to match.
//
// Archicad's polygon convention is 1-INDEXED with the ring closed by REPEATING
// point 1 at [nCoords], so a caller sends N distinct points and this writes N+1
// slots. Callers must not pre-close the ring; both commands below say so.
//
// Shared by the create path (PlaceDrawingFromView) and the change path
// (SetDrawingClipPolygon) — which matters more than saving lines, because those
// two must agree on the convention exactly or a crop written at create time
// would read back differently from one written afterwards.
//
// Returns false only on allocation failure, with the memo already disposed.
bool BuildClipPolygonMemo (const GS::Array<double>& flat, const GS::Array<double>& arcs,
                           API_Polygon& poly, API_ElementMemo& memo)
{
    const Int32 distinct = (Int32) (flat.GetSize () / 2);
    const Int32 nCoords  = distinct + 1;   // +1: the ring closes by repeating point 1

    poly.nCoords   = nCoords;
    poly.nSubPolys = 1;
    poly.nArcs     = (Int32) arcs.GetSize ();

    memo.coords = reinterpret_cast<API_Coord**> (
        BMAllocateHandle ((nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
    memo.pends = reinterpret_cast<Int32**> (
        BMAllocateHandle ((poly.nSubPolys + 1) * sizeof (Int32), ALLOCATE_CLEAR, 0));
    if (memo.coords == nullptr || memo.pends == nullptr) {
        ACAPI_DisposeElemMemoHdls (&memo);
        return false;
    }
    for (Int32 i = 0; i < distinct; ++i) {
        (*memo.coords)[i + 1].x = flat[i * 2 + 0];
        (*memo.coords)[i + 1].y = flat[i * 2 + 1];
    }
    (*memo.coords)[nCoords] = (*memo.coords)[1];    // close the ring
    (*memo.pends)[1]        = nCoords;

    if (!arcs.IsEmpty ()) {
        memo.parcs = reinterpret_cast<API_PolyArc**> (
            BMAllocateHandle ((Int32) arcs.GetSize () * sizeof (API_PolyArc), ALLOCATE_CLEAR, 0));
        if (memo.parcs != nullptr) {
            for (UIndex i = 0; i < arcs.GetSize (); ++i) {
                (*memo.parcs)[i].begIndex = (Int32) i + 1;
                (*memo.parcs)[i].endIndex = (Int32) i + 2;
                (*memo.parcs)[i].arcAngle = arcs[i];
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tapioca.GetDrawingClipPolygon { drawings:[{elementId:{guid}}] }
//   -> { count, drawings: [ {elementId, found, isCutWithFrame, clipPolygon:[x,y,…],
//                                arcs:[…], pos:{x,y}, bounds:{xMin,yMin,xMax,yMax},
//                                ratio, drawingScale, name, error?} ] }
//
// The read half. Present so a repair pass can diff what IS against what it
// wants, and so the write below can be verified without a second command.
// ---------------------------------------------------------------------------
class GetDrawingClipPolygonCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetDrawingClipPolygon"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> requested;
        if (!params.Get ("drawings", requested)) {
            return NativeCommandResult::Failure (EVP_FAIL ("need drawings=[{elementId:{guid}}]", "Tapioca.GetDrawingClipPolygon"));
        }

        GS::Array<GS::ObjectState> records;
        for (const GS::ObjectState& requestedDrawing : requested) {
            GS::ObjectState elementId;
            GS::UniString guidString;
            if (!requestedDrawing.Get ("elementId", elementId) ||
                !elementId.Get ("guid", guidString) || guidString.IsEmpty ()) {
                return NativeCommandResult::Failure (EVP_FAIL ("every drawing needs elementId.guid", "Tapioca.GetDrawingClipPolygon"));
            }
            GS::ObjectState rec;
            rec.Add ("elementId", elementId);

            API_Element   element;
            GS::UniString err;
            if (!GetDrawing (guidString, element, err)) {
                rec.Add ("found", false);
                rec.Add ("error", err);
                records.Push (rec);
                continue;
            }

            GS::Array<double> flat, arcs;
            ReadClipPolygon (element.header.guid, flat, arcs);

            GS::ObjectState pos;
            pos.Add ("x", element.drawing.pos.x);
            pos.Add ("y", element.drawing.pos.y);

            GS::ObjectState bounds;
            bounds.Add ("xMin", element.drawing.bounds.xMin);
            bounds.Add ("yMin", element.drawing.bounds.yMin);
            bounds.Add ("xMax", element.drawing.bounds.xMax);
            bounds.Add ("yMax", element.drawing.bounds.yMax);

            rec.Add ("found",          true);
            rec.Add ("name",           GS::UniString (element.drawing.name));
            rec.Add ("isCutWithFrame", element.drawing.isCutWithFrame);
            rec.Add ("clipPolygon",    flat);
            rec.Add ("arcs",           arcs);
            rec.Add ("pos",            pos);
            rec.Add ("bounds",         bounds);
            rec.Add ("ratio",          element.drawing.ratio);
            rec.Add ("drawingScale",   element.drawing.drawingScale);
            records.Push (rec);
        }

        os.Add ("drawings", records);
        os.Add ("count", (GS::Int32) records.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// Tapioca.SetDrawingClipPolygon { drawing:{elementId:{guid}}, clipPolygon:[x0,y0,x1,y1,…], arcs?:[…],
//                             isCutWithFrame? }
//   -> { elementId, verified, pointsWritten, pointsReadBack }
//
// COORDINATES are the Drawing's own (model) space — the same space
// GetDrawingClipPolygon reports and that Tapir's DrawingDetails calls "model
// coords". Send at least 3 distinct points; do NOT repeat the first point at
// the end, this closes the ring itself (Archicad's 1-indexed convention).
//
// `isCutWithFrame` defaults to TRUE, because a clip polygon that is not
// switched on is exactly the silent no-op this command exists to avoid: the
// header says poly is the clip polygon only when that flag is set, and the
// bounding box otherwise.
//
// `verified` is the whole point — see the caveat at the top of this file. It is
// true only when a fresh read-back returns the same number of points. A false
// means Archicad accepted the call and kept the old crop.
// ---------------------------------------------------------------------------
class SetDrawingClipPolygonCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "SetDrawingClipPolygon"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::ObjectState   drawing, elementId;
        GS::UniString     guidString;
        GS::Array<double> flat;
        if (!params.Get ("drawing", drawing) || !drawing.Get ("elementId", elementId) ||
            !elementId.Get ("guid", guidString) || guidString.IsEmpty () ||
            !params.Get ("clipPolygon", flat)) {
            return NativeCommandResult::Failure (EVP_FAIL ("need drawing.elementId.guid and clipPolygon=[x0,y0,x1,y1,…]", "Tapioca.SetDrawingClipPolygon"));
        }
        if (flat.GetSize () < 6 || flat.GetSize () % 2 != 0) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("clipPolygon needs an even count and at least 3 points (got %u values)", (unsigned) flat.GetSize ()),
                                                           "EvP.SetDrawingClipPolygon"));
        }

        API_Element   element;
        GS::UniString getErr;
        if (!GetDrawing (guidString, element, getErr)) {
            return NativeCommandResult::Failure (getErr);
        }

        GS::Array<double> arcs;
        const bool haveArcs = params.Get ("arcs", arcs);

        bool isCutWithFrame = true;
        params.Get ("isCutWithFrame", isCutWithFrame);
        element.drawing.isCutWithFrame = isCutWithFrame;

        const Int32 distinct = (Int32) (flat.GetSize () / 2);

        API_ElementMemo memo = {};
        if (!BuildClipPolygonMemo (flat, haveArcs ? arcs : GS::Array<double> (),
                                   element.drawing.poly, memo)) {
            return NativeCommandResult::Failure (EVP_FAIL ("out of memory allocating the clip polygon", "EvP.SetDrawingClipPolygon"));
        }

        // Mask: the two things being changed and nothing else. Zeroing the mask
        // and setting only these fields is what stops Change from writing back
        // the whole element we happened to read.
        API_Element mask = {};
        ACAPI_ELEMENT_MASK_CLEAR (mask);
        ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, isCutWithFrame);
        ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, poly);

        // NO undo scope here — see WriteCommand. The caller has one open.
        const GSErrCode err = ACAPI_Element_Change (&element, &mask, &memo,
                                                    APIMemoMask_Polygon, true);
        ACAPI_DisposeElemMemoHdls (&memo);

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_Change", err,
                                                                  GS::UniString::Printf ("clip polygon (%d pts) on drawing %T", (int) distinct, guidString.ToPrintf ())));
        }

        // READ BACK. NoError is not proof — see the file header. A caller that
        // gets ok=true and verified=false has a silent no-op, not a success.
        GS::Array<double> backFlat, backArcs;
        ReadClipPolygon (element.header.guid, backFlat, backArcs);
        const Int32 readBack = (Int32) (backFlat.GetSize () / 2);

        os.Add ("elementId", elementId);
        os.Add ("pointsWritten",  (GS::Int32) distinct);
        os.Add ("pointsReadBack", (GS::Int32) readBack);
        os.Add ("verified",       readBack == distinct);
        if (readBack != distinct)
            os.Add ("note", GS::UniString ("ACAPI_Element_Change reported success but the polygon did not come back — "
                                           "API_DrawingID has no row in the Change support table; treat the crop as UNCHANGED"));
        return os;
    }
};

// ===========================================================================
// EvP.PlaceDrawingFromView — E22's "internal view" route.
//
// Places a Drawing on a LAYOUT whose content is a saved project VIEW: set
// API_DrawingType.drawingGuid to the navigator item's GUID and create with an
// otherwise empty memo. Archicad renders the view into the drawing itself.
//
// ⚠️ THE HEADER'S DOC COMMENT FOR drawingGuid IS MISLEADING and cost real time
// to resolve. It reads "GUID of the drawing for identification by autotext",
// which sounds like an output-only identity field. It is NOT: the DevKit's own
// shipped examples assign a NAVIGATOR ITEM's guid to it and that is what links
// the content — Element_Drawing.cpp's Do_CreateDrawingFromGroundFloor
// ("element.drawing.drawingGuid = navItems.GetLast ().guid; // link to the last
// floor plan view") and Element_Basics.cpp's CreateDrawingFromWorksheet
// ("// link the drawing to the first worksheet"). Working example code over a
// doc comment. There is no ACAPI_Drawing_SetDrawingLink — the link is
// write-once, at create.
//
// ⚠️ LAYOUTS ONLY, AND THIS COMMAND ENFORCES IT. A Drawing sourced from a
// project VIEW can be placed only on a layout; one sourced from the FILESYSTEM
// can go in any 2D view. That is not our restriction, it is Archicad's, and the
// AC29 header states it in the API_DrawingType remarks: "Drawing elements can be
// placed both in the model space (except which come from an internal view) and
// onto layouts." Placing on a worksheet therefore CANNOT work, and the refusal
// below says so by name rather than letting Archicad fail obscurely — this is
// the asymmetry the whole worksheet family is shaped around, so a caller finding
// it needs the reason, not an error code.
//
// ⚠️ WHY THIS IS THE PREFERRED WAY TO SET A CROP. The clip polygon is written
// HERE, at create time, from a memo — the same well-trodden path every other
// polygon element uses, so it never needs ACAPI_Element_Change on a Drawing at
// all. That mattered more when SetDrawingClipPolygon was unproven; it has since
// been verified live, but the caveat behind it stands (API_DrawingID has no row
// in the Change support table; see the top of this file). So if a caller can
// choose between placing with the right crop and placing then re-cropping, it
// should place with the right crop.
//
// COORDINATES are the LAYOUT's own space, in metres — `pos` is where the drawing
// lands on the page, `clipPolygon` is in the same space.
// ===========================================================================
class PlaceDrawingFromViewCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "PlaceDrawingFromView"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        double x = 0.0, y = 0.0;
        if (!params.Get ("x", x) || !params.Get ("y", y)) {
            return NativeCommandResult::Failure (EVP_FAIL ("need x and y", "EvP.PlaceDrawingFromView"));
        }

        // --- WHICH VIEW: by NAME (what a user can supply) or by guid ----------
        // ⚠️ `viewName` is the primary input, and that is a correction from a live
        // run: the first version took a guid only, and the probe's questions could
        // not be answered AT ALL because Archicad's UI never shows a person a
        // navigator guid. A guid stays accepted for script-to-script use, where
        // one command's output feeds another's input.
        //
        // Resolved FIRST because an unresolvable source does not fail the create:
        // it produces an EMPTY drawing on the layout that the user then has to
        // find and delete. A bad source must be a refusal, not a placed blank.
        API_Guid      viewGuid = APINULLGuid;
        GS::UniString viewName;
        GS::UniString viewGuidString;
        const bool    haveGuid = params.Get ("viewGuid", viewGuidString) && !viewGuidString.IsEmpty ();
        GS::UniString wantedName;
        const bool    haveName = params.Get ("viewName", wantedName) && !wantedName.IsEmpty ();

        if (!haveGuid && !haveName) {
            return NativeCommandResult::Failure (EVP_FAIL ("need viewName (as shown in the Navigator) or viewGuid - EvP.ListViews lists the placeable ones",
                                                            "EvP.PlaceDrawingFromView"));
        }

        if (haveName) {
            // Both View Maps, public first: a name in the user's own View Map is
            // just as valid a source, and searching only the public one would
            // refuse a name the user can plainly see.
            GS::Array<NavigatorEntry> entries;
            CollectNavigatorItems (API_PublicViewMap, entries);
            CollectNavigatorItems (API_MyViewMap,     entries);

            GS::Array<NavigatorEntry> matches, unplaceable;
            for (const NavigatorEntry& entry : entries) {
                if (entry.name != wantedName)
                    continue;
                if (IsPlaceableViewItem (entry.itemType)) matches.Push (entry);
                else                                      unplaceable.Push (entry);
            }

            if (matches.IsEmpty ()) {
                // Name the near misses rather than just "not found". The two real
                // causes are a folder/project-map item with that name, and a
                // typo — and they need different fixes.
                GS::UniString detail;
                if (!unplaceable.IsEmpty ()) {
                    detail = GS::UniString::Printf (" - there IS an item called that, but it is not something a Drawing can source from. "
                                                    "If it is a Project Map item, clone it into the View Map first "
                                                    "(API.CloneProjectMapItemToViewMap) and place the saved view.");
                } else {
                    GS::UniString available;
                    Int32         shown = 0;
                    for (const NavigatorEntry& entry : entries) {
                        if (!IsPlaceableViewItem (entry.itemType) || shown >= 12)
                            continue;
                        available += (shown == 0 ? "" : ", ") + entry.name;
                        ++shown;
                    }
                    detail = available.IsEmpty () ? GS::UniString (" - and this project has NO saved views to place. Save one in the View Map first.")
                                                  : GS::UniString (" - available: ") + available
                                                    + (shown >= 12 ? GS::UniString (", ... (EvP.ListViews for the rest)") : GS::UniString ());
                }
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("no placeable view named \"%T\"%T", wantedName.ToPrintf (), detail.ToPrintf ()),
                                                               "EvP.PlaceDrawingFromView"));
            }
            if (matches.GetSize () > 1) {
                // Ambiguity is a refusal, not a first-match: placing a drawing of
                // the wrong view looks like success and is only caught by eye.
                GS::UniString paths;
                for (UIndex i = 0; i < matches.GetSize () && i < 8; ++i)
                    paths += (i == 0 ? "" : ", ") + (matches[i].path.IsEmpty () ? matches[i].name : matches[i].path);
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("\"%T\" matches %u views (%T) - rename one, or pass viewGuid from EvP.ListViews "
                                                                 "to say which you mean",
                                                                 wantedName.ToPrintf (), (unsigned) matches.GetSize (), paths.ToPrintf ()),
                                                               "EvP.PlaceDrawingFromView"));
            }
            viewGuid = matches[0].guid;
            viewName = matches[0].name;

        } else {
            viewGuid = APIGuidFromString (viewGuidString.ToCStr ().Get ());

            API_NavigatorItem navItem = {};
            navItem.mapId = API_PublicViewMap;
            if (ACAPI_Navigator_GetNavigatorItem (&viewGuid, &navItem) != NoError) {
                navItem       = {};
                navItem.mapId = API_MyViewMap;
                if (ACAPI_Navigator_GetNavigatorItem (&viewGuid, &navItem) != NoError) {
                    return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("%T is not a navigator item in either View Map - a Drawing's source must be a "
                                                                     "SAVED VIEW, not a project-map item. Clone the project-map item into the View "
                                                                     "Map first (API.CloneProjectMapItemToViewMap), then place that.",
                                                                     viewGuidString.ToPrintf ()),
                                                                   "EvP.PlaceDrawingFromView"));
                }
            }
            viewName = GS::UniString (navItem.uName);
        }

        // --- the TARGET database, and the layouts-only rule -------------------
        API_DatabaseInfo originalDb = {};
        if (const GSErrCode err = ACAPI_Database_GetCurrentDatabase (&originalDb); err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Database_GetCurrentDatabase", err,
                                                                  "reading the current database before placing a drawing"));
        }

        // `layoutName` (or `layoutGuid`) switches the background database for us;
        // omitted, the caller is responsible for having activated the layout.
        // `switched` records whether we owe a restore.
        //
        // ⚠️ NAME-FIRST for the same reason as the view above: a user can read a
        // layout's name off the Navigator and nothing else. `targetGuid` is also
        // accepted so a script can pass EvP.ListDatabases output straight through.
        //
        // NOTE the search covers EVERY independent database type, not just
        // layouts, even though only a layout is legal here — because resolving a
        // worksheet name and THEN refusing it produces the useful error ("that is
        // a worksheet, and a view drawing cannot go on one"), whereas searching
        // layouts only would report "no such layout" for a name the user can
        // plainly see. The layouts-only refusal happens below, after the switch.
        bool          switched = false;
        GS::UniString layoutGuidString, layoutName;
        const bool    haveTargetGuid = params.Get ("layoutGuid", layoutGuidString) && !layoutGuidString.IsEmpty ();
        const bool    haveTargetName = params.Get ("layoutName", layoutName) && !layoutName.IsEmpty ();

        if (haveTargetGuid || haveTargetName) {
            const API_DatabaseTypeID searched[] = { APIWind_LayoutID, APIWind_MasterLayoutID,
                                                    APIWind_WorksheetID, APIWind_DetailID,
                                                    APIWind_DocumentFrom3DID };
            const API_Guid wanted = haveTargetGuid ? APIGuidFromString (layoutGuidString.ToCStr ().Get ())
                                                   : APINULLGuid;

            GS::Array<API_DatabaseInfo> matches;
            GS::UniString               availableLayouts;
            for (const API_DatabaseTypeID type : searched) {
                for (const API_DatabaseUnId& candidate : ACAPI_Database_GetDatabasesForType (type)) {
                    API_DatabaseInfo info = {};
                    info.typeID       = type;
                    info.databaseUnId = candidate;
                    if (ACAPI_Window_GetDatabaseInfo (&info) != NoError)
                        continue;

                    if (type == APIWind_LayoutID) {
                        availableLayouts += (availableLayouts.IsEmpty () ? "" : ", ") + GS::UniString (info.name);
                    }
                    const bool hit = haveTargetGuid ? (candidate.elemSetId == wanted)
                                                    : (GS::UniString (info.name) == layoutName);
                    if (hit)
                        matches.Push (info);
                }
            }

            if (matches.IsEmpty ()) {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("no database named/with guid \"%T\". A Drawing of a project view can only be placed "
                                                                 "on a LAYOUT - available layouts: %T",
                                                                 (haveTargetName ? layoutName : layoutGuidString).ToPrintf (),
                                                                 availableLayouts.IsEmpty () ? GS::UniString ("(none)").ToPrintf ()
                                                                                             : availableLayouts.ToPrintf ()),
                                                               "EvP.PlaceDrawingFromView"));
            }
            if (matches.GetSize () > 1) {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("\"%T\" matches %u databases - rename one, or pass layoutGuid from "
                                                                 "EvP.ListDatabases to say which you mean",
                                                                 layoutName.ToPrintf (), (unsigned) matches.GetSize ()),
                                                               "EvP.PlaceDrawingFromView"));
            }

            API_DatabaseInfo target = matches[0];
            if (const GSErrCode err = ACAPI_Database_ChangeCurrentDatabase (&target); err != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Database_ChangeCurrentDatabase", err,
                                                                      GS::UniString::Printf ("activating \"%T\" to place a drawing on it",
                                                                                             GS::UniString (target.name).ToPrintf ())));
            }
            switched = true;
        }

        // Whatever database we are now in, it MUST be a layout — see the
        // asymmetry note above. Checked after the optional switch so it also
        // catches the caller who activated the wrong window themselves.
        API_DatabaseInfo currentDb = {};
        ACAPI_Database_GetCurrentDatabase (&currentDb);
        if (currentDb.typeID != APIWind_LayoutID && currentDb.typeID != APIWind_MasterLayoutID) {
            if (switched)
                ACAPI_Database_ChangeCurrentDatabase (&originalDb);
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("the current database is \"%T\", which is NOT a layout. A Drawing whose source is a "
                                                             "project view can be placed ONLY on a layout - Archicad's own rule, stated in the "
                                                             "API_DrawingType remarks. Pass layoutGuid, or activate a layout first. To get 2D "
                                                             "content onto a WORKSHEET you need real 2D geometry there, not a view drawing.",
                                                             GS::UniString (currentDb.title).ToPrintf ()),
                                                           "EvP.PlaceDrawingFromView"));
        }

        // Everything from here on must go through this on the way out, or a
        // failed create leaves the user's active database silently changed.
        const auto restore = [&switched, &originalDb] () {
            if (switched)
                ACAPI_Database_ChangeCurrentDatabase (&originalDb);
        };

        // --- the element ------------------------------------------------------
        API_Element element = {};
        element.header.type = API_DrawingID;
        if (const GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr); err != NoError) {
            restore ();
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_DrawingID"));
        }

        GS::UniString layerErr;
        if (!ResolveLayerParam (params, element.header, layerErr)) {
            restore ();
            return NativeCommandResult::Failure (layerErr);      // includes the hidden/locked refusal
        }

        element.drawing.drawingGuid = viewGuid;      // THE LINK — see the note above
        element.drawing.pos.x       = x;
        element.drawing.pos.y       = y;
        element.drawing.ratio       = 1.0;
        params.Get ("ratio", element.drawing.ratio);
        params.Get ("angle", element.drawing.angle);         // radians

        element.drawing.anchorPoint = APIAnc_LB;
        GS::UniString anchorName;
        if (params.Get ("anchor", anchorName) && !anchorName.IsEmpty ()) {
            API_AnchorID anchor;
            if (!ParseAnchor (anchorName, anchor)) {
                restore ();
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown anchor: %T (want topLeft…bottomRight)", anchorName.ToPrintf ()),
                                                               "EvP.PlaceDrawingFromView"));
            }
            element.drawing.anchorPoint = anchor;
        }

        // A custom name, or the view's own. nameType has to be switched to
        // CustomName or the string is stored and ignored.
        GS::UniString name;
        if (params.Get ("name", name) && !name.IsEmpty ()) {
            CHTruncate (name.ToCStr (), element.drawing.name, sizeof (element.drawing.name));
            element.drawing.nameType = APIName_CustomName;
        }

        // --- the crop, at CREATE time ----------------------------------------
        API_ElementMemo   memo = {};
        GS::Array<double> flat, arcs;
        const bool        clipGiven = params.Get ("clipPolygon", flat) && !flat.IsEmpty ();
        const bool        haveClip  = clipGiven && flat.GetSize () >= 6 && flat.GetSize () % 2 == 0;

        // A clipPolygon that was SENT but is malformed is a refusal, not a
        // silently-uncropped drawing: the caller asked for a crop and would
        // otherwise get a full-page drawing reported as success.
        if (clipGiven && !haveClip) {
            restore ();
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("clipPolygon needs an even count and at least 3 points (got %u values); do NOT repeat "
                                                             "the first point at the end", (unsigned) flat.GetSize ()),
                                                           "EvP.PlaceDrawingFromView"));
        }
        if (haveClip) {
            params.Get ("arcs", arcs);
            if (!BuildClipPolygonMemo (flat, arcs, element.drawing.poly, memo)) {
                restore ();
                return NativeCommandResult::Failure (EVP_FAIL ("out of memory allocating the clip polygon", "EvP.PlaceDrawingFromView"));
            }
            // Without this the polygon is stored as a mere bounding box and the
            // drawing is NOT cropped — the same silent no-op SetDrawingClipPolygon
            // defends against.
            element.drawing.isCutWithFrame = true;

            // Which part of the SOURCE shows through the frame. Omitted, the
            // source's own origin lands at the frame's origin.
            GS::ObjectState modelOffset;
            if (params.Get ("modelOffset", modelOffset)) {
                modelOffset.Get ("x", element.drawing.modelOffset.x);
                modelOffset.Get ("y", element.drawing.modelOffset.y);
            }
        } else {
            element.drawing.isCutWithFrame = false;
        }

        // NO undo scope here — see WriteCommand. The caller has one open.
        const GSErrCode err = ACAPI_Element_Create (&element, &memo);
        ACAPI_DisposeElemMemoHdls (&memo);
        restore ();

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_Create", err,
                                                                  GS::UniString::Printf ("drawing of view \"%T\" at (%.4f, %.4f) on layout \"%T\"",
                                                                                         viewName.ToPrintf (), x, y,
                                                                                         GS::UniString (currentDb.title).ToPrintf ())));
        }

        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ().Get ()));
        os.Add ("elementId", elementId);
        os.Add ("viewName", viewName);
        os.Add ("layout",   GS::UniString (currentDb.title));
        os.Add ("cropped",  haveClip);
        return os;
    }
};

const NativeCommandRegistration DrawingCommandRegistrations[] = {
    { "GetDrawingClipPolygon", &MakeRegisteredNativeCommand<GetDrawingClipPolygonCommand>, false,
      R"json({"type":"object","properties":{"drawings":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["drawings"]})json",
      R"json({"type":"object","properties":{"drawings":{"type":"array","items":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"found":{"type":"boolean"},"name":{"type":"string"},"isCutWithFrame":{"type":"boolean"},"clipPolygon":{"type":"array","items":{"type":"number"}},"arcs":{"type":"array","items":{"type":"number"}},"pos":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false,"required":["x","y"]},"bounds":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","xMax","yMax"]},"ratio":{"type":"number"},"drawingScale":{"type":"number"},"error":{"type":"string"}},"additionalProperties":false,"required":["elementId","found"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["drawings","count"]})json" },
    { "SetDrawingClipPolygon", &MakeRegisteredNativeCommand<SetDrawingClipPolygonCommand>, false,
      R"json({"type":"object","properties":{"drawing":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]},"clipPolygon":{"type":"array","minItems":6,"items":{"type":"number"}},"arcs":{"type":"array","items":{"type":"number"}},"isCutWithFrame":{"type":"boolean"}},"additionalProperties":false,"required":["drawing","clipPolygon"]})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"verified":{"type":"boolean"},"pointsWritten":{"type":"integer","minimum":3},"pointsReadBack":{"type":"integer","minimum":0},"note":{"type":"string"}},"additionalProperties":false,"required":["elementId","verified","pointsWritten","pointsReadBack"]})json" },
    { "PlaceDrawingFromView", &MakeRegisteredNativeCommand<PlaceDrawingFromViewCommand>, false,
      R"json({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"viewName":{"type":"string"},"viewGuid":{"type":"string"},"layoutName":{"type":"string"},"layoutGuid":{"type":"string"},"layer":{"type":"string"},"ratio":{"type":"number"},"angle":{"type":"number"},"anchor":{"type":"string","enum":["topLeft","topCenter","topRight","middleLeft","middleCenter","middleRight","bottomLeft","bottomCenter","bottomRight"]},"name":{"type":"string"},"clipPolygon":{"type":"array","items":{"type":"number"}},"arcs":{"type":"array","items":{"type":"number"}},"modelOffset":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"}},"additionalProperties":false}},"additionalProperties":false,"required":["x","y"]})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"viewName":{"type":"string"},"layout":{"type":"string"},"cropped":{"type":"boolean"}},"additionalProperties":false,"required":["elementId","viewName","layout","cropped"]})json" },
};

}   // namespace

NativeCommandRegistrations GetDrawingCommandRegistrations ()
{
    return MakeRegistrationView (DrawingCommandRegistrations);
}

} // namespace geomsrv

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/LayoutCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"      // CollectNavigatorItems, IsPlaceableViewItem

namespace geomsrv {

namespace {

// ===========================================================================
// The Layout Book domain — the CONTAINERS, not their contents.
//
// WHAT IS ACTUALLY MISSING, AND WHAT IS NOT. This file is deliberately SMALL
// because most of this cluster already has a working, live-verified route, and
// E11's standing decision is that a native write gets built only for what the
// existing routes cannot do. Measured against the vendored Tapir 1.5.4 source
// and the local `archicad` package rather than assumed:
//
//   ALREADY COVERED — do NOT add these here:
//   * worksheet create   -> Tapir.CreateWorksheets     (Commands/MassingFeasibility
//   * detail create      -> Tapir.CreateDetails         /navtree.py wraps the first
//   * subset create      -> Tapir.CreateLayoutSubset    three as ensure_worksheet /
//   * layout create      -> API.CreateLayout            ensure_subset / create_layout,
//                                                       all verified live)
//     navtree.create_layout already reads the master's own LayoutParameters via
//     API.GetLayoutSettings and feeds the 16-field struct, so the "a JSON caller
//     has to invent a page size" objection does not apply — it was answered.
//     Native CreateLayout / CreateSubset commands existed in an early draft of
//     this file and were REMOVED as duplication.
//
//   THE REAL GAPS, which is what is below:
//   * 3D DOCUMENT CREATE — no route anywhere. The local `archicad` package ships
//     no 3d-document create, and Tapir's registrations are CreateWorksheets /
//     CreateDetails / CreateLayout / CreateLayoutSubset / CreateDrawings and
//     nothing for 3D documents. This is the one that blocks the massing flow.
//   * WORKSHEET / DETAIL / 3D-DOCUMENT DELETE — the navigator route does NOT do
//     it. Read Tapir's DeleteNavigatorItemsCommand::Execute: it branches on
//     itemType and sends Layout/MasterLayout to ACAPI_Database_DeleteDatabase,
//     SubSet to APIERR_NOTSUPPORTED, and EVERYTHING ELSE to
//     ACAPI_Navigator_DeleteNavigatorView — which deletes a VIEW, not the
//     independent database. So deleting a worksheet or a 3D document needs
//     DeleteDatabase, i.e. this file.
//   * enumerating independent databases with their API_DatabaseUnId, which is
//     what the two above take as input and what the navigator tree does not give.
//
//   AND ONE THING NOBODY CAN DO: there is NO subset delete, anywhere. Grepped
//   the AC29 headers (default/create/read and no delete of any spelling), and
//   Tapir returns APIERR_NOTSUPPORTED with exactly that explanation. Do not add
//   a DeleteSubset here expecting to find a symbol for it.
//
// ⚠️⚠️ THE RULE THAT SHAPES EVERY COMMAND BELOW: these are StructuralCommands,
// not WriteCommands. ACAPI_Database_NewDatabase and ACAPI_Database_DeleteDatabase
// are both documented in the AC29 headers as non-undoable data-structure
// modifiers that return APIERR_REFUSEDCMD when called inside an undo scope. So
// they get NO undo scope, they cannot take part in evp.transaction (the
// dispatcher refuses them by name), and — the part the user feels — THEY ARE NOT
// UNDOABLE. Ctrl+Z will not bring back a deleted worksheet. That is why
// DeleteDatabase demands `confirm: true`.
//
// ⚠️ ONE STALE DOC TO IGNORE, DELIBERATELY. ACAPI_Database_NewDatabase's own
// @return block says APIERR_REFUSEDPAR for anything but Detail / Worksheet /
// Layout / MasterLayout — i.e. it claims 3D Document is refused. The DevKit's
// OWN SHIPPED EXAMPLE contradicts it: Database_Control.cpp's Do_Create3DDocument
// sets typeID = APIWind_DocumentFrom3DID and calls NewDatabase, and
// Do_DeleteDatabase is wired to APIWind_DocumentFrom3DID from the menu. Working
// example code beats a doc comment, so 3dDocument is in the accepted set here —
// but it is the ONE type in this file whose support rests on the example rather
// than the documentation, so the probe checks it first.
// ===========================================================================

// The database kinds this file will act on, spelled the way a script says them.
// Deliberately NOT the whole API_WindowTypeID enum: most of its members are
// publish-format markers (APIWind_MovieRenderingID, APIWind_AllModelID, …) that
// are not independent databases at all, and NewDatabase/DeleteDatabase would
// refuse them. An unknown name is a refusal listing the legal set, never a
// silent fallback to some default type.
bool ParseDatabaseType (const GS::UniString& name, API_DatabaseTypeID& type)
{
    if (name == "worksheet")    { type = APIWind_WorksheetID;       return true; }
    if (name == "detail")       { type = APIWind_DetailID;          return true; }
    if (name == "layout")       { type = APIWind_LayoutID;          return true; }
    if (name == "masterLayout") { type = APIWind_MasterLayoutID;    return true; }
    if (name == "3dDocument")   { type = APIWind_DocumentFrom3DID;  return true; }
    return false;
}

const char* DatabaseTypeName (API_DatabaseTypeID type)
{
    switch (type) {
        case APIWind_WorksheetID:      return "worksheet";
        case APIWind_DetailID:         return "detail";
        case APIWind_LayoutID:         return "layout";
        case APIWind_MasterLayoutID:   return "masterLayout";
        case APIWind_DocumentFrom3DID: return "3dDocument";
        default:                       return "other";
    }
}

const char* kLegalTypes = "worksheet | detail | layout | masterLayout | 3dDocument";

// A database's identity on the wire is its elemSetId GUID — the single field of
// API_DatabaseUnId in AC29. Everything in this file takes and returns that, so a
// script can round-trip a database it just made without holding a C++ struct.
GS::UniString UnIdToString (const API_DatabaseUnId& unId)
{
    return GS::UniString (APIGuidToString (unId.elemSetId).ToCStr ().Get ());
}

GS::ObjectState GuidId (const GS::UniString& guid)
{
    GS::ObjectState id;
    id.Add ("guid", guid);
    return id;
}

bool GetIdGuid (const GS::ObjectState& item, const char* key, GS::UniString& guid)
{
    GS::ObjectState id;
    return item.Get (key, id) && id.Get ("guid", guid) && !guid.IsEmpty ();
}

// Name / ref / title of a database, by unique id. Kept separate from the
// enumeration below because DeleteDatabase needs it too: it reports WHAT it
// deleted, and after the delete there is nothing left to ask.
bool ReadDatabaseInfo (const API_DatabaseUnId& unId, API_DatabaseTypeID type, API_DatabaseInfo& info)
{
    info = {};
    info.typeID       = type;
    info.databaseUnId = unId;
    return ACAPI_Window_GetDatabaseInfo (&info) == NoError;
}

// Owns an API_DocumentFrom3DType and frees the ONE allocated thing inside it.
//
// ⚠️ ACAPI_View_GetDocumentFrom3DSettings HANDS YOU OWNERSHIP of
// cutSetting.shapes — API_3DCutPlanesInfo's own doc comment says "don't forget
// to dispose of the shapes handle", and the DevKit's Do_Change3DDocument ends
// with exactly this BMKillHandle. Missing it leaks silently, once per call, with
// nothing in any log; SetDocumentFrom3DSettings is meant to run once per massing
// option in a loop, so "once per call" is the whole cost.
//
// A GUARD rather than a free at the end of Execute, deliberately: the command
// below has six early returns and reads the settings TWICE (write, then read
// back to verify), which is two handles to lose track of. Scope-bound is the
// only version that cannot drift.
class DocumentFrom3DSettings {
public:
    DocumentFrom3DSettings () = default;
    ~DocumentFrom3DSettings ()
    {
        if (value.cutSetting.shapes != nullptr)
            BMKillHandle (reinterpret_cast<GSHandle*> (&value.cutSetting.shapes));
    }

    DocumentFrom3DSettings (const DocumentFrom3DSettings&)            = delete;
    DocumentFrom3DSettings& operator= (const DocumentFrom3DSettings&) = delete;

    API_DocumentFrom3DType value = {};
};

// Did the projection we wrote survive the round trip?
//
// EXACT equality on purpose, no tolerance. These doubles are not computed here —
// they are copied verbatim out of one struct and read back out of another, so
// the only question is "did the write land", and any tolerance would mask a
// PARTIAL apply (Archicad accepting the mode but keeping its own camera), which
// is precisely the failure §E20 already saw once: NoError from this family does
// not mean the change took.
bool SameProjection (const API_3DProjectionInfo& a, const API_3DProjectionInfo& b)
{
    if (a.isPersp != b.isPersp)
        return false;

    if (a.isPersp) {
        const API_PerspPars& p = a.u.persp;
        const API_PerspPars& q = b.u.persp;
        return p.pos.x    == q.pos.x    && p.pos.y      == q.pos.y      &&
               p.cameraZ  == q.cameraZ  && p.target.x   == q.target.x   &&
               p.target.y == q.target.y && p.targetZ    == q.targetZ    &&
               p.azimuth  == q.azimuth  && p.rollAngle  == q.rollAngle  &&
               p.viewCone == q.viewCone && p.distance   == q.distance   &&
               p.isTwoPointPersp == q.isTwoPointPersp;
    }

    const API_AxonoPars& p = a.u.axono;
    const API_AxonoPars& q = b.u.axono;
    if (p.azimuth != q.azimuth || p.projMod != q.projMod)
        return false;
    for (int i = 0; i < 12; ++i) {
        if (p.tranmat.tmx[i] != q.tranmat.tmx[i])
            return false;
    }
    return true;
}

// Guid-as-string -> the API_DatabaseUnId of an EXISTING database of that type.
//
// Every command here that takes a guid must first prove the database exists,
// because handing an unknown unId to the ACAPI call answers a bare
// APIERR_BADPARS / APIERR_BADDATABASE — which cannot be told apart from a typo,
// from a database that was already deleted, or from a guid of the WRONG TYPE.
// Resolving first lets the refusal say which of those it was.
//
// For ONE guid. DeleteDatabase keeps its own inline lookup on purpose — it
// enumerates once and scans that array for each of N guids, rather than
// re-enumerating N times. Do not "unify" them.
bool FindDatabase (const GS::UniString& guidString, API_DatabaseTypeID type, API_DatabaseUnId& unId)
{
    const API_Guid wanted = APIGuidFromString (guidString.ToCStr ().Get ());
    for (const API_DatabaseUnId& candidate : ACAPI_Database_GetDatabasesForType (type)) {
        if (candidate.elemSetId == wanted) { unId = candidate; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// EvP.ListDatabases { types?: ["worksheet", …] }
//   -> { ok, count, databases: [ {type, guid, name, ref, title,
//                                masterLayoutGuid?} ] }
//
// The READ half, and the entry point for everything else in this file: every
// other command here takes a database GUID, and this is the only way to learn
// one. Also the verification surface — a create is confirmed by the thing
// appearing in this list, which matters more than usual because a structural
// change leaves no undo entry to inspect.
//
// `types` omitted means all five. ACAPI_Database_GetDatabasesForType returns
// only INDEPENDENT databases of that type, which is exactly the set
// NewDatabase/DeleteDatabase can act on.
// ---------------------------------------------------------------------------
class ListDatabasesCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ListDatabases"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<API_DatabaseTypeID> types;
        GS::Array<GS::UniString>      typeNames;
        if (params.Get ("types", typeNames) && !typeNames.IsEmpty ()) {
            for (const GS::UniString& typeName : typeNames) {
                API_DatabaseTypeID type;
                if (!ParseDatabaseType (typeName, type)) {
                    return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown database type: %T (want %s)", typeName.ToPrintf (), kLegalTypes),
                                                                    "EvP.ListDatabases"));
                }
                types.Push (type);
            }
        } else {
            types = { APIWind_WorksheetID, APIWind_DetailID, APIWind_LayoutID,
                      APIWind_MasterLayoutID, APIWind_DocumentFrom3DID };
        }

        GS::Array<GS::ObjectState> records;
        for (const API_DatabaseTypeID type : types) {
            for (const API_DatabaseUnId& unId : ACAPI_Database_GetDatabasesForType (type)) {
                GS::ObjectState rec;
                rec.Add ("type", GS::UniString (DatabaseTypeName (type)));
                rec.Add ("databaseId", GuidId (UnIdToString (unId)));

                API_DatabaseInfo info;
                if (ReadDatabaseInfo (unId, type, info)) {
                    rec.Add ("name",  GS::UniString (info.name));
                    rec.Add ("ref",   GS::UniString (info.ref));
                    rec.Add ("title", GS::UniString (info.title));
                    if (type == APIWind_LayoutID)
                        rec.Add ("masterLayoutId", GuidId (UnIdToString (info.masterLayoutUnId)));
                } else {
                    // The database exists (it came out of the enumeration) but its
                    // info would not read. Say which one, rather than dropping it
                    // from the list and leaving the caller with a short count.
                    rec.Add ("error", GS::UniString ("ACAPI_Window_GetDatabaseInfo failed for this database"));
                }
                records.Push (rec);
            }
        }

        os.Add ("databases", records);
        os.Add ("count", (GS::Int32) records.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.ListViews { map?: "view" | "myView" | "project", placeableOnly? }
//   -> { ok, count, views: [ {name, guid, path, itemType, placeable, map} ] }
//
// ⚠️⚠️ THE REASON THIS COMMAND EXISTS — a lesson from the first live run of
// LayoutApiProbe2 (2026-07-30), and it applies to every future command:
// A COMMAND THAT ASKS THE USER FOR A GUID IS UNUSABLE. Archicad's UI shows a
// person NAMES; there is no way for them to read a navigator guid out of it. The
// probe's first version took `view_guid` as text and Q6/Q7 could only be SKIPPED,
// because the input could not be filled in at all.
//
// So this is the discovery half: it lists what exists, by name, so a script (or a
// probe's log) can show the user real choices. EvP.PlaceDrawingFromView then
// accepts `viewName` and resolves it here-style. Same shape as the attribute
// pickers, which have always serialised a layer/material/profile NAME rather than
// an index — this just extends the names-not-indices policy to navigator items.
//
// `map` defaults to "view" (the Public View Map), which is where placeable saved
// views live. "project" is the Project Map, whose items are NOT placeable as
// drawings — it is listed only so a caller can tell the user "that name is a
// project-map item, clone it to the View Map first".
//
// `placeableOnly` defaults TRUE: folders and the project root are dropped, so
// what comes back is exactly the set a Drawing can source from.
// ---------------------------------------------------------------------------
class ListViewsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "ListViews"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_NavigatorMapID mapId   = API_PublicViewMap;
        GS::UniString      mapName = "view";
        if (params.Get ("map", mapName) && !mapName.IsEmpty ()) {
            if      (mapName == "view")    mapId = API_PublicViewMap;
            else if (mapName == "myView")  mapId = API_MyViewMap;
            else if (mapName == "project") mapId = API_ProjectMap;
            else if (mapName == "layout")  mapId = API_LayoutMap;
            else {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown map: %T (want view | myView | project | layout)", mapName.ToPrintf ()),
                                                               "EvP.ListViews"));
            }
        }

        bool placeableOnly = true;
        params.Get ("placeableOnly", placeableOnly);

        // ⚠️ MY VIEW MAP IS TEAMWORK-ONLY. APIdefs_Navigator.h says so on the enum
        // member itself ("only in Teamwork mode"), and on a solo project
        // ACAPI_Navigator_GetNavigatorSet answers APIERR_NOTEAMWORKPROJECT — which
        // is a true statement about the project, not a failure of the call. Caught
        // live 2026-07-31: NavigatorPickerProbe raised on views("myView") and lost
        // the rest of its run to an exception, on the one project it was aimed at.
        //
        // So this answers EMPTY WITH A NOTE rather than failing. A map that cannot
        // exist here is not an error to handle, and making every caller wrap
        // views("myView") in a try is the wrong tax.
        if (mapId == API_MyViewMap && !ACAPI_Teamwork_HasConnection ()) {
            os.Add ("views", GS::Array<GS::ObjectState> ());
            os.Add ("count", (GS::Int32) 0);
            os.Add ("note", GS::UniString ("the My View Map exists only in Teamwork mode; this is a solo project"));
            return os;
        }

        GS::Array<NavigatorEntry> entries;
        if (!CollectNavigatorItems (mapId, entries)) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("could not read the %T map", mapName.ToPrintf ()), "EvP.ListViews"));
        }

        GS::Array<GS::ObjectState> records;
        for (const NavigatorEntry& entry : entries) {
            const bool placeable = IsPlaceableViewItem (entry.itemType);
            if (placeableOnly && !placeable)
                continue;

            GS::ObjectState rec;
            rec.Add ("name",      entry.name);
            rec.Add ("navigatorItemId", GuidId (GS::UniString (APIGuidToString (entry.guid).ToCStr ().Get ())));
            rec.Add ("path",      entry.path);
            rec.Add ("itemType",  NavItemTypeName (entry.itemType));
            rec.Add ("placeable", placeable);
            rec.Add ("map",       mapName);
            records.Push (rec);
        }

        os.Add ("views", records);
        os.Add ("count", (GS::Int32) records.GetSize ());
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.CreateDatabase { type, name?, ref? }
//   -> { ok, type, guid, name, ref }
//
// ACAPI_Database_NewDatabase. The reason this command exists is ONE type:
// `3dDocument`, the create with no route anywhere (E19), which is what blocks the
// massing flow.
//
// ⚠️ THE OTHER TYPES ARE ACCEPTED BUT ARE NOT THE PREFERRED ROUTE. `worksheet`
// and `detail` already have live-verified Tapir routes (CreateWorksheets /
// CreateDetails, wrapped as navtree.ensure_worksheet) and those give you
// find-or-create and a navigator id; this gives you a bare database. They stay
// accepted here for two reasons worth keeping: a native fallback, and — the
// useful one — a DIAGNOSTIC CONTROL. `worksheet` is the type NewDatabase's own
// docs say is supported, so if a `worksheet` create fails the bug is in our call,
// whereas if only `3dDocument` fails the doc comment was right and the DevKit
// example is stale. The probe uses exactly that pairing. Prefer Tapir in
// production code.
//
// ⚠️ NOT UNDOABLE (StructuralCommand). ⚠️ Requires a floor-plan window to be
// open — NewDatabase returns APIERR_NOPLAN otherwise, which is why that code is
// called out by name in the failure below.
//
// ⚠️ `type: "layout"` is REFUSED. NewDatabase gives a layout no master, and a
// layout without a master is not usable — refusing beats producing a broken
// layout that fails later somewhere else. Layouts have a real route:
// API.CreateLayout, wrapped as navtree.create_layout, which reads the master's
// own page size and margins.
//
// A 3D DOCUMENT CREATED THIS WAY IS EMPTY of settings — it takes the 3D
// document defaults, NOT the current 3D view. Pointing it at the current view is
// ACAPI_View_ChangeDocumentFrom3DSettings, a separate step that is deliberately
// not folded in here: it is a different question, and the probe answers one
// question per transaction.
// ---------------------------------------------------------------------------
class CreateDatabaseCommand : public StructuralCommand {
public:
    GS::String GetName () const override { return "CreateDatabase"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::UniString typeName;
        if (!params.Get ("type", typeName)) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("need type=<%s>", kLegalTypes), "EvP.CreateDatabase"));
        }

        API_DatabaseTypeID type;
        if (!ParseDatabaseType (typeName, type)) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown database type: %T (want %s)", typeName.ToPrintf (), kLegalTypes),
                                                            "EvP.CreateDatabase"));
        }
        if (type == APIWind_LayoutID) {
            return NativeCommandResult::Failure (EVP_FAIL ("a layout needs a master layout, which ACAPI_Database_NewDatabase cannot supply - use API.CreateLayout "
                                       "(wrapped as navtree.create_layout in Commands/MassingFeasibility, which reads the master's own page size)",
                                                            "EvP.CreateDatabase"));
        }

        API_DatabaseInfo info = {};
        info.typeID = type;

        GS::UniString name, ref;
        if (params.Get ("name", name) && !name.IsEmpty ())
            GS::ucsncpy (info.name, name.ToUStr (), API_UniLongNameLen - 1);
        if (params.Get ("ref", ref) && !ref.IsEmpty ())
            GS::ucsncpy (info.ref, ref.ToUStr (), API_UniLongNameLen - 1);

        // NO undo scope, and none must be open — see StructuralCommand.
        if (const GSErrCode err = ACAPI_Database_NewDatabase (&info); err != NoError) {
            // ⚠️ A DUPLICATE NAME COMES BACK AS APIERR_GENERAL, which says nothing.
            // Observed live 2026-07-31: LayoutApiProbe2 Q3 created a 3D document and
            // Q8 failed nine calls later in the SAME run — the only difference being
            // that Q8's name already existed (its own leftover from the run before).
            // "General error code" would have sent the next reader hunting through
            // window state and undo scopes, so the failure path SPENDS ONE
            // ENUMERATION to check the likeliest cause and name the culprit's guid.
            // The check is here rather than as a pre-flight refusal on purpose: a
            // same-name rule is an INFERENCE from one observation, and inferring is
            // fine in a diagnostic but not in a refusal that blocks a legal call.
            GS::UniString clash;
            if (!name.IsEmpty ()) {
                for (const API_DatabaseUnId& unId : ACAPI_Database_GetDatabasesForType (type)) {
                    API_DatabaseInfo existing;
                    if (ReadDatabaseInfo (unId, type, existing) && GS::UniString (existing.name) == name) {
                        clash = GS::UniString::Printf (" - NOTE: a %T named \"%T\" ALREADY EXISTS (guid %T), and a duplicate name is the "
                                                       "likeliest cause of an otherwise unexplained APIERR_GENERAL here. Delete or rename it, "
                                                       "or pass a different name",
                                                       typeName.ToPrintf (), name.ToPrintf (), UnIdToString (unId).ToPrintf ());
                        break;
                    }
                }
            }

            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Database_NewDatabase", err,
                                                                  GS::UniString::Printf ("creating a %T named \"%T\" - APIERR_NOPLAN here means no floor-plan "
                                                                    "window is open; APIERR_REFUSEDCMD means an undo scope was open, so this "
                                                                    "was called from inside evp.transaction%T",
                                                                    typeName.ToPrintf (), name.ToPrintf (), clash.ToPrintf ())));
        }

        os.Add ("type", typeName);
        os.Add ("databaseId", GuidId (UnIdToString (info.databaseUnId)));
        os.Add ("name", GS::UniString (info.name));
        os.Add ("ref",  GS::UniString (info.ref));
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.SetDocumentFrom3DSettings { guid, fromCurrent3DView?: true,
//                                 transparency?, cutaway3D?, materialFrom3D? }
//   -> { ok, guid, name, isPersp, applied: [...], verified }
//
// The SECOND HALF of "current 3D view -> a 3D Document" (§E19). CreateDatabase
// makes the document but it takes the 3D-document DEFAULTS, so it is pointed at
// whatever camera the defaults carry rather than at the view the user is looking
// at. This copies the live 3D window's projection and appearance into it.
//
// Deliberately NOT folded into EvP.CreateDatabase: they are two questions (does
// the create work / does the settings copy stick) and the probe discipline
// depends on being able to fail them independently. They also cannot share a
// transaction — see the class note below.
//
// ⚠️⚠️ READ-MODIFY-WRITE, NEVER A ZEROED STRUCT. The Get call is not a
// formality: API_DocumentFrom3DType carries ~20 fields this command does not
// touch (filter and cut settings, skeleton options, boundary display, plan
// connectivity, uniform attribute overrides). Writing a zeroed struct would
// silently reset every one of them AND point the document at a camera at the
// origin looking nowhere — which still produces a valid-looking document, which
// is why the mistake survives review. Same inherit-don't-zero rule CreateLayout
// follows.
//
// ⚠️ StructuralCommand, and this was checked rather than assumed. The AC29
// header's @remarks for ACAPI_View_ChangeDocumentFrom3DSettings says, verbatim,
// "This function is a non-undoable data structure modifier function" — the same
// sentence that puts NewDatabase and DeleteDatabase in this category. So it gets
// no undo scope, it is refused by name inside evp.transaction, and IT IS NOT
// UNDOABLE: Ctrl+Z will not restore the document's previous settings. (Unlike
// DeleteDatabase this needs no `confirm` — the thing at risk is one document's
// settings, and re-running against the wanted 3D view restores them.)
//
// ⚠️ Get3DProjectionSets READS THE 3D WINDOW, whatever is frontmost. If the user
// is looking at a floor plan, the settings copied are the 3D window's last state,
// not "what I can see". The command cannot detect the difference; the probe's
// NOW LOOK block is what catches it.
//
// ⚠️ CUT PLANES ARE NOT COPIED. cutSetting is inherited from the document's own
// settings and left alone. The 3D window's cutting planes are a separate call
// (ACAPI_View_Get3DCuttingPlanes) with a second owned handle, and nothing in the
// massing flow has asked for them — `cutaway3D` toggles the document's existing
// planes on and off, which is the part that was actually wanted.
// ---------------------------------------------------------------------------
class SetDocumentFrom3DSettingsCommand : public StructuralCommand {
public:
    GS::String GetName () const override { return "SetDocumentFrom3DSettings"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::UniString guidString;
        if (!GetIdGuid (params, "databaseId", guidString)) {
            return NativeCommandResult::Failure (EVP_FAIL ("need guid=<3D document guid> (EvP.ListDatabases types=[\"3dDocument\"] lists them)",
                                                            "EvP.SetDocumentFrom3DSettings"));
        }

        bool fromCurrent3DView = true;
        params.Get ("fromCurrent3DView", fromCurrent3DView);

        bool       transparency = false, cutaway3D = false, materialFrom3D = false;
        const bool haveTransparency   = params.Get ("transparency",   transparency);
        const bool haveCutaway3D      = params.Get ("cutaway3D",      cutaway3D);
        const bool haveMaterialFrom3D = params.Get ("materialFrom3D", materialFrom3D);

        // Nothing to do is a refusal, not a successful no-op: a caller that meant
        // to pass a flag and misspelled it would otherwise get ok=true and an
        // unchanged document, and never learn which.
        if (!fromCurrent3DView && !haveTransparency && !haveCutaway3D && !haveMaterialFrom3D) {
            return NativeCommandResult::Failure (EVP_FAIL ("nothing to apply: fromCurrent3DView is false and no transparency / cutaway3D / materialFrom3D "
                                       "was given, so this call would write the settings back unchanged",
                                                            "EvP.SetDocumentFrom3DSettings"));
        }

        API_DatabaseUnId unId = {};
        if (!FindDatabase (guidString, APIWind_DocumentFrom3DID, unId)) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("no 3D document with guid %T (EvP.ListDatabases types=[\"3dDocument\"] shows the "
                                                              "ones that exist). If this guid came from an EvP.CreateDatabase that just "
                                                              "returned ok, suspect timing rather than a bad guid - the 3D-document create "
                                                              "measured 2397 ms in the E23 run",
                                                              guidString.ToPrintf ()),
                                                            "EvP.SetDocumentFrom3DSettings"));
        }

        // The name, for the response: a caller (and a probe log) needs to say
        // WHICH document it repointed, and a guid is not something a user can
        // check against Archicad's UI.
        API_DatabaseInfo info;
        const bool       haveInfo = ReadDatabaseInfo (unId, APIWind_DocumentFrom3DID, info);

        // ---- read (never start from a zeroed struct) ----
        DocumentFrom3DSettings settings;
        if (const GSErrCode err = ACAPI_View_GetDocumentFrom3DSettings (&unId, &settings.value); err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_GetDocumentFrom3DSettings", err,
                                                                  GS::UniString::Printf ("reading the current settings of 3D document \"%T\" before modifying them - "
                                                                    "APIERR_BADDATABASE here means the guid names a database that is not a 3D "
                                                                    "document",
                                                                    haveInfo ? GS::UniString (info.name).ToPrintf () : guidString.ToPrintf ())));
        }

        // ---- modify ----
        GS::Array<GS::UniString> applied;

        API_3DProjectionInfo wanted = {};
        if (fromCurrent3DView) {
            if (const GSErrCode err = ACAPI_View_Get3DProjectionSets (&wanted); err != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_Get3DProjectionSets", err,
                                                                      "reading the current 3D window's projection to copy into the 3D document - if no 3D window "
                                                 "has been opened this session there may be no projection to read, in which case open the 3D "
                                                 "view once and re-run rather than accepting a default camera"));
            }
            settings.value.projectionSetting = wanted;
            applied.Push ("projectionSetting");

            API_3DWindowInfo window3D = {};
            if (const GSErrCode err = ACAPI_View_Get3DWindowSets (&window3D); err != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_Get3DWindowSets", err,
                                                                      "reading the current 3D window's appearance to copy into the 3D document"));
            }
            settings.value.window3DInfo = window3D;
            applied.Push ("window3DInfo");
        }

        if (haveTransparency)   { settings.value.transparency   = transparency;   applied.Push ("transparency"); }
        if (haveCutaway3D)      { settings.value.cutaway3D      = cutaway3D;      applied.Push ("cutaway3D"); }
        if (haveMaterialFrom3D) { settings.value.materialFrom3D = materialFrom3D; applied.Push ("materialFrom3D"); }

        // ---- write ----
        if (const GSErrCode err = ACAPI_View_ChangeDocumentFrom3DSettings (&unId, &settings.value); err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_View_ChangeDocumentFrom3DSettings", err,
                                                                  GS::UniString::Printf ("applying the current 3D view's settings to 3D document \"%T\" - "
                                                                    "APIERR_REFUSEDCMD here means an undo scope was open, so this was called "
                                                                    "from inside evp.transaction (it is a structural command and cannot be)",
                                                                    haveInfo ? GS::UniString (info.name).ToPrintf () : guidString.ToPrintf ())));
        }

        // ---- read back ----
        //
        // NOT paranoia and not optional: NoError from this family has already been
        // observed NOT to mean the change landed (§E20's silent no-op), which is
        // why SetDrawingClipPolygon verifies too. `verified` false with ok true is
        // the honest answer — the call succeeded and the data did not change — and
        // it is the one result a caller must not have to infer.
        bool verified = false;
        if (fromCurrent3DView) {
            DocumentFrom3DSettings readBack;
            if (ACAPI_View_GetDocumentFrom3DSettings (&unId, &readBack.value) == NoError)
                verified = SameProjection (wanted, readBack.value.projectionSetting);
        }

        os.Add ("databaseId", GuidId (guidString));
        os.Add ("name",     haveInfo ? GS::UniString (info.name) : GS::UniString ());
        os.Add ("isPersp",  settings.value.projectionSetting.isPersp);
        os.Add ("applied",  applied);
        os.Add ("verified", verified);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.DeleteDatabase { type, guids: [...], confirm: true }
//   -> { ok, deleted, results: [ {guid, ok, name?, error?} ] }
//
// ACAPI_Database_DeleteDatabase. Covers "delete worksheet", "delete 3D document",
// "delete detail" — none of which the JSON API can do. (Deleting a LAYOUT or a
// SUBSET goes through API.DeleteNavigatorItems, which already works; see
// evp.layouts. This command still accepts "layout" because a layout IS an
// independent database and the native path is one call rather than a navigator
// lookup.)
//
// ⚠️⚠️ NOT UNDOABLE. This is the whole reason `confirm: true` is mandatory
// rather than a convenience: there is no undo step to fall back on, so a script
// with a bug in its guid list destroys work permanently. The flag makes the
// caller state that deletion is what it meant. Every other create/read command
// in EvP is recoverable; this one is not, and the API should not read as though
// it were.
//
// Reports the NAME of each deleted database, read BEFORE the delete — afterwards
// there is nothing left to ask, and "deleted 3 databases" is not something a
// user can check.
// ---------------------------------------------------------------------------
class DeleteDatabaseCommand : public StructuralCommand {
public:
    GS::String GetName () const override { return "DeleteDatabase"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::UniString              typeName;
        GS::Array<GS::ObjectState> databaseItems;
        if (!params.Get ("type", typeName) || !params.Get ("databases", databaseItems)) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("need type=<%s> and databases=[{databaseId:{guid}}]", kLegalTypes), "Tapioca.DeleteDatabase"));
        }

        GS::Array<GS::UniString> guidStrings;
        for (const GS::ObjectState& databaseItem : databaseItems) {
            GS::UniString guidString;
            if (!GetIdGuid (databaseItem, "databaseId", guidString)) {
                return NativeCommandResult::Failure (EVP_FAIL ("every database needs databaseId.guid", "Tapioca.DeleteDatabase"));
            }
            guidStrings.Push (guidString);
        }

        API_DatabaseTypeID type;
        if (!ParseDatabaseType (typeName, type)) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown database type: %T (want %s)", typeName.ToPrintf (), kLegalTypes),
                                                            "EvP.DeleteDatabase"));
        }

        bool confirm = false;
        params.Get ("confirm", confirm);
        if (!confirm) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("refusing to delete %u %T database(s) without confirm=true: this is NOT UNDOABLE "
                                                             "(a structural change leaves no undo step, so Ctrl+Z cannot bring it back)",
                                                             (unsigned) guidStrings.GetSize (), typeName.ToPrintf ()),
                                                            "EvP.DeleteDatabase"));
        }

        // Which databases of this type actually exist. A guid that is not in this
        // set is reported as such rather than handed to DeleteDatabase, which
        // would answer APIERR_BADPARS and leave the caller unable to tell a typo
        // from a database that was already gone.
        const GS::Array<API_DatabaseUnId> existing = ACAPI_Database_GetDatabasesForType (type);

        GS::Array<GS::ObjectState> results;
        GS::Int32                  deleted = 0;

        for (const GS::UniString& guidString : guidStrings) {
            GS::ObjectState rec;
            rec.Add ("databaseId", GuidId (guidString));

            const API_Guid wanted = APIGuidFromString (guidString.ToCStr ().Get ());

            bool             found = false;
            API_DatabaseUnId unId  = {};
            for (const API_DatabaseUnId& candidate : existing) {
                if (candidate.elemSetId == wanted) { unId = candidate; found = true; break; }
            }
            if (!found) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_FAIL (GS::UniString::Printf ("no %T database with guid %T (EvP.ListDatabases shows the ones that exist)",
                                                                  typeName.ToPrintf (), guidString.ToPrintf ()),
                                            "EvP.DeleteDatabase"));
                results.Push (rec);
                continue;
            }

            // The name, while it still exists to be read.
            API_DatabaseInfo info;
            const bool       haveInfo = ReadDatabaseInfo (unId, type, info);
            if (haveInfo)
                rec.Add ("name", GS::UniString (info.name));

            API_DatabaseInfo toDelete = {};
            toDelete.typeID       = type;
            toDelete.databaseUnId = unId;

            if (const GSErrCode err = ACAPI_Database_DeleteDatabase (&toDelete); err != NoError) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Database_DeleteDatabase", err,
                                                  GS::UniString::Printf ("deleting %T \"%T\" - APIERR_REFUSEDCMD here means an undo scope was "
                                                                         "open (called from inside evp.transaction); APIERR_NOTMINE means "
                                                                         "insufficient Teamwork privileges",
                                                                         typeName.ToPrintf (),
                                                                         haveInfo ? GS::UniString (info.name).ToPrintf () : guidString.ToPrintf ())));
            } else {
                rec.Add ("succeeded", true);
                ++deleted;
            }
            results.Push (rec);
        }

        os.Add ("deleted", deleted);
        os.Add ("results", results);
        return os;
    }
};

const NativeCommandRegistration LayoutCommandRegistrations[] = {
    { "ListDatabases", &MakeRegisteredNativeCommand<ListDatabasesCommand>, false,
      R"json({"type":"object","properties":{"types":{"type":"array","items":{"type":"string","enum":["worksheet","detail","layout","masterLayout","3dDocument"]}}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"databases":{"type":"array","items":{"type":"object","properties":{"type":{"type":"string","enum":["worksheet","detail","layout","masterLayout","3dDocument"]},"databaseId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"name":{"type":"string"},"ref":{"type":"string"},"title":{"type":"string"},"masterLayoutId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"error":{"type":"string"}},"additionalProperties":false,"required":["type","databaseId"]}},"count":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["databases","count"]})json" },
    { "ListViews", &MakeRegisteredNativeCommand<ListViewsCommand>, false,
      R"json({"type":"object","properties":{"map":{"type":"string","enum":["view","myView","project","layout"]},"placeableOnly":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"views":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"navigatorItemId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"path":{"type":"string"},"itemType":{"type":"string"},"placeable":{"type":"boolean"},"map":{"type":"string","enum":["view","myView","project","layout"]}},"additionalProperties":false,"required":["name","navigatorItemId","path","itemType","placeable","map"]}},"count":{"type":"integer","minimum":0},"note":{"type":"string"}},"additionalProperties":false,"required":["views","count"]})json" },
    { "CreateDatabase", &MakeRegisteredNativeCommand<CreateDatabaseCommand>, false,
      R"json({"type":"object","properties":{"type":{"type":"string","enum":["worksheet","detail","layout","masterLayout","3dDocument"]},"name":{"type":"string"},"ref":{"type":"string"}},"additionalProperties":false,"required":["type"]})json",
      R"json({"type":"object","properties":{"type":{"type":"string","enum":["worksheet","detail","masterLayout","3dDocument"]},"databaseId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"name":{"type":"string"},"ref":{"type":"string"}},"additionalProperties":false,"required":["type","databaseId","name","ref"]})json" },
    { "DeleteDatabase", &MakeRegisteredNativeCommand<DeleteDatabaseCommand>, false,
      R"json({"type":"object","properties":{"type":{"type":"string","enum":["worksheet","detail","layout","masterLayout","3dDocument"]},"databases":{"type":"array","items":{"type":"object","properties":{"databaseId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["databaseId"]}},"confirm":{"type":"boolean"}},"additionalProperties":false,"required":["type","databases","confirm"]})json",
      R"json({"type":"object","properties":{"deleted":{"type":"integer","minimum":0},"results":{"type":"array","items":{"type":"object","properties":{"databaseId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"succeeded":{"type":"boolean"},"name":{"type":"string"},"error":{"type":"string"}},"additionalProperties":false,"required":["databaseId","succeeded"]}}},"additionalProperties":false,"required":["deleted","results"]})json" },
    { "SetDocumentFrom3DSettings", &MakeRegisteredNativeCommand<SetDocumentFrom3DSettingsCommand>, false,
      R"json({"type":"object","properties":{"databaseId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"fromCurrent3DView":{"type":"boolean"},"transparency":{"type":"boolean"},"cutaway3D":{"type":"boolean"},"materialFrom3D":{"type":"boolean"}},"additionalProperties":false,"required":["databaseId"]})json",
      R"json({"type":"object","properties":{"databaseId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"name":{"type":"string"},"isPersp":{"type":"boolean"},"applied":{"type":"array","items":{"type":"string","enum":["projectionSetting","window3DInfo","transparency","cutaway3D","materialFrom3D"]}},"verified":{"type":"boolean"}},"additionalProperties":false,"required":["databaseId","name","isPersp","applied","verified"]})json" },
};

}   // namespace

NativeCommandRegistrations GetLayoutCommandRegistrations ()
{
    return MakeRegistrationView (LayoutCommandRegistrations);
}

} // namespace geomsrv

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Palette/NavItemChoices.hpp"
#include "NativeCommands/CommandUtils.hpp"   // CollectNavigatorItems, IsPlaceableViewItem

// ===========================================================================
// The rows behind evp.View and evp.Database.
//
// WHY THIS IS A FILE AND NOT A BRANCH IN ParamPanel. Two reasons, in order:
//   * ParamPanel builds DG CONTROLS; this is navigator/database enumeration,
//     which is a different concern that happens to feed one. The Story branch
//     gets away with being inline because its whole source is one ACAPI call.
//   * ParamPanel.cpp is at the soft cap. Repo rule: crossing it means splitting
//     in the same commit, so the split happens here rather than after.
//
// ⚠️ READ STRAIGHT FROM ACAPI, NOT OVER THE BUS. EvP.ListViews / EvP.ListDatabases
// return exactly this data, and calling them from here would be wrong anyway: the
// palette is ALREADY on the main thread, so a bus call would be a gate hop to the
// thread we are standing on. Same reason the Story branch reads
// ACAPI_ProjectSetting_GetStorySettings directly instead of calling EvP.GetStories.
// The native commands stay the route for SCRIPTS; this is the route for the dialog.
// ===========================================================================

namespace evp {

namespace {

// Name, then the folder path that tells two same-named items apart.
//
// ⚠️ THE PATH IS NOT DECORATION. The measured design case is 12 views called
// "Story" in one project — a picker showing bare names would reproduce exactly the
// ambiguity that made name-based lookup unusable in the first place.
//
// ASCII " - " rather than an em dash on purpose: the separator has to survive a
// GS::UniString built from a narrow literal, and being pretty is not worth an
// encoding question in a label the user has to read.
GS::UniString RowLabel (const GS::UniString& name, const GS::UniString& path)
{
    if (path.IsEmpty ())
        return name;
    return GS::UniString::Printf ("%T  -  %T", name.ToPrintf (), path.ToPrintf ());
}

const char* DatabaseKindName (API_DatabaseTypeID type)
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

}   // namespace

void CollectViewChoices (GS::Array<NavItemChoice>& rows)
{
    rows.Clear ();

    // The Public View Map, plus My View Map WHEN THERE IS ONE.
    //
    // ⚠️ My View Map is TEAMWORK-ONLY (APIdefs_Navigator.h says so on the enum
    // member), and on a solo project the read answers APIERR_NOTEAMWORKPROJECT.
    // Asking anyway is not harmless here: this runs on every palette rebuild, so
    // it wrote an API error into api_errors.log each time a command was selected —
    // a log full of a non-problem is how a real one gets missed. Caught live
    // 2026-07-31. CollectNavigatorItems APPENDS, which is what lets the two compose.
    GS::Array<geomsrv::NavigatorEntry> entries;
    geomsrv::CollectNavigatorItems (API_PublicViewMap, entries);
    if (ACAPI_Teamwork_HasConnection ())
        geomsrv::CollectNavigatorItems (API_MyViewMap, entries);

    for (const geomsrv::NavigatorEntry& entry : entries) {
        // Folders and the project root are not things a command can act on.
        if (!geomsrv::IsPlaceableViewItem (entry.itemType))
            continue;

        // Row and guid pushed in ONE statement. Any path that appends one without
        // the other shifts every guid after it by a row, and the picker then
        // returns a plausible, WRONG item — the failure with no symptom.
        NavItemChoice row;
        row.label = RowLabel (entry.name, entry.path);
        row.guid  = GS::UniString (APIGuidToString (entry.guid).ToCStr ().Get ());
        rows.Push (row);
    }
}

bool LookUpViewLabel (const GS::UniString& guid, GS::UniString& label)
{
    label.Clear ();
    if (guid.IsEmpty ())
        return false;

    GS::Array<NavItemChoice> rows;
    CollectViewChoices (rows);
    for (const NavItemChoice& row : rows) {
        if (row.guid == guid) { label = row.label; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The Navigator as a tree
// ---------------------------------------------------------------------------

namespace {

// One map's subtree, appended under an already-pushed root node.
//
// A SECOND WALK rather than a reuse of CollectNavigatorItems, and the difference is
// the whole point: that one flattens, which is all a popup needs and is exactly
// what the browser must not do. Same safety properties, which are not optional —
// this runs on the main thread, so a cycle in the tree would hang ARCHICAD:
// depth-capped, and visited-guarded with a linear-scanned array (API_Guid has no
// GenerateHashValue in the AC29 headers, and a navigator map is tens of items).
void AppendMapSubtree (API_NavigatorMapID mapId, Int32 rootIndex, Int32 mapIndex,
                       GS::Array<NavTreeNode>& nodes)
{
    API_NavigatorSet set = {};
    set.mapId = mapId;
    if (ACAPI_Navigator_GetNavigatorSet (&set) != NoError)
        return;     // an absent map is a fact about the project, not a failure

    struct Pending { API_Guid guid; Int32 index; GS::UniString path; Int32 depth; };
    GS::Array<Pending>  queue;
    GS::Array<API_Guid> visited;
    const Int32 maxDepth = 32;

    queue.Push (Pending { set.rootGuid, rootIndex, GS::UniString (), 0 });
    visited.Push (set.rootGuid);

    while (!queue.IsEmpty ()) {
        const Pending current = queue.Pop ();
        if (current.depth >= maxDepth)
            continue;

        API_NavigatorItem parent = {};
        parent.guid  = current.guid;
        parent.mapId = mapId;                       // set for performance, per the header

        GS::Array<API_NavigatorItem> children;
        if (ACAPI_Navigator_GetNavigatorChildrenItems (&parent, &children) != NoError)
            continue;                               // a childless node answers an error

        // Appended IN ORDER, so siblings stay contiguous and in Navigator order —
        // which is what makes the dialog read like the Navigator rather than like a
        // set. Every child is pushed here before it is ever expanded below, which is
        // what guarantees "parent precedes child" for the whole array.
        for (const API_NavigatorItem& child : children) {
            if (visited.Contains (child.guid))
                continue;
            visited.Push (child.guid);

            NavTreeNode node;
            node.name       = GS::UniString (child.uName);
            node.guid       = GS::UniString (APIGuidToString (child.guid).ToCStr ().Get ());
            node.path       = current.path;
            node.typeName   = geomsrv::NavItemTypeName (child.itemType);
            node.selectable = geomsrv::IsPlaceableViewItem (child.itemType);
            node.parent     = current.index;
            node.mapIndex   = mapIndex;
            nodes.Push (node);

            queue.Push (Pending { child.guid, (Int32) (nodes.GetSize () - 1),
                                  current.path.IsEmpty () ? node.name : current.path + "/" + node.name,
                                  current.depth + 1 });
        }
    }
}

}   // namespace

// ⚠️ THE ORDER HERE IS THE TAB ORDER AND THE mapIndex. Change one and you change
// all three. Publisher Sets are deliberately absent: nothing can be aimed at one.
//
// ⚠️ My View Map is NOT among them even in Teamwork mode. It would be a fourth tab
// that is empty on every solo project, and the browser is a UI — a permanently
// empty tab is a thing a user has to learn to ignore. The flat popup still reads it
// (CollectViewChoices), so nothing is lost to a script.
const char* const kNavMapNames[3] = { "Project Map", "View Map", "Layout Book" };

void CollectNavigatorTree (GS::Array<NavTreeNode>& nodes)
{
    nodes.Clear ();

    const API_NavigatorMapID mapIds[3] = { API_ProjectMap, API_PublicViewMap, API_LayoutMap };

    for (Int32 mapIndex = 0; mapIndex < 3; ++mapIndex) {
        // The root is kept in the array as the parent every top-level item points
        // at, but the browser does NOT draw it: the tab already names the map, and a
        // root row inside its own tab would be a pointless level of indentation on
        // everything below it.
        NavTreeNode root;
        root.name       = GS::UniString (kNavMapNames[mapIndex]);
        root.selectable = false;
        root.parent     = -1;
        root.mapIndex   = mapIndex;
        nodes.Push (root);

        AppendMapSubtree (mapIds[mapIndex], (Int32) (nodes.GetSize () - 1), mapIndex, nodes);
    }
}

void CollectDatabaseChoices (GS::Array<NavItemChoice>& rows)
{
    rows.Clear ();

    // The five INDEPENDENT database types, the same set EvP.ListDatabases uses.
    // ACAPI_Database_GetDatabasesForType returns only independent databases, which
    // is exactly the set anything here can be aimed at.
    const API_DatabaseTypeID types[] = {
        APIWind_WorksheetID, APIWind_DetailID, APIWind_LayoutID,
        APIWind_MasterLayoutID, APIWind_DocumentFrom3DID
    };

    for (const API_DatabaseTypeID type : types) {
        for (const API_DatabaseUnId& unId : ACAPI_Database_GetDatabasesForType (type)) {
            API_DatabaseInfo info = {};
            info.typeID       = type;
            info.databaseUnId = unId;

            // TITLE first: "A.01.3 2. Story" carries the ID prefix the user reads in
            // the Navigator, where `name` is only "2. Story". If the info will not
            // read at all the row still exists — dropping it would leave a shorter
            // list with no explanation, and the guid is still perfectly usable.
            GS::UniString text;
            if (ACAPI_Window_GetDatabaseInfo (&info) == NoError) {
                text = GS::UniString (info.title);
                if (text.IsEmpty ())
                    text = GS::UniString (info.name);
            }
            if (text.IsEmpty ())
                text = "(unnamed)";

            NavItemChoice row;
            row.label = GS::UniString::Printf ("%s  -  %T", DatabaseKindName (type), text.ToPrintf ());
            // A database's identity on the wire is its elemSetId guid — the single
            // field of API_DatabaseUnId in AC29, and what every EvP.*Database
            // command takes.
            row.guid = GS::UniString (APIGuidToString (unId.elemSetId).ToCStr ().Get ());
            rows.Push (row);
        }
    }
}

}   // namespace evp

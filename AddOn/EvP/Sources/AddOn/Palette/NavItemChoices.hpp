#ifndef GEOMETRYSERVER_PALETTE_NAVITEMCHOICES_HPP
#define GEOMETRYSERVER_PALETTE_NAVITEMCHOICES_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

namespace evp {

// One row of an evp.View / evp.Database picker.
//
// ⚠️ THE LABEL AND THE GUID ARE THE SAME OBJECT ON PURPOSE. The row a user reads
// and the value run() receives are DIFFERENT THINGS, and the only thing keeping
// them in step is that they are pushed together. Story already learned this with
// its parallel storyIndices array; carrying the pair in one struct is the version
// that cannot drift, because there is no way to append a row and forget its guid.
struct NavItemChoice {
    GS::UniString label;   // "Ground Floor  —  Projektas/Plans" — name, then folder path
    GS::UniString guid;    // what run() actually gets
};

// Every placeable saved view, across BOTH view maps. Empty is a legitimate answer
// (a bare template project has no saved views) — the caller shows a fallback row.
// MAIN THREAD ONLY: this is ACAPI, called from the palette, which is already there.
void CollectViewChoices (GS::Array<NavItemChoice>& rows);

// The row text for one view guid, if that guid still names a placeable view.
// False when it does not — a guid from a deleted or renamed-away item, or a
// non-guid string. The caller must then show NOTHING rather than the guid: a
// control displaying a label for an item that no longer exists reads as a valid
// choice, which is the worst of the three states. MAIN THREAD ONLY.
bool LookUpViewLabel (const GS::UniString& guid, GS::UniString& label);

// ---------------------------------------------------------------------------
// The Navigator as a TREE, for the browser dialog.
//
// Why a second collector rather than reusing the flat one: the flat list answers
// "which items exist", which is all a popup needs, and it throws the SHAPE away.
// The browser's whole value is the shape — a user recognises "Project Map >
// Sections > S-01", not a list of 200 rows. The Project Map's own top-level nodes
// ARE the categories (Stories, Sections, Elevations, Interior Elevations,
// Worksheets, Details, 3D Documents, 3D, Schedules), so mirroring Archicad's real
// hierarchy gives that grouping for free and gives the View Map and Layout Book
// their real user folders instead of a synthetic regrouping.
// ---------------------------------------------------------------------------
struct NavTreeNode {
    GS::UniString name;        // what the row shows
    GS::UniString guid;        // EMPTY for a synthetic map root and for folders
    GS::UniString path;        // slash-joined ancestors, for the selection line
    GS::UniString typeName;    // "story", "section", … — empty for roots/folders
    bool          selectable = false;   // a leaf a command can actually be given
    Int32         parent     = -1;      // index into the array; -1 for a map root
    Int32         mapIndex   = 0;       // 0 Project Map, 1 View Map, 2 Layout Book
};

// The tab labels, in mapIndex order. Here rather than in the dialog so the names
// and the indices cannot drift apart.
extern const char* const kNavMapNames[3];

// The three maps a user chooses from, each as a root node with its subtree below.
//
// ⚠️ PARENTS ALWAYS PRECEDE THEIR CHILDREN in the returned array, so a consumer can
// build a DG tree in one forward pass without a second lookup. The walk pushes a
// node before it ever pushes that node's children, which is what guarantees it.
//
// Project Map items are included and marked `selectable` by the same rule as
// everywhere else (IsPlaceableViewItem): they are NOT placeable as drawings, so
// they come back unselectable rather than silently absent — a user who cannot find
// "Ground Floor" in the View Map needs to SEE it greyed out in the Project Map,
// not wonder where it went. MAIN THREAD ONLY.
void CollectNavigatorTree (GS::Array<NavTreeNode>& nodes);

// Every independent database — worksheets, details, layouts, master layouts, 3D
// documents. Labelled by TITLE where there is one ("A.01.3 2. Story"), because the
// title carries the ID prefix a user reads in the Navigator; `name` alone does not.
// MAIN THREAD ONLY.
void CollectDatabaseChoices (GS::Array<NavItemChoice>& rows);

}   // namespace evp

#endif

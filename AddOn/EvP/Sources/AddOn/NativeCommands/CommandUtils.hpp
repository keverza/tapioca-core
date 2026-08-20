#ifndef EVP_NATIVECOMMANDS_COMMANDUTILS_HPP
#define EVP_NATIVECOMMANDS_COMMANDUTILS_HPP

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "ObjectState.hpp"

// Cross-domain helpers for the native commands — and ONLY cross-domain ones.
//
// THE RULE: a helper moves here on its SECOND domain, never speculatively. Most
// of the free functions in the old AddOnCommands.cpp turned out to serve exactly
// one domain; those stayed `static` in their domain file, where their callers
// are. If you are about to add something here that only one file calls, put it
// in that file instead — this is the place a dumping ground would form.
namespace geomsrv {

// ---------------------------------------------------------------------------
// Attribute name <-> index
//
// Attribute pickers hand a script the attribute NAME, never the index
// (ControlPalette serialises index -> name). A create command needs the index,
// so it resolves the name here. Used by CreateCommands, ElementReadCommands
// (GetAttributeInfo) and ControlPalette — three call sites, hence shared.
//
// Neither side silently falls back to a default: an unresolved name is a
// reported error, so a typo can never quietly place a wall in the wrong
// material. MAIN THREAD ONLY (both call ACAPI_Attribute_GetAttributesByType).
// ---------------------------------------------------------------------------
bool AttributeNameToIndex (API_AttrTypeID type, const GS::UniString& name, API_AttributeIndex& index);

// The reverse lookup. Returns an empty string if the index does not resolve.
GS::UniString AttributeIndexToName (API_AttrTypeID type, const API_AttributeIndex& index);

// ---------------------------------------------------------------------------
// Layer: the optional `layer` param -> head.layer
//
// Every create command wants the same thing — an evp.Layer picker hands the
// script a layer NAME, and the element header wants an index. Shared rather
// than copied because it also has to make the same DECISION every time: an
// OMITTED layer keeps whatever ACAPI_Element_GetDefaults chose (the user's
// current default layer, which is usually what they want), while a layer that
// is PRESENT but unresolvable is a reported error, never a silent fallback —
// the same "a typo must not quietly place the element somewhere else" rule the
// attribute lookups above follow.
//
// It ALSO refuses a HIDDEN or LOCKED target layer, whether the caller named it
// or inherited it from the tool default — because that failure is otherwise
// near-invisible (Archicad returns a bare code with no layer in it, and the user
// just sees an empty drawing). This is why the check lives here and not in each
// command: one seam, and every create command gets it.
//
// Returns false with `err` set for the present-but-unresolvable case AND for the
// hidden/locked case. MAIN THREAD ONLY. Callers: the drafting creates (text,
// picture) and the structural creates.
// ---------------------------------------------------------------------------
bool ResolveLayerParam (const GS::ObjectState& params, API_Elem_Head& head, GS::UniString& err);

// ---------------------------------------------------------------------------
// Anchor: which point of an element's box its placement coordinate means
//
// Spelled the way a script thinks about it ("topLeft") rather than the API's
// two-letter APIAnc_LT codes, because getting this wrong is the single most
// common reason a placed element looks offset. Returns false for an unknown
// name — a caller reports that rather than defaulting, so a typo cannot quietly
// shift the placement.
//
// Shared on its SECOND domain: the drafting creates (text, picture) and the
// drawing placement. No ACAPI, so callable from anywhere.
// ---------------------------------------------------------------------------
bool ParseAnchor (const GS::UniString& name, API_AnchorID& anchor);

// ---------------------------------------------------------------------------
// Navigator: every item in one of the Navigator's maps, flattened
//
// WHY THIS EXISTS AT ALL — the lesson that paid for it (2026-07-30). A command
// that takes a navigator GUID is UNUSABLE from inside Archicad: the UI shows a
// user names, never guids, so there is no way for them to supply one. Every
// EvP surface that identifies a navigator item therefore has to accept a NAME
// and resolve it here, exactly as the attribute pickers already do for layers,
// materials and profiles (the names-not-indices policy). A guid stays accepted
// for script-to-script use.
//
// `ACAPI_Navigator_SearchNavigatorItem` cannot do this: its header makes
// `itemType` compulsory, so it searches for items referring to one database
// rather than listing a map. The listing is a tree walk —
// ACAPI_Navigator_GetNavigatorSet for the root, then
// ACAPI_Navigator_GetNavigatorChildrenItems down. Depth-capped and
// visited-guarded: a malformed tree must not hang Archicad on the main thread.
//
// `path` is the slash-joined ancestor names, so an ambiguous name can be
// reported with enough context for the user to tell the two apart. Shared on its
// second domain: the view listing and the drawing placement. MAIN THREAD ONLY.
// ---------------------------------------------------------------------------
struct NavigatorEntry {
    API_Guid                guid;
    GS::UniString           name;
    GS::UniString           path;       // "Folder/Sub" — ancestors, not including name
    API_NavigatorItemTypeID itemType;
    API_NavigatorMapID      mapId;
};

bool CollectNavigatorItems (API_NavigatorMapID mapId, GS::Array<NavigatorEntry>& items);

// Is this item type something a Drawing can take as its SOURCE? Folders, the
// project root and the layout-book types are not. Used to filter the listing and
// to refuse a bad name with a reason rather than placing an empty drawing.
bool IsPlaceableViewItem (API_NavigatorItemTypeID itemType);

// A navigator item type as a name a script (or a picker row) can read. Only the
// types a Drawing can actually source from are spelled out; the rest report the raw
// enum value, which is enough to diagnose "why is my item not placeable".
// Shared on its second consumer: EvP.ListViews and the Navigator browser dialog.
GS::UniString NavItemTypeName (API_NavigatorItemTypeID itemType);

// ---------------------------------------------------------------------------
// Story: a world-Z elevation -> (owning story, offset from that story's level)
//
// The piece worth absorbing from Tapir's element-creation base: an element
// BELONGS to a story, and its vertical fields (wall bottomOffset, roof
// shellBase level, column bottomOffset) are measured FROM that story's level,
// not from world zero. If the caller names a floor, that story owns the
// element; otherwise pick the nearest story at or below z (Tapir's
// GetFloorIndexAndOffset rule), falling back to the lowest story when z is
// below them all.
//
// Shared on its second domain — the structural creates and the roof creates,
// after the roofs moved to their own file. MAIN THREAD ONLY.
// ---------------------------------------------------------------------------
bool ResolveStory (double z, bool haveFloor, GS::Int32 requestedFloor,
                   short& floorInd, double& offset, GS::UniString& err);

// ---------------------------------------------------------------------------
// Archicad's polygon handle triple, walked into flat rings
//
// Archicad spells EVERY polygon the same way — a coordinate handle, a
// sub-polygon end-index handle and an arc handle — and the indexing is subtle
// enough that a second copy of the walk is a bug waiting to happen. It now has
// exactly one implementation, and this is it.
//
// ⚠️ THE CONVENTION, verified against CreateMesh / Element_Basics
// Do_CreateIrregularMesh and the API_Polygon remark in APIdefs_Base.h:
// `coords` is 1-INDEXED ([0] is unused); sub-polygon `sub` (1-based, with
// pends[0] == 0) spans coords[pends[sub-1]+1 .. pends[sub]], the last index
// being a CLOSING REPEAT of the first, so a contour's distinct vertices number
// pends[sub] - pends[sub-1] - 1. Sub 1 is the outer contour, subs 2..n are
// holes. `parcs` is 0-indexed with nArcs = handle size / sizeof(API_PolyArc),
// and arcAngle is positive when the arc bulges to the RIGHT of the
// begIndex->endIndex direction.
//
// ARC edges: one arc angle is emitted PER VERTEX — the signed angle (radians)
// of the edge LEAVING that vertex, or 0 for a straight edge. Vertex k of a ring
// lives at coord index start+k and its arc record stores begIndex == start+k,
// so the lookup is exact. Without it a curved slab edge reads as its chord.
//
// `polylineMode` — a POLYGON always repeats its first node, so the closing
// vertex is dropped unconditionally. An OPEN polyline does NOT repeat it
// (Element_Basics builds a 5-vertex polyline as nCoords=5, pends[1]==5, all
// distinct), so that same drop would silently eat its last vertex. In polyline
// mode the repeat is DETECTED rather than assumed — which is exactly what
// `outerClosed` answers — and only dropped if it is really there. The mode is
// chosen by ELEMENT TYPE at the call site, never inferred from the data, so a
// slab's reading cannot change. A polyline has one contour, so the walk stops
// after sub 1 rather than filing later subs as holes.
//
// `polyZ` (the mesh kind) — one elevation per contour vertex, indexed exactly
// like `coords`. Pass it together with `outerZ`/`holeZ` and each kept vertex's
// elevation comes back in the SAME walk, which is the point: the indexing above
// must not be reimplemented next to a second handle. Pass nullptr and the
// arrays stay EMPTY rather than zero-filled, so "no elevations" stays visibly
// different from "elevations that happen to be 0".
//
// No ACAPI, no ownership: the caller fetches the handles (from a memo, from an
// API_WallRelation, …) and disposes them. Shared on its second domain — the
// element reads and the plan-outline read, which walks the CONNECTION polygon
// out of API_WallRelation instead of a memo.
// ---------------------------------------------------------------------------
struct PolygonHandles {
    API_Coord**   coords = nullptr;
    Int32**       pends  = nullptr;
    API_PolyArc** parcs  = nullptr;      // may be null: a polygon with no arcs
};

bool WalkPolygonRings (const PolygonHandles& polygon, const double* polyZ,
                       GS::Array<double>& outerCoords, GS::Array<double>& outerArcs,
                       GS::Int32& outerCount,
                       GS::Array<double>& holeCoords, GS::Array<double>& holeArcs,
                       GS::Array<GS::Int32>& holeCounts, GS::Int32& nHoles,
                       bool polylineMode = false, bool* outerClosed = nullptr,
                       GS::Array<double>* outerZ = nullptr, GS::Array<double>* holeZ = nullptr);

// ---------------------------------------------------------------------------
// Retained memory
//
// Total bytes we are keeping the user's RAM hostage with — mesh + metadata +
// screenshot stores. Reported by the Snapshot commands and CaptureScreenshot,
// so it spans two domains. The store includes stay in the .cpp on purpose:
// this header is pulled in by domain files that touch none of them.
// ---------------------------------------------------------------------------
size_t RetainedBytes ();

// Adds `retainedBytes` to a command response.
void AddMemory (GS::ObjectState& os);

} // namespace geomsrv

#endif

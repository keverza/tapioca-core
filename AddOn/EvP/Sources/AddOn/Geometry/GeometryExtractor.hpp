#ifndef GEOMETRYSERVER_GEOMETRYEXTRACTOR_HPP
#define GEOMETRYSERVER_GEOMETRYEXTRACTOR_HPP

#include "Mesh.hpp"
#include <memory>
#include <set>
#include <string>

namespace ModelerAPI { class Model; }

// MAIN THREAD ONLY. Walks the current 3D model and produces an immutable
// Snapshot of per-element triangle meshes in world meters.
namespace geomsrv {

// Fill `model` with the project's full 3D geometry; false if there is none.
//
// The ONE place that knows how to get a model from ANY window (see the .cpp for
// why `ACAPI_Sight_GetSelectedSightModel` alone silently returns an empty model
// when the floor plan is active). Lives here rather than in NativeCommands
// because the Geometry layer owns model acquisition and the command layer sits
// on top of it — the structured ModelerAPI reads
// (NativeCommands/ModelAccessUtils.hpp) all start from this call.
// MAIN THREAD ONLY.
bool AcquireCurrentModel (ModelerAPI::Model& model);

// Extract every element in the current 3D window's model ("all"). Null on error.
std::shared_ptr<const Snapshot> ExtractAllElements (uint64_t snapshotId);

// Extract only the currently selected elements ("selection"). Null on error;
// an empty (0-element) snapshot means nothing is selected.
std::shared_ptr<const Snapshot> ExtractSelectedElements (uint64_t snapshotId);

// How many elements a model holds. 0 for an empty or unusable model.
//
// Exists so a SLICED walk can be written without including <Model.hpp> — the
// ArchViz extraction thread (ArchViz/ExtractionThread.hpp) needs the count on
// the main thread before it starts stepping through, and it has no other reason
// to reach into the modeler headers.
// MAIN THREAD ONLY.
int32_t ModelElementCount (const ModelerAPI::Model& model);

// One element of `model`, by its 1-BASED index, into `mesh`. False if the index
// is out of range or the element yielded nothing drawable.
//
// ⚠️ 1-BASED, like every other ModelerAPI index. `ModelElementCount` returns N
// and the valid indices are 1..N; a 0-based loop silently skips the first
// element and reads one past the end of the last.
//
// THE REASON THIS IS PUBLIC. ExtractAllElements does the whole model in ONE
// uninterruptible call, which is fine for a snapshot command and fatal for the
// viewer: that call is minutes of main thread on a real project, and plan §0
// forbids stalling Archicad at all. The extraction thread walks the same model
// a few milliseconds at a time through MainThreadGate, which needs an entry
// point at ELEMENT granularity. It is the same `ExtractElement` both paths use —
// deliberately, so the viewer and the snapshot cannot disagree about what a mesh
// is.
// MAIN THREAD ONLY.
bool ExtractElementAt (const ModelerAPI::Model& model, int32_t index1Based, Mesh& mesh);

// The GUID of the element at a 1-based index, WITHOUT tessellating it.
//
// The cheap half of `ExtractElementAt`, and what makes a PARTIAL refresh
// affordable: live sync knows which GUIDs changed, the model is indexed by
// position, and the only way across is to walk it — so the walk must not pay for
// geometry it is going to discard. Empty string if the index is out of range.
// MAIN THREAD ONLY.
std::string ElementGuidAt (const ModelerAPI::Model& model, int32_t index1Based);

// A GUID plus the GUIDs of its sub-parts, if it has any.
//
// ⚠️ THE MODELER DOES NOT KNOW THE PARENT. Stairs, railings, curtain walls,
// columns and beams reach the model as their SUB-PARTS, which carry different
// GUIDs — but Archicad's change notification names the PARENT. A live sync that
// looked up the parent GUID in the model would find nothing, refresh nothing,
// and leave a stale stair on screen while reporting success. Expanding first is
// what closes that gap. (Mirrors Speckle's CollectPartIDs; the same expansion
// already backs selection-scoped extraction.)
// MAIN THREAD ONLY — it reads the element and its memo.
void ExpandElementAndParts (const API_Guid& guid, std::set<std::string>& out);

// THE INVERSE OF ExpandElementAndParts: a sub-part GUID mapped back up to the
// element a user can actually select. Returns `guid` unchanged when it is
// already selectable, and `APINULLGuid` when the database does not know it.
//
// ⚠️ THIS IS WHY PICKING "DID NOT WORK" FOR COLUMNS, RAILINGS, CURTAIN WALLS AND
// STAIRS while walls, slabs and objects were fine. Those five are exactly the
// hierarchical types: the 3D model enumerates their SUB-PARTS — a stair tread, a
// curtain-wall panel, a column segment — so the GPU pick resolves to a sub-part
// GUID, and `ACAPI_Selection_SetSelectedElementNeig` refuses it. The click was
// then dropped, which looks precisely like picking being broken for those types.
// The same mismatch runs the other way: Archicad selects the STAIR, and a viewer
// comparing that GUID against its tread GUIDs highlights nothing.
//
// ⚠️ IT WALKS A CHAIN, NOT ONE HOP. A railing's rail is owned by a railing
// SEGMENT, which is owned by the railing; stopping at the first owner selects
// something the user still cannot see in the plan. The walk is capped so a
// database that ever reported a cycle cannot hang Archicad's main thread.
//
// MAIN THREAD ONLY — it reads the element.
API_Guid ResolveSelectableOwner (const API_Guid& guid);

} // namespace geomsrv

#endif

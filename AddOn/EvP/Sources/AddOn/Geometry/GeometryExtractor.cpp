#include "APIEnvir.h"
#include "ACAPinc.h"

#include "GeometryExtractor.hpp"
#include "VertexWeld.hpp"
#include "WireframeEdges.hpp"

#include <exp.h>
#include <Sight.hpp>
#include <Model.hpp>
#include <ModelElement.hpp>
#include <ModelMeshBody.hpp>
#include <ModelEdge.hpp>
#include <Polygon.hpp>
#include <ConvexPolygon.hpp>
#include <AttributeIndex.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace geomsrv {

namespace {

// One element -> one Mesh. Bug-avoidance rules verified against the AC29
// ModelAccess_Test exporter: 1-based loops, guard empty bodies, and skip
// self-intersecting polygons via try/catch.
//
// NORMALS: taken from ConvexPolygon::GetNormalVectorByVertex(), which is what
// the DevKit's own exporter uses (ModelAccess_Test_Exporter.cpp, "Normals"
// block) with the same 1-based corner index it passes to GetVertexIndex().
// These are Archicad's true normals — hard across a box edge, smooth across a
// tessellated cylinder. We used to discard them and average adjacent face
// normals into the body's SHARED vertex list instead, which welded every hard
// edge smooth and shaded flat boxes like spheres. See VertexWeld.hpp.
//
// Because one shared body vertex now carries a different normal per adjacent
// face, vertices get split (VertexWelder). Positions and the surface itself
// are unchanged — only the vertex count and the indices differ.
struct Corner {
    Int32 vertex; // body-local, 0-based
    double nx, ny, nz;
};

bool ExtractElement (const ModelerAPI::Element& elem, Mesh& mesh)
{
    mesh.guid = APIGuidToString (GSGuid2APIGuid (elem.GetElemGuid ())).ToCStr ().Get ();
    mesh.elemType = static_cast<int32_t> (elem.GetType ());

    VertexWelder welder (mesh);
    std::vector<Corner> corners;
    size_t degenerateNormals = 0;

    const Int32 nBodies = elem.GetTessellatedBodyCount ();
    for (Int32 iBody = 1; iBody <= nBodies; ++iBody) {
        ModelerAPI::MeshBody body;
        elem.GetTessellatedBody (iBody, &body);

        const Int32 nVert = body.GetVertexCount ();
        const Int32 nPoly = body.GetPolygonCount ();
        if (nVert <= 0 || nPoly <= 0)
            continue;

        // Read the body's shared vertex list once. It is NOT appended to the
        // mesh directly any more — corners are emitted through the welder,
        // which splits a vertex whenever two faces disagree about its normal.
        std::vector<double> bodyPos;
        bodyPos.reserve (static_cast<size_t> (nVert) * 3);
        for (Int32 iVert = 1; iVert <= nVert; ++iVert) {
            ModelerAPI::Vertex v;
            body.GetVertex (iVert, &v); // default CoordinateSystem::World
            bodyPos.push_back (v.x);
            bodyPos.push_back (v.y);
            bodyPos.push_back (v.z);
            // Bounds still span every body vertex, exactly as before, so the
            // spatial index sees no change from this commit.
            mesh.bounds.Expand (v.x, v.y, v.z);
        }

        welder.Reset (); // source indices below are body-local

        for (Int32 iPoly = 1; iPoly <= nPoly; ++iPoly) {
            ModelerAPI::Polygon polygon;
            body.GetPolygon (iPoly, &polygon);

            // Preserve only the source polygon's visible boundary. Convex
            // decomposition seams and fan diagonals are absent from this set.
            std::vector<WireEdgeKey> visibleEdges;
            visibleEdges.reserve (static_cast<size_t> (std::max<Int32> (polygon.GetEdgeCount (), 0)));
            for (Int32 iEdge = 1; iEdge <= polygon.GetEdgeCount (); ++iEdge) {
                ModelerAPI::Edge edge;
                body.GetEdge (polygon.GetEdgeIndex (iEdge), &edge);
                if (edge.IsInvisible () && !edge.IsVisibleIfContour ())
                    continue;
                const Int32 v1 = edge.GetVertexIndex1 () - 1;
                const Int32 v2 = edge.GetVertexIndex2 () - 1;
                if (v1 >= 0 && v2 >= 0)
                    visibleEdges.push_back (MakeWireEdgeKey (uint32_t (v1), uint32_t (v2)));
            }
            std::sort (visibleEdges.begin (), visibleEdges.end ());
            visibleEdges.erase (std::unique (visibleEdges.begin (), visibleEdges.end ()), visibleEdges.end ());

            ModelerAPI::AttributeIndex matIdx;
            polygon.GetMaterialIndex (matIdx);
            const int32_t material = matIdx.GetIndex ();

            const Int32 nConvex = polygon.GetConvexPolygonCount ();
            for (Int32 iConvex = 1; iConvex <= nConvex; ++iConvex) {
                try {
                    ModelerAPI::ConvexPolygon convex;
                    polygon.GetConvexPolygon (iConvex, &convex);

                    const Int32 nCV = convex.GetVertexCount ();
                    if (nCV < 3)
                        continue;

                    // Gather this polygon's corners: body vertex index +
                    // Archicad's own normal at that corner.
                    corners.clear ();
                    bool indicesOk = true;
                    for (Int32 k = 1; k <= nCV; ++k) {
                        const Int32 bi = convex.GetVertexIndex (k) - 1; // 1-based
                        if (bi < 0 || bi >= nVert) {
                            indicesOk = false;
                            break;
                        }
                        const ModelerAPI::Vector n = convex.GetNormalVectorByVertex (k);
                        corners.push_back (Corner { bi, n.x, n.y, n.z });
                    }
                    if (!indicesOk || corners.size () < 3)
                        continue;

                    // A degenerate corner normal (Archicad occasionally reports
                    // one on a sliver) falls back to the polygon's geometric
                    // normal rather than shipping a zero vector to the shader.
                    double fnx = 0.0, fny = 0.0, fnz = 0.0;
                    TriangleNormal (&bodyPos[corners[0].vertex * 3], &bodyPos[corners[1].vertex * 3],
                                    &bodyPos[corners[2].vertex * 3], fnx, fny, fnz);
                    const bool faceOk = NormalizeVector (fnx, fny, fnz);

                    for (Corner& c : corners) {
                        if (!NormalizeVector (c.nx, c.ny, c.nz)) {
                            if (!faceOk) {
                                indicesOk = false;
                                break;
                            }
                            c.nx = fnx;
                            c.ny = fny;
                            c.nz = fnz;
                            ++degenerateNormals;
                        }
                    }
                    if (!indicesOk)
                        continue;

                    // Fan-triangulate: (1, k, k+1) over the gathered corners.
                    const auto emit = [&] (const Corner& c) {
                        return welder.Add (c.vertex, bodyPos[c.vertex * 3], bodyPos[c.vertex * 3 + 1],
                                           bodyPos[c.vertex * 3 + 2], c.nx, c.ny, c.nz);
                    };
                    const uint32_t i0 = emit (corners[0]);
                    for (size_t k = 1; k + 1 < corners.size (); ++k) {
                        const uint32_t sourceVertices[3] = { uint32_t (corners[0].vertex), uint32_t (corners[k].vertex),
                                                             uint32_t (corners[k + 1].vertex) };
                        mesh.triangles.push_back (i0);
                        mesh.triangles.push_back (emit (corners[k]));
                        mesh.triangles.push_back (emit (corners[k + 1]));
                        mesh.triMaterial.push_back (material);
                        mesh.triWireEdges.push_back (BuildTriangleWireEdgeMask (sourceVertices, visibleEdges));
                    }
                }
                catch (const GS::Exception&) {
                    continue; // skip degenerate / self-intersecting polygon
                }
            }
        }
    }

    if (mesh.TriangleCount () == 0)
        return false;

    // Last-resort safety net. If Archicad gave us nothing usable for this
    // element we still ship something shadeable rather than black geometry —
    // but smoothed, so it will look like the old bug and is worth reporting.
    if (!NormalsAreUsable (mesh))
        ComputeSmoothNormals (mesh);

    return true;
}

std::string ElemGuidString (const ModelerAPI::Element& elem)
{
    return APIGuidToString (GSGuid2APIGuid (elem.GetElemGuid ())).ToCStr ().Get ();
}

// Walk a model and build a snapshot. If `filter` is non-null, only elements
// whose GUID is in the set are kept. Shared by "all" and "selection".
std::shared_ptr<const Snapshot> BuildSnapshot (const ModelerAPI::Model& model, uint64_t snapshotId, const char* scope,
                                               const std::set<std::string>* filter)
{
    auto snap = std::make_shared<Snapshot> ();
    snap->id = snapshotId;
    snap->scope = scope;

    const Int32 nElements = model.GetElementCount ();
    snap->meshes.reserve (static_cast<size_t> (nElements > 0 ? nElements : 0));

    for (Int32 iElem = 1; iElem <= nElements; ++iElem) {
        ModelerAPI::Element elem;
        model.GetElement (iElem, &elem);

        if (filter != nullptr && filter->find (ElemGuidString (elem)) == filter->end ())
            continue;

        Mesh mesh;
        if (ExtractElement (elem, mesh))
            snap->meshes.push_back (std::move (mesh));
    }
    return snap;
}

// Add every GUID in a memo sub-part array. Each part struct carries a .head.guid.
template <typename T> void AddPartGuids (T* parts, std::set<std::string>& out)
{
    if (parts == nullptr)
        return;
    const GSSize n = BMGetPtrSize (reinterpret_cast<GSConstPtr> (parts)) / static_cast<GSSize> (sizeof (T));
    for (GSSize i = 0; i < n; ++i)
        out.insert (APIGuidToString (parts[i].head.guid).ToCStr ().Get ());
}

// The modeler yields composite elements (stairs, railings, curtain walls,
// columns, beams) as their sub-parts, which carry different GUIDs than the
// selected parent. Expand the parent GUID to include those sub-part GUIDs so
// selection-scoped extraction matches them. (Mirrors Speckle's CollectPartIDs.)
void AddSelectedElementAndParts (const API_Guid& guid, std::set<std::string>& out)
{
    out.insert (APIGuidToString (guid).ToCStr ().Get ()); // the element itself

    API_Element elem;
    BNZeroMemory (&elem, sizeof (elem));
    elem.header.guid = guid;
    if (ACAPI_Element_Get (&elem) != NoError)
        return;

    const API_ElemTypeID typeID = elem.header.type.typeID;
    if (typeID != API_StairID && typeID != API_RailingID && typeID != API_CurtainWallID && typeID != API_ColumnID &&
        typeID != API_BeamID)
        return;

    API_ElementMemo memo;
    BNZeroMemory (&memo, sizeof (memo));
    if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_All) != NoError)
        return;

    switch (typeID) {
        case API_StairID:
            AddPartGuids (memo.stairRisers, out);
            AddPartGuids (memo.stairTreads, out);
            AddPartGuids (memo.stairStructures, out);
            break;
        case API_RailingID:
            AddPartGuids (memo.railingSegments, out);
            AddPartGuids (memo.railingNodes, out);
            AddPartGuids (memo.railingPosts, out);
            AddPartGuids (memo.railingInnerPosts, out);
            AddPartGuids (memo.railingRails, out);
            AddPartGuids (memo.railingToprails, out);
            AddPartGuids (memo.railingHandrails, out);
            AddPartGuids (memo.railingPanels, out);
            AddPartGuids (memo.railingBalusters, out);
            AddPartGuids (memo.railingBalusterSets, out);
            AddPartGuids (memo.railingPatterns, out);
            AddPartGuids (memo.railingRailEnds, out);
            AddPartGuids (memo.railingHandrailEnds, out);
            AddPartGuids (memo.railingToprailEnds, out);
            AddPartGuids (memo.railingRailConnections, out);
            AddPartGuids (memo.railingHandrailConnections, out);
            AddPartGuids (memo.railingToprailConnections, out);
            break;
        case API_CurtainWallID:
            AddPartGuids (memo.cWallSegments, out);
            AddPartGuids (memo.cWallFrames, out);
            AddPartGuids (memo.cWallPanels, out);
            AddPartGuids (memo.cWallJunctions, out);
            AddPartGuids (memo.cWallAccessories, out);
            break;
        case API_ColumnID:
            AddPartGuids (memo.columnSegments, out);
            break;
        case API_BeamID:
            AddPartGuids (memo.beamSegments, out);
            break;
        default:
            break;
    }
    ACAPI_DisposeElemMemoHdls (&memo);
}

} // namespace

// Fill `model` with the project's full 3D geometry. Returns false on failure.
//
// Must work from ANY window, not just the 3D window -- users select roofs on
// the floor plan. `GetSelectedSightModel` alone is not enough: it reads whatever
// sight is currently *selected*, which from a 2D window is the plan sight (no
// modeler bodies) and it returns NoError with an empty model -- so 2D selections
// silently came back with zero meshes.
//
// `ACAPI_Sight_SelectSight (nullptr, ...)` selects the 3D window's sight
// (per the DevKit docs), regardless of the active window. So: select the 3D
// sight, read its model, then restore whatever sight was selected before so we
// don't disturb other add-ons' 3D state.
//
// PUBLIC (declared in the header) because every structured ModelerAPI read
// added by the model-access domains starts here. It was file-local until those
// existed; there must not be a second copy of this decision.
bool AcquireCurrentModel (ModelerAPI::Model& model)
{
    void* prevSight = nullptr;
    const bool switched = (ACAPI_Sight_SelectSight (nullptr, &prevSight) == NoError);

    const GSErrCode err = ACAPI_Sight_GetSelectedSightModel (model);

    if (switched && prevSight != nullptr) {
        void* dummy = nullptr;
        ACAPI_Sight_SelectSight (prevSight, &dummy); // restore previous selection
    }

    if (err == NoError)
        return true;

    // Fallback: the current window's own sight via the exporter path.
    void* rawSight = nullptr;
    if (ACAPI_Sight_GetCurrentWindowSight (&rawSight) != NoError || rawSight == nullptr)
        return false;
    Modeler::SightPtr sight (reinterpret_cast<Modeler::Sight*> (rawSight));
    Modeler::IAttributeReader* reader = ACAPI_Attribute_GetCurrentAttributeSetReader ();
    return EXPGetModel (sight, &model, reader) == NoError;
}

std::string ElementGuidAt (const ModelerAPI::Model& model, int32_t index1Based)
{
    if (index1Based < 1 || index1Based > model.GetElementCount ())
        return std::string ();

    ModelerAPI::Element elem;
    model.GetElement (static_cast<Int32> (index1Based), &elem);
    return ElemGuidString (elem);
}

void ExpandElementAndParts (const API_Guid& guid, std::set<std::string>& out)
{
    // One implementation, two callers: selection-scoped extraction and live
    // sync. See the header for why a live sync that skipped this leaves stale
    // stairs on screen.
    AddSelectedElementAndParts (guid, out);
}

API_Guid ResolveSelectableOwner (const API_Guid& guid)
{
    // ⚠️ CAPPED. Each hop is one ACAPI_Element_Get on the main thread; a
    // database that ever reported an owner cycle would otherwise hang Archicad
    // with no message. Four is one more than the deepest real chain
    // (rail -> segment -> railing).
    constexpr int kMaxOwnerHops = 4;

    API_Guid current = guid;
    for (int hop = 0; hop < kMaxOwnerHops; ++hop) {
        API_Element elem;
        BNZeroMemory (&elem, sizeof (elem));
        elem.header.guid = current;
        if (ACAPI_Element_Get (&elem) != NoError)
            return APINULLGuid;

        // ⚠️ EVERY ONE OF THESE HAS AN `owner` FIELD IN ITS OWN UNION MEMBER, and
        // there is no generic accessor -- API_Elem_Head does not carry the owner.
        // The switch is the API surface. A type missing from it is not a
        // failure: it means "already selectable", which is the right answer for
        // walls, slabs, objects and everything else.
        API_Guid owner = APINULLGuid;
        switch (elem.header.type.typeID) {
            case API_CurtainWallSegmentID:
                owner = elem.cwSegment.owner;
                break;
            case API_CurtainWallFrameID:
                owner = elem.cwFrame.owner;
                break;
            case API_CurtainWallPanelID:
                owner = elem.cwPanel.owner;
                break;
            case API_CurtainWallJunctionID:
                owner = elem.cwJunction.owner;
                break;
            case API_CurtainWallAccessoryID:
                owner = elem.cwAccessory.owner;
                break;

            case API_RiserID:
                owner = elem.stairRiser.owner;
                break;
            case API_TreadID:
                owner = elem.stairTread.owner;
                break;
            case API_StairStructureID:
                owner = elem.stairStructure.owner;
                break;

            case API_RailingToprailID:
                owner = elem.railingToprail.owner;
                break;
            case API_RailingHandrailID:
                owner = elem.railingHandrail.owner;
                break;
            case API_RailingRailID:
                owner = elem.railingRail.owner;
                break;
            case API_RailingToprailEndID:
                owner = elem.railingToprailEnd.owner;
                break;
            case API_RailingHandrailEndID:
                owner = elem.railingHandrailEnd.owner;
                break;
            case API_RailingRailEndID:
                owner = elem.railingRailEnd.owner;
                break;
            case API_RailingEndFinishID:
                owner = elem.railingEndFinish.owner;
                break;
            case API_RailingToprailConnectionID:
                owner = elem.railingToprailConnection.owner;
                break;
            case API_RailingHandrailConnectionID:
                owner = elem.railingHandrailConnection.owner;
                break;
            case API_RailingRailConnectionID:
                owner = elem.railingRailConnection.owner;
                break;
            case API_RailingPostID:
                owner = elem.railingPost.owner;
                break;
            case API_RailingInnerPostID:
                owner = elem.railingInnerPost.owner;
                break;
            case API_RailingBalusterSetID:
                owner = elem.railingBalusterSet.owner;
                break;
            case API_RailingBalusterID:
                owner = elem.railingBaluster.owner;
                break;
            case API_RailingPanelID:
                owner = elem.railingPanel.owner;
                break;
            case API_RailingNodeID:
                owner = elem.railingNode.owner;
                break;
            case API_RailingSegmentID:
                owner = elem.railingSegment.owner;
                break;
            case API_RailingPatternID:
                owner = elem.railingPattern.owner;
                break;

            case API_ColumnSegmentID:
                owner = elem.columnSegment.owner;
                break;
            case API_BeamSegmentID:
                owner = elem.beamSegment.owner;
                break;

            default:
                return current; // already a selectable element
        }

        // A sub-part whose owner is null is a database inconsistency, not a
        // reason to select the sub-part: selecting it would fail anyway.
        if (owner == APINULLGuid)
            return APINULLGuid;
        current = owner;
    }
    return current;
}

int32_t ModelElementCount (const ModelerAPI::Model& model)
{
    const Int32 n = model.GetElementCount ();
    return n > 0 ? static_cast<int32_t> (n) : 0;
}

bool ExtractElementAt (const ModelerAPI::Model& model, int32_t index1Based, Mesh& mesh)
{
    if (index1Based < 1 || index1Based > model.GetElementCount ())
        return false;

    ModelerAPI::Element elem;
    model.GetElement (static_cast<Int32> (index1Based), &elem);
    return ExtractElement (elem, mesh);
}

std::shared_ptr<const Snapshot> ExtractAllElements (uint64_t snapshotId)
{
    ModelerAPI::Model model;
    if (!AcquireCurrentModel (model))
        return nullptr;

    return BuildSnapshot (model, snapshotId, "all", nullptr);
}

std::shared_ptr<const Snapshot> ExtractSelectedElements (uint64_t snapshotId)
{
    // Collect the GUIDs of the currently selected elements. Selection is
    // available in any window (incl. floor plan), unlike a selected-sight model.
    API_SelectionInfo selInfo;
    BNZeroMemory (&selInfo, sizeof (selInfo));
    GS::Array<API_Neig> neigs;
    const GSErrCode err = ACAPI_Selection_Get (&selInfo, &neigs, false);
    if (selInfo.marquee.coords != nullptr)
        BMKillHandle (reinterpret_cast<GSHandle*> (&selInfo.marquee.coords));
    if (err != NoError && err != APIERR_NOSEL)
        return nullptr;

    std::set<std::string> selected;
    for (UInt32 i = 0; i < neigs.GetSize (); ++i)
        AddSelectedElementAndParts (neigs[i].guid, selected);

    // Extract the full model, keeping only selected elements.
    ModelerAPI::Model model;
    if (!AcquireCurrentModel (model))
        return nullptr;

    return BuildSnapshot (model, snapshotId, "selection", &selected);
}

} // namespace geomsrv

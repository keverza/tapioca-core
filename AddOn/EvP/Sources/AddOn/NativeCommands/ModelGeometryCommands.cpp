#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/ModelGeometryCommands.hpp"
#include "NativeCommands/CommandBase.hpp"
#include "NativeCommands/ModelAccessUtils.hpp"

#include "Geometry/GeometryExtractor.hpp"     // AcquireCurrentModel

#include <Model.hpp>
#include <ModelElement.hpp>
#include <ModelMeshBody.hpp>
#include <ModelEdge.hpp>
#include <Polygon.hpp>
#include <ConvexPolygon.hpp>
#include <AttributeIndex.hpp>
#include <Transformation.hpp>
#include <TextureCoordinate.hpp>
#include <Box3DData.h>

#include <optional>

namespace geomsrv {

namespace {

// ===========================================================================
// E24 — the STRUCTURED ModelerAPI read.
//
// EvP already had exactly ONE geometry path: Geometry/GeometryExtractor.cpp,
// which walks the same ModelerAPI and throws almost everything away — it emits
// welded triangle soup (position + normal + material index) because that is what
// the BVH, the renderer and numpy want. Eight of the API's forty geometry calls.
//
// Triangle soup cannot answer a whole class of question, and no amount of
// post-processing recovers the answer:
//   * "which faces are coplanar / which is the top face"  — needs POLYGONS, not
//     the fan-triangulation of them;
//   * "what is this body's outline"                        — needs EDGES and the
//     polygon contour, including hole markers;
//   * "is this body closed / solid / a surface"            — body flags;
//   * "what surface is on this face, and where does the texture sit"
//                                                          — per-polygon material
//     and the UV frame;
//   * "how does this element sit in the world"             — the element's
//     local-to-world transformation, discarded by the extractor which bakes
//     everything into world space.
//
// So these commands return the modeler's own topology, not a rendering of it.
// They are READS: no undo scope, no project change. They DO take the gate (they
// call ACAPI + ModelerAPI), unlike the E2 data plane, which reads the immutable
// snapshot and is the only thing allowed to skip it.
//
// SHAPE (E16.0): nested records for the per-element/per-body facts, FLAT arrays
// for the bulk numerics inside them (vertex coordinates, index lists). Both
// rules apply here at once and they do not conflict — a body IS a record, its
// vertex list IS bulk numerics.
//
// ⚠️ INDEX BASE. ModelerAPI is 1-BASED throughout: bodies, vertices, edges,
// polygons and the per-polygon corner index all start at 1. The indices in the
// responses below are the modeler's own, unmodified, so they can be fed straight
// back into another call. This is the opposite of the extractor's output, which
// converts to 0-based for numpy — do not mix the two.
// ===========================================================================

// The `include` section defaults for GetBodyGeometry, in one place. Vertices and
// polygons are on because a body read that returns neither is useless; edges,
// convex decomposition and per-body normals are off because they are large and
// most callers do not want them.
constexpr bool kDefaultVertices  = true;
constexpr bool kDefaultPolygons  = true;
constexpr bool kDefaultEdges     = false;
constexpr bool kDefaultConvex    = false;
constexpr bool kDefaultNormals   = false;
constexpr bool kDefaultHardFlags = false;

void AddElementId (const GS::UniString& guid, GS::ObjectState& record)
{
    GS::ObjectState elementId;
    elementId.Add ("guid", guid);
    record.Add ("elementId", elementId);
}

bool ReadElementId (const GS::ObjectState& record, GS::UniString& guid)
{
    GS::ObjectState elementId;
    return record.Get ("elementId", elementId) && elementId.Get ("guid", guid) && !guid.IsEmpty ();
}

bool ResolveElement (const ModelerAPI::Model& model, const GS::ObjectState& params,
                     ModelerAPI::Element& elem, Int32& elemIndex, GS::UniString& err)
{
    GS::ObjectState elementId;
    GS::UniString guid;
    if (params.Get ("elementId", elementId) && elementId.Get ("guid", guid) && !guid.IsEmpty ()) {
        const std::optional<Int32> found = model.GetElementIndex (
            APIGuid2GSGuid (APIGuidFromString (guid.ToCStr ().Get ())));
        if (!found.has_value ()) {
            err = "no 3D model element for elementId.guid \"" + guid + "\"";
            return false;
        }
        elemIndex = *found;
        model.GetElement (elemIndex, &elem);
        return true;
    }
    return ResolveModelElement (model, params, elem, elemIndex, err);
}

// A cap the caller asked for, or 0 for "no cap". Returns how many of `available`
// to actually emit.
Int32 Capped (const GS::ObjectState& params, const char* key, Int32 available)
{
    GS::Int32 cap = 0;
    if (!params.Get (key, cap) || cap <= 0)
        return available;
    return cap < available ? (Int32) cap : available;
}


// The element's 3x4 affine, flattened row-major: [m00 m01 m02 m03 m10 …]. The
// last column of each row is the translation, matching ModelerAPI::Transformation
// (and TRANMAT) exactly — no transposition, no reinterpretation.
GS::Array<double> FlattenTransformation (const ModelerAPI::Transformation& t)
{
    GS::Array<double> out;
    for (short row = 0; row < 3; ++row)
        for (short col = 0; col < 4; ++col)
            out.Push (t.matrix[row][col]);
    return out;
}


// ---------------------------------------------------------------------------
// EvP.GetModelInfo {}
//   -> { ok, guid, bounds, elementCount, colorCount, materialCount,
//        textureCount, fillCount, lightCount }
//
// What is in the current 3D model, before you ask for any of it. Cheap enough to
// call first every time, and the only way to size a subsequent read: a model with
// 40,000 elements is not one you enumerate into JSON without a filter.
//
// `guid` is the MODEL's guid (ModelerAPI::Model::GetGuid), not an element's — it
// changes when the model is regenerated, so a caller can tell a cached read from
// a stale one.
// ---------------------------------------------------------------------------
class GetModelInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetModelInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model",
                          "is a project open, and does it have 3D content?"));
        }

        const Int32 elementCount = model.GetElementCount ();

        os.Add ("guid", GS::UniString (model.GetGuid ().ToUniString ()));
        os.Add ("bounds", BoxToObjectState (model.GetBounds ()));
        os.Add ("elementCount",  (GS::Int32) elementCount);
        // THE FIRST THING A CALLER SHOULD BRANCH ON. Zero elements with a full
        // attribute pool is a sight that was never generated, and it is
        // indistinguishable from "an empty project" unless we say so — on the
        // first live run it surfaced three commands down as a bogus complaint
        // about the caller's guid.
        os.Add ("generated", elementCount > 0);
        if (elementCount <= 0)
            os.Add ("hint", EmptyModelHint ());
        os.Add ("colorCount",    (GS::Int32) model.GetColorCount ());
        os.Add ("materialCount", (GS::Int32) model.GetMaterialCount ());
        os.Add ("textureCount",  (GS::Int32) model.GetTextureCount ());
        os.Add ("fillCount",     (GS::Int32) model.GetFillCount ());
        os.Add ("lightCount",    (GS::Int32) model.GetLightCount ());
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetModelElements { elements?:[{elementId:{guid}}], types?:["wall","slab"], skipEmpty?:bool,
//                        coordinateSystem?:"world"|"local",
//                        include?:["bounds","transform"], offset?, limit? }
//   -> { ok, count, totalCount, offset, elements:[{ index, guid, type, typeName,
//        invalid, genId, tessellatedBodyCount, meshBodyCount, nurbsBodyCount,
//        pointCloudCount, lightCount, bounds?, transform? }] }
//
// The model's element table — the index a body read needs, plus what kind of
// geometry each element actually HAS. That last part is the point: an element
// with `nurbsBodyCount > 0` needs EvP.GetNurbsBody, one with only tessellated
// bodies needs EvP.GetBodyGeometry, and nothing else tells you which.
//
// `elements` and `types` filter; `offset`/`limit` page, because a real project model
// is tens of thousands of elements and one JSON response should not be all of
// them. `totalCount` is always the unpaged number so a caller can loop.
//
// ⚠️ `guids` here filters the MODEL's elements. Composite elements (stair,
// railing, curtain wall, column, beam) appear in the model as their SUB-PARTS,
// which carry different guids than the element you selected — the same trap
// GeometryExtractor documents. Pass the sub-part guids, or filter by type.
// ---------------------------------------------------------------------------
class GetModelElementsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetModelElements"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model",
                          "is a project open, and does it have 3D content?"));
        }

        GS::Array<GS::UniString> wantedGuids, wantedTypes;
        GS::Array<GS::ObjectState> wantedElements;
        const bool filterGuids = params.Get ("elements", wantedElements) && !wantedElements.IsEmpty ();
        for (const GS::ObjectState& item : wantedElements) {
            GS::UniString guid;
            if (!ReadElementId (item, guid))
                return NativeCommandResult::Failure (
                    EVP_FAIL ("every element needs elementId.guid", "Tapioca.GetModelElements"));
            wantedGuids.Push (guid);
        }
        const bool filterTypes = params.Get ("types", wantedTypes) && !wantedTypes.IsEmpty ();

        bool skipEmpty = false;
        params.Get ("skipEmpty", skipEmpty);        // drop elements with no body at all

        GS::Int32 offset = 0, limit = 0;
        params.Get ("offset", offset);
        params.Get ("limit", limit);
        if (offset < 0) offset = 0;

        const bool wantBounds    = WantsSection (params, "bounds", true);
        const bool wantTransform = WantsSection (params, "transform", false);
        const ModelerAPI::CoordinateSystem cs = ParseCoordinateSystem (params);

        GS::Array<GS::ObjectState> elements;
        GS::Int32 matched = 0;
        const Int32 total = model.GetElementCount ();

        for (Int32 i = 1; i <= total; ++i) {
            ModelerAPI::Element elem;
            model.GetElement (i, &elem);

            const GS::UniString guid = ElementGuidString (elem);
            if (filterGuids && !wantedGuids.Contains (guid))
                continue;

            const ModelerAPI::Element::Type type = elem.GetType ();
            const GS::UniString typeName = ElementTypeName (type);
            if (filterTypes && !wantedTypes.Contains (typeName))
                continue;

            const Int32 tessellated = elem.GetTessellatedBodyCount ();
            const Int32 meshBodies  = elem.GetMeshBodyCount ();
            const Int32 nurbsBodies = elem.GetNurbsBodyCount ();
            const Int32 pointClouds = elem.GetPointCloudCount ();
            const Int32 lights      = elem.GetLightCount ();
            if (skipEmpty && tessellated == 0 && meshBodies == 0 && nurbsBodies == 0 &&
                pointClouds == 0 && lights == 0)
                continue;

            ++matched;
            if (matched <= offset)
                continue;                                   // paged past
            if (limit > 0 && (GS::Int32) elements.GetSize () >= limit)
                continue;                                   // still count, don't emit

            GS::ObjectState record;
            record.Add ("index", (GS::Int32) i);            // 1-based, feeds GetBodyGeometry
            AddElementId (guid, record);
            record.Add ("type", (GS::Int32) type);
            record.Add ("typeName", typeName);
            record.Add ("invalid", elem.IsInvalid ());
            record.Add ("genId", (GS::Int32) elem.GetGenId ());
            record.Add ("tessellatedBodyCount", (GS::Int32) tessellated);
            record.Add ("meshBodyCount", (GS::Int32) meshBodies);
            record.Add ("nurbsBodyCount", (GS::Int32) nurbsBodies);
            record.Add ("pointCloudCount", (GS::Int32) pointClouds);
            record.Add ("lightCount", (GS::Int32) lights);
            if (wantBounds)
                record.Add ("bounds", BoxToObjectState (elem.GetBounds (cs)));
            if (wantTransform)
                record.Add ("transform", FlattenTransformation (elem.GetElemLocalToWorldTransformation ()));
            elements.Push (record);
        }

        os.Add ("coordinateSystem", CoordinateSystemName (cs));
        os.Add ("modelElementCount", (GS::Int32) total);
        os.Add ("generated", total > 0);
        if (total <= 0)
            os.Add ("hint", EmptyModelHint ());   // not "your filter matched nothing"
        os.Add ("totalCount", matched);                     // matched the filters, unpaged
        os.Add ("offset", offset);
        os.Add ("count", (GS::Int32) elements.GetSize ());
        os.Add ("elements", elements);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetBodyGeometry { guid | elementIndex, body?:1, source?:"tessellated"|"mesh",
//                       coordinateSystem?:"world"|"local",
//                       include?:["vertices","polygons","edges","convex",
//                                 "normals","vertexHardFlags","all"],
//                       maxVertices?, maxPolygons?, maxEdges? }
//   -> { ok, guid, elementIndex, elementType, bodyIndex, bodyCount, source,
//        coordinateSystem, body:{…}, vertices?, vertexHardFlags?, edges?,
//        polygons?, normals?, convex? }
//
// ONE body's full mesh BREP: the modeler's own vertices, edges, polygons and
// normals, not the welded triangle soup the snapshot path produces.
//
// TESSELLATED vs MESH (`source`). Element::GetTessellatedBody returns the body
// with curved surfaces already broken into planar polygons — that is what the
// extractor uses and what a renderer wants. GetMeshBody returns the body as
// modelled, where a curved surface is ONE polygon flagged curved. Ask for "mesh"
// when you want faces as the user drew them (how many faces does this column
// have), "tessellated" when you want geometry you can triangulate. The counts
// differ, sometimes by a lot, and neither is wrong.
//
// POLYGON CONTOURS AND HOLES. Within a polygon, corner k (1..edgeCount) gives a
// vertex index and an edge index. Those raw values carry two conventions
// straight from the API and are passed through UNCHANGED:
//   * a NEGATIVE edge index means the edge runs the other way (from its vertex2
//     to its vertex1);
//   * a ZERO edge index is a CONTOUR BREAK — everything after it belongs to a
//     hole, not the outer boundary.
// Rewriting either would destroy information no caller can recover. `isComplex`
// on the polygon tells you up front whether to expect them.
//
// CONVEX DECOMPOSITION (`include:["convex"]`) is the triangulation-ready form:
// each polygon split into convex pieces with a per-corner normal. This is
// exactly what GeometryExtractor consumes, exposed so a script can reproduce or
// audit the snapshot's triangles without the C++ path.
//
// The per-polygon read is wrapped in try/catch for the same reason the extractor
// is: Archicad throws on a self-intersecting or degenerate polygon, and one bad
// polygon must not lose the whole body. A skipped polygon is COUNTED and
// reported (`skippedPolygons`) rather than silently dropped.
// ---------------------------------------------------------------------------
class GetBodyGeometryCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetBodyGeometry"; }

    // A dense body is a lot of ObjectState building. Show Archicad's progress
    // window so the user gets a Cancel button rather than a frozen UI.
    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model",
                          "is a project open, and does it have 3D content?"));
        }

        ModelerAPI::Element elem;
        Int32 elemIndex = 0;
        GS::UniString err;
        if (!ResolveElement (model, params, elem, elemIndex, err))
            return NativeCommandResult::Failure (
                EVP_FAIL (err, "EvP.GetBodyGeometry element lookup"));

        GS::UniString source ("tessellated");
        params.Get ("source", source);
        const bool useMeshBody = (source == "mesh");
        const Int32 bodyCount = useMeshBody ? elem.GetMeshBodyCount ()
                                            : elem.GetTessellatedBodyCount ();

        GS::Int32 bodyIndex = 1;
        params.Get ("body", bodyIndex);
        if (bodyCount <= 0) {
            return NativeCommandResult::Failure (EVP_FAIL (
                GS::UniString ("this element has no ") + source + " body",
                "check nurbsBodyCount / pointCloudCount from EvP.GetModelElements — a "
                "shell or a morph may be NURBS-only"));
        }
        if (bodyIndex < 1 || bodyIndex > bodyCount) {
            return NativeCommandResult::Failure (EVP_FAIL (
                GS::UniString::Printf ("body %d is out of range (1..%d)", (int) bodyIndex, (int) bodyCount),
                "body indices are 1-based"));
        }

        ModelerAPI::MeshBody body;
        if (useMeshBody) elem.GetMeshBody ((Int32) bodyIndex, &body);
        else             elem.GetTessellatedBody ((Int32) bodyIndex, &body);

        const ModelerAPI::CoordinateSystem cs = ParseCoordinateSystem (params);

        const Int32 vertexCount  = body.GetVertexCount ();
        const Int32 edgeCount    = body.GetEdgeCount ();
        const Int32 polygonCount = body.GetPolygonCount ();
        const Int32 vectorCount  = body.GetPolygonVectorCount ();

        AddElementId (ElementGuidString (elem), os);
        os.Add ("elementIndex", (GS::Int32) elemIndex);
        os.Add ("elementType", ElementTypeName (elem.GetType ()));
        os.Add ("source", source);
        os.Add ("bodyIndex", bodyIndex);
        os.Add ("bodyCount", (GS::Int32) bodyCount);
        os.Add ("coordinateSystem", CoordinateSystemName (cs));
        os.Add ("body", DescribeBody (body, cs, vertexCount, edgeCount, polygonCount, vectorCount));

        if (WantsSection (params, "vertices", kDefaultVertices))
            AddVertices (os, params, body, cs, vertexCount);
        if (WantsSection (params, "vertexHardFlags", kDefaultHardFlags))
            AddVertexHardFlags (os, body, vertexCount);
        if (WantsSection (params, "normals", kDefaultNormals))
            AddBodyVectors (os, body, cs, vectorCount);
        if (WantsSection (params, "edges", kDefaultEdges))
            AddEdges (os, params, body, edgeCount);
        if (WantsSection (params, "polygons", kDefaultPolygons) ||
            WantsSection (params, "convex", kDefaultConvex))
            AddPolygons (os, params, body, cs, polygonCount);

        return os;
    }

private:
    static GS::ObjectState DescribeBody (const ModelerAPI::MeshBody& body,
                                         ModelerAPI::CoordinateSystem cs,
                                         Int32 vertexCount, Int32 edgeCount,
                                         Int32 polygonCount, Int32 vectorCount)
    {
        GS::ObjectState b;
        // Classification. "Solid" is the one that decides whether a volume
        // question is even meaningful; a wire or surface body has no inside.
        b.Add ("isWireBody", body.IsWireBody ());
        b.Add ("isSurfaceBody", body.IsSurfaceBody ());
        b.Add ("isSolidBody", body.IsSolidBody ());
        b.Add ("isClosed", body.IsClosed ());
        b.Add ("isVisibleIfContour", body.IsVisibleIfContour ());
        b.Add ("hasSharpEdge", body.HasSharpEdge ());
        b.Add ("alwaysCastsShadow", body.AlwaysCastsShadow ());
        b.Add ("neverCastsShadow", body.NeverCastsShadow ());
        b.Add ("doesNotReceiveShadow", body.DoesNotReceiveShadow ());

        b.Add ("vertexCount", (GS::Int32) vertexCount);
        b.Add ("edgeCount", (GS::Int32) edgeCount);
        b.Add ("polygonCount", (GS::Int32) polygonCount);
        b.Add ("polygonVectorCount", (GS::Int32) vectorCount);
        b.Add ("bounds", BoxToObjectState (body.GetBounds (cs)));

        // Body-level defaults. A polygon can override the material; the edge
        // colour and the texture link are body-wide.
        b.Add ("hasColor", body.HasColor ());
        if (body.HasColor ()) {
            ModelerAPI::Color color;
            body.GetColor (&color);
            b.Add ("color", ColorToObjectState (color));
        }
        ModelerAPI::AttributeIndex colorIndex, materialIndex, textureIndex;
        body.GetColorIndex (colorIndex);
        body.GetMaterialIndex (materialIndex);
        body.GetTextureIndex (textureIndex);
        b.Add ("colorIndex", AttributeIndexToObjectState (colorIndex));
        b.Add ("materialIndex", AttributeIndexToObjectState (materialIndex));
        b.Add ("textureIndex", AttributeIndexToObjectState (textureIndex));

        // May be null — a body with no explicit UV frame inherits it, and that
        // is a real answer, not a failure.
        const ModelerAPI::TextureCoordinateSystem* uv = body.GetTextureCoordinateSystem ();
        b.Add ("hasTextureCoordinateSystem", uv != nullptr);
        if (uv != nullptr)
            b.Add ("textureCoordinateSystem", TextureCoordSysToObjectState (*uv));
        return b;
    }

    // Flat [x,y,z, x,y,z, …] — bulk numerics, per E16.0. Vertex index i (1-based)
    // is at offset (i-1)*3.
    static void AddVertices (GS::ObjectState& os, const GS::ObjectState& params,
                             const ModelerAPI::MeshBody& body,
                             ModelerAPI::CoordinateSystem cs, Int32 vertexCount)
    {
        const Int32 emit = Capped (params, "maxVertices", vertexCount);
        GS::Array<double> coords;
        for (Int32 i = 1; i <= emit; ++i) {
            ModelerAPI::Vertex v;
            body.GetVertex (i, &v, cs);
            coords.Push (v.x);
            coords.Push (v.y);
            coords.Push (v.z);
        }
        os.Add ("vertices", coords);
        os.Add ("verticesTruncated", emit < vertexCount);
    }

    // "Hard" = this vertex breaks shading. The flag GeometryExtractor's welder
    // reconstructs from per-corner normals; here it is read directly.
    static void AddVertexHardFlags (GS::ObjectState& os, const ModelerAPI::MeshBody& body,
                                    Int32 vertexCount)
    {
        GS::Array<bool> flags;
        for (Int32 i = 1; i <= vertexCount; ++i)
            flags.Push (body.GetVertexHardFlag (i));
        os.Add ("vertexHardFlags", flags);
    }

    // The body's normal-vector pool, flat [x,y,z, …]. A polygon's
    // `normalVectorIndex` points in here — SIGNED: a negative index means the
    // vector at |index|, negated.
    static void AddBodyVectors (GS::ObjectState& os, const ModelerAPI::MeshBody& body,
                                ModelerAPI::CoordinateSystem cs, Int32 vectorCount)
    {
        GS::Array<double> normals;
        for (Int32 i = 1; i <= vectorCount; ++i) {
            ModelerAPI::Vector v;
            body.GetVector (i, &v, cs);
            normals.Push (v.x);
            normals.Push (v.y);
            normals.Push (v.z);
        }
        os.Add ("normals", normals);
    }

    // Edge topology: the pair of vertices and the (up to) two polygons that share
    // it. polygonIndex -1 means "no polygon on that side" — a boundary of an open
    // surface, or a wire edge.
    static void AddEdges (GS::ObjectState& os, const GS::ObjectState& params,
                          const ModelerAPI::MeshBody& body, Int32 edgeCount)
    {
        const Int32 emit = Capped (params, "maxEdges", edgeCount);
        GS::Array<GS::Int32> v1, v2, p1, p2, colorIndices;
        GS::Array<bool> invisible, visibleIfContour, hasColor;
        for (Int32 i = 1; i <= emit; ++i) {
            ModelerAPI::Edge edge;
            body.GetEdge (i, &edge);
            v1.Push ((GS::Int32) edge.GetVertexIndex1 ());
            v2.Push ((GS::Int32) edge.GetVertexIndex2 ());
            p1.Push ((GS::Int32) edge.GetPolygonIndex1 ());
            p2.Push ((GS::Int32) edge.GetPolygonIndex2 ());
            invisible.Push (edge.IsInvisible ());
            visibleIfContour.Push (edge.IsVisibleIfContour ());
            hasColor.Push (edge.HasColor ());
            ModelerAPI::AttributeIndex colorIndex;
            edge.GetColorIndex (colorIndex);
            colorIndices.Push ((GS::Int32) colorIndex.GetIndex ());
        }

        GS::ObjectState edges;
        edges.Add ("count", (GS::Int32) emit);
        edges.Add ("truncated", emit < edgeCount);
        edges.Add ("vertex1", v1);
        edges.Add ("vertex2", v2);
        edges.Add ("polygon1", p1);        // -1 = none
        edges.Add ("polygon2", p2);
        edges.Add ("invisible", invisible);
        edges.Add ("visibleIfContour", visibleIfContour);
        edges.Add ("hasColor", hasColor);
        edges.Add ("colorIndex", colorIndices);
        os.Add ("edges", edges);
    }

    // Polygons, and optionally their convex decomposition. Both walk the same
    // loop because both need the Polygon object, and reading it twice would
    // double the cost of the expensive part.
    static void AddPolygons (GS::ObjectState& os, const GS::ObjectState& params,
                             const ModelerAPI::MeshBody& body,
                             ModelerAPI::CoordinateSystem cs, Int32 polygonCount)
    {
        const bool wantPolygons = WantsSection (params, "polygons", kDefaultPolygons);
        const bool wantConvex   = WantsSection (params, "convex", kDefaultConvex);
        const Int32 emit = Capped (params, "maxPolygons", polygonCount);

        GS::Array<GS::Int32> materialIndex, normalIndex, edgeCounts, polygonIds;
        GS::Array<GS::Int32> materialTexture, polygonTexture;
        GS::Array<bool> invisible, visibleIfContour, complex, gravity;
        GS::Array<bool> hasMaterialTexture, hasPolygonTexture;
        GS::Array<GS::Int32> vertexIndices, edgeIndices;   // flat, split by edgeCounts

        GS::Array<GS::Int32> convexPolygon, convexVertexCounts, convexVertexIndices;
        GS::Array<double>    convexNormals;

        GS::Int32 skipped = 0;

        for (Int32 i = 1; i <= emit; ++i) {
            ModelerAPI::Polygon polygon;
            try {
                body.GetPolygon (i, &polygon);

                if (wantPolygons) {
                    ModelerAPI::AttributeIndex matIdx;
                    polygon.GetMaterialIndex (matIdx);
                    materialIndex.Push ((GS::Int32) matIdx.GetIndex ());
                    normalIndex.Push ((GS::Int32) polygon.GetNormalVectorIndex ());
                    polygonIds.Push ((GS::Int32) polygon.GetPolygonId ());
                    invisible.Push (polygon.IsInvisible ());
                    visibleIfContour.Push (polygon.IsVisibleIfContour ());
                    complex.Push (polygon.IsComplex ());
                    gravity.Push (polygon.IsGravity ());

                    const bool matTex = polygon.HasMaterialTexture ();
                    const bool pgnTex = polygon.HasPolygonTexture ();
                    hasMaterialTexture.Push (matTex);
                    hasPolygonTexture.Push (pgnTex);
                    ModelerAPI::AttributeIndex texIdx;
                    if (matTex) polygon.GetMaterialTextureIndex (texIdx);
                    materialTexture.Push (matTex ? (GS::Int32) texIdx.GetIndex () : -1);
                    ModelerAPI::AttributeIndex pgnTexIdx;
                    if (pgnTex) polygon.GetPolygonTextureIndex (pgnTexIdx);
                    polygonTexture.Push (pgnTex ? (GS::Int32) pgnTexIdx.GetIndex () : -1);

                    const Int32 corners = polygon.GetEdgeCount ();
                    edgeCounts.Push ((GS::Int32) corners);
                    for (Int32 k = 1; k <= corners; ++k) {
                        vertexIndices.Push ((GS::Int32) polygon.GetVertexIndex (k));
                        edgeIndices.Push ((GS::Int32) polygon.GetEdgeIndex (k));   // signed; 0 = hole break
                    }
                }

                if (wantConvex) {
                    const Int32 pieces = polygon.GetConvexPolygonCount ();
                    for (Int32 c = 1; c <= pieces; ++c) {
                        ModelerAPI::ConvexPolygon convex;
                        polygon.GetConvexPolygon (c, &convex);
                        const Int32 n = convex.GetVertexCount ();
                        convexPolygon.Push ((GS::Int32) i);
                        convexVertexCounts.Push ((GS::Int32) n);
                        for (Int32 k = 1; k <= n; ++k) {
                            convexVertexIndices.Push ((GS::Int32) convex.GetVertexIndex (k));
                            const ModelerAPI::Vector nrm = convex.GetNormalVectorByVertex (k, cs);
                            convexNormals.Push (nrm.x);
                            convexNormals.Push (nrm.y);
                            convexNormals.Push (nrm.z);
                        }
                    }
                }
            } catch (const GS::Exception&) {
                // Degenerate / self-intersecting polygon. Counted, not hidden —
                // silently dropping it is how a body comes back looking complete
                // and being wrong.
                ++skipped;
                continue;
            }
        }

        if (wantPolygons) {
            GS::ObjectState pgons;
            pgons.Add ("count", (GS::Int32) edgeCounts.GetSize ());
            pgons.Add ("truncated", emit < polygonCount);
            pgons.Add ("skipped", skipped);
            pgons.Add ("materialIndex", materialIndex);
            pgons.Add ("normalVectorIndex", normalIndex);   // signed into body `normals`
            pgons.Add ("polygonId", polygonIds);
            pgons.Add ("invisible", invisible);
            pgons.Add ("visibleIfContour", visibleIfContour);
            pgons.Add ("isComplex", complex);               // concave and/or has holes
            pgons.Add ("isGravity", gravity);
            pgons.Add ("hasMaterialTexture", hasMaterialTexture);
            pgons.Add ("hasPolygonTexture", hasPolygonTexture);
            pgons.Add ("materialTextureIndex", materialTexture);
            pgons.Add ("polygonTextureIndex", polygonTexture);
            pgons.Add ("edgeCounts", edgeCounts);           // corners per polygon
            pgons.Add ("vertexIndices", vertexIndices);     // flat, split by edgeCounts
            pgons.Add ("edgeIndices", edgeIndices);         // flat; signed, 0 = hole break
            os.Add ("polygons", pgons);
        }

        if (wantConvex) {
            GS::ObjectState cvx;
            cvx.Add ("count", (GS::Int32) convexVertexCounts.GetSize ());
            cvx.Add ("polygonIndex", convexPolygon);        // which polygon each piece came from
            cvx.Add ("vertexCounts", convexVertexCounts);
            cvx.Add ("vertexIndices", convexVertexIndices); // flat, split by vertexCounts
            cvx.Add ("normals", convexNormals);             // flat [x,y,z] per corner
            os.Add ("convex", cvx);
        }
    }
};

constexpr const char kEmptyInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
constexpr const char kGetModelInfoOutput[] = R"json(
{"type":"object","properties":{"guid":{"type":"string"},"bounds":{"$ref":"#/$defs/box"},"elementCount":{"type":"integer","minimum":0},"generated":{"type":"boolean"},"hint":{"type":"string"},"colorCount":{"type":"integer","minimum":0},"materialCount":{"type":"integer","minimum":0},"textureCount":{"type":"integer","minimum":0},"fillCount":{"type":"integer","minimum":0},"lightCount":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["guid","bounds","elementCount","generated","colorCount","materialCount","textureCount","fillCount","lightCount"],"$defs":{"box":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"zMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"zMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","zMin","xMax","yMax","zMax"]}}}
)json";
constexpr const char kGetModelElementsInput[] = R"json(
{"type":"object","properties":{"elements":{"type":"array","items":{"$ref":"#/$defs/element"}},"types":{"type":"array","items":{"type":"string"}},"skipEmpty":{"type":"boolean"},"coordinateSystem":{"type":"string","enum":["world","local"]},"include":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["bounds","transform","all"]}},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":0}},"additionalProperties":false,"$defs":{"element":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]},"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}}
)json";
constexpr const char kGetModelElementsOutput[] = R"json(
{"type":"object","properties":{"coordinateSystem":{"type":"string","enum":["world","local"]},"modelElementCount":{"type":"integer","minimum":0},"generated":{"type":"boolean"},"hint":{"type":"string"},"totalCount":{"type":"integer","minimum":0},"offset":{"type":"integer","minimum":0},"count":{"type":"integer","minimum":0},"elements":{"type":"array","items":{"$ref":"#/$defs/modelElement"}}},"additionalProperties":false,"required":["coordinateSystem","modelElementCount","generated","totalCount","offset","count","elements"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"box":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"zMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"zMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","zMin","xMax","yMax","zMax"]},"modelElement":{"type":"object","properties":{"index":{"type":"integer","minimum":1},"elementId":{"$ref":"#/$defs/elementId"},"type":{"type":"integer"},"typeName":{"type":"string"},"invalid":{"type":"boolean"},"genId":{"type":"integer"},"tessellatedBodyCount":{"type":"integer","minimum":0},"meshBodyCount":{"type":"integer","minimum":0},"nurbsBodyCount":{"type":"integer","minimum":0},"pointCloudCount":{"type":"integer","minimum":0},"lightCount":{"type":"integer","minimum":0},"bounds":{"$ref":"#/$defs/box"},"transform":{"type":"array","description":"Packed row-major 3x4 local-to-world affine matrix; 12 numbers.","minItems":12,"maxItems":12,"items":{"type":"number"}}},"additionalProperties":false,"required":["index","elementId","type","typeName","invalid","genId","tessellatedBodyCount","meshBodyCount","nurbsBodyCount","pointCloudCount","lightCount"]}}}
)json";
constexpr const char kGetBodyGeometryInput[] = R"json(
{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"body":{"type":"integer","minimum":1},"source":{"type":"string","enum":["tessellated","mesh"]},"coordinateSystem":{"type":"string","enum":["world","local"]},"include":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["vertices","polygons","edges","convex","normals","vertexHardFlags","all"]}},"maxVertices":{"type":"integer","minimum":1},"maxPolygons":{"type":"integer","minimum":1},"maxEdges":{"type":"integer","minimum":1}},"additionalProperties":false,"anyOf":[{"required":["elementId"]},{"required":["elementIndex"]}],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}}
)json";
constexpr const char kGetBodyGeometryOutput[] = R"json(
{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"elementType":{"type":"string"},"source":{"type":"string","enum":["tessellated","mesh"]},"bodyIndex":{"type":"integer","minimum":1},"bodyCount":{"type":"integer","minimum":1},"coordinateSystem":{"type":"string","enum":["world","local"]},"body":{"$ref":"#/$defs/body"},"vertices":{"type":"array","description":"Packed xyz coordinates; stride 3, ModelerAPI vertex order, 1-based topology indices.","items":{"type":"number"}},"verticesTruncated":{"type":"boolean"},"vertexHardFlags":{"type":"array","items":{"type":"boolean"}},"normals":{"type":"array","description":"Packed xyz normal-vector pool; stride 3; signed polygon indices address this 1-based pool.","items":{"type":"number"}},"edges":{"$ref":"#/$defs/edges"},"polygons":{"$ref":"#/$defs/polygons"},"convex":{"$ref":"#/$defs/convex"}},"additionalProperties":false,"required":["elementId","elementIndex","elementType","source","bodyIndex","bodyCount","coordinateSystem","body"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"box":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"zMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"zMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","zMin","xMax","yMax","zMax"]},"color":{"type":"object","properties":{"red":{"type":"number"},"green":{"type":"number"},"blue":{"type":"number"}},"additionalProperties":false,"required":["red","green","blue"]},"attributeIndex":{"type":"object","properties":{"index":{"type":"integer"},"originalModelerIndex":{"type":"integer"},"originalIndex":{"type":"integer"},"valid":{"type":"boolean"}},"additionalProperties":false,"required":["index","originalModelerIndex","originalIndex","valid"]},"point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]},"textureSystem":{"type":"object","properties":{"mode":{"type":"integer"},"modeName":{"type":"string"},"origo":{"$ref":"#/$defs/point"},"xAxis":{"$ref":"#/$defs/point"},"yAxis":{"$ref":"#/$defs/point"},"zAxis":{"$ref":"#/$defs/point"}},"additionalProperties":false,"required":["mode","modeName","origo","xAxis","yAxis","zAxis"]},"body":{"type":"object","properties":{"isWireBody":{"type":"boolean"},"isSurfaceBody":{"type":"boolean"},"isSolidBody":{"type":"boolean"},"isClosed":{"type":"boolean"},"isVisibleIfContour":{"type":"boolean"},"hasSharpEdge":{"type":"boolean"},"alwaysCastsShadow":{"type":"boolean"},"neverCastsShadow":{"type":"boolean"},"doesNotReceiveShadow":{"type":"boolean"},"vertexCount":{"type":"integer","minimum":0},"edgeCount":{"type":"integer","minimum":0},"polygonCount":{"type":"integer","minimum":0},"polygonVectorCount":{"type":"integer","minimum":0},"bounds":{"$ref":"#/$defs/box"},"hasColor":{"type":"boolean"},"color":{"$ref":"#/$defs/color"},"colorIndex":{"$ref":"#/$defs/attributeIndex"},"materialIndex":{"$ref":"#/$defs/attributeIndex"},"textureIndex":{"$ref":"#/$defs/attributeIndex"},"hasTextureCoordinateSystem":{"type":"boolean"},"textureCoordinateSystem":{"$ref":"#/$defs/textureSystem"}},"additionalProperties":false,"required":["isWireBody","isSurfaceBody","isSolidBody","isClosed","isVisibleIfContour","hasSharpEdge","alwaysCastsShadow","neverCastsShadow","doesNotReceiveShadow","vertexCount","edgeCount","polygonCount","polygonVectorCount","bounds","hasColor","colorIndex","materialIndex","textureIndex","hasTextureCoordinateSystem"]},"integerArray":{"type":"array","items":{"type":"integer"}},"booleanArray":{"type":"array","items":{"type":"boolean"}},"edges":{"type":"object","description":"Packed structure-of-arrays edge topology. Every field has count entries and indices are 1-based; polygon -1 means absent.","properties":{"count":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"vertex1":{"$ref":"#/$defs/integerArray"},"vertex2":{"$ref":"#/$defs/integerArray"},"polygon1":{"$ref":"#/$defs/integerArray"},"polygon2":{"$ref":"#/$defs/integerArray"},"invisible":{"$ref":"#/$defs/booleanArray"},"visibleIfContour":{"$ref":"#/$defs/booleanArray"},"hasColor":{"$ref":"#/$defs/booleanArray"},"colorIndex":{"$ref":"#/$defs/integerArray"}},"additionalProperties":false,"required":["count","truncated","vertex1","vertex2","polygon1","polygon2","invisible","visibleIfContour","hasColor","colorIndex"]},"polygons":{"type":"object","description":"Packed structure-of-arrays polygon topology. edgeCounts splits vertexIndices and signed edgeIndices; zero edge index is a contour break.","properties":{"count":{"type":"integer","minimum":0},"truncated":{"type":"boolean"},"skipped":{"type":"integer","minimum":0},"materialIndex":{"$ref":"#/$defs/integerArray"},"normalVectorIndex":{"$ref":"#/$defs/integerArray"},"polygonId":{"$ref":"#/$defs/integerArray"},"invisible":{"$ref":"#/$defs/booleanArray"},"visibleIfContour":{"$ref":"#/$defs/booleanArray"},"isComplex":{"$ref":"#/$defs/booleanArray"},"isGravity":{"$ref":"#/$defs/booleanArray"},"hasMaterialTexture":{"$ref":"#/$defs/booleanArray"},"hasPolygonTexture":{"$ref":"#/$defs/booleanArray"},"materialTextureIndex":{"$ref":"#/$defs/integerArray"},"polygonTextureIndex":{"$ref":"#/$defs/integerArray"},"edgeCounts":{"$ref":"#/$defs/integerArray"},"vertexIndices":{"$ref":"#/$defs/integerArray"},"edgeIndices":{"$ref":"#/$defs/integerArray"}},"additionalProperties":false,"required":["count","truncated","skipped","materialIndex","normalVectorIndex","polygonId","invisible","visibleIfContour","isComplex","isGravity","hasMaterialTexture","hasPolygonTexture","materialTextureIndex","polygonTextureIndex","edgeCounts","vertexIndices","edgeIndices"]},"convex":{"type":"object","description":"Packed convex pieces. vertexCounts splits vertexIndices; normals are packed xyz per corner (stride 3).","properties":{"count":{"type":"integer","minimum":0},"polygonIndex":{"$ref":"#/$defs/integerArray"},"vertexCounts":{"$ref":"#/$defs/integerArray"},"vertexIndices":{"$ref":"#/$defs/integerArray"},"normals":{"type":"array","items":{"type":"number"}}},"additionalProperties":false,"required":["count","polygonIndex","vertexCounts","vertexIndices","normals"]}}}
)json";

const NativeCommandRegistration commandRegistrations[] = {
    { "GetModelInfo",     &MakeRegisteredNativeCommand<GetModelInfoCommand>,     false, kEmptyInput,                kGetModelInfoOutput },
    { "GetModelElements", &MakeRegisteredNativeCommand<GetModelElementsCommand>, false, kGetModelElementsInput,     kGetModelElementsOutput },
    { "GetBodyGeometry",  &MakeRegisteredNativeCommand<GetBodyGeometryCommand>,  false, kGetBodyGeometryInput,      kGetBodyGeometryOutput },
};

}   // namespace

NativeCommandRegistrations GetModelGeometryCommandRegistrations ()
{
    return MakeRegistrationView (commandRegistrations);
}

} // namespace geomsrv

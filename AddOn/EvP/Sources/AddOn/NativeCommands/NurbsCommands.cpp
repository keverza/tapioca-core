#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/NurbsCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/ModelAccessUtils.hpp"

#include "Geometry/GeometryExtractor.hpp"     // AcquireCurrentModel

#include <Model.hpp>
#include <ModelElement.hpp>
#include <ModelNurbsBody.hpp>
#include <ModelPointCloud.hpp>
#include <ModelMaterial.hpp>
#include <NurbsAttributes.hpp>
#include <AttributeIndex.hpp>
#include <Interval.hpp>
#include <Box3DData.h>
#include <NurbsCurve2D.hpp>
#include <NurbsCurve3D.hpp>
#include <NurbsSurface.hpp>

#include <optional>

namespace geomsrv {

namespace {

void AddElementId (const GS::UniString& guid, GS::ObjectState& record)
{
    GS::ObjectState elementId;
    elementId.Add ("guid", guid);
    record.Add ("elementId", elementId);
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

// ===========================================================================
// E24 — curved geometry, which the snapshot path cannot represent at all.
//
// A Shell, a Morph or a revolved object is a NURBS body: trimmed surfaces with
// a real topology tree. GeometryExtractor only ever asks for the TESSELLATED
// body, so everything downstream — the BVH, the renderer, the slice engine —
// sees the polygon approximation and there is no way back from it to the curve.
// "Is this wall actually curved, and with what radius" is unanswerable from
// triangles, and this is the read that answers it.
//
// THE TOPOLOGY TREE, bottom up (each level indexes the one below):
//   Vertex  — a point, with a tolerance
//   Edge    — part of a 3D curve between two vertices; 0/1/2/many trims
//   Trim    — attaches an edge (or a single vertex, "singular") to ONE face,
//             and carries the 2D curve bounding that face in surface space
//   Loop    — a circular sequence of directed trims; the first loop of a face
//             is its outer boundary, the rest are holes
//   Face    — a bounded piece of ONE surface
//   Shell   — a closed set of faces
//   Lump    — a solid region bounded by shells (first = outer, rest = cavities)
// Plus three geometry pools the topology points into: Curve2D, Curve3D, Surface.
//
// ⚠️ EVERY INDEX ACCESSOR ON THESE OBJECTS IS 1-BASED — `GetTrimIndex (i)` is
// literally `trimIndices[i-1]` in the header — while the VALUES they return are
// 0-based indices into the body's pools, and `Geometry::NurbsCurve3D` /
// `NurbsSurface` are 0-based throughout. That mismatch is the API's, not ours.
// The loops below are written 1-based over counts and the values are emitted
// unchanged; do not "normalise" one side without the other.
//
// Reads. Gate, no undo scope.
// ===========================================================================

GS::ObjectState IntervalToObjectState (const ModelerAPI::Interval& interval)
{
    GS::ObjectState os;
    os.Add ("begin", interval.begin);
    os.Add ("end", interval.end);
    return os;
}


GS::UniString VisibilityName (ModelerAPI::NurbsEdgeAttributes::Visibility v)
{
    switch (v) {
        case ModelerAPI::NurbsEdgeAttributes::Visibility::Visible:          return "visible";
        case ModelerAPI::NurbsEdgeAttributes::Visibility::Invisible:        return "invisible";
        case ModelerAPI::NurbsEdgeAttributes::Visibility::VisibleIfContour: return "visibleIfContour";
    }
    return "unknown";
}


// One 3D curve: degree, control net, knots, weights. 0-based, per
// Geometry::NurbsCurveBase.
GS::ObjectState Curve3DToObjectState (const Geometry::NurbsCurve3D& curve)
{
    GS::ObjectState os;
    os.Add ("degree", (GS::Int32) curve.GetDegree ());
    os.Add ("rational", curve.IsRational ());
    os.Add ("periodic", curve.IsPeriodic ());
    os.Add ("domainStart", curve.GetDomainStart ());
    os.Add ("domainEnd", curve.GetDomainEnd ());

    GS::Array<double> controlPoints;
    const UInt32 cpCount = curve.GetControlPointCount ();
    for (UInt32 i = 0; i < cpCount; ++i) {
        const Point3D p = curve.GetControlPoint (i);
        controlPoints.Push (p.x);
        controlPoints.Push (p.y);
        controlPoints.Push (p.z);
    }
    os.Add ("controlPointCount", (GS::Int32) cpCount);
    os.Add ("controlPoints", controlPoints);         // flat [x,y,z, …]

    GS::Array<double> knots;
    const UInt32 knotCount = curve.GetKnotCount ();
    for (UInt32 i = 0; i < knotCount; ++i)
        knots.Push (curve.GetKnot (i));
    os.Add ("knots", knots);

    GS::Array<double> weights;
    const UInt32 weightCount = curve.GetWeightCount ();
    for (UInt32 i = 0; i < weightCount; ++i)
        weights.Push (curve.GetWeight (i));
    os.Add ("weights", weights);                     // empty when not rational
    return os;
}


GS::ObjectState Curve2DToObjectState (const Geometry::NurbsCurve2D& curve)
{
    GS::ObjectState os;
    os.Add ("degree", (GS::Int32) curve.GetDegree ());
    os.Add ("rational", curve.IsRational ());
    os.Add ("periodic", curve.IsPeriodic ());
    os.Add ("domainStart", curve.GetDomainStart ());
    os.Add ("domainEnd", curve.GetDomainEnd ());

    GS::Array<double> controlPoints;
    const UInt32 cpCount = curve.GetControlPointCount ();
    for (UInt32 i = 0; i < cpCount; ++i) {
        const Point2D p = curve.GetControlPoint (i);
        controlPoints.Push (p.x);
        controlPoints.Push (p.y);
    }
    os.Add ("controlPointCount", (GS::Int32) cpCount);
    os.Add ("controlPoints", controlPoints);         // flat [u,v, …] in surface space

    GS::Array<double> knots;
    const UInt32 knotCount = curve.GetKnotCount ();
    for (UInt32 i = 0; i < knotCount; ++i)
        knots.Push (curve.GetKnot (i));
    os.Add ("knots", knots);

    GS::Array<double> weights;
    const UInt32 weightCount = curve.GetWeightCount ();
    for (UInt32 i = 0; i < weightCount; ++i)
        weights.Push (curve.GetWeight (i));
    os.Add ("weights", weights);
    return os;
}


// One surface: a control GRID, so the control points are emitted row-major with
// the two dimensions reported rather than as an opaque flat list.
GS::ObjectState SurfaceToObjectState (const Geometry::NurbsSurface& surface)
{
    GS::ObjectState os;
    os.Add ("degreeU", (GS::Int32) surface.GetDegreeU ());
    os.Add ("degreeV", (GS::Int32) surface.GetDegreeV ());
    os.Add ("rational", surface.IsRational ());
    os.Add ("periodicU", surface.IsPeriodicU ());
    os.Add ("periodicV", surface.IsPeriodicV ());

    const UInt32 uCount = surface.GetControlPointUCount ();
    const UInt32 vCount = surface.GetControlPointVCount ();
    os.Add ("controlPointUCount", (GS::Int32) uCount);
    os.Add ("controlPointVCount", (GS::Int32) vCount);

    GS::Array<double> controlPoints;
    for (UInt32 u = 0; u < uCount; ++u) {
        for (UInt32 v = 0; v < vCount; ++v) {
            const Point3D p = surface.GetControlPoint (u, v);
            controlPoints.Push (p.x);
            controlPoints.Push (p.y);
            controlPoints.Push (p.z);
        }
    }
    os.Add ("controlPoints", controlPoints);   // row-major: u outer, v inner

    GS::Array<double> knotsU, knotsV;
    for (UInt32 i = 0, n = surface.GetKnotUCount (); i < n; ++i) knotsU.Push (surface.GetKnotU (i));
    for (UInt32 i = 0, n = surface.GetKnotVCount (); i < n; ++i) knotsV.Push (surface.GetKnotV (i));
    os.Add ("knotsU", knotsU);
    os.Add ("knotsV", knotsV);

    GS::Array<double> weights;
    const UInt32 wU = surface.GetWeightUCount ();
    const UInt32 wV = surface.GetWeightVCount ();
    for (UInt32 u = 0; u < wU; ++u)
        for (UInt32 v = 0; v < wV; ++v)
            weights.Push (surface.GetWeight (u, v));
    os.Add ("weightUCount", (GS::Int32) wU);
    os.Add ("weightVCount", (GS::Int32) wV);
    os.Add ("weights", weights);
    return os;
}


// ---------------------------------------------------------------------------
// EvP.GetNurbsBody { guid | elementIndex, body?:1, coordinateSystem?,
//                    include?:["vertices","edges","trims","loops","faces",
//                              "shells","lumps","curves2d","curves3d",
//                              "surfaces","all"] }
//   -> { ok, guid, elementIndex, bodyIndex, bodyCount, coordinateSystem,
//        body:{ counts…, material, edgePen, smoothness, textureCoordSys, bounds },
//        vertices?, edges?, trims?, loops?, faces?, shells?, lumps?,
//        curves2d?, curves3d?, surfaces? }
//
// The whole tree for ONE NURBS body. Topology sections are ON by default (they
// are small and useless individually); the three GEOMETRY pools —
// `curves2d`, `curves3d`, `surfaces` — are OFF by default because each control
// net and knot vector is real bulk and most callers want the topology plus a
// couple of named surfaces, not every one.
//
// ⚠️ NOT every element has a NURBS body. Walls, slabs and objects are mesh
// bodies; `nurbsBodyCount` on EvP.GetModelElements says which are which, and
// this command refuses rather than inventing an empty tree.
// ---------------------------------------------------------------------------
class GetNurbsBodyCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetNurbsBody"; }

    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetNurbsBody"));
        }

        ModelerAPI::Element elem;
        Int32 elemIndex = 0;
        GS::UniString err;
        if (!ResolveElement (model, params, elem, elemIndex, err)) {
            return NativeCommandResult::Failure (
                EVP_FAIL (err, "EvP.GetNurbsBody element lookup"));
        }

        const Int32 bodyCount = elem.GetNurbsBodyCount ();
        GS::Int32 bodyIndex = 1;
        params.Get ("body", bodyIndex);
        if (bodyCount <= 0) {
            return NativeCommandResult::Failure (EVP_FAIL (
                "this element has no NURBS body",
                "walls/slabs/objects are mesh bodies — use EvP.GetBodyGeometry"));
        }
        if (bodyIndex < 1 || bodyIndex > bodyCount) {
            return NativeCommandResult::Failure (EVP_FAIL (
                GS::UniString::Printf ("body %d is out of range (1..%d)", (int) bodyIndex, (int) bodyCount),
                "body indices are 1-based"));
        }

        ModelerAPI::NurbsBody body;
        elem.GetNurbsBody ((Int32) bodyIndex, &body);
        const ModelerAPI::CoordinateSystem cs = ParseCoordinateSystem (params);

        AddElementId (ElementGuidString (elem), os);
        os.Add ("elementIndex", (GS::Int32) elemIndex);
        os.Add ("elementType", ElementTypeName (elem.GetType ()));
        os.Add ("bodyIndex", bodyIndex);
        os.Add ("bodyCount", (GS::Int32) bodyCount);
        os.Add ("coordinateSystem", CoordinateSystemName (cs));
        os.Add ("body", DescribeBody (body, cs));

        if (WantsSection (params, "vertices", true))  AddVertices (os, body, cs);
        if (WantsSection (params, "edges", true))     AddEdges (os, body, cs);
        if (WantsSection (params, "trims", true))     AddTrims (os, body);
        if (WantsSection (params, "loops", true))     AddLoops (os, body);
        if (WantsSection (params, "faces", true))     AddFaces (os, body, cs);
        if (WantsSection (params, "shells", true))    AddShells (os, body);
        if (WantsSection (params, "lumps", true))     AddLumps (os, body);
        if (WantsSection (params, "curves3d", false)) AddCurves3D (os, body, cs);
        if (WantsSection (params, "curves2d", false)) AddCurves2D (os, body);
        if (WantsSection (params, "surfaces", false)) AddSurfaces (os, body, cs);
        return os;
    }

private:
    static GS::ObjectState DescribeBody (const ModelerAPI::NurbsBody& body,
                                         ModelerAPI::CoordinateSystem cs)
    {
        GS::ObjectState b;
        b.Add ("vertexCount", (GS::Int32) body.GetVertexCount ());
        b.Add ("edgeCount", (GS::Int32) body.GetEdgeCount ());
        b.Add ("trimCount", (GS::Int32) body.GetTrimCount ());
        b.Add ("loopCount", (GS::Int32) body.GetLoopCount ());
        b.Add ("faceCount", (GS::Int32) body.GetFaceCount ());
        b.Add ("shellCount", (GS::Int32) body.GetShellCount ());
        b.Add ("lumpCount", (GS::Int32) body.GetLumpCount ());
        b.Add ("curve2DCount", (GS::Int32) body.GetCurve2DCount ());
        b.Add ("curve3DCount", (GS::Int32) body.GetCurve3DCount ());
        b.Add ("surfaceCount", (GS::Int32) body.GetSurfaceCount ());

        b.Add ("bounds", BoxToObjectState (body.GetBounds (cs)));
        b.Add ("alwaysCastsShadow", body.AlwaysCastsShadow ());
        b.Add ("neverCastsShadow", body.NeverCastsShadow ());
        b.Add ("doesNotReceiveShadow", body.DoesNotReceiveShadow ());
        b.Add ("smoothness", IntervalToObjectState (body.GetSmoothness ()));
        b.Add ("edgePenIndex", AttributeIndexToObjectState (body.GetEdgePenIdx ()));
        b.Add ("edgePen", ColorToObjectState (body.GetEdgePen ()));
        b.Add ("materialIndex", AttributeIndexToObjectState (body.GetMaterialIdx ()));
        b.Add ("material", MaterialToObjectState (body.GetMaterial ()));
        b.Add ("textureCoordSys", TextureCoordSysToObjectState (body.GetTextureCoordSys ()));
        return b;
    }

    static void AddVertices (GS::ObjectState& os, const ModelerAPI::NurbsBody& body,
                             ModelerAPI::CoordinateSystem cs)
    {
        GS::Array<double> coords, tolerances;
        GS::Array<bool>   hard;
        const UInt32 count = body.GetVertexCount ();
        for (UInt32 i = 1; i <= count; ++i) {
            const ModelerAPI::NurbsVertex v = body.GetVertex (i, cs);
            coords.Push (v.x);
            coords.Push (v.y);
            coords.Push (v.z);
            tolerances.Push (v.GetTolerance ());
            // Hardness = "this vertex breaks shading", the NURBS equivalent of
            // MeshBody::GetVertexHardFlag.
            hard.Push (body.GetVertexAttributes (i).hardness == ModelerAPI::NurbsVertexAttributes::Hardness::Hard);
        }
        GS::ObjectState v;
        v.Add ("count", (GS::Int32) count);
        v.Add ("coords", coords);            // flat [x,y,z, …]
        v.Add ("tolerances", tolerances);
        v.Add ("hard", hard);
        os.Add ("vertices", v);
    }

    static void AddEdges (GS::ObjectState& os, const ModelerAPI::NurbsBody& body,
                          ModelerAPI::CoordinateSystem cs)
    {
        GS::Array<GS::Int32> beginVertex, endVertex, curve3DIndex, trimCounts, trimIndices, colors;
        GS::Array<double>    tolerances, subdomainBegin, subdomainEnd;
        GS::Array<bool>      loopEdge, ringEdge, wire, surfaceBoundary, manifold, smooth;
        GS::Array<GS::UniString> visibility;

        const UInt32 count = body.GetEdgeCount ();
        for (UInt32 i = 1; i <= count; ++i) {
            const ModelerAPI::NurbsEdge e = body.GetEdge (i, cs);
            beginVertex.Push ((GS::Int32) e.GetBeginVertexIndex ());   // negative for a ring edge
            endVertex.Push ((GS::Int32) e.GetEndVertexIndex ());
            curve3DIndex.Push ((GS::Int32) e.GetCurve3DIndex ());
            tolerances.Push (e.GetTolerance ());
            const ModelerAPI::Interval sub = e.GetCurveSubdomain ();
            subdomainBegin.Push (sub.begin);
            subdomainEnd.Push (sub.end);
            loopEdge.Push (e.IsLoopEdge ());
            ringEdge.Push (e.IsRingEdge ());
            wire.Push (e.IsWire ());
            surfaceBoundary.Push (e.IsSurfaceBoundary ());
            manifold.Push (e.Is2Manifold ());

            const UInt32 trims = e.GetTrimIndexCount ();
            trimCounts.Push ((GS::Int32) trims);
            for (UInt32 t = 1; t <= trims; ++t)
                trimIndices.Push ((GS::Int32) e.GetTrimIndex (t));

            const ModelerAPI::NurbsEdgeAttributes attrs = body.GetEdgeAttributes (i);
            visibility.Push (VisibilityName (attrs.visibility));
            smooth.Push (attrs.smoothness == ModelerAPI::NurbsEdgeAttributes::Smoothness::Smooth);
            colors.Push ((GS::Int32) attrs.color);
        }

        GS::ObjectState e;
        e.Add ("count", (GS::Int32) count);
        e.Add ("beginVertex", beginVertex);
        e.Add ("endVertex", endVertex);
        e.Add ("curve3DIndex", curve3DIndex);
        e.Add ("tolerances", tolerances);
        e.Add ("subdomainBegin", subdomainBegin);
        e.Add ("subdomainEnd", subdomainEnd);
        e.Add ("isLoopEdge", loopEdge);
        e.Add ("isRingEdge", ringEdge);
        e.Add ("isWire", wire);
        e.Add ("isSurfaceBoundary", surfaceBoundary);
        e.Add ("is2Manifold", manifold);
        e.Add ("trimCounts", trimCounts);      // per-edge split of trimIndices
        e.Add ("trimIndices", trimIndices);
        e.Add ("visibility", visibility);
        e.Add ("smooth", smooth);
        e.Add ("color", colors);
        os.Add ("edges", e);
    }

    static void AddTrims (GS::ObjectState& os, const ModelerAPI::NurbsBody& body)
    {
        GS::Array<GS::Int32> edgeIndex, vertexIndex, loopIndex, curve2DIndex;
        GS::Array<double>    tolerances, subdomainBegin, subdomainEnd;
        GS::Array<bool>      singular;

        const UInt32 count = body.GetTrimCount ();
        for (UInt32 i = 1; i <= count; ++i) {
            const ModelerAPI::NurbsTrim t = body.GetTrim (i);
            // Exactly one of these is >= 0: an ordinary trim names an edge, a
            // SINGULAR trim (the surface collapses to a point along it, e.g. a
            // sphere's pole) names a vertex instead.
            edgeIndex.Push ((GS::Int32) t.GetEdgeIndex ());
            vertexIndex.Push ((GS::Int32) t.GetVertexIndex ());
            loopIndex.Push ((GS::Int32) t.GetLoopIndex ());
            curve2DIndex.Push ((GS::Int32) t.GetTrimcurve2DIndex ());
            tolerances.Push (t.GetTolerance ());
            const ModelerAPI::Interval sub = t.GetCurveSubdomain ();
            subdomainBegin.Push (sub.begin);
            subdomainEnd.Push (sub.end);
            singular.Push (t.IsSingular ());
        }

        GS::ObjectState t;
        t.Add ("count", (GS::Int32) count);
        t.Add ("edgeIndex", edgeIndex);
        t.Add ("vertexIndex", vertexIndex);
        t.Add ("loopIndex", loopIndex);
        t.Add ("curve2DIndex", curve2DIndex);
        t.Add ("tolerances", tolerances);
        t.Add ("subdomainBegin", subdomainBegin);
        t.Add ("subdomainEnd", subdomainEnd);
        t.Add ("isSingular", singular);
        os.Add ("trims", t);
    }

    static void AddLoops (GS::ObjectState& os, const ModelerAPI::NurbsBody& body)
    {
        GS::Array<GS::Int32> faceIndex, trimCounts, trimIndices;
        GS::Array<bool>      trimReversed;

        const UInt32 count = body.GetLoopCount ();
        for (UInt32 i = 1; i <= count; ++i) {
            const ModelerAPI::NurbsLoop loop = body.GetLoop (i);
            faceIndex.Push ((GS::Int32) loop.GetFaceIndex ());
            const UInt32 trims = loop.GetTrimIndexCount ();
            trimCounts.Push ((GS::Int32) trims);
            for (UInt32 t = 1; t <= trims; ++t) {
                const ModelerAPI::DirectedTrimIndex directed = loop.GetTrimIndex (t);
                trimIndices.Push ((GS::Int32) directed.trim);
                trimReversed.Push (directed.reversed);
            }
        }

        GS::ObjectState l;
        l.Add ("count", (GS::Int32) count);
        l.Add ("faceIndex", faceIndex);
        l.Add ("trimCounts", trimCounts);        // per-loop split of the two arrays below
        l.Add ("trimIndices", trimIndices);
        l.Add ("trimReversed", trimReversed);
        os.Add ("loops", l);
    }

    static void AddFaces (GS::ObjectState& os, const ModelerAPI::NurbsBody& body,
                          ModelerAPI::CoordinateSystem cs)
    {
        GS::Array<GS::Int32> shellIndex, surfaceIndex, loopCounts, loopIndices;
        GS::Array<GS::Int32> material, segmentationPen;
        GS::Array<double>    tolerances;
        GS::Array<GS::ObjectState> textureCoordSys;

        const UInt32 count = body.GetFaceCount ();
        for (UInt32 i = 1; i <= count; ++i) {
            const ModelerAPI::NurbsFace f = body.GetFace (i, cs);
            shellIndex.Push ((GS::Int32) f.GetShellIndex ());     // negative = lamina face
            surfaceIndex.Push ((GS::Int32) f.GetSurfaceIndex ());
            tolerances.Push (f.GetTolerance ());
            const UInt32 loops = f.GetLoopIndexCount ();
            loopCounts.Push ((GS::Int32) loops);
            for (UInt32 l = 1; l <= loops; ++l)
                loopIndices.Push ((GS::Int32) f.GetLoopIndex (l));   // first = outer loop

            const ModelerAPI::NurbsFaceAttributes attrs = body.GetFaceAttributes (i);
            material.Push ((GS::Int32) attrs.material);
            segmentationPen.Push ((GS::Int32) attrs.segmentationPen);
            textureCoordSys.Push (TextureCoordSysToObjectState (attrs.textureCoordSys));
        }

        GS::ObjectState f;
        f.Add ("count", (GS::Int32) count);
        f.Add ("shellIndex", shellIndex);
        f.Add ("surfaceIndex", surfaceIndex);
        f.Add ("tolerances", tolerances);
        f.Add ("loopCounts", loopCounts);        // per-face split of loopIndices
        f.Add ("loopIndices", loopIndices);
        f.Add ("material", material);
        f.Add ("segmentationPen", segmentationPen);
        f.Add ("textureCoordSys", textureCoordSys);
        os.Add ("faces", f);
    }

    static void AddShells (GS::ObjectState& os, const ModelerAPI::NurbsBody& body)
    {
        GS::Array<GS::Int32> lumpIndex, faceCounts, faceIndices;
        GS::Array<bool>      faceReversed;

        const UInt32 count = body.GetShellCount ();
        for (UInt32 i = 1; i <= count; ++i) {
            const ModelerAPI::NurbsShell s = body.GetShell (i);
            lumpIndex.Push ((GS::Int32) s.GetLumpIndex ());
            const UInt32 faces = s.GetFaceIndexCount ();
            faceCounts.Push ((GS::Int32) faces);
            for (UInt32 f = 1; f <= faces; ++f) {
                const ModelerAPI::DirectedFaceIndex directed = s.GetFaceIndex (f);
                faceIndices.Push ((GS::Int32) directed.face);
                faceReversed.Push (directed.reversed);
            }
        }

        GS::ObjectState s;
        s.Add ("count", (GS::Int32) count);
        s.Add ("lumpIndex", lumpIndex);
        s.Add ("faceCounts", faceCounts);
        s.Add ("faceIndices", faceIndices);
        s.Add ("faceReversed", faceReversed);
        os.Add ("shells", s);
    }

    static void AddLumps (GS::ObjectState& os, const ModelerAPI::NurbsBody& body)
    {
        GS::Array<GS::Int32> shellCounts, shellIndices;
        const UInt32 count = body.GetLumpCount ();
        for (UInt32 i = 1; i <= count; ++i) {
            const ModelerAPI::NurbsLump l = body.GetLump (i);
            const UInt32 shells = l.GetShellIndexCount ();
            shellCounts.Push ((GS::Int32) shells);
            for (UInt32 s = 1; s <= shells; ++s)
                shellIndices.Push ((GS::Int32) l.GetShellIndex (s));   // first = outer shell
        }

        GS::ObjectState l;
        l.Add ("count", (GS::Int32) count);
        l.Add ("shellCounts", shellCounts);
        l.Add ("shellIndices", shellIndices);
        os.Add ("lumps", l);
    }

    static void AddCurves3D (GS::ObjectState& os, const ModelerAPI::NurbsBody& body,
                             ModelerAPI::CoordinateSystem cs)
    {
        GS::Array<GS::ObjectState> curves;
        for (UInt32 i = 1, n = body.GetCurve3DCount (); i <= n; ++i)
            curves.Push (Curve3DToObjectState (body.GetCurve3D (i, cs)));
        os.Add ("curves3d", curves);
    }

    static void AddCurves2D (GS::ObjectState& os, const ModelerAPI::NurbsBody& body)
    {
        GS::Array<GS::ObjectState> curves;
        for (UInt32 i = 1, n = body.GetCurve2DCount (); i <= n; ++i)
            curves.Push (Curve2DToObjectState (body.GetCurve2D (i)));
        os.Add ("curves2d", curves);
    }

    static void AddSurfaces (GS::ObjectState& os, const ModelerAPI::NurbsBody& body,
                             ModelerAPI::CoordinateSystem cs)
    {
        GS::Array<GS::ObjectState> surfaces;
        for (UInt32 i = 1, n = body.GetSurfaceCount (); i <= n; ++i)
            surfaces.Push (SurfaceToObjectState (body.GetSurface (i, cs)));
        os.Add ("surfaces", surfaces);
    }
};


// ---------------------------------------------------------------------------
// EvP.GetPointClouds { guid?, elementIndex?, coordinateSystem? }
//   -> { ok, count, pointClouds:[{ elementIndex, guid, cloudIndex, bounds,
//        transform:[16 floats] }] }
//
// Where the surveyed point clouds are. No points — a cloud is millions of them
// and there is no bus that should carry that — but the BOUNDS and the
// data→world matrix, which is what a script needs to decide whether a cloud is
// relevant, place something against it, or hand its transform to a real point
// cloud library.
//
// `transform` is PC::Matrix's own `float data[16]`, row-major, unmodified.
// ---------------------------------------------------------------------------
class GetPointCloudsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetPointClouds"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        ModelerAPI::Model model;
        if (!AcquireCurrentModel (model)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("could not read the 3D model", "EvP.GetPointClouds"));
        }

        const ModelerAPI::CoordinateSystem cs = ParseCoordinateSystem (params);

        // One element, or a sweep of the whole model. The sweep is cheap: point
        // clouds are rare, and GetPointCloudCount is a counter read.
        Int32 first = 1, last = model.GetElementCount ();
        GS::ObjectState requestedElementId;
        GS::Int32 requestedIndex = 0;
        if (params.Get ("elementId", requestedElementId) ||
            params.Get ("elementIndex", requestedIndex)) {
            ModelerAPI::Element elem;
            Int32 resolved = 0;
            GS::UniString err;
            if (!ResolveElement (model, params, elem, resolved, err)) {
                return NativeCommandResult::Failure (
                    EVP_FAIL (err, "EvP.GetPointClouds element lookup"));
            }
            first = last = resolved;
        }

        GS::Array<GS::ObjectState> clouds;
        for (Int32 i = first; i <= last; ++i) {
            ModelerAPI::Element elem;
            model.GetElement (i, &elem);
            const Int32 count = elem.GetPointCloudCount ();
            for (Int32 c = 1; c <= count; ++c) {
                ModelerAPI::PointCloud cloud;
                elem.GetPointCloud (c, &cloud);

                GS::Array<double> matrix;
                const PC::Matrix m = cloud.GetDataToTargetCoordSysTransformation (cs);
                for (int k = 0; k < 16; ++k)
                    matrix.Push ((double) m.data[k]);

                GS::ObjectState record;
                record.Add ("elementIndex", (GS::Int32) i);
                AddElementId (ElementGuidString (elem), record);
                record.Add ("cloudIndex", (GS::Int32) c);
                record.Add ("bounds", BoxToObjectState (cloud.GetBounds (cs)));
                record.Add ("transform", matrix);
                clouds.Push (record);
            }
        }

        os.Add ("coordinateSystem", CoordinateSystemName (cs));
        os.Add ("count", (GS::Int32) clouds.GetSize ());
        os.Add ("pointClouds", clouds);
        return os;
    }
};

constexpr const char kNurbsInput[] = R"json({"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"body":{"type":"integer","minimum":1},"coordinateSystem":{"type":"string","enum":["world","local"]},"include":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["vertices","edges","trims","loops","faces","shells","lumps","curves2d","curves3d","surfaces","all"]}}},"additionalProperties":false,"anyOf":[{"required":["elementId"]},{"required":["elementIndex"]}],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kNurbsOutput[] = R"json(
{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"elementType":{"type":"string"},"bodyIndex":{"type":"integer","minimum":1},"bodyCount":{"type":"integer","minimum":1},"coordinateSystem":{"type":"string","enum":["world","local"]},"body":{"$ref":"#/$defs/body"},"vertices":{"$ref":"#/$defs/vertices"},"edges":{"$ref":"#/$defs/edges"},"trims":{"$ref":"#/$defs/trims"},"loops":{"$ref":"#/$defs/loops"},"faces":{"$ref":"#/$defs/faces"},"shells":{"$ref":"#/$defs/shells"},"lumps":{"$ref":"#/$defs/lumps"},"curves2d":{"type":"array","items":{"$ref":"#/$defs/curve2d"}},"curves3d":{"type":"array","items":{"$ref":"#/$defs/curve3d"}},"surfaces":{"type":"array","items":{"$ref":"#/$defs/surface"}}},"additionalProperties":false,"required":["elementId","elementIndex","elementType","bodyIndex","bodyCount","coordinateSystem","body"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]},"color":{"type":"object","properties":{"red":{"type":"number"},"green":{"type":"number"},"blue":{"type":"number"}},"additionalProperties":false,"required":["red","green","blue"]},"box":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"zMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"zMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","zMin","xMax","yMax","zMax"]},"interval":{"type":"object","properties":{"begin":{"type":"number"},"end":{"type":"number"}},"additionalProperties":false,"required":["begin","end"]},"attributeIndex":{"type":"object","properties":{"index":{"type":"integer"},"originalModelerIndex":{"type":"integer"},"originalIndex":{"type":"integer"},"valid":{"type":"boolean"}},"additionalProperties":false,"required":["index","originalModelerIndex","originalIndex","valid"]},"textureSystem":{"type":"object","properties":{"mode":{"type":"integer"},"modeName":{"type":"string"},"origo":{"$ref":"#/$defs/point"},"xAxis":{"$ref":"#/$defs/point"},"yAxis":{"$ref":"#/$defs/point"},"zAxis":{"$ref":"#/$defs/point"}},"additionalProperties":false,"required":["mode","modeName","origo","xAxis","yAxis","zAxis"]},"material":{"type":"object","properties":{"type":{"type":"integer"},"typeName":{"type":"string"},"name":{"type":"string"},"surfaceColor":{"$ref":"#/$defs/color"},"ambientReflection":{"type":"number"},"diffuseReflection":{"type":"number"},"specularReflection":{"type":"number"},"specularColor":{"$ref":"#/$defs/color"},"transparency":{"type":"number"},"transparencyAttenuation":{"type":"number"},"shining":{"type":"number"},"emissionColor":{"$ref":"#/$defs/color"},"emissionAttenuation":{"type":"number"},"externalReference":{"type":"integer"},"hasTexture":{"type":"boolean"},"textureName":{"type":"string"},"textureRotationAngle":{"type":"number"},"textureIndex":{"$ref":"#/$defs/attributeIndex"},"fillIndex":{"$ref":"#/$defs/attributeIndex"},"fillColorIndex":{"$ref":"#/$defs/attributeIndex"}},"additionalProperties":false,"required":["type","typeName","name","surfaceColor","ambientReflection","diffuseReflection","specularReflection","specularColor","transparency","transparencyAttenuation","shining","emissionColor","emissionAttenuation","externalReference","hasTexture","fillIndex","fillColorIndex"]},"body":{"type":"object","properties":{"vertexCount":{"type":"integer","minimum":0},"edgeCount":{"type":"integer","minimum":0},"trimCount":{"type":"integer","minimum":0},"loopCount":{"type":"integer","minimum":0},"faceCount":{"type":"integer","minimum":0},"shellCount":{"type":"integer","minimum":0},"lumpCount":{"type":"integer","minimum":0},"curve2DCount":{"type":"integer","minimum":0},"curve3DCount":{"type":"integer","minimum":0},"surfaceCount":{"type":"integer","minimum":0},"bounds":{"$ref":"#/$defs/box"},"alwaysCastsShadow":{"type":"boolean"},"neverCastsShadow":{"type":"boolean"},"doesNotReceiveShadow":{"type":"boolean"},"smoothness":{"$ref":"#/$defs/interval"},"edgePenIndex":{"$ref":"#/$defs/attributeIndex"},"edgePen":{"$ref":"#/$defs/color"},"materialIndex":{"$ref":"#/$defs/attributeIndex"},"material":{"$ref":"#/$defs/material"},"textureCoordSys":{"$ref":"#/$defs/textureSystem"}},"additionalProperties":false,"required":["vertexCount","edgeCount","trimCount","loopCount","faceCount","shellCount","lumpCount","curve2DCount","curve3DCount","surfaceCount","bounds","alwaysCastsShadow","neverCastsShadow","doesNotReceiveShadow","smoothness","edgePenIndex","edgePen","materialIndex","material","textureCoordSys"]},"intArray":{"type":"array","items":{"type":"integer"}},"numberArray":{"type":"array","items":{"type":"number"}},"boolArray":{"type":"array","items":{"type":"boolean"}},"stringArray":{"type":"array","items":{"type":"string"}},"vertices":{"type":"object","description":"Packed NURBS vertex structure-of-arrays; coords use xyz stride 3 and all arrays follow 1-based vertex order.","properties":{"count":{"type":"integer","minimum":0},"coords":{"$ref":"#/$defs/numberArray"},"tolerances":{"$ref":"#/$defs/numberArray"},"hard":{"$ref":"#/$defs/boolArray"}},"additionalProperties":false,"required":["count","coords","tolerances","hard"]},"edges":{"type":"object","description":"Packed NURBS edge structure-of-arrays. trimCounts splits trimIndices; topology values retain the API pool index conventions.","properties":{"count":{"type":"integer","minimum":0},"beginVertex":{"$ref":"#/$defs/intArray"},"endVertex":{"$ref":"#/$defs/intArray"},"curve3DIndex":{"$ref":"#/$defs/intArray"},"tolerances":{"$ref":"#/$defs/numberArray"},"subdomainBegin":{"$ref":"#/$defs/numberArray"},"subdomainEnd":{"$ref":"#/$defs/numberArray"},"isLoopEdge":{"$ref":"#/$defs/boolArray"},"isRingEdge":{"$ref":"#/$defs/boolArray"},"isWire":{"$ref":"#/$defs/boolArray"},"isSurfaceBoundary":{"$ref":"#/$defs/boolArray"},"is2Manifold":{"$ref":"#/$defs/boolArray"},"trimCounts":{"$ref":"#/$defs/intArray"},"trimIndices":{"$ref":"#/$defs/intArray"},"visibility":{"$ref":"#/$defs/stringArray"},"smooth":{"$ref":"#/$defs/boolArray"},"color":{"$ref":"#/$defs/intArray"}},"additionalProperties":false,"required":["count","beginVertex","endVertex","curve3DIndex","tolerances","subdomainBegin","subdomainEnd","isLoopEdge","isRingEdge","isWire","isSurfaceBoundary","is2Manifold","trimCounts","trimIndices","visibility","smooth","color"]},"trims":{"type":"object","description":"Packed NURBS trim structure-of-arrays in 1-based trim order.","properties":{"count":{"type":"integer","minimum":0},"edgeIndex":{"$ref":"#/$defs/intArray"},"vertexIndex":{"$ref":"#/$defs/intArray"},"loopIndex":{"$ref":"#/$defs/intArray"},"curve2DIndex":{"$ref":"#/$defs/intArray"},"tolerances":{"$ref":"#/$defs/numberArray"},"subdomainBegin":{"$ref":"#/$defs/numberArray"},"subdomainEnd":{"$ref":"#/$defs/numberArray"},"isSingular":{"$ref":"#/$defs/boolArray"}},"additionalProperties":false,"required":["count","edgeIndex","vertexIndex","loopIndex","curve2DIndex","tolerances","subdomainBegin","subdomainEnd","isSingular"]},"loops":{"type":"object","description":"Packed loop topology; trimCounts splits trimIndices and trimReversed.","properties":{"count":{"type":"integer","minimum":0},"faceIndex":{"$ref":"#/$defs/intArray"},"trimCounts":{"$ref":"#/$defs/intArray"},"trimIndices":{"$ref":"#/$defs/intArray"},"trimReversed":{"$ref":"#/$defs/boolArray"}},"additionalProperties":false,"required":["count","faceIndex","trimCounts","trimIndices","trimReversed"]},"faces":{"type":"object","description":"Packed face topology; loopCounts splits loopIndices.","properties":{"count":{"type":"integer","minimum":0},"shellIndex":{"$ref":"#/$defs/intArray"},"surfaceIndex":{"$ref":"#/$defs/intArray"},"tolerances":{"$ref":"#/$defs/numberArray"},"loopCounts":{"$ref":"#/$defs/intArray"},"loopIndices":{"$ref":"#/$defs/intArray"},"material":{"$ref":"#/$defs/intArray"},"segmentationPen":{"$ref":"#/$defs/intArray"},"textureCoordSys":{"type":"array","items":{"$ref":"#/$defs/textureSystem"}}},"additionalProperties":false,"required":["count","shellIndex","surfaceIndex","tolerances","loopCounts","loopIndices","material","segmentationPen","textureCoordSys"]},"shells":{"type":"object","description":"Packed shell topology; faceCounts splits faceIndices and faceReversed.","properties":{"count":{"type":"integer","minimum":0},"lumpIndex":{"$ref":"#/$defs/intArray"},"faceCounts":{"$ref":"#/$defs/intArray"},"faceIndices":{"$ref":"#/$defs/intArray"},"faceReversed":{"$ref":"#/$defs/boolArray"}},"additionalProperties":false,"required":["count","lumpIndex","faceCounts","faceIndices","faceReversed"]},"lumps":{"type":"object","description":"Packed lump topology; shellCounts splits shellIndices.","properties":{"count":{"type":"integer","minimum":0},"shellCounts":{"$ref":"#/$defs/intArray"},"shellIndices":{"$ref":"#/$defs/intArray"}},"additionalProperties":false,"required":["count","shellCounts","shellIndices"]},"curve2d":{"type":"object","properties":{"degree":{"type":"integer","minimum":0},"rational":{"type":"boolean"},"periodic":{"type":"boolean"},"domainStart":{"type":"number"},"domainEnd":{"type":"number"},"controlPointCount":{"type":"integer","minimum":0},"controlPoints":{"type":"array","description":"Packed uv control points; stride 2.","items":{"type":"number"}},"knots":{"$ref":"#/$defs/numberArray"},"weights":{"$ref":"#/$defs/numberArray"}},"additionalProperties":false,"required":["degree","rational","periodic","domainStart","domainEnd","controlPointCount","controlPoints","knots","weights"]},"curve3d":{"type":"object","properties":{"degree":{"type":"integer","minimum":0},"rational":{"type":"boolean"},"periodic":{"type":"boolean"},"domainStart":{"type":"number"},"domainEnd":{"type":"number"},"controlPointCount":{"type":"integer","minimum":0},"controlPoints":{"type":"array","description":"Packed xyz control points; stride 3.","items":{"type":"number"}},"knots":{"$ref":"#/$defs/numberArray"},"weights":{"$ref":"#/$defs/numberArray"}},"additionalProperties":false,"required":["degree","rational","periodic","domainStart","domainEnd","controlPointCount","controlPoints","knots","weights"]},"surface":{"type":"object","properties":{"degreeU":{"type":"integer","minimum":0},"degreeV":{"type":"integer","minimum":0},"rational":{"type":"boolean"},"periodicU":{"type":"boolean"},"periodicV":{"type":"boolean"},"controlPointUCount":{"type":"integer","minimum":0},"controlPointVCount":{"type":"integer","minimum":0},"controlPoints":{"type":"array","description":"Packed xyz control grid; stride 3, u outer and v inner.","items":{"type":"number"}},"knotsU":{"$ref":"#/$defs/numberArray"},"knotsV":{"$ref":"#/$defs/numberArray"},"weightUCount":{"type":"integer","minimum":0},"weightVCount":{"type":"integer","minimum":0},"weights":{"type":"array","description":"Packed row-major weight grid; u outer and v inner.","items":{"type":"number"}}},"additionalProperties":false,"required":["degreeU","degreeV","rational","periodicU","periodicV","controlPointUCount","controlPointVCount","controlPoints","knotsU","knotsV","weightUCount","weightVCount","weights"]}}}
)json";
constexpr const char kPointCloudInput[] = R"json({"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elementIndex":{"type":"integer","minimum":1},"coordinateSystem":{"type":"string","enum":["world","local"]}},"additionalProperties":false,"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kPointCloudOutput[] = R"json({"type":"object","properties":{"coordinateSystem":{"type":"string","enum":["world","local"]},"count":{"type":"integer","minimum":0},"pointClouds":{"type":"array","items":{"type":"object","properties":{"elementIndex":{"type":"integer","minimum":1},"elementId":{"$ref":"#/$defs/elementId"},"cloudIndex":{"type":"integer","minimum":1},"bounds":{"$ref":"#/$defs/box"},"transform":{"type":"array","description":"Packed row-major point-cloud data-to-target 4x4 matrix; 16 numbers.","minItems":16,"maxItems":16,"items":{"type":"number"}}},"additionalProperties":false,"required":["elementIndex","elementId","cloudIndex","bounds","transform"]}}},"additionalProperties":false,"required":["coordinateSystem","count","pointClouds"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"box":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"zMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"zMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","zMin","xMax","yMax","zMax"]}}})json";

const NativeCommandRegistration kNurbsCommandRegistrations[] = {
    { "GetNurbsBody",   &MakeRegisteredNativeCommand<GetNurbsBodyCommand>,   false, kNurbsInput,      kNurbsOutput },
    { "GetPointClouds", &MakeRegisteredNativeCommand<GetPointCloudsCommand>, false, kPointCloudInput, kPointCloudOutput },
};

}   // namespace

NativeCommandRegistrations GetNurbsCommandRegistrations ()
{
    return MakeRegistrationView (kNurbsCommandRegistrations);
}

} // namespace geomsrv

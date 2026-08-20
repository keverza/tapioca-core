#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/Component3DCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"

namespace geomsrv {

namespace {

bool ReadElementId (const GS::ObjectState& record, GS::UniString& guid)
{
    GS::ObjectState elementId;
    return record.Get ("elementId", elementId) && elementId.Get ("guid", guid) && !guid.IsEmpty ();
}

void AddElementId (const GS::UniString& guid, GS::ObjectState& record)
{
    GS::ObjectState elementId;
    elementId.Add ("guid", guid);
    record.Add ("elementId", elementId);
}

// ===========================================================================
// E24 — the OTHER geometry API: the C ModelAccess component database.
//
// Archicad exposes the same 3D data twice. The ModelerAPI (C++) path is the one
// EvP already used; this is `ACAPI_ModelAccess_GetComponent`, a flat indexed
// database of API_Component3D records — BODY, PGON, PEDG, EDGE, VERT, VECT,
// UMAT, LGHT — and it is NOT redundant with the C++ path:
//
//   * `ACAPI_ModelAccess_Get3DInfo` converts an element to 3D ON DEMAND,
//     "independently of any existing 3D window data" (its own header). It works
//     when the 3D window has never been opened, and the DevKit states it is
//     "currently the only way to obtain information on holes in walls" — you
//     look for polygons whose normal is horizontal and perpendicular to the
//     wall's reference line. The ModelerAPI path cannot answer that.
//   * The body's `status` bits (closed / curved / multi-material) and its
//     `tranmat` are on this side.
//   * `elemIndex`/`bodyIndex` from a body here are the exact inputs
//     `ACAPI_ModelAccess_GetTextureCoord` and the cutting-plane calls demand.
//
// ⚠️ THE READ IS STATEFUL, AND THAT IS THE WHOLE TRAP. `GetComponent` answers
// about the ACTIVE BODY: you fetch a BODY record first, and only then are its
// PGON/PEDG/EDGE/VERT/VECT sub-components reachable — indices 1..nPgon and so
// on, which are body-local. Ask for a polygon without having fetched its body
// and you get APIERR_REFUSEDCMD ("no active body") or, worse, the previous
// body's polygon. Every command below therefore fetches the body itself and
// walks its own sub-components in one call; there is deliberately no
// "EvP.GetPolygon3D" that a caller could invoke out of order.
//
// ⚠️ SUB-COMPONENT INDICES ARE 1-BASED (the header: "use indices from 1 to
// nPgon, nPedg, nEdge, nVert, or nVect"), while `elemIdx`/`bodyIdx` fed to the
// texture-coordinate and cutting-plane calls are 0-BASED. Both conventions are
// preserved exactly as the API states them, and each is named where it appears.
//
// Reads. Gate, no undo scope.
// ===========================================================================

GS::ObjectState BoxToOs (const API_Box3D& box)
{
    GS::ObjectState os;
    os.Add ("xMin", box.xMin);   os.Add ("yMin", box.yMin);   os.Add ("zMin", box.zMin);
    os.Add ("xMax", box.xMax);   os.Add ("yMax", box.yMax);   os.Add ("zMax", box.zMax);
    return os;
}


// API_RGBColor is 0..1 doubles, same scale as ModelerAPI::Color.
GS::ObjectState RgbToOs (const API_RGBColor& rgb)
{
    GS::ObjectState os;
    os.Add ("red", rgb.f_red);
    os.Add ("green", rgb.f_green);
    os.Add ("blue", rgb.f_blue);
    return os;
}


// ---------------------------------------------------------------------------
// EvP.Get3DComponentCounts {}
//   -> { ok, bodyCount, lightCount, materialCount }
//
// How big the active 3D database is. Only these three types can be counted —
// `ACAPI_ModelAccess_GetNum` accepts BodyID, LghtID and UmatID and nothing else,
// because polygons/edges/vertices are counted PER BODY (nPgon, nEdge, nVert on
// the body record), not globally. Asking for those is a documented error, so
// they are simply not offered here.
// ---------------------------------------------------------------------------
class Get3DComponentCountsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "Get3DComponentCounts"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState&, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        Int32 bodies = 0, lights = 0, materials = 0;

        const GSErrCode err = ACAPI_ModelAccess_GetNum (API_BodyID, &bodies);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_ModelAccess_GetNum", err,
                "counting bodies in the active 3D sight — the 3D model may not be available "
                "in this context"));
        }
        ACAPI_ModelAccess_GetNum (API_LghtID, &lights);
        ACAPI_ModelAccess_GetNum (API_UmatID, &materials);

        os.Add ("bodyCount", (GS::Int32) bodies);
        os.Add ("lightCount", (GS::Int32) lights);
        os.Add ("materialCount", (GS::Int32) materials);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetElement3DInfo { guids:[…] }
//   -> { ok, count, elements:[{ guid, found, error?, firstBody, lastBody,
//        bodyCount, firstLight, lastLight, lightCount, bounds }] }
//
// Which BODY indices belong to an element — the bridge from a guid, which is
// what every other EvP command speaks, to the body index this domain needs.
//
// ⚠️ This CONVERTS THE ELEMENT TO 3D on demand and does not need the 3D window
// to have been opened. The consequence, straight from the header: the result
// "doesn't contain perspective cuts and 3D cut planes". It is the element's own
// solid, not what you see on screen — which is usually what you want for a
// measurement and never what you want for a render.
//
// Rows stay PARALLEL to the input: an element with no 3D representation reports
// found:false with the reason, rather than shifting every later row.
// ---------------------------------------------------------------------------
class GetElement3DInfoCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetElement3DInfo"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> requestedElements;
        if (!params.Get ("elements", requestedElements) || requestedElements.IsEmpty ()) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("need elements=[{elementId:{guid}}]", "Tapioca.GetElement3DInfo"));
        }

        GS::Array<GS::ObjectState> elements;
        for (const GS::ObjectState& requested : requestedElements) {
            GS::UniString guidString;
            if (!ReadElementId (requested, guidString)) {
                return NativeCommandResult::Failure (
                    EVP_FAIL ("every element needs elementId.guid", "Tapioca.GetElement3DInfo"));
            }
            GS::ObjectState record;
            AddElementId (guidString, record);

            API_Element element = {};
            element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
            if (ACAPI_Element_Get (&element) != NoError) {
                record.Add ("found", false);
                record.Add ("error", GS::UniString ("no such element"));
                elements.Push (record);
                continue;
            }

            API_ElemInfo3D info = {};
            const GSErrCode err = ACAPI_ModelAccess_Get3DInfo (element.header, &info);
            if (err != NoError) {
                record.Add ("found", false);
                // Not reported through EVP_ACAPI_FAIL: "this element has no 3D"
                // is a normal answer for a line or a dimension, and filling the
                // error log with it on a whole-selection sweep would bury the
                // failures that matter.
                record.Add ("error", GS::UniString (
                    "no 3D representation (or the layer is hidden/locked/not yours)"));
                elements.Push (record);
                continue;
            }

            record.Add ("found", true);
            record.Add ("firstBody", (GS::Int32) info.fbody);
            record.Add ("lastBody", (GS::Int32) info.lbody);
            record.Add ("bodyCount", (GS::Int32) (info.lbody - info.fbody + 1));
            record.Add ("firstLight", (GS::Int32) info.flight);
            record.Add ("lastLight", (GS::Int32) info.llight);
            record.Add ("lightCount", (GS::Int32) (info.llight - info.flight + 1));
            record.Add ("bounds", BoxToOs (info.bounds));
            elements.Push (record);
        }

        os.Add ("count", (GS::Int32) elements.GetSize ());
        os.Add ("elements", elements);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetBodyComponents { body:N, include?:["vertices","vectors","polygons",
//                                           "polyEdges","edges","material","all"] }
//   -> { ok, bodyIndex, body:{…}, vertices?, vectors?, polygons?, polyEdges?,
//        edges?, materials? }
//
// ONE body's complete C-API record and its sub-components, fetched in the order
// the API requires (body first — see the stateful warning at the top of this
// file).
//
// THE POLYGON→CONTOUR WALK, which is the whole reason this shape exists.
// A polygon does not hold its vertices. It holds `fpedg`..`lpedg`, a RANGE into
// the body's PEDG array. Each PEDG holds a SIGNED edge index:
//   * positive  -> use the edge as-is (vert1 → vert2)
//   * negative  -> use edge |pedg| REVERSED (vert2 → vert1)
//   * ZERO      -> end of a contour; everything after it is a HOLE
// and each EDGE holds its two vertex indices and the (up to two) polygons that
// share it. So `polygons.firstPolyEdge/lastPolyEdge` + `polyEdges.edge` +
// `edges.vertex1/vertex2` is the complete boundary, holes included. All three
// arrays are returned raw; reassembling them in Python is a dozen lines and
// doing it in C++ would throw away the hole markers.
//
// `body.status` bits are decoded into named booleans (closed, curved,
// multiMaterial, multiColor, multiTexture) rather than left as a number.
// ---------------------------------------------------------------------------
class GetBodyComponentsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetBodyComponents"; }

    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Int32 bodyIndex = 0;
        if (!params.Get ("body", bodyIndex)) {
            return NativeCommandResult::Failure (EVP_FAIL (
                "need body=N (from EvP.GetElement3DInfo's firstBody..lastBody)",
                "EvP.GetBodyComponents"));
        }

        // Fetch the BODY first: this is what makes its sub-components reachable.
        API_Component3D component = {};
        component.header.typeID = API_BodyID;
        component.header.index  = (Int32) bodyIndex;
        const GSErrCode err = ACAPI_ModelAccess_GetComponent (&component);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_ModelAccess_GetComponent", err,
                GS::UniString::Printf ("fetching body %d — is it in range? (EvP.Get3DComponentCounts)",
                                       (int) bodyIndex)));
        }
        const API_BodyType body = component.body;

        os.Add ("bodyIndex", bodyIndex);
        os.Add ("body", DescribeBody (body));

        if (Wants (params, "vertices", true))  AddVertices (os, body);
        if (Wants (params, "vectors", false))  AddVectors (os, body);
        if (Wants (params, "polygons", true))  AddPolygons (os, body);
        if (Wants (params, "polyEdges", true)) AddPolyEdges (os, body);
        if (Wants (params, "edges", false))    AddEdges (os, body);
        if (Wants (params, "material", false)) AddMaterials (os, body);
        return os;
    }

private:
    static bool Wants (const GS::ObjectState& params, const char* section, bool byDefault)
    {
        GS::Array<GS::UniString> include;
        if (!params.Get ("include", include))
            return byDefault;
        for (const GS::UniString& s : include)
            if (s == section || s == "all")
                return true;
        return false;
    }

    static GS::ObjectState DescribeBody (const API_BodyType& body)
    {
        GS::ObjectState b;
        b.Add ("index", (GS::Int32) body.head.index);
        // These two are the OUTPUT fields the header calls out specifically: they
        // are what ACAPI_ModelAccess_GetTextureCoord and the cutting-plane calls
        // want — MINUS ONE, because those are 0-based and these are plus-one.
        b.Add ("elemIndexPlus1", (GS::Int32) body.head.elemIndex);
        b.Add ("bodyIndexPlus1", (GS::Int32) body.head.bodyIndex);
        GS::ObjectState parentElement;
        AddElementId (GS::UniString (APIGuidToString (body.parent.guid).ToCStr ()), parentElement);
        b.Add ("parentElement", parentElement);
        b.Add ("parentType", (GS::Int32) body.parent.type.typeID);

        b.Add ("status", (GS::Int32) body.status);
        b.Add ("isClosed", (body.status & APIBody_Closed) != 0);
        b.Add ("isCurved", (body.status & APIBody_Curved) != 0);
        b.Add ("multiMaterial", (body.status & APIBody_MulMater) != 0);
        b.Add ("multiColor", (body.status & APIBody_MulColor) != 0);
        b.Add ("multiTexture", (body.status & APIBody_MulRtxt) != 0);

        b.Add ("color", (GS::Int32) body.color);
        b.Add ("materialIndex", (GS::Int32) body.iumat);
        b.Add ("polygonCount", (GS::Int32) body.nPgon);
        b.Add ("polyEdgeCount", (GS::Int32) body.nPedg);
        b.Add ("edgeCount", (GS::Int32) body.nEdge);
        b.Add ("vertexCount", (GS::Int32) body.nVert);
        b.Add ("vectorCount", (GS::Int32) body.nVect);

        // The bounding box here is FLOAT in the API, unlike everything else in
        // this file — widened, not "converted".
        GS::ObjectState bounds;
        bounds.Add ("xMin", (double) body.xmin);   bounds.Add ("yMin", (double) body.ymin);
        bounds.Add ("zMin", (double) body.zmin);   bounds.Add ("xMax", (double) body.xmax);
        bounds.Add ("yMax", (double) body.ymax);   bounds.Add ("zMax", (double) body.zmax);
        b.Add ("bounds", bounds);

        GS::Array<double> tranmat;
        for (int i = 0; i < 12; ++i)
            tranmat.Push (body.tranmat.tmx[i]);
        b.Add ("transform", tranmat);       // 3x4 row-major, API_Tranmat's own order
        return b;
    }

    // Sub-component fetch. `index` is 1-based within the ACTIVE body.
    static bool Sub (API_3DTypeID type, Int32 index, API_Component3D& out)
    {
        out = {};
        out.header.typeID = type;
        out.header.index  = index;
        return ACAPI_ModelAccess_GetComponent (&out) == NoError;
    }

    static void AddVertices (GS::ObjectState& os, const API_BodyType& body)
    {
        GS::Array<double> coords;
        for (Int32 i = 1; i <= body.nVert; ++i) {
            API_Component3D c;
            if (!Sub (API_VertID, i, c))
                break;
            coords.Push (c.vert.x);
            coords.Push (c.vert.y);
            coords.Push (c.vert.z);
        }
        os.Add ("vertices", coords);        // flat [x,y,z, …]; vertex i is at (i-1)*3
    }

    static void AddVectors (GS::ObjectState& os, const API_BodyType& body)
    {
        GS::Array<double> vectors;
        for (Int32 i = 1; i <= body.nVect; ++i) {
            API_Component3D c;
            if (!Sub (API_VectID, i, c))
                break;
            vectors.Push (c.vect.x);
            vectors.Push (c.vect.y);
            vectors.Push (c.vect.z);
        }
        os.Add ("vectors", vectors);        // polygon `normalIndex` is SIGNED into this
    }

    static void AddPolygons (GS::ObjectState& os, const API_BodyType& body)
    {
        GS::Array<GS::Int32> materialIndex, normalIndex, firstPolyEdge, lastPolyEdge, status;
        GS::Array<bool> invisible, curved, concave, hasHoles, holesConvex;
        for (Int32 i = 1; i <= body.nPgon; ++i) {
            API_Component3D c;
            if (!Sub (API_PgonID, i, c))
                break;
            materialIndex.Push ((GS::Int32) c.pgon.iumat);
            normalIndex.Push ((GS::Int32) c.pgon.ivect);      // signed: negative = flip
            firstPolyEdge.Push ((GS::Int32) c.pgon.fpedg);
            lastPolyEdge.Push ((GS::Int32) c.pgon.lpedg);
            status.Push ((GS::Int32) c.pgon.status);
            invisible.Push ((c.pgon.status & APIPgon_Invis) != 0);
            curved.Push ((c.pgon.status & APIPgon_Curved) != 0);
            concave.Push ((c.pgon.status & APIPgon_Concav) != 0);
            hasHoles.Push ((c.pgon.status & APIPgon_PHole) != 0);
            holesConvex.Push ((c.pgon.status & APIPgon_HolesCnv) != 0);
        }

        GS::ObjectState p;
        p.Add ("count", (GS::Int32) materialIndex.GetSize ());
        p.Add ("materialIndex", materialIndex);
        p.Add ("normalIndex", normalIndex);
        p.Add ("firstPolyEdge", firstPolyEdge);   // range into the polyEdges arrays
        p.Add ("lastPolyEdge", lastPolyEdge);
        p.Add ("status", status);
        p.Add ("invisible", invisible);
        p.Add ("curved", curved);
        p.Add ("concave", concave);
        p.Add ("hasHoles", hasHoles);
        p.Add ("holesConvex", holesConvex);
        os.Add ("polygons", p);
    }

    static void AddPolyEdges (GS::ObjectState& os, const API_BodyType& body)
    {
        GS::Array<GS::Int32> pedg;
        for (Int32 i = 1; i <= body.nPedg; ++i) {
            API_Component3D c;
            if (!Sub (API_PedgID, i, c))
                break;
            pedg.Push ((GS::Int32) c.pedg.pedg);
        }
        GS::ObjectState pe;
        pe.Add ("count", (GS::Int32) pedg.GetSize ());
        // Signed edge index. 0 = CONTOUR BREAK (a hole starts after it).
        pe.Add ("edge", pedg);
        os.Add ("polyEdges", pe);
    }

    static void AddEdges (GS::ObjectState& os, const API_BodyType& body)
    {
        GS::Array<GS::Int32> v1, v2, p1, p2, colors, status;
        GS::Array<bool> invisible, curved;
        for (Int32 i = 1; i <= body.nEdge; ++i) {
            API_Component3D c;
            if (!Sub (API_EdgeID, i, c))
                break;
            v1.Push ((GS::Int32) c.edge.vert1);
            v2.Push ((GS::Int32) c.edge.vert2);
            p1.Push ((GS::Int32) c.edge.pgon1);      // -1 = no polygon on that side
            p2.Push ((GS::Int32) c.edge.pgon2);
            colors.Push ((GS::Int32) c.edge.color);
            status.Push ((GS::Int32) c.edge.status);
            invisible.Push ((c.edge.status & APIEdge_Invis) != 0);
            curved.Push ((c.edge.status & APIEdge_Curved) != 0);
        }

        GS::ObjectState e;
        e.Add ("count", (GS::Int32) v1.GetSize ());
        e.Add ("vertex1", v1);
        e.Add ("vertex2", v2);
        e.Add ("polygon1", p1);
        e.Add ("polygon2", p2);
        e.Add ("color", colors);
        e.Add ("status", status);
        e.Add ("invisible", invisible);
        e.Add ("curved", curved);
        os.Add ("edges", e);
    }

    // The 3D database's own surface pool. Unlike the sub-components above these
    // are GLOBAL (indexed 1..materialCount from Get3DComponentCounts), not
    // body-local — but a body's polygons refer into it, so it belongs here.
    static void AddMaterials (GS::ObjectState& os, const API_BodyType&)
    {
        Int32 count = 0;
        ACAPI_ModelAccess_GetNum (API_UmatID, &count);

        GS::Array<GS::ObjectState> materials;
        for (Int32 i = 1; i <= count; ++i) {
            API_Component3D c;
            if (!Sub (API_UmatID, i, c))
                break;
            GS::ObjectState m;
            m.Add ("index", (GS::Int32) c.umat.head.index);
            // index == 0 means the surface came from a GDL script rather than the
            // global attribute pool — there is no attribute to look up.
            m.Add ("fromGDL", c.umat.head.index == 0);
            m.Add ("name", GS::UniString (c.umat.mater.head.name));
            m.Add ("attributeIndex", GS::UniString (c.umat.mater.head.index.ToUniString ()));
            m.Add ("materialType", (GS::Int32) c.umat.mater.mtype);
            // API_MaterialType is PERCENTAGES (short 0..100), not the 0..1
            // doubles ModelerAPI::Material reports. Same surfaces, two scales —
            // passed through in the API's own units rather than silently
            // normalised into looking like the C++ path's numbers.
            m.Add ("ambientPc", (GS::Int32) c.umat.mater.ambientPc);
            m.Add ("diffusePc", (GS::Int32) c.umat.mater.diffusePc);
            m.Add ("specularPc", (GS::Int32) c.umat.mater.specularPc);
            m.Add ("transparencyPc", (GS::Int32) c.umat.mater.transpPc);
            m.Add ("shine", (GS::Int32) c.umat.mater.shine);
            m.Add ("transparencyAttenuation", (GS::Int32) c.umat.mater.transpAtt);
            m.Add ("emissionAttenuation", (GS::Int32) c.umat.mater.emissionAtt);
            m.Add ("surfaceRGB", RgbToOs (c.umat.mater.surfaceRGB));
            m.Add ("specularRGB", RgbToOs (c.umat.mater.specularRGB));
            m.Add ("emissionRGB", RgbToOs (c.umat.mater.emissionRGB));
            m.Add ("fillIndex", GS::UniString (c.umat.mater.ifill.ToUniString ()));
            m.Add ("fillColor", (GS::Int32) c.umat.mater.fillCol);
            materials.Push (m);
        }
        os.Add ("materials", materials);
    }
};


// ---------------------------------------------------------------------------
// EvP.DecomposePolygon { polygon:N }
//   -> { ok, polygonIndex, subPolygonCount, vertexCounts, vertexIndices }
//
// One 3D-database polygon split into CONVEX sub-polygons —
// `ACAPI_ModelAccess_DecomposePgon`, the C-API twin of
// ModelerAPI::Polygon::GetConvexPolygon. It generates no new points: the indices
// it returns are the body's own VERT indices, so a concave face with holes comes
// back as triangulable pieces that still share the original vertices.
//
// ⚠️ The polygon must belong to the ACTIVE body (fetch it with
// EvP.GetBodyComponents first) — same statefulness as everything else here.
//
// The API hands back a handle laid out as
//   [-n] [-m1] i1 … i(m1) [-m2] j1 … j(m2) …
// (negatives are counts, positives are vertex indices). That is unpacked here
// into the count/index pair every other EvP bulk read uses, and the handle is
// disposed — the header's "do not forget to dispose" is a leak the caller must
// never be handed responsibility for.
// ---------------------------------------------------------------------------
class DecomposePolygonCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "DecomposePolygon"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Int32 polygonIndex = 0;
        if (!params.Get ("polygon", polygonIndex)) {
            return NativeCommandResult::Failure (EVP_FAIL (
                "need polygon=N (1-based, within the active body)",
                "EvP.DecomposePolygon"));
        }

        Int32** cpoly = nullptr;
        const GSErrCode err = ACAPI_ModelAccess_DecomposePgon ((Int32) polygonIndex, &cpoly);

        // A REAL ACAPI failure. Only this branch is an error.
        if (err != NoError) {
            if (cpoly != nullptr)
                BMKillHandle (reinterpret_cast<GSHandle*> (&cpoly));
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_ModelAccess_DecomposePgon", err,
                GS::UniString::Printf ("polygon %d — has its body been fetched with "
                                       "EvP.GetBodyComponents first?", (int) polygonIndex)));
        }

        // NoError + a null handle. This is NOT a failure and must not be reported as
        // one: the old guard folded it in with the branch above and emitted the
        // self-contradictory "failed: NoError (0) - The operation succeeded."
        // ⚠️ What it MEANS is unverified — the header documents only that the count
        // "can be 0 on error, or if the polygon is degenerate" and says nothing about a
        // null handle. Observed live on a polygon that needed no decomposition. So the
        // response says exactly what happened and claims nothing more.
        if (cpoly == nullptr) {
            os.Add ("polygonIndex", polygonIndex);
            os.Add ("subPolygonCount", (GS::Int32) 0);
            os.Add ("declaredSubPolygonCount", (GS::Int32) 0);
            os.Add ("vertexCounts", GS::Array<GS::Int32> ());
            os.Add ("vertexIndices", GS::Array<GS::Int32> ());
            os.Add ("note", GS::UniString ("ACAPI_ModelAccess_DecomposePgon succeeded but "
                                           "returned no handle — no decomposition for this "
                                           "polygon (already convex, or degenerate)"));
            return os;
        }

        // How many Int32 the handle actually holds — the walk below is driven by
        // the embedded counts, but a malformed handle must not run off the end.
        const GSSize slots = BMGetHandleSize (reinterpret_cast<GSHandle> (cpoly)) /
                             (GSSize) sizeof (Int32);
        const Int32* data = *cpoly;

        GS::Array<GS::Int32> vertexCounts, vertexIndices;
        GS::Int32 subPolygonCount = 0;
        if (slots > 0) {
            subPolygonCount = (GS::Int32) -data[0];     // [-n]
            GSSize cursor = 1;
            for (GS::Int32 p = 0; p < subPolygonCount && cursor < slots; ++p) {
                const Int32 m = -data[cursor];          // [-m]
                ++cursor;
                if (m <= 0 || cursor + m > slots)
                    break;                              // malformed: stop, keep what we have
                vertexCounts.Push ((GS::Int32) m);
                for (Int32 k = 0; k < m; ++k)
                    vertexIndices.Push ((GS::Int32) data[cursor + k]);
                cursor += m;
            }
        }
        BMKillHandle (reinterpret_cast<GSHandle*> (&cpoly));

        os.Add ("polygonIndex", polygonIndex);
        os.Add ("subPolygonCount", (GS::Int32) vertexCounts.GetSize ());
        os.Add ("declaredSubPolygonCount", subPolygonCount);   // differs only if malformed
        os.Add ("vertexCounts", vertexCounts);
        os.Add ("vertexIndices", vertexIndices);               // flat, split by vertexCounts
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetTextureCoordAtPoint { elemIdx:N, bodyIdx:N, polygon:N, points:[x,y,z,…] }
//   -> { ok, count, u, v }
//
// The C-API route to a UV coordinate: `ACAPI_ModelAccess_GetTextureCoord`.
//
// ⚠️ ITS INPUTS DIFFER FROM THE C++ ROUTE'S IN BOTH INDEX BASE AND COORDINATE
// FRAME, and mixing them up produces plausible wrong numbers rather than an
// error:
//   * `elemIdx`/`bodyIdx` are 0-BASED — they are `elemIndexPlus1 - 1` and
//     `bodyIndexPlus1 - 1` from EvP.GetBodyComponents' body record, which is
//     exactly why those two fields are named that way there;
//   * `points` are in LOCAL coordinates, not world. EvP.GetTextureCoordinates
//     (the ModelerAPI route) takes WORLD points. Two calls, same question,
//     opposite conventions — read the one you are actually using.
// ---------------------------------------------------------------------------
class GetTextureCoordAtPointCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetTextureCoordAtPoint"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Int32 elemIdx = -1, bodyIdx = -1, polygonIndex = -1;
        if (!params.Get ("elemIdx", elemIdx) || !params.Get ("bodyIdx", bodyIdx) ||
            !params.Get ("polygon", polygonIndex)) {
            return NativeCommandResult::Failure (EVP_FAIL (
                "need elemIdx, bodyIdx (both 0-based) and polygon",
                "take elemIdx/bodyIdx from EvP.GetBodyComponents' elemIndexPlus1/bodyIndexPlus1, MINUS 1"));
        }

        GS::Array<double> points;
        if (!params.Get ("points", points) || points.GetSize () < 3 || points.GetSize () % 3 != 0) {
            return NativeCommandResult::Failure (EVP_FAIL (
                "need points=[x,y,z, …] (a multiple of 3, LOCAL coordinates)",
                "EvP.GetTextureCoordAtPoint"));
        }

        GS::Array<double> us, vs;
        for (UIndex i = 0; i + 2 < points.GetSize (); i += 3) {
            API_TexCoordPars pars = {};
            pars.elemIdx      = (UInt32) elemIdx;
            pars.bodyIdx      = (UInt32) bodyIdx;
            pars.pgonIndex    = (UInt32) polygonIndex;
            pars.surfacePoint = { points[i], points[i + 1], points[i + 2] };

            API_UVCoord uv = {};
            const GSErrCode err = ACAPI_ModelAccess_GetTextureCoord (&pars, &uv);
            if (err != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                    "ACAPI_ModelAccess_GetTextureCoord", err,
                    GS::UniString::Printf ("elem %d body %d polygon %d, point %u of %u",
                                           (int) elemIdx, (int) bodyIdx, (int) polygonIndex,
                                           (unsigned) (i / 3 + 1), (unsigned) (points.GetSize () / 3))));
            }
            us.Push (uv.u);
            vs.Push (uv.v);
        }

        os.Add ("count", (GS::Int32) us.GetSize ());
        os.Add ("u", us);
        os.Add ("v", vs);
        return os;
    }
};

constexpr const char kEmptyInput[] = R"json({"type":"object","properties":{},"additionalProperties":false})json";
constexpr const char kCountsOutput[] = R"json({"type":"object","properties":{"bodyCount":{"type":"integer","minimum":0},"lightCount":{"type":"integer","minimum":0},"materialCount":{"type":"integer","minimum":0}},"additionalProperties":false,"required":["bodyCount","lightCount","materialCount"]})json";
constexpr const char kElementInfoInput[] = R"json({"type":"object","properties":{"elements":{"type":"array","minItems":1,"items":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["elements"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kElementInfoOutput[] = R"json({"type":"object","properties":{"count":{"type":"integer","minimum":0},"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"found":{"type":"boolean"},"error":{"type":"string"},"firstBody":{"type":"integer"},"lastBody":{"type":"integer"},"bodyCount":{"type":"integer","minimum":0},"firstLight":{"type":"integer"},"lastLight":{"type":"integer"},"lightCount":{"type":"integer","minimum":0},"bounds":{"$ref":"#/$defs/box"}},"additionalProperties":false,"required":["elementId","found"]}}},"additionalProperties":false,"required":["count","elements"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"box":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"zMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"zMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","zMin","xMax","yMax","zMax"]}}})json";
constexpr const char kBodyInput[] = R"json({"type":"object","properties":{"body":{"type":"integer","minimum":1},"include":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["vertices","vectors","polygons","polyEdges","edges","material","all"]}}},"additionalProperties":false,"required":["body"]})json";
constexpr const char kBodyOutput[] = R"json(
{"type":"object","properties":{"bodyIndex":{"type":"integer","minimum":1},"body":{"$ref":"#/$defs/body"},"vertices":{"type":"array","description":"Packed xyz vertices; stride 3 in 1-based body-local vertex order.","items":{"type":"number"}},"vectors":{"type":"array","description":"Packed xyz vector pool; stride 3; signed polygon normalIndex addresses this 1-based pool.","items":{"type":"number"}},"polygons":{"$ref":"#/$defs/polygons"},"polyEdges":{"$ref":"#/$defs/polyEdges"},"edges":{"$ref":"#/$defs/edges"},"materials":{"type":"array","items":{"$ref":"#/$defs/material"}}},"additionalProperties":false,"required":["bodyIndex","body"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"element":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]},"box":{"type":"object","properties":{"xMin":{"type":"number"},"yMin":{"type":"number"},"zMin":{"type":"number"},"xMax":{"type":"number"},"yMax":{"type":"number"},"zMax":{"type":"number"}},"additionalProperties":false,"required":["xMin","yMin","zMin","xMax","yMax","zMax"]},"rgb":{"type":"object","properties":{"red":{"type":"number"},"green":{"type":"number"},"blue":{"type":"number"}},"additionalProperties":false,"required":["red","green","blue"]},"body":{"type":"object","properties":{"index":{"type":"integer"},"elemIndexPlus1":{"type":"integer"},"bodyIndexPlus1":{"type":"integer"},"parentElement":{"$ref":"#/$defs/element"},"parentType":{"type":"integer"},"status":{"type":"integer"},"isClosed":{"type":"boolean"},"isCurved":{"type":"boolean"},"multiMaterial":{"type":"boolean"},"multiColor":{"type":"boolean"},"multiTexture":{"type":"boolean"},"color":{"type":"integer"},"materialIndex":{"type":"integer"},"polygonCount":{"type":"integer","minimum":0},"polyEdgeCount":{"type":"integer","minimum":0},"edgeCount":{"type":"integer","minimum":0},"vertexCount":{"type":"integer","minimum":0},"vectorCount":{"type":"integer","minimum":0},"bounds":{"$ref":"#/$defs/box"},"transform":{"type":"array","description":"Packed row-major API_Tranmat 3x4 affine matrix; 12 numbers.","minItems":12,"maxItems":12,"items":{"type":"number"}}},"additionalProperties":false,"required":["index","elemIndexPlus1","bodyIndexPlus1","parentElement","parentType","status","isClosed","isCurved","multiMaterial","multiColor","multiTexture","color","materialIndex","polygonCount","polyEdgeCount","edgeCount","vertexCount","vectorCount","bounds","transform"]},"intArray":{"type":"array","items":{"type":"integer"}},"boolArray":{"type":"array","items":{"type":"boolean"}},"polygons":{"type":"object","description":"Packed polygon structure-of-arrays in 1-based body-local order; firstPolyEdge/lastPolyEdge address polyEdges.","properties":{"count":{"type":"integer","minimum":0},"materialIndex":{"$ref":"#/$defs/intArray"},"normalIndex":{"$ref":"#/$defs/intArray"},"firstPolyEdge":{"$ref":"#/$defs/intArray"},"lastPolyEdge":{"$ref":"#/$defs/intArray"},"status":{"$ref":"#/$defs/intArray"},"invisible":{"$ref":"#/$defs/boolArray"},"curved":{"$ref":"#/$defs/boolArray"},"concave":{"$ref":"#/$defs/boolArray"},"hasHoles":{"$ref":"#/$defs/boolArray"},"holesConvex":{"$ref":"#/$defs/boolArray"}},"additionalProperties":false,"required":["count","materialIndex","normalIndex","firstPolyEdge","lastPolyEdge","status","invisible","curved","concave","hasHoles","holesConvex"]},"polyEdges":{"type":"object","description":"Packed signed edge indices; zero is a contour break and all other values address the 1-based edge pool.","properties":{"count":{"type":"integer","minimum":0},"edge":{"$ref":"#/$defs/intArray"}},"additionalProperties":false,"required":["count","edge"]},"edges":{"type":"object","description":"Packed edge structure-of-arrays in 1-based body-local order.","properties":{"count":{"type":"integer","minimum":0},"vertex1":{"$ref":"#/$defs/intArray"},"vertex2":{"$ref":"#/$defs/intArray"},"polygon1":{"$ref":"#/$defs/intArray"},"polygon2":{"$ref":"#/$defs/intArray"},"color":{"$ref":"#/$defs/intArray"},"status":{"$ref":"#/$defs/intArray"},"invisible":{"$ref":"#/$defs/boolArray"},"curved":{"$ref":"#/$defs/boolArray"}},"additionalProperties":false,"required":["count","vertex1","vertex2","polygon1","polygon2","color","status","invisible","curved"]},"material":{"type":"object","properties":{"index":{"type":"integer"},"fromGDL":{"type":"boolean"},"name":{"type":"string"},"attributeIndex":{"type":"string"},"materialType":{"type":"integer"},"ambientPc":{"type":"integer"},"diffusePc":{"type":"integer"},"specularPc":{"type":"integer"},"transparencyPc":{"type":"integer"},"shine":{"type":"integer"},"transparencyAttenuation":{"type":"integer"},"emissionAttenuation":{"type":"integer"},"surfaceRGB":{"$ref":"#/$defs/rgb"},"specularRGB":{"$ref":"#/$defs/rgb"},"emissionRGB":{"$ref":"#/$defs/rgb"},"fillIndex":{"type":"string"},"fillColor":{"type":"integer"}},"additionalProperties":false,"required":["index","fromGDL","name","attributeIndex","materialType","ambientPc","diffusePc","specularPc","transparencyPc","shine","transparencyAttenuation","emissionAttenuation","surfaceRGB","specularRGB","emissionRGB","fillIndex","fillColor"]}}}
)json";
constexpr const char kDecomposeInput[] = R"json({"type":"object","properties":{"polygon":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["polygon"]})json";
constexpr const char kDecomposeOutput[] = R"json({"type":"object","properties":{"polygonIndex":{"type":"integer","minimum":1},"subPolygonCount":{"type":"integer","minimum":0},"declaredSubPolygonCount":{"type":"integer","minimum":0},"vertexCounts":{"type":"array","description":"Vertex count per convex sub-polygon; splits vertexIndices.","items":{"type":"integer","minimum":1}},"vertexIndices":{"type":"array","description":"Packed 1-based body-local vertex indices split by vertexCounts.","items":{"type":"integer","minimum":1}},"note":{"type":"string"}},"additionalProperties":false,"required":["polygonIndex","subPolygonCount","declaredSubPolygonCount","vertexCounts","vertexIndices"]})json";
constexpr const char kTextureInput[] = R"json({"type":"object","properties":{"elemIdx":{"type":"integer","minimum":0},"bodyIdx":{"type":"integer","minimum":0},"polygon":{"type":"integer","minimum":1},"points":{"type":"array","minItems":3,"description":"Packed local xyz points; stride 3.","items":{"type":"number"}}},"additionalProperties":false,"required":["elemIdx","bodyIdx","polygon","points"]})json";
constexpr const char kTextureOutput[] = R"json({"type":"object","properties":{"count":{"type":"integer","minimum":0},"u":{"type":"array","description":"Packed U coordinates aligned with v and input points.","items":{"type":"number"}},"v":{"type":"array","description":"Packed V coordinates aligned with u and input points.","items":{"type":"number"}}},"additionalProperties":false,"required":["count","u","v"]})json";

const NativeCommandRegistration kComponent3DCommandRegistrations[] = {
    { "Get3DComponentCounts",   &MakeRegisteredNativeCommand<Get3DComponentCountsCommand>,   false, kEmptyInput,       kCountsOutput },
    { "GetElement3DInfo",       &MakeRegisteredNativeCommand<GetElement3DInfoCommand>,       false, kElementInfoInput, kElementInfoOutput },
    { "GetBodyComponents",      &MakeRegisteredNativeCommand<GetBodyComponentsCommand>,      false, kBodyInput,        kBodyOutput },
    { "DecomposePolygon",       &MakeRegisteredNativeCommand<DecomposePolygonCommand>,       false, kDecomposeInput,   kDecomposeOutput },
    { "GetTextureCoordAtPoint", &MakeRegisteredNativeCommand<GetTextureCoordAtPointCommand>, false, kTextureInput,     kTextureOutput },
};

}   // namespace

NativeCommandRegistrations GetComponent3DCommandRegistrations ()
{
    return MakeRegistrationView (kComponent3DCommandRegistrations);
}

} // namespace geomsrv

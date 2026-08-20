#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CuttingPlaneCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"       // AttributeIndexToName
#include "NativeCommands/ModelAccessUtils.hpp"

#include "Geometry/GeometryExtractor.hpp"     // AcquireCurrentModel

#include <Model.hpp>
#include <ModelElement.hpp>

#include <Plane.hpp>
#include <Poly2DTypes.hpp>
#include <Polygon2D.hpp>
#include <Polygon2DClassDefinition.hpp>

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
// E24 — the reads that need a SEPARATE-COMPONENTS model.
//
// Three questions the tessellated snapshot cannot answer, and they share one
// precondition, which is why they share a file:
//
//   * What BUILDING MATERIAL is this body made of? (Not the surface — the
//     structural material, with its thermal conductivity.) EvP could already
//     read a composite's layers off the ATTRIBUTE, but not which material a
//     given piece of built geometry actually is.
//   * What is the CROSS-SECTION AREA of a solid at a given plane? EvP's own
//     SliceEngine cuts the triangle snapshot and gets outlines; this gets
//     Archicad's own answer, with the area, off the real solid.
//   * WHICH ELEMENTS TOUCH, and over what surface? `EvP.GetCollisions`
//     (TopologyCommands) answers overlap; this answers CONTACT, and hands back
//     the connecting polygons — the input to any thermal-bridge or
//     junction-detailing work.
//
// ⚠️ THE PRECONDITION. `ACAPI_ModelAccess_GetBuildingMaterial` states it "can
// not work properly unless the model of the active sight was generated for
// separate components with ACAPI_ModelAccess_GenerateModelWithSeparateComponents",
// and that function in turn REFUSES to run when the active sight is the 3D
// window's ("APIERR_GENERAL - The active sight is the sight of 3D Window"). So
// the sequence is fixed: create a sight, select it, generate, read, restore the
// previous sight, DELETE the sight. Miss the last two and the user's 3D window
// is left pointing at a scratch sight — that is what SeparateComponentsSight
// below exists to make impossible.
//
// The whole sequence is verified against the DevKit's own ThermalBridge example
// (Examples/ThermalBridge/Src/DumpThermalBridges.cpp), including the two index
// conventions below.
//
// ⚠️ INDEX BASE, AGAIN AND DIFFERENTLY. `ModelerAPI::Model::GetElementIndex`
// returns a 1-BASED index; `ACAPI_CuttingPlane_GetCutPolygonInfo_New` and
// `ACAPI_ModelAccess_GetBuildingMaterial` take a 0-BASED `elemIdx`, and their
// `bodyIdx` is 0-based too while `Element::GetTessellatedBody` is 1-based. The
// example calls this out in a comment and subtracts one; so does everything
// here. Get it wrong and you silently read the NEIGHBOURING element.
//
// Reads. Gate, no undo scope. They do create and destroy a sight, which is not
// a project change and takes no undo step.
// ===========================================================================

// RAII for the scratch sight. Restores and deletes in the destructor, so an
// early return or a thrown GS::Exception cannot leak it.
class SeparateComponentsSight {
public:
    SeparateComponentsSight () = default;

    SeparateComponentsSight (const SeparateComponentsSight&)            = delete;
    SeparateComponentsSight& operator= (const SeparateComponentsSight&) = delete;

    ~SeparateComponentsSight ()
    {
        if (sight == nullptr)
            return;
        void* dummy = nullptr;
        if (selected)
            ACAPI_Sight_SelectSight (previous, &dummy);   // put the user's sight back FIRST
        ACAPI_Sight_DeleteSight (sight);
    }

    bool Open (const GS::Array<API_Guid>& guids, GS::UniString& err)
    {
        GSErrCode e = ACAPI_Sight_CreateSight (&sight);
        if (e != NoError || sight == nullptr) {
            err = EVP_ACAPI_FAIL ("ACAPI_Sight_CreateSight", e, "scratch sight for separate components");
            return false;
        }
        e = ACAPI_Sight_SelectSight (sight, &previous);
        if (e != NoError) {
            err = EVP_ACAPI_FAIL ("ACAPI_Sight_SelectSight", e, "selecting the scratch sight");
            return false;
        }
        selected = true;

        e = ACAPI_ModelAccess_GenerateModelWithSeparateComponents (guids);
        if (e != NoError) {
            err = EVP_ACAPI_FAIL ("ACAPI_ModelAccess_GenerateModelWithSeparateComponents", e,
                GS::UniString::Printf ("%u element(s) — APIERR_BADID means one guid is not in the "
                                       "project", (unsigned) guids.GetSize ()));
            return false;
        }

        e = ACAPI_Sight_GetSelectedSightModel (model);
        if (e != NoError) {
            err = EVP_ACAPI_FAIL ("ACAPI_Sight_GetSelectedSightModel", e,
                                  "reading the separate-components model back");
            return false;
        }
        return true;
    }

    const ModelerAPI::Model& Model () const { return model; }

private:
    void*             sight    = nullptr;
    void*             previous = nullptr;
    bool              selected = false;
    ModelerAPI::Model model;
};


// Every element that has 3D geometry. The whole-project default for the
// connection table, mirroring the ThermalBridge example's APIFilt_In3D sweep.
bool CollectGuids (const GS::ObjectState& params, GS::Array<API_Guid>& guids, GS::UniString& err)
{
    GS::Array<GS::ObjectState> elements;
    if (params.Get ("elements", elements) && !elements.IsEmpty ()) {
        for (const GS::ObjectState& element : elements) {
            GS::UniString guid;
            if (!ReadElementId (element, guid)) {
                err = "every element needs elementId.guid";
                return false;
            }
            guids.Push (APIGuidFromString (guid.ToCStr ().Get ()));
        }
        return true;
    }

    const GSErrCode e = ACAPI_Element_GetElemList (API_ZombieElemID, &guids, APIFilt_In3D);
    if (e != NoError) {
        err = EVP_ACAPI_FAIL ("ACAPI_Element_GetElemList", e, "listing every element with 3D geometry");
        return false;
    }
    if (guids.IsEmpty ()) {
        err = EVP_FAIL ("no elements with 3D geometry in this project", "nothing to read");
        return false;
    }
    return true;
}


// `plane: {basePoint:{x,y,z}, normal:{x,y,z}}` — or all three axes explicitly.
// The normal form is the one callers actually want (a horizontal cut is
// normal (0,0,1)); Geometry::Plane derives an orthonormal frame from it, which
// is exactly what the example does.
bool ParsePlane (const GS::ObjectState& params, API_Plane3D& plane, GS::UniString& err)
{
    GS::ObjectState spec;
    if (!params.Get ("plane", spec)) {
        err = "need plane={basePoint:{x,y,z}, normal:{x,y,z}} "
              "(or basePoint + axisX/axisY/axisZ)";
        return false;
    }

    const auto readPoint = [] (const GS::ObjectState& os, const char* key,
                               double& x, double& y, double& z) {
        GS::ObjectState p;
        if (!os.Get (key, p))
            return false;
        return p.Get ("x", x) && p.Get ("y", y) && p.Get ("z", z);
    };

    double bx = 0.0, by = 0.0, bz = 0.0;
    if (!readPoint (spec, "basePoint", bx, by, bz)) {
        err = "plane needs basePoint={x,y,z}";
        return false;
    }
    plane.basePoint = { bx, by, bz };

    double ax = 0.0, ay = 0.0, az = 0.0;
    if (readPoint (spec, "axisX", ax, ay, az)) {
        plane.axisX = { ax, ay, az };
        if (!readPoint (spec, "axisY", ax, ay, az)) { err = "plane has axisX but no axisY"; return false; }
        plane.axisY = { ax, ay, az };
        if (!readPoint (spec, "axisZ", ax, ay, az)) { err = "plane has axisX but no axisZ"; return false; }
        plane.axisZ = { ax, ay, az };
        return true;
    }

    double nx = 0.0, ny = 0.0, nz = 1.0;
    if (!readPoint (spec, "normal", nx, ny, nz)) {
        err = "plane needs normal={x,y,z} (or the three explicit axes)";
        return false;
    }
    if (nx == 0.0 && ny == 0.0 && nz == 0.0) {
        err = "plane normal is the zero vector";
        return false;
    }

    const Geometry::Plane frame (Point3D (bx, by, bz), Vector3D (nx, ny, nz));
    const Vector3D x = frame.GetXAxis ();
    const Vector3D y = frame.GetYAxis ();
    const Vector3D z = frame.GetZAxis ();
    plane.axisX = { x.x, x.y, x.z };
    plane.axisY = { y.x, y.y, y.z };
    plane.axisZ = { z.x, z.y, z.z };
    return true;
}


Geometry::Plane ToGeometryPlane (const API_Plane3D& plane)
{
    return { Point3D (plane.basePoint.x, plane.basePoint.y, plane.basePoint.z),
             Vector3D (plane.axisZ.x, plane.axisZ.y, plane.axisZ.z),
             Vector3D (plane.axisX.x, plane.axisX.y, plane.axisX.z),
             Vector3D (plane.axisY.x, plane.axisY.y, plane.axisY.z) };
}


GS::ObjectState PlaneToObjectState (const API_Plane3D& plane)
{
    GS::ObjectState os;
    os.Add ("basePoint", PointToObjectState (plane.basePoint.x, plane.basePoint.y, plane.basePoint.z));
    os.Add ("axisX", PointToObjectState (plane.axisX.x, plane.axisX.y, plane.axisX.z));
    os.Add ("axisY", PointToObjectState (plane.axisY.x, plane.axisY.y, plane.axisY.z));
    os.Add ("axisZ", PointToObjectState (plane.axisZ.x, plane.axisZ.y, plane.axisZ.z));
    return os;
}


// One cut polygon, contour by contour. The FIRST contour is the outer boundary
// and the rest are holes — losing that split would turn a cut through a hollow
// column into a solid one. Points come back in BOTH frames: `uv` (the plane's
// own 2D space, which is what an area or a fill wants) and `coords` (world,
// which is what a placement wants).
GS::ObjectState DescribeCutPolygon (const Geometry::Polygon2D& polygon, const Geometry::Plane& frame)
{
    GS::ObjectState os;
    GS::Array<GS::Int32> contourVertexCounts;
    GS::Array<double>    uv, coords;

    for (auto contour = polygon.EnumerateContour (); contour != nullptr; ++contour) {
        GS::Int32 emitted = 0;
        for (auto vertex = contour->BeginVertex (); vertex != contour->EndVertex (); ++vertex) {
            const Point2D p = polygon.GetCoord (vertex);
            uv.Push (p.x);
            uv.Push (p.y);
            const Point3D world = frame.PlaneToWorld (p);
            coords.Push (world.x);
            coords.Push (world.y);
            coords.Push (world.z);
            ++emitted;
        }
        contourVertexCounts.Push (emitted);
    }

    os.Add ("contourCount", (GS::Int32) polygon.GetContourNum ());
    os.Add ("contourVertexCounts", contourVertexCounts);   // first = outer, rest = holes
    os.Add ("uv", uv);            // flat [u,v, …] in the cut plane
    os.Add ("coords", coords);    // flat [x,y,z, …] in world
    os.Add ("area", polygon.CalcArea ());                  // signed
    os.Add ("perimeter", polygon.CalcPerimeter ());
    return os;
}


// Resolve a guid to the element index the ACTIVE 3D DATABASE uses — NOT the
// modeler model's index. See the long note in GetCutPolygonsCommand::Execute:
// the two are different lists, and passing the modeler's index to a C-API
// cutting/material call yields APIERR_BADINDEX for every element whose two
// indices happen not to coincide.
//
// The route is the one EvP.GetBodyComponents already exposes as
// `elemIndexPlus1`: an element's body range comes from Get3DInfo, and the body
// record carries the element index the database knows it by.
//
// `outBodyCount` is the element's own body count, so the caller's body loop
// stays 0-based WITHIN the element — which is what the cutting call wants, and
// what the DevKit example passes.
static bool Resolve3DDatabaseElement (const GS::UniString& guidString,
                                      GS::Int32& outElemIdx, Int32& outBodyCount,
                                      GS::UniString& outError)
{
    API_Element element = {};
    element.header.guid = APIGuidFromString (guidString.ToCStr ().Get ());
    if (ACAPI_Element_Get (&element) != NoError) {
        outError = "no such element: " + guidString;
        return false;
    }

    API_ElemInfo3D info = {};
    const GSErrCode infoErr = ACAPI_ModelAccess_Get3DInfo (element.header, &info);
    if (infoErr != NoError) {
        outError = "that element has no 3D representation "
                   "(or its layer is hidden/locked/not yours)";
        return false;
    }
    if (info.lbody < info.fbody) {
        outError = "that element has no bodies in the 3D database";
        return false;
    }

    // The first body names the element; every body of one element reports the
    // same head.elemIndex.
    API_Component3D component = {};
    component.header.typeID = API_BodyID;
    component.header.index  = info.fbody;
    const GSErrCode bodyErr = ACAPI_ModelAccess_GetComponent (&component);
    if (bodyErr != NoError) {
        outError = EVP_ACAPI_FAIL ("ACAPI_ModelAccess_GetComponent", bodyErr,
            GS::UniString::Printf ("reading body %d to learn its element index",
                                   (int) info.fbody));
        return false;
    }

    outElemIdx   = (GS::Int32) (component.body.head.elemIndex - 1);   // plus-one -> 0-based
    outBodyCount = info.lbody - info.fbody + 1;
    return true;
}


// ---------------------------------------------------------------------------
// EvP.GetCutPolygons { guid | elemIdx, body?:N (0-based, all if omitted),
//                      plane:{basePoint, normal | axisX+axisY+axisZ},
//                      separateComponents?:bool }
//   -> { ok, guid?, elemIdx, plane, totalArea, bodies:[{ bodyIdx, area,
//        polygonCount, polygons:[{ contourCount, contourVertexCounts, uv,
//        coords, area, perimeter }] }] }
//
// Archicad's own cross-section of a solid at a plane, with the area.
//
// `separateComponents` (default false) decides WHAT gets cut: false cuts the
// element's whole body from the current sight; true regenerates it split into
// its components first, so a composite wall yields one body per skin and each
// area is per material. That is the mode a takeoff wants, and it is the same
// model EvP.GetBodyBuildingMaterials reads, so the two line up body for body.
// ---------------------------------------------------------------------------
class GetCutPolygonsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetCutPolygons"; }

    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        API_Plane3D plane = {};
        GS::UniString err;
        if (!ParsePlane (params, plane, err)) {
            return NativeCommandResult::Failure (EVP_FAIL (err, "EvP.GetCutPolygons"));
        }

        bool separate = false;
        params.Get ("separateComponents", separate);

        GS::ObjectState requestedElementId;
        GS::UniString guidString;
        const bool haveGuid = params.Get ("elementId", requestedElementId) &&
                              requestedElementId.Get ("guid", guidString) && !guidString.IsEmpty ();

        // The two sight modes need different model acquisition and must not be
        // mixed: AcquireCurrentModel switches sights itself, which would fight
        // the scratch sight.
        SeparateComponentsSight scratch;
        ModelerAPI::Model plain;
        const ModelerAPI::Model* model = nullptr;

        if (separate) {
            if (!haveGuid) {
                return NativeCommandResult::Failure (EVP_FAIL (
                    "separateComponents needs guid=\"…\"",
                    "the model is regenerated for that element"));
            }
            GS::Array<API_Guid> guids;
            guids.Push (APIGuidFromString (guidString.ToCStr ().Get ()));
            if (!scratch.Open (guids, err)) {
                return NativeCommandResult::Failure (err);
            }
            model = &scratch.Model ();
        } else {
            if (!AcquireCurrentModel (plain)) {
                return NativeCommandResult::Failure (
                    EVP_FAIL ("could not read the 3D model", "EvP.GetCutPolygons"));
            }
            model = &plain;
        }

        // ⚠️ TWO DIFFERENT INDEX SPACES. THIS IS THE BUG E24 SPENT FOUR RUNS ON.
        //
        // `ACAPI_CuttingPlane_GetCutPolygonInfo_New` indexes the ACTIVE 3D DATABASE.
        // The modeler model's element index is a DIFFERENT list, and
        // `modelIndex - 1` only happens to be right when the two coincide.
        //
        // The DevKit's own reference (Examples/ThermalBridge/Src/DumpThermalBridges.cpp)
        // does use `*elemIndexOpt - 1` and says so in a comment — but it takes that
        // index from a model it CREATED AND REGENERATED in its own sight, so for it
        // the two lists are the same one. The separate-components branch below has
        // that same property and is correct for the same reason.
        //
        // The plain branch does not: its index comes from AcquireCurrentModel.
        // PROVEN LIVE 2026-08-03 with three walls cut at the middle of their own
        // bounds — modeler index 1 (elemIdx 0) cut 3.14 m2, while modeler indices
        // 11 and 12 (elemIdx 10, 11) both returned APIERR_BADINDEX, and the SAME
        // walls cut correctly under separate components at elemIdx 0.
        //
        // So the plain path resolves the index through the 3D database itself:
        //   ACAPI_ModelAccess_Get3DInfo  -> the element's body range (fbody..lbody)
        //   ACAPI_ModelAccess_GetComponent(API_BodyID, fbody) -> head.elemIndex
        // which is the `elemIndexPlus1` EvP.GetBodyComponents already reports, and
        // which EVP-API-DOCS.md has always said is where elemIdx comes from. The
        // docs were right; this code was not.
        GS::Int32 elemIdx = -1;
        Int32 bodyCount = 0;
        if (haveGuid && !separate) {
            if (!Resolve3DDatabaseElement (guidString, elemIdx, bodyCount, err)) {
                return NativeCommandResult::Failure (
                    EVP_FAIL (err, "EvP.GetCutPolygons element lookup"));
            }
            AddElementId (guidString, os);
        } else if (haveGuid) {
            // Separate components: the scratch sight regenerated the model for this
            // element alone, so the modeler index and the database index DO agree —
            // exactly as in the DevKit example.
            const std::optional<Int32> found = model->GetElementIndex (
                APIGuid2GSGuid (APIGuidFromString (guidString.ToCStr ().Get ())));
            if (!found.has_value ()) {
                return NativeCommandResult::Failure (EVP_FAIL (
                    "elementId is not present in the separate-components model",
                    "Tapioca.GetCutPolygons element lookup"));
            }
            ModelerAPI::Element elem;
            const Int32 modelIndex = *found;
            model->GetElement (modelIndex, &elem);
            elemIdx = (GS::Int32) (modelIndex - 1);
            bodyCount = elem.GetTessellatedBodyCount ();
            AddElementId (ElementGuidString (elem), os);
        } else if (!params.Get ("elemIdx", elemIdx)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("need guid=\"…\" or elemIdx=N (0-based)", "EvP.GetCutPolygons"));
        } else {
            // Raw elemIdx: the caller owns the index, so we cannot know the body
            // count. `body` is then required.
            bodyCount = 0;
        }

        GS::Int32 requestedBody = -1;
        const bool oneBody = params.Get ("body", requestedBody);
        if (!oneBody && bodyCount <= 0) {
            return NativeCommandResult::Failure (EVP_FAIL (
                "need body=N (0-based) when identifying the element by elemIdx",
                "pass guid instead and every body is cut"));
        }

        const Geometry::Plane frame = ToGeometryPlane (plane);
        const GS::Int32 firstBody = oneBody ? requestedBody : 0;
        const GS::Int32 lastBody  = oneBody ? requestedBody : (GS::Int32) bodyCount - 1;

        GS::Array<GS::ObjectState> bodies;
        double totalArea = 0.0;
        for (GS::Int32 b = firstBody; b <= lastBody; ++b) {
            GS::Array<Geometry::MultiPolygon2D> result;
            double area = 0.0;
            const GSErrCode e = ACAPI_CuttingPlane_GetCutPolygonInfo_New (
                (Int32) elemIdx, (Int32) b, plane, &result, &area);

            GS::ObjectState record;
            record.Add ("bodyIdx", b);
            record.Add ("elemIdx", elemIdx);
            if (e != NoError) {
                // A body that does not intersect the plane is a normal outcome,
                // not something to abort the whole read for.
                //
                // ⚠️ But it must still SAY WHAT HAPPENED. This used to be
                // Printf("cut failed (error %d)") — a bare GSErrCode, which the
                // project forbids for exactly the reason it cost here: three runs
                // reported "0 of 1 bodies" and no way to tell a refused index from
                // a plane that genuinely missed. EVP_ACAPI_FAIL decodes the code
                // and records the call site.
                record.Add ("succeeded", false);
                record.Add ("error", EVP_ACAPI_FAIL ("ACAPI_CuttingPlane_GetCutPolygonInfo_New", e,
                    GS::UniString::Printf ("elemIdx %d body %d — elemIdx must index the 3D "
                                           "database the cut reads, not the modeler model",
                                           (int) elemIdx, (int) b)));
                bodies.Push (record);
                continue;
            }

            GS::Array<GS::ObjectState> polygons;
            for (const Geometry::MultiPolygon2D& multi : result)
                for (UIndex i = 0; i < multi.GetSize (); ++i)
                    polygons.Push (DescribeCutPolygon (multi[i], frame));

            record.Add ("succeeded", true);
            record.Add ("area", area);
            record.Add ("polygonCount", (GS::Int32) polygons.GetSize ());
            record.Add ("polygons", polygons);
            bodies.Push (record);
            totalArea += area;
        }

        os.Add ("elemIdx", elemIdx);
        os.Add ("separateComponents", separate);
        os.Add ("plane", PlaneToObjectState (plane));
        os.Add ("totalArea", totalArea);
        os.Add ("bodyCount", (GS::Int32) bodies.GetSize ());
        os.Add ("bodies", bodies);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetBodyBuildingMaterials { guids:[…] }
//   -> { ok, count, elements:[{ guid, found, elemIdx, bodyCount,
//        bodies:[{ bodyIdx, attributeIndex, name }] }] }
//
// Which BUILDING MATERIAL each body of an element is made of — the structural
// material (with its conductivity, density, fill), not the visible surface.
//
// This is the read that makes a composite legible from geometry: with
// `GenerateModelWithSeparateComponents` a composite wall becomes one body PER
// SKIN, and this names each one. Pair it with EvP.GetCutPolygons
// (`separateComponents:true`) — same model, same body indices — and you have
// per-material section areas.
//
// `name` is resolved through the same AttributeIndexToName every other EvP
// command uses, so it is the name the user sees in the attribute list. Empty
// means the index did not resolve.
// ---------------------------------------------------------------------------
class GetBodyBuildingMaterialsCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetBodyBuildingMaterials"; }

    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<GS::ObjectState> requestedElements;
        if (!params.Get ("elements", requestedElements) || requestedElements.IsEmpty ()) {
            return NativeCommandResult::Failure (EVP_FAIL (
                "need elements=[{elementId:{guid}}]", "Tapioca.GetBodyBuildingMaterials"));
        }

        GS::Array<GS::UniString> guidStrings;
        GS::Array<API_Guid> guids;
        for (const GS::ObjectState& requested : requestedElements) {
            GS::UniString g;
            if (!ReadElementId (requested, g)) {
                return NativeCommandResult::Failure (EVP_FAIL (
                    "every element needs elementId.guid", "Tapioca.GetBodyBuildingMaterials"));
            }
            guidStrings.Push (g);
            guids.Push (APIGuidFromString (g.ToCStr ().Get ()));
        }

        SeparateComponentsSight scratch;
        GS::UniString err;
        if (!scratch.Open (guids, err)) {
            return NativeCommandResult::Failure (err);
        }
        const ModelerAPI::Model& model = scratch.Model ();

        GS::Array<GS::ObjectState> elements;
        for (UIndex i = 0; i < guids.GetSize (); ++i) {
            GS::ObjectState record;
            AddElementId (guidStrings[i], record);

            const std::optional<Int32> found = model.GetElementIndex (APIGuid2GSGuid (guids[i]));
            if (!found.has_value ()) {
                record.Add ("found", false);
                record.Add ("error", GS::UniString ("not present in the separate-components model"));
                elements.Push (record);
                continue;
            }

            ModelerAPI::Element elem;
            model.GetElement (*found, &elem);
            const Int32 bodyCount = elem.GetTessellatedBodyCount ();
            const Int32 elemIdx = *found - 1;      // 0-based for the C API. See the file header.

            GS::Array<GS::ObjectState> bodies;
            for (Int32 b = 0; b < bodyCount; ++b) {
                API_AttributeIndex materialIdx;
                const GSErrCode e = ACAPI_ModelAccess_GetBuildingMaterial (
                    (UInt32) elemIdx, (UInt32) b, &materialIdx);

                GS::ObjectState body;
                body.Add ("bodyIdx", (GS::Int32) b);
                if (e != NoError) {
                    // Decoded, not a bare code — same rule, same reason as the
                    // cutting call: "error -2130313114" hides APIERR_BADINDEX,
                    // which is the difference between "this body has no building
                    // material" and "the element index is wrong".
                    body.Add ("succeeded", false);
                    body.Add ("error", EVP_ACAPI_FAIL ("ACAPI_ModelAccess_GetBuildingMaterial", e,
                        GS::UniString::Printf ("elemIdx %d body %d in the separate-components model",
                                               (int) elemIdx, (int) b)));
                } else {
                    body.Add ("succeeded", true);
                    body.Add ("attributeIndex", GS::UniString (materialIdx.ToUniString ()));
                    body.Add ("name", AttributeIndexToName (API_BuildingMaterialID, materialIdx));
                }
                bodies.Push (body);
            }

            record.Add ("found", true);
            record.Add ("elemIdx", (GS::Int32) elemIdx);
            record.Add ("modelIndex", (GS::Int32) *found);
            record.Add ("bodyCount", (GS::Int32) bodyCount);
            record.Add ("bodies", bodies);
            elements.Push (record);
        }

        os.Add ("count", (GS::Int32) elements.GetSize ());
        os.Add ("elements", elements);
        return os;
    }
};


// ---------------------------------------------------------------------------
// EvP.GetConnectionTable { guids?:[…] }
//   -> { ok, pairCount, connections:[{ guid1, guid2, polygonCount,
//        polygons:[{ vertexCount, coords, plane }] }] }
//
// Which elements actually TOUCH, and the surface they touch over. Omit `guids`
// and every element with 3D geometry is considered.
//
// ⚠️ NOT the same question as EvP.GetCollisions. That one (TopologyCommands,
// `ACAPI_Element_GetCollisions`) reports elements whose solids OVERLAP — it is
// a clash test. This reports elements that are CONNECTED, with the connecting
// polygon: a slab sitting exactly on a wall clashes with nothing and connects
// over its whole bearing area. Junction, thermal-bridge and interface work
// wants this one.
//
// Polygon coordinates are world-space, converted through each polygon's own
// plane exactly as the DevKit example does. Archicad's 2D polygons index from 1
// and repeat the first point at index n, which is why the loop below starts at
// 1 — that is the API's convention, not an off-by-one.
//
// The table hands back API_ElementMemo handles inside each polygon; they are
// disposed here, unconditionally, before returning.
// ---------------------------------------------------------------------------
class GetConnectionTableCommand : public MainThreadCommand {
public:
    GS::String GetName () const override { return "GetConnectionTable"; }

    bool IsProcessWindowVisible () const override { return true; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<API_Guid> guids;
        GS::UniString err;
        if (!CollectGuids (params, guids, err)) {
            return NativeCommandResult::Failure (err);
        }

        SeparateComponentsSight scratch;
        if (!scratch.Open (guids, err)) {
            return NativeCommandResult::Failure (err);
        }

        // The connection table wants a SET, not the array everything else here
        // speaks — duplicates in the list would otherwise be a silent ambiguity.
        GS::HashSet<API_Guid> guidSet;
        for (const API_Guid& g : guids)
            guidSet.Add (g);

        API_ElementConnectionTable table;
        const GSErrCode e = ACAPI_ModelAccess_GetConnectionTable (guidSet, &table);
        if (e != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                "ACAPI_ModelAccess_GetConnectionTable", e,
                GS::UniString::Printf ("%u element(s)", (unsigned) guids.GetSize ())));
        }

        GS::Array<GS::ObjectState> connections;
        for (const auto& entry : table) {
            const auto& pair = entry.key;
            GS::ObjectState record;
            GS::ObjectState firstElement, secondElement;
            AddElementId (GS::UniString (APIGuidToString (pair.first).ToCStr ()), firstElement);
            AddElementId (GS::UniString (APIGuidToString (pair.second).ToCStr ()), secondElement);
            record.Add ("firstElement", firstElement);
            record.Add ("secondElement", secondElement);

            GS::Array<GS::ObjectState> polygons;
            for (const API_PolygonExt& polygon : entry.value) {
                const Geometry::Plane frame = ToGeometryPlane (polygon.plane);

                GS::Array<double> coords;
                if (polygon.polygonData.coords != nullptr) {
                    // 1-based, and index n repeats index 1 — Archicad's own 2D
                    // polygon layout.
                    for (Int32 c = 1; c <= polygon.polygon.nCoords; ++c) {
                        const API_Coord flat = (*polygon.polygonData.coords)[c];
                        const Point3D world = frame.PlaneToWorld (Point2D (flat.x, flat.y));
                        coords.Push (world.x);
                        coords.Push (world.y);
                        coords.Push (world.z);
                    }
                }

                GS::ObjectState p;
                p.Add ("vertexCount", (GS::Int32) polygon.polygon.nCoords);
                p.Add ("coords", coords);        // flat [x,y,z, …] world
                p.Add ("plane", PlaneToObjectState (polygon.plane));
                polygons.Push (p);
            }

            record.Add ("polygonCount", (GS::Int32) polygons.GetSize ());
            record.Add ("polygons", polygons);
            connections.Push (record);
        }

        // The table owns memo handles. Disposing them is the caller's job per the
        // DevKit example, and there is exactly one place to do it: here, before
        // the response goes out, whether or not anything above found geometry.
        for (GS::Array<API_PolygonExt>& polygonArray : table.Values ())
            for (API_PolygonExt& polygon : polygonArray)
                ACAPI_DisposeElemMemoHdls (&polygon.polygonData);

        os.Add ("elementCount", (GS::Int32) guids.GetSize ());
        os.Add ("pairCount", (GS::Int32) connections.GetSize ());
        os.Add ("connections", connections);
        return os;
    }
};

constexpr const char kCutInput[] = R"json({"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elemIdx":{"type":"integer","minimum":0},"body":{"type":"integer","minimum":0},"plane":{"$ref":"#/$defs/planeInput"},"separateComponents":{"type":"boolean"}},"additionalProperties":false,"required":["plane"],"anyOf":[{"required":["elementId"]},{"required":["elemIdx","body"]}],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]},"planeInput":{"type":"object","properties":{"basePoint":{"$ref":"#/$defs/point"},"normal":{"$ref":"#/$defs/point"},"axisX":{"$ref":"#/$defs/point"},"axisY":{"$ref":"#/$defs/point"},"axisZ":{"$ref":"#/$defs/point"}},"additionalProperties":false,"required":["basePoint"],"anyOf":[{"required":["normal"]},{"required":["axisX","axisY","axisZ"]}]}}})json";
constexpr const char kCutOutput[] = R"json(
{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"elemIdx":{"type":"integer","minimum":0},"separateComponents":{"type":"boolean"},"plane":{"$ref":"#/$defs/plane"},"totalArea":{"type":"number"},"bodyCount":{"type":"integer","minimum":0},"bodies":{"type":"array","items":{"$ref":"#/$defs/body"}}},"additionalProperties":false,"required":["elemIdx","separateComponents","plane","totalArea","bodyCount","bodies"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]},"plane":{"type":"object","properties":{"basePoint":{"$ref":"#/$defs/point"},"axisX":{"$ref":"#/$defs/point"},"axisY":{"$ref":"#/$defs/point"},"axisZ":{"$ref":"#/$defs/point"}},"additionalProperties":false,"required":["basePoint","axisX","axisY","axisZ"]},"polygon":{"type":"object","properties":{"contourCount":{"type":"integer","minimum":0},"contourVertexCounts":{"type":"array","description":"Vertex count for each contour; first contour is outer and the rest are holes; splits uv and coords.","items":{"type":"integer","minimum":0}},"uv":{"type":"array","description":"Packed cut-plane uv coordinates; stride 2 and split by contourVertexCounts.","items":{"type":"number"}},"coords":{"type":"array","description":"Packed world xyz coordinates; stride 3 and split by contourVertexCounts.","items":{"type":"number"}},"area":{"type":"number"},"perimeter":{"type":"number"}},"additionalProperties":false,"required":["contourCount","contourVertexCounts","uv","coords","area","perimeter"]},"body":{"type":"object","properties":{"bodyIdx":{"type":"integer","minimum":0},"elemIdx":{"type":"integer","minimum":0},"succeeded":{"type":"boolean"},"error":{"type":"string"},"area":{"type":"number"},"polygonCount":{"type":"integer","minimum":0},"polygons":{"type":"array","items":{"$ref":"#/$defs/polygon"}}},"additionalProperties":false,"required":["bodyIdx","elemIdx","succeeded"]}}}
)json";
constexpr const char kMaterialsInput[] = R"json({"type":"object","properties":{"elements":{"type":"array","minItems":1,"items":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"required":["elements"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kMaterialsOutput[] = R"json({"type":"object","properties":{"count":{"type":"integer","minimum":0},"elements":{"type":"array","items":{"$ref":"#/$defs/element"}}},"additionalProperties":false,"required":["count","elements"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"body":{"type":"object","properties":{"bodyIdx":{"type":"integer","minimum":0},"succeeded":{"type":"boolean"},"error":{"type":"string"},"attributeIndex":{"type":"string"},"name":{"type":"string"}},"additionalProperties":false,"required":["bodyIdx","succeeded"]},"element":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"},"found":{"type":"boolean"},"error":{"type":"string"},"elemIdx":{"type":"integer","minimum":0},"modelIndex":{"type":"integer","minimum":1},"bodyCount":{"type":"integer","minimum":0},"bodies":{"type":"array","items":{"$ref":"#/$defs/body"}}},"additionalProperties":false,"required":["elementId","found"]}}})json";
constexpr const char kConnectionsInput[] = R"json({"type":"object","properties":{"elements":{"type":"array","items":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]}}},"additionalProperties":false,"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}}})json";
constexpr const char kConnectionsOutput[] = R"json({"type":"object","properties":{"elementCount":{"type":"integer","minimum":0},"pairCount":{"type":"integer","minimum":0},"connections":{"type":"array","items":{"$ref":"#/$defs/connection"}}},"additionalProperties":false,"required":["elementCount","pairCount","connections"],"$defs":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"element":{"type":"object","properties":{"elementId":{"$ref":"#/$defs/elementId"}},"additionalProperties":false,"required":["elementId"]},"point":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"additionalProperties":false,"required":["x","y","z"]},"plane":{"type":"object","properties":{"basePoint":{"$ref":"#/$defs/point"},"axisX":{"$ref":"#/$defs/point"},"axisY":{"$ref":"#/$defs/point"},"axisZ":{"$ref":"#/$defs/point"}},"additionalProperties":false,"required":["basePoint","axisX","axisY","axisZ"]},"polygon":{"type":"object","properties":{"vertexCount":{"type":"integer","minimum":0},"coords":{"type":"array","description":"Packed world xyz polygon coordinates; stride 3 in API polygon order.","items":{"type":"number"}},"plane":{"$ref":"#/$defs/plane"}},"additionalProperties":false,"required":["vertexCount","coords","plane"]},"connection":{"type":"object","properties":{"firstElement":{"$ref":"#/$defs/element"},"secondElement":{"$ref":"#/$defs/element"},"polygonCount":{"type":"integer","minimum":0},"polygons":{"type":"array","items":{"$ref":"#/$defs/polygon"}}},"additionalProperties":false,"required":["firstElement","secondElement","polygonCount","polygons"]}}})json";

const NativeCommandRegistration kCuttingPlaneCommandRegistrations[] = {
    { "GetCutPolygons",           &MakeRegisteredNativeCommand<GetCutPolygonsCommand>,           false, kCutInput,         kCutOutput },
    { "GetBodyBuildingMaterials", &MakeRegisteredNativeCommand<GetBodyBuildingMaterialsCommand>, false, kMaterialsInput,   kMaterialsOutput },
    { "GetConnectionTable",       &MakeRegisteredNativeCommand<GetConnectionTableCommand>,       false, kConnectionsInput, kConnectionsOutput },
};

}   // namespace

NativeCommandRegistrations GetCuttingPlaneCommandRegistrations ()
{
    return MakeRegistrationView (kCuttingPlaneCommandRegistrations);
}

} // namespace geomsrv

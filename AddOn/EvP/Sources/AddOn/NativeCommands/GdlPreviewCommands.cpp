#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/GdlPreviewCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

namespace geomsrv {

namespace {

// The part the mesh is fed into. Compiled from
// private/Library/TapiocaPreview/TapiocaPreview.xml by
// private/Library/Build-LibParts.ps1 and LOADED BY HAND through Library Manager —
// Sync-All.ps1 does not deploy library parts, and deliberately does not, since
// loading one is a per-project user action.
constexpr const char* kPartName = "TapiocaPreview";

constexpr const char* kVertsParam = "pvVerts";
constexpr const char* kTrisParam = "pvTris";

// The PLAN carrier is a SEPARATE part, not a second script on the first one.
// Sharing one part would put the plan benchmark's polygons into the 3D window
// and the 3D benchmark's body onto the floor plan, and each would then be paying
// the other's cost inside its own measurement.
constexpr const char* kPlanPartName = "TapiocaPlanPreview";
constexpr const char* kPlanVertsParam = "pv2Verts";
constexpr const char* kPlanPolysParam = "pv2Polys";

using Clock = std::chrono::steady_clock;

double MsSince (const Clock::time_point& from)
{
    return std::chrono::duration<double, std::milli> (Clock::now () - from).count ();
}

// ---------------------------------------------------------------------------
// The test mesh
// ---------------------------------------------------------------------------
//
// A TORUS on a rows x cols grid, and the shape is chosen, not incidental:
//
//   * it is CLOSED. The GDL Reference is explicit that "the efficiency of the
//     cutting, hidden line removal or rendering algorithms is lower for open
//     bodies", so an open test mesh would measure the slow path and report it as
//     the route's cost;
//   * every vertex has the same valence, so the per-triangle cost is uniform and
//     the C-to-D slope means something;
//   * the vertex and triangle counts are exactly rows*cols and 2*rows*cols, so a
//     case is a grid size and not a lookup table.
//
// Deterministic: the same grid always yields the same doubles. That is what lets
// two runs be compared, and it is why the mesh is built here rather than sent.
struct Mesh {
    GS::Array<double> verts; // x,y,z triples
    GS::Array<Int32> tris;   // 1-BASED vertex indices, triples — GDL indexes from 1
};

void BuildTorus (Int32 rows, Int32 cols, double majorR, double minorR, Mesh& mesh)
{
    const double twoPi = 2.0 * 3.14159265358979323846;
    mesh.verts.SetCapacity ((USize) rows * cols * 3);
    mesh.tris.SetCapacity ((USize) rows * cols * 6);

    for (Int32 i = 0; i < rows; ++i) {
        const double u = twoPi * (double) i / (double) rows;
        const double cu = std::cos (u), su = std::sin (u);
        for (Int32 j = 0; j < cols; ++j) {
            const double v = twoPi * (double) j / (double) cols;
            const double cv = std::cos (v), sv = std::sin (v);
            mesh.verts.Push ((majorR + minorR * cv) * cu);
            mesh.verts.Push ((majorR + minorR * cv) * su);
            mesh.verts.Push (minorR * sv);
        }
    }

    // Two triangles per grid quad, wrapping in both directions — which is what
    // closes the body. Counter-clockwise seen from outside, so the computed
    // normals point out; the GDL Reference requires that for a closed body.
    for (Int32 i = 0; i < rows; ++i) {
        const Int32 iN = (i + 1) % rows;
        for (Int32 j = 0; j < cols; ++j) {
            const Int32 jN = (j + 1) % cols;
            const Int32 a = i * cols + j + 1; // +1: GDL VERT indices start at 1
            const Int32 b = iN * cols + j + 1;
            const Int32 c = iN * cols + jN + 1;
            const Int32 d = i * cols + jN + 1;
            mesh.tris.Push (a);
            mesh.tris.Push (b);
            mesh.tris.Push (c);
            mesh.tris.Push (a);
            mesh.tris.Push (c);
            mesh.tris.Push (d);
        }
    }
}

// Case A0 from the benchmark: ONE triangle, to calibrate the fixed overhead.
// Without it case A cannot be read — 200 triangles at 15 ms says nothing until
// you know whether 14 of those milliseconds are the round trip.
void BuildSingleTriangle (Mesh& mesh)
{
    const double p[9] = { 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0 };
    for (double v : p)
        mesh.verts.Push (v);
    mesh.tris.Push (1);
    mesh.tris.Push (2);
    mesh.tris.Push (3);
}

// ---------------------------------------------------------------------------
// Writing one array parameter
// ---------------------------------------------------------------------------
//
// ⚠️ THIS IS THE LOAD-BEARING CLAIM OF THE WHOLE ROUTE, quoted from
// APIdefs_LibraryParts.h:596 verbatim: "For parameters with array modifier, you
// can change the dimensions of the array ON THE FLY when you create or modify an
// element. This enables you to put just a placeholder for an array into the
// library part parameter, and adjust that when you actually place the library
// part." If that sentence does not hold for an ALREADY PLACED element, the
// no-regeneration claim fails and the route becomes delete-and-replace per
// update — which changes the undo story completely. That is benchmark item U1,
// and this function is what tests it.
//
// The layout is double[dim1*dim2], ROW-MAJOR with dim2 as the stride. Taken from
// the DevKit's own examples: LibPart_Test.cpp:318-322 fills
// (*arrHdl)[k * dim2 + j], and Element_Modify_ChangeParameters.cpp:520-596 does
// the resize exactly this way — kill the old handle, allocate a new one, set
// dim1/dim2, assign value.array.
//
// ⚠️ EVERY GDL TYPE LIVES IN THE SAME double. pvTris is declared Integer in the
// part, but an integer array parameter is still an array OF DOUBLES; writing
// Int32 into it would be read as garbage. The indices are widened, not cast.
//
// Returns false if the parameter is not in the list, which must stay an error:
// a misspelled name would otherwise leave the mesh unwritten under a cheerful
// ok:true, and the object would simply render nothing.
bool WriteDoubleArray (API_ElementMemo& memo, const char* name, Int32 dim1, Int32 dim2, const double* values,
                       USize count)
{
    if (memo.params == nullptr)
        return false;

    const GSSize paramCount = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
    for (GSIndex p = 0; p < paramCount; ++p) {
        API_AddParType& param = (*memo.params)[p];
        if (std::strcmp (param.name, name) != 0)
            continue;

        GSHandle newHandle = BMAllocateHandle ((GSSize) (count * sizeof (double)), 0, 0);
        if (newHandle == nullptr)
            return false;
        std::memcpy (*newHandle, values, count * sizeof (double));

        // Kill BEFORE reassigning: the old handle is the library default's 1x3
        // placeholder and nothing else will ever free it once the field is
        // overwritten. DisposeElemMemoHdls frees whatever is in the field at the
        // end, so the new handle is not leaked.
        BMKillHandle (&param.value.array);

        param.typeMod = API_ParArray;
        param.dim1 = dim1;
        param.dim2 = dim2;
        param.value.array = newHandle;

        // ⚠️ NEVER SET API_ParFlg_SHidden HERE. It hides the parameter FROM
        // SCRIPTS (APIdefs_LibraryParts.h:586, "hidden from script"), not from
        // the dialog — the 3D script would read nothing and emit empty geometry
        // with no error anywhere. API_ParFlg_Hidden, which the .gsm already
        // carries, is the one that keeps it out of Settings.
        param.flags = (unsigned short) (param.flags & ~API_ParFlg_SHidden);
        return true;
    }
    return false;
}

// A plain numeric parameter. Every GDL type that is not a string lives in the
// same `real` double — Integer, Length, Boolean, Material and FillPattern alike
// — so one path covers all of them.
void WriteScalar (API_ElementMemo& memo, const char* name, double value)
{
    if (memo.params == nullptr)
        return;
    const GSSize count = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
    for (GSIndex p = 0; p < count; ++p) {
        if (std::strcmp ((*memo.params)[p].name, name) == 0) {
            (*memo.params)[p].value.real = value;
            return;
        }
    }
}

// The library-part index for one of the preview carriers, or a sentence saying
// why not.
bool ResolvePreviewPart (const char* partName, API_LibPart& libPart, GS::UniString& refusal)
{
    libPart = {};
    GS::ucscpy (libPart.docu_UName, GS::UniString (partName).ToUStr ());

    // createIfMissing=false. A virtual reference would place Archicad's "missing
    // part" dot and report success while having drawn nothing — the exact silent
    // wrongness the benchmark must not measure.
    const GSErrCode err = ACAPI_LibraryPart_Search (&libPart, false, true);
    delete libPart.location;
    libPart.location = nullptr;

    if (err != NoError) {
        refusal = GS::UniString ("The library part \"") + partName +
                  "\" is not loaded. Compile it with private\\Library\\Build-LibParts.ps1, then add "
                  "private\\Library\\build to the project's libraries in File > Libraries and Objects > "
                  "Library Manager and reload. NOTHING was placed.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tapioca.GdlPreviewFeed
// ---------------------------------------------------------------------------
class GdlPreviewFeedCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "GdlPreviewFeed";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 rows = 0, cols = 0;
        params.Get ("rows", rows);
        params.Get ("cols", cols);

        // WARNING: `jitter` EXISTS TO DEFEAT A NO-OP, and the benchmark is wrong
        // without it. Pushing the SAME grid twice writes byte-identical parameter
        // data, and Archicad then has nothing to regenerate: the second push comes
        // back an order of magnitude faster because it CONVERTED NOTHING. The first
        // benchmark runs took the minimum across repeats and so reported that no-op
        // as the settle cost - case C read 18 ms where a real settle is ~150 ms.
        // A caller that repeats a case MUST vary this so every push is new data.
        GS::Int32 jitter = 0;
        params.Get ("jitter", jitter);
        if (rows < 0 || cols < 0 || (rows > 0 && cols < 3) || (cols > 0 && rows < 3)) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("rows and cols must both be 0 (the single-triangle A0 case) or both at least 3 "
                          "(a torus grid: rows*cols vertices, 2*rows*cols triangles)",
                          "Tapioca.GdlPreviewFeed"));
        }

        API_LibPart libPart;
        GS::UniString refusal;
        if (!ResolvePreviewPart (kPartName, libPart, refusal))
            return NativeCommandResult::Failure (refusal);

        // ---- t0a: generate ------------------------------------------------
        const Clock::time_point genStart = Clock::now ();
        Mesh mesh;
        if (rows == 0)
            BuildSingleTriangle (mesh);
        else
            // The jitter rides on the minor radius: enough that no two pushes share
            // a byte, small enough that the mesh keeps its size and the timings stay
            // comparable across repeats.
            BuildTorus (rows, cols, 4.0, 1.5 + 0.001 * (double) jitter, mesh);
        const double genMs = MsSince (genStart);

        const Int32 vertexCount = (Int32) (mesh.verts.GetSize () / 3);
        const Int32 triangleCount = (Int32) (mesh.tris.GetSize () / 3);

        // The triangle indices widened to doubles. Done OUTSIDE the fill timer's
        // memcpy but INSIDE t0, because a real feed would pay it too — the mesh
        // upstream is not going to arrive as doubles either.
        GS::Array<double> triDoubles;
        triDoubles.SetCapacity (mesh.tris.GetSize ());
        for (Int32 index : mesh.tris)
            triDoubles.Push ((double) index);

        // ---- fetch the element to write into -------------------------------
        GS::ObjectState elementIdIn;
        GS::UniString guidIn;
        const bool updating =
            params.Get ("elementId", elementIdIn) && elementIdIn.Get ("guid", guidIn) && !guidIn.IsEmpty ();

        API_Element element = {};
        API_ElementMemo memo = {};
        element.header.type = API_ObjectID;
        GSErrCode err = NoError;

        if (updating) {
            element.header.guid = APIGuidFromString (guidIn.ToCStr ().Get ());
            err = ACAPI_Element_Get (&element);
            if (err == NoError)
                err = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_AddPars);
            if (err != NoError) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_Get/GetMemo", err,
                                    GS::UniString::Printf ("elementId.guid %T — pass an object this command placed, "
                                                           "or omit elementId to place a new one",
                                                           guidIn.ToPrintf ())));
            }
            if (element.object.libInd != libPart.index) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (
                    GS::UniString ("That element is not a TapiocaPreview object. Feeding a mesh into some "
                                   "other library part's parameter list would either fail or quietly rewrite "
                                   "whatever happens to share those names."));
            }
        }
        else {
            err = ACAPI_Element_GetDefaults (&element, &memo);
            if (err != NoError) {
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_ObjectID (tool defaults)"));
            }

            // ⚠️ GetDefaults returns the parameter list of whatever part the
            // Object TOOL currently defaults to, NOT the part just resolved.
            // Searching those for pvVerts looks for our parameters in an
            // unrelated object and reports them missing. Same trap as
            // PlaceLibraryObject.
            double a = 0.0, b = 0.0;
            Int32 addParNum = 0;
            API_AddParType** libParams = nullptr;
            const GSErrCode paramErr = ACAPI_LibraryPart_GetParams (libPart.index, &a, &b, &addParNum, &libParams);
            if (paramErr != NoError) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_LibraryPart_GetParams", paramErr, GS::UniString (kPartName)));
            }
            ACAPI_DisposeAddParHdl (&memo.params);
            memo.params = libParams;

            element.object.libInd = libPart.index;
            element.object.xRatio = a;
            element.object.yRatio = b;

            double x = 0.0, y = 0.0;
            params.Get ("x", x);
            params.Get ("y", y);
            element.object.pos.x = x;
            element.object.pos.y = y;

            GS::UniString layerErr;
            if (!ResolveLayerParam (params, element.header, layerErr)) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (layerErr);
            }
        }

        // ---- t0b: build the handles ---------------------------------------
        const Clock::time_point fillStart = Clock::now ();
        const bool wroteVerts =
            WriteDoubleArray (memo, kVertsParam, vertexCount, 3, mesh.verts.GetContent (), mesh.verts.GetSize ());
        const bool wroteTris =
            WriteDoubleArray (memo, kTrisParam, triangleCount, 3, triDoubles.GetContent (), triDoubles.GetSize ());
        const double fillMs = MsSince (fillStart);

        if (!wroteVerts || !wroteTris) {
            ACAPI_DisposeElemMemoHdls (&memo);
            return NativeCommandResult::Failure (
                GS::UniString (GS::UniString ("Could not write ") + (wroteVerts ? kTrisParam : kVertsParam) +
                               " - the loaded TapiocaPreview has no parameter by that name, or the "
                               "allocation failed. An older build of the part is the usual cause; recompile it. "
                               "NOTHING was written."));
        }

        // Scalars that ride along. pvRevision is a STALENESS ASSERT, not a
        // trigger: writing the arrays IS the invalidation, because the geometry
        // is the parameter data. A revision counter that had to be bumped to
        // make regeneration happen would mean the arrays were not being read.
        GS::Int32 revision = 0;
        if (params.Get ("revision", revision))
            WriteScalar (memo, "pvRevision", (double) revision);
        GS::Int32 bodyStatus = 0;
        if (params.Get ("bodyStatus", bodyStatus))
            WriteScalar (memo, "pvBodyStatus", (double) bodyStatus);
        bool enabled = true;
        if (params.Get ("enabled", enabled))
            WriteScalar (memo, "pvEnabled", enabled ? 1.0 : 0.0);

        // ---- t1: the element write ----------------------------------------
        // NO undo scope here — WriteCommand's contract. The dispatcher has one
        // open, so this whole call is ONE undo entry, which is exactly the
        // granularity the settle-only rule is built on.
        const Clock::time_point changeStart = Clock::now ();
        if (updating) {
            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            // Nothing in API_Element changes — the geometry is entirely in the
            // memo — but Change still needs the memo mask to know what to take.
            err = ACAPI_Element_Change (&element, &mask, &memo, APIMemoMask_AddPars, true);
        }
        else {
            err = ACAPI_Element_Create (&element, &memo);
        }
        const double changeMs = MsSince (changeStart);
        ACAPI_DisposeElemMemoHdls (&memo);

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL (
                updating ? "ACAPI_Element_Change" : "ACAPI_Element_Create", err,
                GS::UniString::Printf (
                    "%d vertices, %d triangles (%.0f KB of array data)", (int) vertexCount, (int) triangleCount,
                    (double) ((mesh.verts.GetSize () + triDoubles.GetSize ()) * sizeof (double)) / 1024.0)));
        }

        GS::ObjectState os;
        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        os.Add ("elementId", elementId);
        os.Add ("placed", !updating);
        os.Add ("vertexCount", (GS::Int32) vertexCount);
        os.Add ("triangleCount", (GS::Int32) triangleCount);
        os.Add ("arrayBytes", (GS::Int32) ((mesh.verts.GetSize () + triDoubles.GetSize ()) * sizeof (double)));

        GS::ObjectState timings;
        timings.Add ("generateMs", genMs);
        timings.Add ("fillMs", fillMs);
        timings.Add ("buildMs", genMs + fillMs); // t0
        timings.Add ("changeMs", changeMs);      // t1

        // ---- t2: force the 3D conversion ----------------------------------
        //
        // ⚠️ THIS SEAM IS ONLY MEANINGFUL IF IT REALLY CONVERTS. The header says
        // it does — "The 3D data of the element doesn't need to exist; Archicad
        // will convert the element to 3D independently of any existing 3D window
        // data" (ACAPinc.h:4069) — but a documented sentence is not a
        // measurement, and a cached answer would make t2 a number about nothing.
        // That is benchmark item U7, and the check is to compare t2 against the
        // caller's wall clock on the big case: if t2 collapses while the wall
        // clock does not, it is reading cache.
        bool measure3D = true;
        params.Get ("measure3D", measure3D);
        if (measure3D) {
            API_ElemInfo3D info3D = {};
            const Clock::time_point convertStart = Clock::now ();
            const GSErrCode infoErr = ACAPI_ModelAccess_Get3DInfo (element.header, &info3D);
            timings.Add ("convertMs", MsSince (convertStart)); // t2

            GS::ObjectState model;
            if (infoErr == NoError) {
                model.Add ("firstBody", (GS::Int32) info3D.fbody);
                model.Add ("lastBody", (GS::Int32) info3D.lbody);
                model.Add ("bodyCount", (GS::Int32) (info3D.lbody - info3D.fbody + 1));

                // READ THE BODY BACK. NoError is not proof the script ran: an
                // empty body and a correct one both return NoError, and "the
                // parameters were accepted" is not the question — "did the 3D
                // script re-run over the NEW data" is (benchmark item U2). A
                // polygon count equal to the triangle count is that proof.
                API_Component3D component = {};
                component.header.typeID = API_BodyID;
                component.header.index = info3D.fbody;
                if (ACAPI_ModelAccess_GetComponent (&component) == NoError) {
                    model.Add ("polygonCount", (GS::Int32) component.body.nPgon);
                    model.Add ("edgeCount", (GS::Int32) component.body.nEdge);
                    model.Add ("modelVertexCount", (GS::Int32) component.body.nVert);
                    model.Add ("closed", (component.body.status & APIBody_Closed) != 0);
                    model.Add ("matchesRequest", component.body.nPgon == triangleCount);
                }
            }
            else {
                model.Add ("error",
                           GS::UniString ("ACAPI_ModelAccess_Get3DInfo failed — the element has no 3D representation. "
                                          "Either pvEnabled is off, or the 3D script rejected the arrays."));
            }
            os.Add ("model", model);
        }

        os.Add ("timings", timings);
        return os;
    }
};

// ---------------------------------------------------------------------------
// The plan half
// ---------------------------------------------------------------------------
//
// ⚠️ THIS IS NOT THE 3D BENCHMARK WITH DIFFERENT NUMBERS. Two things change, and
// both change what the answer means:
//
//   * the 3D route pays its cost ONCE at conversion and then navigates on the
//     GPU. The plan does not convert to anything — the 2D script's primitives
//     are what the floor plan draws, so the cost sits between the model and
//     every redraw rather than in front of them;
//   * `POLY2_` is VARIADIC, and `PUT`/`GET(NSP)` is the only bulk argument feed
//     GDL has. So the same picture can be drawn as many small polygons (many
//     statements, few arguments each) or as one long polyline (one statement,
//     thousands of arguments). If GDL's per-argument cost is far below its
//     per-statement cost, those two are NOT the same price, and a plan feed
//     should be built to prefer the cheap one. That is the question this half
//     exists to answer, and the 3D half could not ask it — `VERT`/`EDGE`/`PGON`
//     are fixed-arity, so there was never a choice to make.
struct PlanMesh {
    GS::Array<double> verts; // x, y, POLY2_ status triples
    GS::Array<double> polys; // (firstNode, nodeCount) pairs, 1-BASED
};

// GRID: `count` closed quads on a near-square lattice. Many POLY2_ statements,
// four nodes each — the statement-bound shape.
void BuildPlanGrid (Int32 count, double cell, double jitter, PlanMesh& mesh)
{
    Int32 cols = (Int32) std::ceil (std::sqrt ((double) count));
    if (cols < 1)
        cols = 1;

    mesh.verts.SetCapacity ((USize) count * 12);
    mesh.polys.SetCapacity ((USize) count * 2);

    const double size = cell * 0.7 + jitter;
    for (Int32 i = 0; i < count; ++i) {
        const double ox = (double) (i % cols) * cell;
        const double oy = (double) (i / cols) * cell;
        const Int32 first = i * 4 + 1; // 1-based, as the 2D script indexes it
        // status 1 = the segment leaving this node is visible. The polygon is
        // closed by POLY2_'s frame_fill bit, not by repeating the first node.
        const double corners[4][2] = { { ox, oy }, { ox + size, oy }, { ox + size, oy + size }, { ox, oy + size } };
        for (const auto& corner : corners) {
            mesh.verts.Push (corner[0]);
            mesh.verts.Push (corner[1]);
            mesh.verts.Push (1.0);
        }
        mesh.polys.Push ((double) first);
        mesh.polys.Push (4.0);
    }
}

// SPIRAL: ONE polyline of `count` segments. One POLY2_ statement carrying
// thousands of arguments through the parameter buffer — the argument-bound
// shape, and the direct comparison the grid exists to be measured against.
void BuildPlanSpiral (Int32 count, double spacing, double jitter, PlanMesh& mesh)
{
    const Int32 nodes = count + 1;
    mesh.verts.SetCapacity ((USize) nodes * 3);

    const double turns = 12.0;
    const double twoPi = 2.0 * 3.14159265358979323846;
    for (Int32 i = 0; i < nodes; ++i) {
        const double t = (double) i / (double) count;
        const double angle = t * turns * twoPi;
        const double radius = (0.5 + t * spacing * 20.0) + jitter;
        mesh.verts.Push (radius * std::cos (angle));
        mesh.verts.Push (radius * std::sin (angle));
        mesh.verts.Push (1.0);
    }
    mesh.polys.Push (1.0);
    mesh.polys.Push ((double) nodes);
}

// ---------------------------------------------------------------------------
// Counting what the 2D script actually emitted
// ---------------------------------------------------------------------------
//
// ⚠️ THIS IS `ShapePrims` PUT TO ITS REAL USE. The handoff's §1.1 killed it as a
// geometry INJECTOR — it hands primitives out, and they are 2D — and said to
// keep it as a diagnostic instead. This is that diagnostic: it re-runs the
// element's 2D drawing and reports what the interpreter produced, which is both
// the plan's `t2` seam and the only proof that the script re-ran over new data.
//
// The counter is a file-static because `ShapePrimsProc` is a bare C function
// pointer with no user-data argument. That is safe here and only here: the call
// is main-thread-only and the SDK refuses to nest these procedures at all
// (`APIERR_NESTING`), so there can never be two counts in flight.
struct PrimCount {
    Int32 total = 0;
    Int32 polys = 0;
    Int32 plines = 0;
    Int32 lines = 0;
    Int32 other = 0;
};

PrimCount g_primCount;

GSErrCode CountPrimitive (const API_PrimElement* prim, const void*, const void*, const void*)
{
    if (prim == nullptr)
        return NoError;
    switch (prim->header.typeID) {
        case API_PrimPolyID:
            ++g_primCount.polys;
            ++g_primCount.total;
            break;
        case API_PrimPLineID:
            ++g_primCount.plines;
            ++g_primCount.total;
            break;
        case API_PrimLineID:
            ++g_primCount.lines;
            ++g_primCount.total;
            break;
        // Control codes (Beg/End, hatch borders, element refs) are NOT drawing
        // primitives and must not inflate the count — the number is supposed to
        // be comparable with the polygon count the feed asked for.
        default:
            if (prim->header.typeID == API_PrimPointID || prim->header.typeID == API_PrimArcID ||
                prim->header.typeID == API_PrimTextID || prim->header.typeID == API_PrimTriID) {
                ++g_primCount.other;
                ++g_primCount.total;
            }
            break;
    }
    return NoError;
}

// ---------------------------------------------------------------------------
// Tapioca.PlanPreviewFeed
// ---------------------------------------------------------------------------
class PlanPreviewFeedCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "PlanPreviewFeed";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int32 count = 1;
        params.Get ("count", count);
        if (count < 1) {
            return NativeCommandResult::Failure (
                EVP_FAIL ("count must be at least 1 — it is the number of quads for shape \"grid\", "
                          "or the number of segments in the single polyline for shape \"spiral\"",
                          "Tapioca.PlanPreviewFeed"));
        }

        GS::UniString shape = "grid";
        params.Get ("shape", shape);
        const bool spiral = (shape == "spiral");
        if (!spiral && shape != "grid") {
            return NativeCommandResult::Failure (
                GS::UniString ("shape must be \"grid\" (many small polygons — statement-bound) or \"spiral\" "
                               "(one long polyline — argument-bound). Comparing those two IS the plan benchmark."));
        }

        // Same reason as the 3D feed: two pushes of identical bytes make Archicad
        // regenerate nothing, and the second one would be timed as if it had.
        GS::Int32 jitter = 0;
        params.Get ("jitter", jitter);

        API_LibPart libPart;
        GS::UniString refusal;
        if (!ResolvePreviewPart (kPlanPartName, libPart, refusal))
            return NativeCommandResult::Failure (refusal);

        // ---- t0a: generate ------------------------------------------------
        const Clock::time_point genStart = Clock::now ();
        PlanMesh mesh;
        if (spiral)
            BuildPlanSpiral (count, 0.05, 0.001 * (double) jitter, mesh);
        else
            BuildPlanGrid (count, 0.4, 0.001 * (double) jitter, mesh);
        const double genMs = MsSince (genStart);

        const Int32 nodeCount = (Int32) (mesh.verts.GetSize () / 3);
        const Int32 polyCount = (Int32) (mesh.polys.GetSize () / 2);

        // ---- fetch the element to write into -------------------------------
        GS::ObjectState elementIdIn;
        GS::UniString guidIn;
        const bool updating =
            params.Get ("elementId", elementIdIn) && elementIdIn.Get ("guid", guidIn) && !guidIn.IsEmpty ();

        API_Element element = {};
        API_ElementMemo memo = {};
        element.header.type = API_ObjectID;
        GSErrCode err = NoError;

        if (updating) {
            element.header.guid = APIGuidFromString (guidIn.ToCStr ().Get ());
            err = ACAPI_Element_Get (&element);
            if (err == NoError)
                err = ACAPI_Element_GetMemo (element.header.guid, &memo, APIMemoMask_AddPars);
            if (err != NoError) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_Get/GetMemo", err,
                                    GS::UniString::Printf ("elementId.guid %T — pass an object this command placed, "
                                                           "or omit elementId to place a new one",
                                                           guidIn.ToPrintf ())));
            }
            if (element.object.libInd != libPart.index) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (
                    GS::UniString ("That element is not a TapiocaPlanPreview object. Note that the 3D and plan "
                                   "carriers are SEPARATE parts on purpose, so a guid from GdlPreviewFeed is "
                                   "refused here rather than half-written."));
            }
        }
        else {
            err = ACAPI_Element_GetDefaults (&element, &memo);
            if (err != NoError) {
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_ObjectID (tool defaults)"));
            }

            double a = 0.0, b = 0.0;
            Int32 addParNum = 0;
            API_AddParType** libParams = nullptr;
            const GSErrCode paramErr = ACAPI_LibraryPart_GetParams (libPart.index, &a, &b, &addParNum, &libParams);
            if (paramErr != NoError) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_LibraryPart_GetParams", paramErr, GS::UniString (kPlanPartName)));
            }
            ACAPI_DisposeAddParHdl (&memo.params);
            memo.params = libParams;

            element.object.libInd = libPart.index;
            element.object.xRatio = a;
            element.object.yRatio = b;

            double x = 0.0, y = 0.0;
            params.Get ("x", x);
            params.Get ("y", y);
            element.object.pos.x = x;
            element.object.pos.y = y;

            GS::UniString layerErr;
            if (!ResolveLayerParam (params, element.header, layerErr)) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (layerErr);
            }
        }

        // ---- t0b: build the handles ---------------------------------------
        const Clock::time_point fillStart = Clock::now ();
        const bool wroteVerts =
            WriteDoubleArray (memo, kPlanVertsParam, nodeCount, 3, mesh.verts.GetContent (), mesh.verts.GetSize ());
        const bool wrotePolys =
            WriteDoubleArray (memo, kPlanPolysParam, polyCount, 2, mesh.polys.GetContent (), mesh.polys.GetSize ());
        const double fillMs = MsSince (fillStart);

        if (!wroteVerts || !wrotePolys) {
            ACAPI_DisposeElemMemoHdls (&memo);
            return NativeCommandResult::Failure (
                GS::UniString ("Could not write ") + (wroteVerts ? kPlanPolysParam : kPlanVertsParam) +
                " — the loaded TapiocaPlanPreview has no parameter by that name. An older build of "
                "the part is the usual cause; recompile it. NOTHING was written.");
        }

        GS::Int32 revision = 0;
        if (params.Get ("revision", revision))
            WriteScalar (memo, "pv2Revision", (double) revision);
        GS::Int32 frame = 0;
        if (params.Get ("frame", frame))
            WriteScalar (memo, "pv2Frame", (double) frame);
        bool enabled = true;
        if (params.Get ("enabled", enabled))
            WriteScalar (memo, "pv2Enabled", enabled ? 1.0 : 0.0);

        // ---- t1: the element write ----------------------------------------
        const Clock::time_point changeStart = Clock::now ();
        if (updating) {
            API_Element mask = {};
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            err = ACAPI_Element_Change (&element, &mask, &memo, APIMemoMask_AddPars, true);
        }
        else {
            err = ACAPI_Element_Create (&element, &memo);
        }
        const double changeMs = MsSince (changeStart);
        ACAPI_DisposeElemMemoHdls (&memo);

        if (err != NoError) {
            return NativeCommandResult::Failure (
                EVP_ACAPI_FAIL (updating ? "ACAPI_Element_Change" : "ACAPI_Element_Create", err,
                                GS::UniString::Printf ("%d nodes in %d polygon(s)", (int) nodeCount, (int) polyCount)));
        }

        GS::ObjectState os;
        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        os.Add ("elementId", elementId);
        os.Add ("placed", !updating);
        os.Add ("shape", shape);
        os.Add ("nodeCount", (GS::Int32) nodeCount);
        os.Add ("polygonCount", (GS::Int32) polyCount);
        os.Add ("arrayBytes", (GS::Int32) ((mesh.verts.GetSize () + mesh.polys.GetSize ()) * sizeof (double)));

        GS::ObjectState timings;
        timings.Add ("generateMs", genMs);
        timings.Add ("fillMs", fillMs);
        timings.Add ("buildMs", genMs + fillMs);
        timings.Add ("changeMs", changeMs);

        // ---- t2: force the 2D drawing --------------------------------------
        bool measure2D = true;
        params.Get ("measure2D", measure2D);
        if (measure2D) {
            g_primCount = PrimCount ();
            const Clock::time_point drawStart = Clock::now ();
            const GSErrCode drawErr = ACAPI_DrawingPrimitive_ShapePrims (element.header, CountPrimitive);
            timings.Add ("drawMs", MsSince (drawStart));

            GS::ObjectState drawn;
            if (drawErr == NoError) {
                drawn.Add ("primitives", (GS::Int32) g_primCount.total);
                drawn.Add ("polygons", (GS::Int32) g_primCount.polys);
                drawn.Add ("polylines", (GS::Int32) g_primCount.plines);
                drawn.Add ("lines", (GS::Int32) g_primCount.lines);
                drawn.Add ("other", (GS::Int32) g_primCount.other);
                // The plan's answer to "did the 2D script re-run over the new
                // data". One POLY2_ per requested polygon is what a correct run
                // looks like; Archicad may split a filled polygon into a border
                // plus hatch lines, so this is >= rather than ==.
                drawn.Add ("matchesRequest", g_primCount.total >= polyCount);
            }
            else {
                // GS::UniString(...) around the whole thing is REQUIRED: `a + b`
                // yields the lazy GS::UniString::Concatenation, which
                // ObjectState::Add cannot store.
                drawn.Add ("error", GS::UniString::Printf (
                                        "ACAPI_DrawingPrimitive_ShapePrims failed (%d). APIERR_BADDATABASE here means "
                                        "the 3D WINDOW IS ACTIVE — this call needs the floor plan to be the current "
                                        "database. Switch to the plan and run again.",
                                        (int) drawErr));
            }
            os.Add ("drawn", drawn);
        }

        os.Add ("timings", timings);
        return os;
    }
};

const NativeCommandRegistration GdlPreviewCommandRegistrations[] = {
    { "PlanPreviewFeed", &MakeRegisteredNativeCommand<PlanPreviewFeedCommand>, false,
      R"json({"type":"object","properties":{"count":{"type":"integer","minimum":1},"shape":{"type":"string"},"jitter":{"type":"integer"},"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"x":{"type":"number"},"y":{"type":"number"},"layer":{"type":"string"},"revision":{"type":"integer"},"frame":{"type":"integer"},"enabled":{"type":"boolean"},"measure2D":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"placed":{"type":"boolean"},"shape":{"type":"string"},"nodeCount":{"type":"integer"},"polygonCount":{"type":"integer"},"arrayBytes":{"type":"integer"},"timings":{"type":"object","properties":{"generateMs":{"type":"number"},"fillMs":{"type":"number"},"buildMs":{"type":"number"},"changeMs":{"type":"number"},"drawMs":{"type":"number"}},"additionalProperties":false,"required":["generateMs","fillMs","buildMs","changeMs"]},"drawn":{"type":"object","properties":{"primitives":{"type":"integer"},"polygons":{"type":"integer"},"polylines":{"type":"integer"},"lines":{"type":"integer"},"other":{"type":"integer"},"matchesRequest":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false}},"additionalProperties":false,"required":["elementId","placed","shape","nodeCount","polygonCount","arrayBytes","timings"]})json" },

    { "GdlPreviewFeed", &MakeRegisteredNativeCommand<GdlPreviewFeedCommand>, false, R"json({"type":"object","properties":{"rows":{"type":"integer","minimum":0},"cols":{"type":"integer","minimum":0},"jitter":{"type":"integer"},"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]},"x":{"type":"number"},"y":{"type":"number"},"layer":{"type":"string"},"revision":{"type":"integer"},"bodyStatus":{"type":"integer"},"enabled":{"type":"boolean"},"measure3D":{"type":"boolean"}},"additionalProperties":false})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"placed":{"type":"boolean"},"vertexCount":{"type":"integer"},"triangleCount":{"type":"integer"},"arrayBytes":{"type":"integer"},"timings":{"type":"object","properties":{"generateMs":{"type":"number"},"fillMs":{"type":"number"},"buildMs":{"type":"number"},"changeMs":{"type":"number"},"convertMs":{"type":"number"}},"additionalProperties":false,"required":["generateMs","fillMs","buildMs","changeMs"]},"model":{"type":"object","properties":{"firstBody":{"type":"integer"},"lastBody":{"type":"integer"},"bodyCount":{"type":"integer"},"polygonCount":{"type":"integer"},"edgeCount":{"type":"integer"},"modelVertexCount":{"type":"integer"},"closed":{"type":"boolean"},"matchesRequest":{"type":"boolean"},"error":{"type":"string"}},"additionalProperties":false}},"additionalProperties":false,"required":["elementId","placed","vertexCount","triangleCount","arrayBytes","timings"]})json" },
};

} // namespace

NativeCommandRegistrations GetGdlPreviewCommandRegistrations ()
{
    return MakeRegistrationView (GdlPreviewCommandRegistrations);
}

} // namespace geomsrv

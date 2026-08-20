#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/CreateCommands.hpp"
#include "NativeCommands/CommandRegistration.hpp"
#include "NativeCommands/CommandUtils.hpp"      // AttributeNameToIndex

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace geomsrv {

namespace {

// ---------------------------------------------------------------------------
// Tapioca.PlaceLevelDimension { x, y, mode: "static"|"associative", value?, parent?, text? }
//
// Places an API_LevelDimensionID (=13) marker. Two modes:
//   "static"      value is a fixed Z (meters); independent of any element.
//   "associative" tracks the Z of parent.elementId.guid live in Archicad —
//                 the element's type is looked up automatically via its header,
//                 so the caller only needs the GUID (typically a just-created
//                 Mesh from CreateMesh below).
// `text` optionally overrides the note content (API_NoteContent_Custom);
// omitted, it keeps Archicad's own default (typically the measured value).
//
// Verified against real AC29 headers + a real DevKit example in the SAME
// AddOnCommand context (Examples/AddOnCommandTest CreateColumnCommand):
// GetDefaults -> fill fields -> ACAPI_CallUndoableCommand{ Element_Create } is
// the confirmed pattern for element creation from a JSON Add-On Command.
// (An earlier AI-generated Archicad doc's `ceilNum` field does NOT exist on
// API_LevelDimensionType in AC29 — verified absent from the real header, not
// used here.)
// ---------------------------------------------------------------------------
class PlaceLevelDimensionCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "PlaceLevelDimension"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;
        double x = 0.0, y = 0.0;
        if (!params.Get ("x", x) || !params.Get ("y", y)) {
            return NativeCommandResult::Failure ("need x and y");
        }

        GS::UniString mode ("static");
        params.Get ("mode", mode);
        const bool associative = (mode == "associative");

        double value = 0.0;
        GS::UniString parentGuidStr, text;
        GS::ObjectState parent, parentElementId;
        const bool haveValue  = params.Get ("value", value);
        const bool haveParent = params.Get ("parent", parent) &&
                                parent.Get ("elementId", parentElementId) &&
                                parentElementId.Get ("guid", parentGuidStr) &&
                                !parentGuidStr.IsEmpty ();
        const bool haveText   = params.Get ("text", text);

        if (associative && !haveParent) {
            return NativeCommandResult::Failure ("mode=\"associative\" needs parent.elementId.guid");
        }
        if (!associative && !haveValue) {
            return NativeCommandResult::Failure ("mode=\"static\" needs value (the fixed Z)");
        }

        API_Element element = {};
        element.header.type = API_LevelDimensionID;
        GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_LevelDimensionID"));
        }

        element.levelDimension.loc.x = x;
        element.levelDimension.loc.y = y;

        if (associative) {
            const API_Guid parentGuid = APIGuidFromString (parentGuidStr.ToCStr ());
            API_Elem_Head parentHead = {};
            parentHead.guid = parentGuid;
            if (const GSErrCode headErr = ACAPI_Element_GetHeader (&parentHead); headErr != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetHeader", headErr, GS::UniString ("parentGuid ") + parentGuidStr + " - not found, deleted, or not in your Teamwork workspace"));
            }
            element.levelDimension.staticLevel = false;
            element.levelDimension.parentType  = parentHead.type;   // auto-detected, not guessed
            element.levelDimension.parentGuid  = parentGuid;
        } else {
            element.levelDimension.staticLevel = true;
            element.levelDimension.level = value;
        }

        if (haveText) {
            element.levelDimension.note1.contentType = API_NoteContent_Custom;
            element.levelDimension.note1.contentUStr = new GS::UniString (text);
        }

        // NO undo scope here — see WriteCommand. The caller has one open.
        err = ACAPI_Element_Create (&element, nullptr);

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_Create", err, GS::UniString::Printf ("level dimension, mode=%T at (%.3f, %.3f)", mode.ToPrintf (), x, y)));
        }

        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        os.Add ("elementId", elementId);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.CreateMesh { outline:[x0,y0,x1,y1,...], polyZ?:[z0,z1,...], baseLevel?,
//                  ridgeCoords?:[x,y,z, x,y,z,...], ridgeCounts?:[n0,n1,...],
//                  skirtLevel?, skirt?, floorInd?, onFloorPlan?, layer? }
//
// `outline` is the DISTINCT boundary ring (do not repeat the closing point —
// this command closes it). `polyZ` is one Z per outline vertex, same order/
// count; omitted, the boundary is flat at `baseLevel` (default 0).
//
// `ridgeCoords`/`ridgeCounts` are the internal, user-editable level lines
// (topography "ridges"/breaklines): all ridge points concatenated flat in
// ridgeCoords, with ridgeCounts giving the point count of each ridge in order
// (e.g. ridgeCounts=[3,4] means the first 3 XYZ triples are ridge 1, the next
// 4 are ridge 2). No nested-array JSON needed on the Python side.
//
// VERIFIED against a real DevKit creation example (Element_Test/Element_Basics
// .cpp Do_CreateIrregularMesh): the outer polygon's coords/pends/meshPolyZ
// memo arrays are 1-indexed with a closing repeat of point 1 at index nCoords
// (nCoords therefore = distinct-point-count + 1) — reproduced exactly below.
//
// LEVEL LINES — the convention, now settled. It was originally guessed by analogy
// with `pends` (1-indexed, unused [0], nSubLines+1 slots) and that was wrong on all
// three counts. **VERIFIED on real Archicad data 2026-07-26**, by reading meshes back
// through EvP.GetElementDetails (kind "mesh"): a hand-made 5-point ridge reports
// nSubLines=1, meshLevelEnds=[5], vertexIds=[1..5] — one slot, no unused [0]. So
// meshLevelCoords and meshLevelEnds are both 0-indexed, ends[] holds exactly
// nSubLines cumulative EXCLUSIVE ends, and element.mesh.levelLines must be declared.
// Fixed below; see plan §E16.5. The read kind was built FIRST precisely so this could
// be measured rather than argued.
// Note a real terrain mesh's level lines are mostly ONE POINT each (level points — 50
// to 305 per mesh in that survey), so ridgeCounts=[1,1,1,…] is a normal input here.
//
// THE Z FRAME, and the skirt bug it caused. `baseLevel` is the mesh's BASE PLANE,
// measured from its story's level; `polyZ` and every ridge Z are measured FROM THAT
// PLANE, not from project zero (the DevKit's Do_CreateIrregularMesh never sets
// mesh.level and writes meshPolyZ 1..4 — heights above the base plane). `skirtLevel`
// is then the distance of the mesh's SOLID BOTTOM below the base plane.
//   Leaving baseLevel at 0 while feeding real survey altitudes (~94 m) therefore
// produces a mesh whose skirt is 94 m deep — a wall of earth from sea level up to
// the terrain. That is exactly what the first live run showed. The fix is the
// caller's: put the base plane just under the terrain (baseLevel = minZ - 1.0) and
// send heights relative to it. `skirtLevel` and `skirt` are exposed here so the
// caller can say it in one call instead of creating and then modifying.
//
// `onFloorPlan` exists because element creation targets the CURRENT DATABASE. A
// command run while the user is looking at a WORKSHEET (which is where a surveyor's
// labels live) would otherwise create the mesh inside that worksheet, where it is 2D
// and invisible in the model. With onFloorPlan the command switches to the floor plan,
// creates, and switches back — the same borrow-and-restore shape
// DrawingCommands.cpp's PlaceDrawingFromView uses, restore on EVERY exit path.
// `floorInd` then names the story (0 = ground floor); omitted, the element keeps
// whatever story GetDefaults chose.
// ---------------------------------------------------------------------------
class CreateMeshCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "CreateMesh"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<double> outline;
        if (!params.Get ("outline", outline) || outline.GetSize () < 6 || (outline.GetSize () % 2) != 0) {
            return NativeCommandResult::Failure ("need outline=[x0,y0,x1,y1,...] with >=3 points (even count)");
        }
        const Int32 distinct = static_cast<Int32> (outline.GetSize () / 2);

        GS::Array<double> polyZ;
        const bool haveZ = params.Get ("polyZ", polyZ);
        if (haveZ && static_cast<Int32> (polyZ.GetSize ()) != distinct) {
            return NativeCommandResult::Failure ("polyZ must have exactly one Z per outline vertex");
        }

        // The same degenerate-contour guard CreateRoof carries, for the same reason: a
        // JSON *integer* does not survive Get<GS::Array<double>> (it reads as 0.0), so an
        // outline of [0.0, 0.0, 4, 0.0, 4, 4, 0.0, 4] arrives as four coincident points.
        // CreateRoof answered APIERR_IRREGULARPOLY and cost this project six rounds; a
        // mesh would have been WORSE, because a collapsed mesh can be created without any
        // error at all. Caught here with a message that names the cause.
        {
            double area2 = 0.0;
            for (Int32 i = 0; i < distinct; ++i) {
                const Int32 j = (i + 1) % distinct;
                area2 += outline[i * 2 + 0] * outline[j * 2 + 1]
                       - outline[j * 2 + 0] * outline[i * 2 + 1];
            }
            if (std::fabs (area2) < 1e-9) {
                return NativeCommandResult::Failure (GS::UniString ("outline encloses no area — the contour is "
                        "degenerate (points coincide or are collinear). If you passed whole "
                        "numbers, send them as REALS: a JSON integer (4) does not survive as "
                        "a double and arrives as 0.0, while 4.0 does."));
            }
        }

        double baseLevel = 0.0;
        params.Get ("baseLevel", baseLevel);

        GS::Array<double>   ridgeCoords;
        GS::Array<GS::Int32> ridgeCounts;
        const bool haveRidges = params.Get ("ridgeCoords", ridgeCoords) && params.Get ("ridgeCounts", ridgeCounts);
        if (haveRidges) {
            Int64 total = 0;
            for (GS::Int32 c : ridgeCounts) total += c;
            if (total * 3 != static_cast<Int64> (ridgeCoords.GetSize ())) {
                return NativeCommandResult::Failure ("sum(ridgeCounts)*3 must equal ridgeCoords.size()");
            }
        }

        // --- borrow the floor plan database, if asked ------------------------
        // Everything after this point must go out through `restore ()`, or a
        // command run from a worksheet silently leaves the user in another
        // window. Same contract as PlaceDrawingFromView.
        bool             switchedDb = false;
        API_DatabaseInfo originalDb = {};
        bool onFloorPlan = false;
        params.Get ("onFloorPlan", onFloorPlan);
        if (onFloorPlan) {
            if (const GSErrCode dbErr = ACAPI_Database_GetCurrentDatabase (&originalDb); dbErr != NoError) {
                return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Database_GetCurrentDatabase", dbErr,
                                                                      "reading the current database before creating a mesh on the floor plan"));
            }
            if (originalDb.typeID != APIWind_FloorPlanID) {
                API_DatabaseInfo target = {};
                target.typeID = APIWind_FloorPlanID;
                if (const GSErrCode dbErr = ACAPI_Database_ChangeCurrentDatabase (&target); dbErr != NoError) {
                    return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Database_ChangeCurrentDatabase", dbErr,
                                                                          "activating the floor plan to create a mesh in the model rather than in the current 2D database"));
                }
                switchedDb = true;
            }
        }
        const auto restore = [&switchedDb, &originalDb] () {
            if (switchedDb)
                ACAPI_Database_ChangeCurrentDatabase (&originalDb);
        };

        API_Element element = {};
        element.header.type = API_MeshID;
        GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
        if (err != NoError) {
            restore ();
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_MeshID"));
        }
        element.mesh.level = baseLevel;

        // Story. Omitted => whatever GetDefaults chose (the user's current story).
        GS::Int32 floorInd = 0;
        if (params.Get ("floorInd", floorInd))
            element.header.floorInd = (short) floorInd;

        GS::UniString layerErr;
        if (!ResolveLayerParam (params, element.header, layerErr)) {
            restore ();
            return NativeCommandResult::Failure (layerErr);
        }

        // Skirt. The enum spelling matches what EvP.GetElementDetails reports for
        // kind "mesh", so a read and a write name the same thing; the numbers are
        // the header's ("Skirt: 1 = yes; 2 = no bottom; 3 = no").
        GS::UniString skirtName;
        if (params.Get ("skirt", skirtName) && !skirtName.IsEmpty ()) {
            if      (skirtName == "SolidBodyWithSkirt")       element.mesh.skirt = 1;
            else if (skirtName == "WithSkirt")                element.mesh.skirt = 2;
            else if (skirtName == "SurfaceOnlyWithoutSkirt")  element.mesh.skirt = 3;
            else {
                restore ();
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown skirt: %T (want SolidBodyWithSkirt, WithSkirt or SurfaceOnlyWithoutSkirt)",
                                                                                      skirtName.ToPrintf ()),
                                                                "EvP.CreateMesh"));
            }
        }
        // Explicit 0 rather than "leave the default": GetDefaults carries whatever
        // skirt depth the Mesh tool was last left with, and inheriting it is how a
        // caller that carefully placed its base plane still gets a mesh with a
        // surprise slab hanging under it.
        double skirtLevel = 0.0;
        params.Get ("skirtLevel", skirtLevel);
        element.mesh.skirtLevel = skirtLevel;

        const Int32 nCoords = distinct + 1;   // +1: Archicad's polygon convention closes the ring by repeating point 1
        element.mesh.poly.nCoords    = nCoords;
        element.mesh.poly.nSubPolys  = 1;
        element.mesh.poly.nArcs      = 0;

        API_ElementMemo memo = {};
        memo.coords = reinterpret_cast<API_Coord**> (
            BMAllocateHandle ((nCoords + 1) * sizeof (API_Coord), ALLOCATE_CLEAR, 0));
        memo.pends = reinterpret_cast<Int32**> (
            BMAllocateHandle (2 * sizeof (Int32), ALLOCATE_CLEAR, 0));
        memo.parcs = reinterpret_cast<API_PolyArc**> (
            BMAllocateHandle (0, ALLOCATE_CLEAR, 0));
        memo.meshPolyZ = reinterpret_cast<double**> (
            BMAllocateHandle ((nCoords + 1) * sizeof (double), ALLOCATE_CLEAR, 0));

        for (Int32 i = 0; i < distinct; ++i) {
            (*memo.coords)[i + 1].x = outline[i * 2 + 0];
            (*memo.coords)[i + 1].y = outline[i * 2 + 1];
            (*memo.meshPolyZ)[i + 1] = haveZ ? polyZ[i] : 0.0;
        }
        (*memo.coords)[nCoords]   = (*memo.coords)[1];      // close the ring
        (*memo.meshPolyZ)[nCoords] = (*memo.meshPolyZ)[1];
        (*memo.pends)[1] = nCoords;

        if (haveRidges && !ridgeCounts.IsEmpty ()) {
            const Int32 nLevelCoords = static_cast<Int32> (ridgeCoords.GetSize () / 3);
            const Int32 nSubLines    = static_cast<Int32> (ridgeCounts.GetSize ());

            // The element must DECLARE its level lines; the memo handles alone are not
            // enough. Omitting this was one of the three defects the read exposed.
            element.mesh.levelLines.nCoords   = nLevelCoords;
            element.mesh.levelLines.nSubLines = nSubLines;

            memo.meshLevelCoords = reinterpret_cast<API_MeshLevelCoord**> (
                BMAllocateHandle (nLevelCoords * sizeof (API_MeshLevelCoord), ALLOCATE_CLEAR, 0));
            // EXACTLY nSubLines slots, 0-indexed — not nSubLines+1 with an unused [0].
            memo.meshLevelEnds = reinterpret_cast<Int32**> (
                BMAllocateHandle (nSubLines * sizeof (Int32), ALLOCATE_CLEAR, 0));

            for (Int32 i = 0; i < nLevelCoords; ++i) {
                (*memo.meshLevelCoords)[i].c.x = ridgeCoords[i * 3 + 0];
                (*memo.meshLevelCoords)[i].c.y = ridgeCoords[i * 3 + 1];
                (*memo.meshLevelCoords)[i].c.z = ridgeCoords[i * 3 + 2];
                // 1-based, matching what Archicad's own meshes report (a read of a
                // hand-made mesh came back vertexIds=[1..5]).
                (*memo.meshLevelCoords)[i].vertexID = i + 1;
            }
            // ends[i] = cumulative EXCLUSIVE end, so line i spans
            // meshLevelCoords[ends[i-1] .. ends[i]-1] with ends[-1] taken as 0.
            Int32 cum = 0;
            for (Int32 i = 0; i < nSubLines; ++i) {
                cum += ridgeCounts[i];
                (*memo.meshLevelEnds)[i] = cum;
            }
        }

        // NO undo scope here — see WriteCommand. The caller has one open.
        err = ACAPI_Element_Create (&element, &memo);

        ACAPI_DisposeElemMemoHdls (&memo);
        restore ();

        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_Create", err, GS::UniString::Printf ("mesh, %d outline pts, polyZ=%s, %u ridge pts", (int) distinct, haveZ ? "yes" : "no", (unsigned) (ridgeCoords.GetSize () / 3))));
        }

        GS::ObjectState elementId;
        elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
        os.Add ("elementId", elementId);
        // Echoed so the caller's log records the Z frame the mesh actually got —
        // the one thing a dry run cannot show and the skirt bug turned on.
        os.Add ("baseLevel",  baseLevel);
        os.Add ("skirtLevel", skirtLevel);
        os.Add ("floorInd",   (GS::Int32) element.header.floorInd);
        os.Add ("switchedToFloorPlan", switchedDb);
        return os;
    }
};

// AttributeNameToIndex -> NativeCommands/CommandUtils.hpp (also used by
// GetAttributeInfo and ControlPalette — three call sites, hence shared).
// ResolveStory and ResolveLayerParam -> the same header: both now serve this
// domain AND NativeCommands/RoofCreateCommands.cpp / DraftingCommands.cpp.


// ---------------------------------------------------------------------------
// EvP.CreateWall — one or more walls in a single call (one undo step; the batch
// shape absorbed from Tapir's CreateElementsCommandBase). Geometry is flat
// parallel arrays, EvP style (mirrors CreateMesh); one wall per begC/endC pair.
//
//   begX,begY,endX,endY : double[N]   reference-line endpoints of each wall
//   arcAngles           : double[N]?  arc central angle per wall (radians); 0 or
//                                      omitted => straight. Carry a hole ring's
//                                      bulge through here for a curved wall.
//   base                : double      world-Z of the wall bottom
//   floorInd            : int?        story that OWNS the walls; omitted => nearest
//                                      story at/below `base`. When given, `base` is
//                                      measured from that story's level.
//   height              : double      wall height above its bottom
//   structure           : "basic"|"composite"|"profile"  (default: GetDefaults)
//   attrName            : string      building-material / composite / profile NAME
//   thickness           : double?     basic-structure wall thickness (metres)
//   refLine             : "outside"|"center"|"inside"|"coreOutside"|"coreCenter"|
//                         "coreInside"   (default: GetDefaults)
//   flipped             : bool?        mirror the body across the reference line
//                                      (refLine + flipped = the "which side of the
//                                      edge does the body grow" knobs; verify the
//                                      combination with a probe, per ShaftShell).
// ---------------------------------------------------------------------------
class CreateWallCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "CreateWall"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<double> begX, begY, endX, endY, arcAngles;
        if (!params.Get ("begX", begX) || !params.Get ("begY", begY) ||
            !params.Get ("endX", endX) || !params.Get ("endY", endY)) {
            return NativeCommandResult::Failure ("need begX/begY/endX/endY parallel arrays");
        }
        const USize n = begX.GetSize ();
        if (n == 0 || begY.GetSize () != n || endX.GetSize () != n || endY.GetSize () != n) {
            return NativeCommandResult::Failure ("begX/begY/endX/endY must be non-empty and the same length");
        }
        const bool haveArcs = params.Get ("arcAngles", arcAngles);
        if (haveArcs && arcAngles.GetSize () != n) {
            return NativeCommandResult::Failure ("arcAngles must have one angle per wall");
        }

        double base = 0.0, height = 0.0, thickness = 0.0;
        params.Get ("base", base);
        const bool haveHeight    = params.Get ("height", height);
        const bool haveThickness = params.Get ("thickness", thickness);

        GS::Int32  requestedFloor = 0;
        const bool haveFloor = params.Get ("floorInd", requestedFloor);
        short  floorInd = 0;
        double bottomOffset = 0.0;
        GS::UniString storyErr;
        if (!ResolveStory (base, haveFloor, requestedFloor, floorInd, bottomOffset, storyErr)) {
            return NativeCommandResult::Failure (storyErr);
        }

        API_Element element = {};
        element.header.type = API_WallID;
        GSErrCode err = ACAPI_Element_GetDefaults (&element, nullptr);
        if (err != NoError) {
            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_WallID"));
        }

        // Structure — resolve the picker NAME to an index into the right field.
        GS::UniString structure, attrName;
        if (params.Get ("structure", structure) && !structure.IsEmpty ()) {
            params.Get ("attrName", attrName);
            API_AttributeIndex idx;
            if (structure == "basic") {
                element.wall.modelElemStructureType = API_BasicStructure;
                if (!attrName.IsEmpty ()) {
                    if (!AttributeNameToIndex (API_BuildingMaterialID, attrName, idx)) {
                        return NativeCommandResult::Failure (GS::UniString::Printf ("building material not found: %T", attrName.ToPrintf ()));
                    }
                    element.wall.buildingMaterial = idx;
                }
            } else if (structure == "composite") {
                element.wall.modelElemStructureType = API_CompositeStructure;
                if (!AttributeNameToIndex (API_CompWallID, attrName, idx)) {
                    return NativeCommandResult::Failure (GS::UniString::Printf ("composite not found: %T", attrName.ToPrintf ()));
                }
                element.wall.composite = idx;
            } else if (structure == "profile") {
                element.wall.modelElemStructureType = API_ProfileStructure;
                element.wall.profileType = APISect_Poly;
                if (!AttributeNameToIndex (API_ProfileID, attrName, idx)) {
                    return NativeCommandResult::Failure (GS::UniString::Printf ("profile not found: %T", attrName.ToPrintf ()));
                }
                element.wall.profileAttr = idx;
            } else {
                return NativeCommandResult::Failure ("structure must be basic|composite|profile");
            }
        }

        if (haveThickness) {
            element.wall.thickness  = thickness;
            element.wall.thickness1 = thickness;
        }

        GS::UniString refLine;
        if (params.Get ("refLine", refLine)) {
            if      (refLine == "outside")      element.wall.referenceLineLocation = APIWallRefLine_Outside;
            else if (refLine == "center")       element.wall.referenceLineLocation = APIWallRefLine_Center;
            else if (refLine == "inside")       element.wall.referenceLineLocation = APIWallRefLine_Inside;
            else if (refLine == "coreOutside")  element.wall.referenceLineLocation = APIWallRefLine_CoreOutside;
            else if (refLine == "coreCenter")   element.wall.referenceLineLocation = APIWallRefLine_CoreCenter;
            else if (refLine == "coreInside")   element.wall.referenceLineLocation = APIWallRefLine_CoreInside;
        }
        bool flipped = false;
        if (params.Get ("flipped", flipped))
            element.wall.flipped = flipped;

        element.header.floorInd   = floorInd;
        element.wall.bottomOffset = bottomOffset;
        if (haveHeight)
            element.wall.height       = height;
        element.wall.type             = APIWtyp_Normal;
        element.wall.relativeTopStory = 0;   // unlink — GetDefaults may return a story-linked wall

        // One wall per segment, all sharing the resolved template. Reusing `element`
        // across creates (GetDefaults once, mutate, create) is Tapir's proven shape.
        GS::Array<GS::ObjectState> results;
        USize         createdCount = 0;
        GS::UniString firstError;
        for (USize i = 0; i < n; ++i) {
            GS::ObjectState rec;
            element.header.guid = APINULLGuid;     // reused template: never re-submit the previous guid
            element.wall.begC.x = begX[i];
            element.wall.begC.y = begY[i];
            element.wall.endC.x = endX[i];
            element.wall.endC.y = endY[i];
            element.wall.angle  = haveArcs ? arcAngles[i] : 0.0;

            // NO undo scope here — see WriteCommand. The caller has one open.
            const GSErrCode cerr = ACAPI_Element_Create (&element, nullptr);
            if (cerr != NoError) {
                const GS::UniString createError = EVP_ACAPI_FAIL ("ACAPI_Element_Create", cerr, GS::UniString::Printf ("wall %u of %u, (%.3f,%.3f)->(%.3f,%.3f)", (unsigned) i, (unsigned) begX.GetSize (), begX[i], begY[i], endX[i], endY[i]));
                if (firstError.IsEmpty ())
                    firstError = createError;
                rec.Add ("succeeded", false);
                rec.Add ("error", createError);
                results.Push (rec);
                continue;
            }
            GS::ObjectState elementId;
            elementId.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
            rec.Add ("succeeded", true);
            rec.Add ("elementId", elementId);
            results.Push (rec);
            ++createdCount;
        }

        if (createdCount == 0) {
            return NativeCommandResult::Failure (firstError.IsEmpty () ? GS::UniString ("no walls created") : firstError);
        }
        os.Add ("results", results);
        os.Add ("created", (GS::Int32) createdCount);
        os.Add ("floorInd", (GS::Int32) floorInd);
        return os;
    }
};

// ---------------------------------------------------------------------------
// EvP.CreateColumn — one or more columns in a single call (one undo step).
//
//   x, y          : double[N]   origin of each column
//   base          : double?     world-Z of the column bottom (default 0)
//   floorInd      : int?        story that OWNS the columns; omitted => nearest
//   height        : double?     column height; omitted => the tool's default
//   layer         : string?     LAYER NAME from an evp.Layer picker
//   shape         : "rectangular" | "circular" | "profile" | omitted
//   width, height2: double?     section size in METRES  (shape="rectangular")
//   diameter      : double?     section diameter, metres (shape="circular")
//   profile       : string?     PROFILE NAME from an evp.ColumnProfile picker
//                               (shape="profile"; required there)
//   material      : string?     BUILDING MATERIAL NAME, rectangular/circular only
//   angle         : double?     plan rotation of the section, radians
//   -> { count, floorInd, results: [ {succeeded, elementId?, error?} ] }
//
// THE THREE SHAPES are the three a column actually has, and they are two
// different switches, not one — which is why `shape` collapses them into one
// input a UI can offer as a radio group:
//   rectangular  API_BasicStructure   + circleBased = false
//   circular     API_BasicStructure   + circleBased = TRUE   (the flag is
//                documented as exactly this: "circular (true) or
//                rectangular/profiled (false)")
//   profile      API_ProfileStructure + profileAttr; circleBased is irrelevant,
//                the profile carries the outline
// Omit `shape` entirely and the column tool's current default stands, untouched.
//
// THE TREES-FROM-DXF CASE, and why the picker inputs are the whole point: a
// script that scatters columns wants to say "this profile, on this layer" by
// NAME, because that is what an attribute picker hands it (ControlPalette
// serialises index -> name). No name is resolved with a fallback — an
// unresolvable one is a reported error, never a silent default, so a typo
// cannot quietly place a hundred columns in the wrong material.
//
// SIZES ARE METRES, like every other length this add-on takes. A circular
// column's `diameter` is written to BOTH nominalWidth and nominalHeight (a
// circle-based section is defined by one number, and leaving height at the
// tool's default would give an ellipse-shaped bounding box).
//
// THE SEGMENT TRAP: a column's structure does NOT live on API_ColumnType. It
// lives per SEGMENT, in memo.columnSegments[i].assemblySegmentData — that is
// where modelElemStructureType and profileAttr are (verified in APIdefs_Elements.h;
// API_ColumnSegmentType -> API_AssemblySegmentData). Setting anything on
// element.column for this has no effect. The segments themselves are allocated
// by ACAPI_Element_GetDefaults(&element, &memo) — the DevKit's own Do_CreateColumn
// passes the memo for exactly this reason — so this command NEVER allocates them,
// it only overwrites what the defaults produced. That also means the profile is
// applied to EVERY segment: a script asking for "a column of this profile" means
// the whole column, and a multi-segment column with one segment changed would be
// a surprise.
// ---------------------------------------------------------------------------
class CreateColumnCommand : public WriteCommand {
public:
    GS::String GetName () const override { return "CreateColumn"; }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::ObjectState os;

        GS::Array<double> xs, ys;
        if (!params.Get ("x", xs) || !params.Get ("y", ys)) {
            return NativeCommandResult::Failure (EVP_FAIL ("need x/y parallel arrays", "EvP.CreateColumn"));
        }
        const USize n = xs.GetSize ();
        if (n == 0 || ys.GetSize () != n) {
            return NativeCommandResult::Failure (EVP_FAIL ("x/y must be non-empty and the same length", "EvP.CreateColumn"));
        }

        double base = 0.0;
        params.Get ("base", base);
        GS::Int32  requestedFloor = 0;
        const bool haveFloor = params.Get ("floorInd", requestedFloor);
        short  floorInd = 0;
        double bottomOffset = 0.0;
        GS::UniString storyErr;
        if (!ResolveStory (base, haveFloor, requestedFloor, floorInd, bottomOffset, storyErr)) {
            return NativeCommandResult::Failure (storyErr);
        }

        double height = 0.0, angle = 0.0;
        const bool haveHeight = params.Get ("height", height) && height > 0.0;
        const bool haveAngle  = params.Get ("angle", angle);

        // The shape, and the attribute names behind it, resolve ONCE before the
        // loop — each lookup walks every attribute of its type, and doing that
        // per column would make a thousand-tree scatter quadratic.
        GS::UniString shape;
        const bool haveShape = params.Get ("shape", shape) && !shape.IsEmpty ();

        const bool rectangular = (shape == "rectangular");
        const bool circular    = (shape == "circular");
        const bool profiled    = (shape == "profile");
        if (haveShape && !rectangular && !circular && !profiled) {
            return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("unknown shape: %T (want \"rectangular\", \"circular\" or \"profile\")", shape.ToPrintf ()),
                                                           "EvP.CreateColumn"));
        }

        API_AttributeIndex profileIdx, materialIdx;
        bool               haveProfile = false, haveMaterial = false;

        GS::UniString profileName;
        if (params.Get ("profile", profileName) && !profileName.IsEmpty ()) {
            if (!AttributeNameToIndex (API_ProfileID, profileName, profileIdx)) {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("profile not found: %T", profileName.ToPrintf ()), "EvP.CreateColumn"));
            }
            haveProfile = true;
        }
        // A profiled column with no profile would silently be an ordinary
        // rectangular one — the exact "created fine, wrong thing" outcome the
        // name-resolution rule above exists to prevent.
        if (profiled && !haveProfile) {
            return NativeCommandResult::Failure (EVP_FAIL ("shape=\"profile\" needs profile=\"<profile name>\"", "EvP.CreateColumn"));
        }

        GS::UniString materialName;
        if (params.Get ("material", materialName) && !materialName.IsEmpty ()) {
            if (!AttributeNameToIndex (API_BuildingMaterialID, materialName, materialIdx)) {
                return NativeCommandResult::Failure (EVP_FAIL (GS::UniString::Printf ("building material not found: %T", materialName.ToPrintf ()), "EvP.CreateColumn"));
            }
            haveMaterial = true;
        }

        // Section size. `height` is already the column's vertical extent, so the
        // rectangular section's second dimension is `height2` — ugly, but far
        // safer than overloading a key that already means something else.
        double sectionWidth = 0.0, sectionHeight = 0.0, diameter = 0.0;
        const bool haveWidth    = params.Get ("width",  sectionWidth)  && sectionWidth  > 0.0;
        const bool haveHeight2  = params.Get ("height2", sectionHeight) && sectionHeight > 0.0;
        const bool haveDiameter = params.Get ("diameter", diameter)    && diameter      > 0.0;
        if (circular && haveDiameter) {
            // One number defines a circle-based section; both fields take it, or
            // the untouched one keeps the tool's default and the section is not
            // round.
            sectionWidth  = diameter;
            sectionHeight = diameter;
        }
        const bool setSize = circular ? haveDiameter : (haveWidth || haveHeight2);

        GS::Array<GS::ObjectState> results;
        GS::Int32                  created = 0;

        for (USize i = 0; i < n; ++i) {
            GS::ObjectState rec;

            API_Element     element = {};
            API_ElementMemo memo    = {};
            element.header.type = API_ColumnID;
            // Memo passed on purpose: this is what allocates columnSegments.
            if (const GSErrCode err = ACAPI_Element_GetDefaults (&element, &memo); err != NoError) {
                ACAPI_DisposeElemMemoHdls (&memo);
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_GetDefaults", err, "API_ColumnID"));
                results.Push (rec);
                continue;
            }

            GS::UniString layerErr;
            if (!ResolveLayerParam (params, element.header, layerErr)) {
                ACAPI_DisposeElemMemoHdls (&memo);
                return NativeCommandResult::Failure (layerErr);   // a bad layer is bad for ALL of them
            }

            element.header.floorInd   = floorInd;
            element.column.origoPos.x = xs[i];
            element.column.origoPos.y = ys[i];
            element.column.bottomOffset = bottomOffset;
            if (haveHeight)
                element.column.height = height;
            if (haveAngle)
                element.column.axisRotationAngle = angle;

            // Applied to EVERY segment: a script asking for "a circular column"
            // means the whole column, and a multi-segment one with a single
            // segment changed would be a surprise.
            if (memo.columnSegments != nullptr) {
                for (UInt32 s = 0; s < element.column.nSegments; ++s) {
                    API_AssemblySegmentData& seg = memo.columnSegments[s].assemblySegmentData;

                    if (profiled) {
                        seg.modelElemStructureType = API_ProfileStructure;
                        seg.profileAttr            = profileIdx;
                    } else if (rectangular || circular) {
                        seg.modelElemStructureType = API_BasicStructure;
                        seg.circleBased            = circular;
                    }
                    // A building material is meaningful for a basic section only —
                    // a profile carries its own materials per component.
                    if (haveMaterial && !profiled)
                        seg.buildingMaterial = materialIdx;

                    if (setSize) {
                        // Unlink first: with width and height linked, writing one
                        // drags the other and the second write fights the first.
                        seg.isWidthAndHeightLinked = false;
                        if (haveWidth || circular)
                            seg.nominalWidth = sectionWidth;
                        if (haveHeight2 || circular)
                            seg.nominalHeight = sectionHeight;
                    }
                }
            }

            // NO undo scope here — see WriteCommand. The caller has one open.
            const GSErrCode err = ACAPI_Element_Create (&element, &memo);
            ACAPI_DisposeElemMemoHdls (&memo);

            if (err != NoError) {
                rec.Add ("succeeded", false);
                rec.Add ("error", EVP_ACAPI_FAIL ("ACAPI_Element_Create", err,
                                                  GS::UniString::Printf ("column at (%.3f, %.3f), shape=%T profile=%T material=%T",
                                                                         xs[i], ys[i], shape.ToPrintf (),
                                                                         profileName.ToPrintf (), materialName.ToPrintf ())));
            } else {
                const GS::UniString guid (APIGuidToString (element.header.guid).ToCStr ());
                GS::ObjectState elementId;
                elementId.Add ("guid", guid);
                rec.Add ("succeeded", true);
                rec.Add ("elementId", elementId);
                ++created;
            }
            results.Push (rec);
        }

        os.Add ("results",  results);
        os.Add ("count",    created);
        os.Add ("floorInd", (GS::Int32) floorInd);
        return os;
    }
};

const NativeCommandRegistration CreateCommandRegistrations[] = {
    { "PlaceLevelDimension", &MakeRegisteredNativeCommand<PlaceLevelDimensionCommand>, false,
      R"json({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"mode":{"type":"string","enum":["static","associative"]},"value":{"type":"number"},"parent":{"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string","minLength":1}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]},"text":{"type":"string"}},"additionalProperties":false,"required":["x","y"]})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]}},"additionalProperties":false,"required":["elementId"]})json" },
    { "CreateMesh", &MakeRegisteredNativeCommand<CreateMeshCommand>, false,
      R"json({"type":"object","properties":{"outline":{"type":"array","minItems":6,"items":{"type":"number"}},"polyZ":{"type":"array","items":{"type":"number"}},"baseLevel":{"type":"number"},"ridgeCoords":{"type":"array","items":{"type":"number"}},"ridgeCounts":{"type":"array","items":{"type":"integer","minimum":0}},"skirtLevel":{"type":"number"},"skirt":{"type":"string","enum":["SolidBodyWithSkirt","WithSkirt","SurfaceOnlyWithoutSkirt"]},"floorInd":{"type":"integer"},"onFloorPlan":{"type":"boolean"},"layer":{"type":"string"}},"additionalProperties":false,"required":["outline"]})json",
      R"json({"type":"object","properties":{"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"baseLevel":{"type":"number"},"skirtLevel":{"type":"number"},"floorInd":{"type":"integer"},"switchedToFloorPlan":{"type":"boolean"}},"additionalProperties":false,"required":["elementId","baseLevel","skirtLevel","floorInd","switchedToFloorPlan"]})json" },
    { "CreateWall", &MakeRegisteredNativeCommand<CreateWallCommand>, false,
      R"json({"type":"object","properties":{"begX":{"type":"array","minItems":1,"items":{"type":"number"}},"begY":{"type":"array","minItems":1,"items":{"type":"number"}},"endX":{"type":"array","minItems":1,"items":{"type":"number"}},"endY":{"type":"array","minItems":1,"items":{"type":"number"}},"arcAngles":{"type":"array","items":{"type":"number"}},"base":{"type":"number"},"floorInd":{"type":"integer"},"height":{"type":"number"},"structure":{"type":"string","enum":["basic","composite","profile"]},"attrName":{"type":"string"},"thickness":{"type":"number"},"refLine":{"type":"string","enum":["outside","center","inside","coreOutside","coreCenter","coreInside"]},"flipped":{"type":"boolean"}},"additionalProperties":false,"required":["begX","begY","endX","endY"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"succeeded":{"type":"boolean"},"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"error":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}},"created":{"type":"integer","minimum":1},"floorInd":{"type":"integer"}},"additionalProperties":false,"required":["results","created","floorInd"]})json" },
    { "CreateColumn", &MakeRegisteredNativeCommand<CreateColumnCommand>, false,
      R"json({"type":"object","properties":{"x":{"type":"array","minItems":1,"items":{"type":"number"}},"y":{"type":"array","minItems":1,"items":{"type":"number"}},"base":{"type":"number"},"floorInd":{"type":"integer"},"height":{"type":"number"},"layer":{"type":"string"},"shape":{"type":"string","enum":["rectangular","circular","profile"]},"width":{"type":"number"},"height2":{"type":"number"},"diameter":{"type":"number"},"profile":{"type":"string"},"material":{"type":"string"},"angle":{"type":"number"}},"additionalProperties":false,"required":["x","y"]})json",
      R"json({"type":"object","properties":{"results":{"type":"array","items":{"type":"object","properties":{"succeeded":{"type":"boolean"},"elementId":{"type":"object","properties":{"guid":{"type":"string"}},"additionalProperties":false,"required":["guid"]},"error":{"type":"string"}},"additionalProperties":false,"required":["succeeded"]}},"count":{"type":"integer","minimum":0},"floorInd":{"type":"integer"}},"additionalProperties":false,"required":["results","count","floorInd"]})json" },
};

}   // namespace

NativeCommandRegistrations GetCreateCommandRegistrations ()
{
    return MakeRegistrationView (CreateCommandRegistrations);
}

} // namespace geomsrv

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NativeCommands/GhElementQueryCommands.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "Geometry/GeometryExtractor.hpp"

#include <Model.hpp>
#include <algorithm>
#include <cmath>

namespace geomsrv {

namespace {

struct GhDrawingCapture {
    GS::Array<GS::ObjectState> paths;
    USize points = 0;
    bool oversized = false;
    bool hatchLines = false;
};

GhDrawingCapture* s_ghDrawingCapture = nullptr;

void AddGhDrawingPath (const GS::Array<double>& xy, bool closed)
{
    GhDrawingCapture* capture = s_ghDrawingCapture;
    if (capture == nullptr || xy.GetSize () < 4)
        return;
    if (xy.GetSize () % 2 != 0 || capture->paths.GetSize () >= 128 ||
        capture->points + xy.GetSize () / 2 > 1024) {
        capture->oversized = true;
        return;
    }
    GS::ObjectState item;
    item.Add ("xy", xy);
    item.Add ("closed", closed);
    capture->paths.Push (item);
    capture->points += xy.GetSize () / 2;
}

void AddGhDrawingEdge (const API_Coord& first, const API_Coord& last, double sweep)
{
    GS::Array<double> xy;
    if (std::abs (sweep) < 1e-12) {
        xy.Push (first.x); xy.Push (first.y);
        xy.Push (last.x); xy.Push (last.y);
    }
    else {
        const double dx = last.x - first.x, dy = last.y - first.y;
        const double chord = std::hypot (dx, dy);
        const double tangent = std::tan (sweep * 0.5);
        if (chord < 1e-12 || std::abs (tangent) < 1e-12) return;
        const double distance = chord / (2.0 * tangent);
        const double cx = (first.x + last.x) * 0.5 - dy * distance / chord;
        const double cy = (first.y + last.y) * 0.5 + dx * distance / chord;
        const double a0 = std::atan2 (first.y - cy, first.x - cx);
        const double radius = std::hypot (first.x - cx, first.y - cy);
        const int count = std::min (128, std::max (2, static_cast<int> (std::ceil (std::abs (sweep) * 16))));
        for (int i = 0; i <= count; ++i) {
            const double a = a0 + sweep * i / count;
            xy.Push (cx + radius * std::cos (a));
            xy.Push (cy + radius * std::sin (a));
        }
    }
    AddGhDrawingPath (xy, false);
}

double GhEdgeSweep (const API_PolyArc* arcs, Int32 count, Int32 first, Int32 last)
{
    for (Int32 i = 0; arcs != nullptr && i < count; ++i)
        if (arcs[i].begIndex == first && arcs[i].endIndex == last) return arcs[i].arcAngle;
    return 0;
}

GSErrCode CollectGhDrawingPath (const API_PrimElement* primitive, const void* par1, const void* par2, const void* par3)
{
    if (primitive == nullptr || s_ghDrawingCapture == nullptr)
        return NoError;
    if (primitive->header.typeID == API_PrimCtrl_HatchLinesBegID) {
        s_ghDrawingCapture->hatchLines = true;
        return NoError;
    }
    if (primitive->header.typeID == API_PrimCtrl_HatchLinesEndID) {
        s_ghDrawingCapture->hatchLines = false;
        return NoError;
    }
    if (s_ghDrawingCapture->hatchLines) return NoError;
    GS::Array<double> xy;
    if (primitive->header.typeID == API_PrimLineID) {
        xy.Push (primitive->line.c1.x); xy.Push (primitive->line.c1.y);
        xy.Push (primitive->line.c2.x); xy.Push (primitive->line.c2.y);
        AddGhDrawingPath (xy, false);
    }
    else if (primitive->header.typeID == API_PrimArcID) {
        const API_PrimArc& arc = primitive->arc;
        constexpr double twoPi = 6.283185307179586;
        double sweep = arc.whole ? twoPi : arc.endAng - arc.begAng;
        if (arc.reflected) { while (sweep >= 0.0) sweep -= twoPi; }
        else { while (sweep <= 0.0) sweep += twoPi; }
        const int segments = std::min (128, std::max (4, static_cast<int> (std::ceil (std::abs (sweep) * 16))));
        const double minor = std::abs (arc.ratio) > 1e-12 ? arc.r / arc.ratio : arc.r;
        for (int i = 0; i <= segments; ++i) {
            const double a = arc.begAng + sweep * i / segments;
            const double x = arc.r * std::cos (a), y = minor * std::sin (a);
            xy.Push (arc.orig.x + x * std::cos (arc.angle) - y * std::sin (arc.angle));
            xy.Push (arc.orig.y + x * std::sin (arc.angle) + y * std::cos (arc.angle));
        }
        AddGhDrawingPath (xy, arc.whole);
    }
    else if (primitive->header.typeID == API_PrimPLineID && par1 != nullptr) {
        const auto* coords = static_cast<const API_Coord*> (par1);
        const auto* arcs = static_cast<const API_PolyArc*> (par3);
        if (primitive->pline.nCoords > 1024) { s_ghDrawingCapture->oversized = true; return NoError; }
        for (Int32 i = 1; i < primitive->pline.nCoords; ++i)
            AddGhDrawingEdge (coords[i], coords[i + 1], GhEdgeSweep (arcs, primitive->pline.nArcs, i, i + 1));
    }
    else if (primitive->header.typeID == API_PrimPolyID && par1 != nullptr && par2 != nullptr) {
        const auto* coords = static_cast<const API_Coord*> (par1);
        const auto* ends = static_cast<const Int32*> (par2);
        const auto* arcs = static_cast<const API_PolyArc*> (par3);
        for (Int32 polygon = 1; polygon <= primitive->poly.nSubPolys; ++polygon) {
            const Int32 first = ends[polygon - 1] + 1, last = ends[polygon];
            if (last - first > 1024) { s_ghDrawingCapture->oversized = true; break; }
            for (Int32 i = first; i < last; ++i)
                AddGhDrawingEdge (coords[i], coords[i + 1], GhEdgeSweep (arcs, primitive->poly.nArcs, i, i + 1));
        }
    }
    return NoError;
}

// Bounded, position-aligned GH2 reads. The mode is part of each request so no
// unused model representation is loaded. Unsupported modes are data, not a
// transport failure; this distinction matters for annotation-only elements.
class GetGhElementQueryCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "GetGhElementQuery";
    }

    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Array<GS::UniString> guids, selectors;
        GS::UniString kind, mode, search;
        GS::Int32 offset = 0;
        if (!params.Get ("guids", guids) || guids.GetSize () > 8 || !params.Get ("kind", kind) ||
            !params.Get ("mode", mode) || !params.Get ("selectors", selectors) || selectors.GetSize () > 64 ||
            !params.Get ("search", search) || search.GetLength () > 128 ||
            !params.Get ("offset", offset) || offset < 0 || offset > 2048 || offset % 128 != 0)
            return NativeCommandResult::Failure ("Invalid GH2 query batch.");
        if (kind != "contours" && kind != "relationships" && kind != "properties" && kind != "geometry" &&
            kind != "gdl")
            return NativeCommandResult::Failure ("Unknown GH2 query kind.");

        const GS::UniString wanted = search.ToLowerCase ();
        ModelerAPI::Model model;
        const bool haveModel = kind == "geometry" && mode == "Surface mesh" && AcquireCurrentModel (model);
        GS::Array<GS::ObjectState> rows;
        for (const GS::UniString& text : guids) {
            if (text.IsEmpty () || text.GetLength () > 64)
                return NativeCommandResult::Failure ("Invalid GH2 query GUID.");
            const API_Guid guid = APIGuidFromString (text.ToCStr ().Get ());
            API_Element element = {};
            element.header.guid = guid;
            GS::ObjectState row;
            row.Add ("guid", text);
            GS::Array<GS::ObjectState> items;
            GS::UniString status ("Empty"), diagnostic;
            bool more = false;
            if (guid == APINULLGuid || ACAPI_Element_Get (&element) != NoError) {
                status = "Unavailable";
                diagnostic = "Element not found in the active project.";
            }
            else if (kind == "contours") {
                if (mode != "Definition" && mode != "Boundary") {
                    status = "Unavailable";
                    diagnostic = "This view-dependent contour representation is not implemented.";
                }
                else if (element.header.type.typeID == API_WallID) {
                    API_WallRelation wall = {};
                    const GSErrCode relationError = ACAPI_Element_GetRelations (guid, API_ZombieElemID, &wall);
                    if (relationError == NoError) {
                        GS::Array<double> outer, arcs, holes, holeArcs;
                        GS::Array<GS::Int32> holeCounts;
                        GS::Int32 outerCount = 0, holeCount = 0;
                        const bool valid = WalkPolygonRings (
                            { wall.coords, wall.pends, wall.parcs }, nullptr, outer, arcs, outerCount,
                            holes, holeArcs, holeCounts, holeCount);
                        if (valid && outer.GetSize () + holes.GetSize () <= 2048 && holeCount <= 255) {
                            GS::ObjectState ring;
                            ring.Add ("role", GS::UniString ("outer"));
                            ring.Add ("closed", true);
                            ring.Add ("xy", outer);
                            ring.Add ("arcs", arcs);
                            items.Push (ring);
                            USize cursor = 0;
                            for (GS::Int32 h = 0; h < holeCount; ++h) {
                                GS::Array<double> xy, angles;
                                for (GS::Int32 i = 0; i < holeCounts[h]; ++i, ++cursor) {
                                    xy.Push (holes[2 * cursor]); xy.Push (holes[2 * cursor + 1]);
                                    angles.Push (holeArcs[cursor]);
                                }
                                GS::ObjectState hole;
                                hole.Add ("role", GS::UniString ("hole"));
                                hole.Add ("closed", true);
                                hole.Add ("xy", xy);
                                hole.Add ("arcs", angles);
                                items.Push (hole);
                            }
                            status = "Success";
                        }
                        else {
                            status = "Unavailable";
                            diagnostic = "Wall connection polygon is empty or exceeds the contour budget.";
                        }
                    }
                    else {
                        status = "Unavailable";
                        diagnostic = GS::UniString::Printf ("Wall connection polygon unavailable (SDK error %d).",
                                                                static_cast<int> (relationError));
                    }
                    ACAPI_DisposeWallRelationHdls (&wall);
                }
                else if (element.header.type.typeID != API_SlabID && element.header.type.typeID != API_HatchID &&
                         element.header.type.typeID != API_PolyLineID && element.header.type.typeID != API_MeshID) {
                    status = "Unavailable";
                    diagnostic = "No native polygon adapter for this element type.";
                }
                else {
                    API_ElementMemo memo = {};
                    const GSErrCode memoError = ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_Polygon);
                    if (memoError == NoError) {
                        GS::Array<double> outer, arcs, holes, holeArcs;
                        GS::Array<GS::Int32> holeCounts;
                        GS::Int32 outerCount = 0, holeCount = 0;
                        bool outerClosed = false;
                        const bool valid = WalkPolygonRings (
                            { memo.coords, memo.pends, memo.parcs }, nullptr, outer, arcs, outerCount, holes, holeArcs,
                            holeCounts, holeCount, element.header.type.typeID == API_PolyLineID, &outerClosed);
                        if (valid && (outer.GetSize () + holes.GetSize () > 2048 || holeCount > 255)) {
                            status = "Unavailable";
                            diagnostic = "Polygon exceeds the bounded contour read (2,048 coordinates or 256 rings).";
                        }
                        else if (valid) {
                            GS::ObjectState ring;
                            ring.Add ("role", GS::UniString (outerClosed ? "outer" : "open"));
                            ring.Add ("closed", outerClosed);
                            ring.Add ("xy", outer);
                            ring.Add ("arcs", arcs);
                            items.Push (ring);
                            USize cursor = 0;
                            for (GS::Int32 h = 0; h < holeCount; ++h) {
                                GS::Array<double> xy, angles;
                                for (GS::Int32 i = 0; i < holeCounts[h]; ++i) {
                                    xy.Push (holes[2 * cursor]);
                                    xy.Push (holes[2 * cursor + 1]);
                                    angles.Push (holeArcs[cursor]);
                                    ++cursor;
                                }
                                GS::ObjectState hole;
                                hole.Add ("role", GS::UniString ("hole"));
                                hole.Add ("closed", true);
                                hole.Add ("xy", xy);
                                hole.Add ("arcs", angles);
                                items.Push (hole);
                            }
                            status = "Success";
                        }
                        else {
                            status = "Error";
                            diagnostic = "Archicad returned a polygon memo without a usable outer ring.";
                        }
                    }
                    else {
                        status = "Unavailable";
                        diagnostic = GS::UniString::Printf ("Archicad polygon memo read failed (SDK error %d).",
                                                             static_cast<int> (memoError));
                    }
                    ACAPI_DisposeElemMemoHdls (&memo);
                }
            }
            else if (kind == "relationships") {
                if (mode != "Hosted") {
                    status = "Unavailable";
                    diagnostic = "Only native wall-hosted openings are supported; spatial inference is not performed.";
                }
                else if (element.header.type.typeID != API_WallID) {
                    status = "NotApplicable";
                    diagnostic = "Hosted opening extraction currently supports wall owners only.";
                }
                else {
                    for (API_ElemTypeID childType : { API_WindowID, API_DoorID }) {
                        GS::Array<API_Guid> children;
                        const GSErrCode connectionError =
                            ACAPI_Grouping_GetConnectedElements (guid, API_ElemType (childType), &children);
                        if (connectionError != NoError) {
                            status = "Unavailable";
                            diagnostic = GS::UniString::Printf ("Archicad wall connections failed (SDK error %d).",
                                                                 static_cast<int> (connectionError));
                            items.Clear ();
                            break;
                        }
                        for (const API_Guid& child : children) {
                            GS::ObjectState edge;
                            edge.Add ("target", GS::UniString (APIGuidToString (child).ToCStr ()));
                            edge.Add ("relation",
                                      GS::UniString (childType == API_WindowID ? "HostsWindow" : "HostsDoor"));
                            edge.Add ("provenance", GS::UniString ("ACAPI_Grouping_GetConnectedElements"));
                            items.Push (edge);
                        }
                    }
                    if (items.GetSize () > 256) {
                        items.Clear ();
                        status = "Unavailable";
                        diagnostic = "More than 256 hosted elements; narrow the query before extraction.";
                    }
                    else if (status == "Empty" && !items.IsEmpty ())
                        status = "Success";
                }
            }
            else if (kind == "geometry") {
                if (mode == "Surface mesh") {
                    if (!haveModel) {
                        status = "Unavailable";
                        diagnostic = "No 3D model is available in this project context.";
                    }
                    else {
                        const auto index = model.GetElementIndex (APIGuid2GSGuid (guid));
                        Mesh mesh;
                        if (!index.has_value () || !ExtractElementAt (model, *index, mesh)) {
                            status = "NotApplicable";
                            diagnostic = "This element has no tessellated 3D body in the current model.";
                        }
                        else if (mesh.VertexCount () > 4096 || mesh.TriangleCount () > 8192) {
                            status = "Unavailable";
                            diagnostic = "Tessellated body exceeds 4,096 vertices or 8,192 triangles; no partial mesh returned.";
                        }
                        else {
                            GS::Array<double> vertices;
                            GS::Array<GS::Int32> triangles;
                            for (double coordinate : mesh.vertices)
                                vertices.Push (coordinate);
                            for (uint32_t index : mesh.triangles)
                                triangles.Push (static_cast<GS::Int32> (index));
                            GS::ObjectState surface;
                            surface.Add ("vertices", vertices);
                            surface.Add ("triangles", triangles);
                            items.Push (surface);
                            status = "Success";
                        }
                    }
                }
                else if (mode == "2D drawing") {
                    GhDrawingCapture captured;
                    s_ghDrawingCapture = &captured;
                    const GSErrCode drawError = ACAPI_DrawingPrimitive_ShapePrims (element.header, CollectGhDrawingPath);
                    s_ghDrawingCapture = nullptr;
                    if (drawError != NoError || captured.oversized) {
                        status = "Unavailable";
                        diagnostic = drawError != NoError
                            ? GS::UniString::Printf ("2D primitives unavailable in this view (SDK error %d).",
                                                    static_cast<int> (drawError))
                            : GS::UniString ("2D drawing exceeds 128 paths or 1,024 vertices.");
                    }
                    else {
                        items = captured.paths;
                        status = items.IsEmpty () ? GS::UniString ("Empty") : GS::UniString ("Success");
                        if (items.IsEmpty ())
                            diagnostic = "No evaluated 2D primitives in the current floor-plan context; check the object's plan visibility.";
                    }
                }
                else if (mode != "Bounding box") {
                    status = "Unavailable";
                    diagnostic = "Native definition and derived Brep are not implemented; use Surface mesh, Bounding box or 2D drawing.";
                }
                else if (element.header.type.typeID == API_DimensionID || element.header.type.typeID == API_HatchID ||
                         element.header.type.typeID == API_PolyLineID || element.header.type.typeID == API_CutPlaneID) {
                    status = "NotApplicable";
                    diagnostic = "This annotation has no 3D model bounds.";
                }
                else {
                    API_ElemInfo3D info = {};
                    const GSErrCode geometryError = ACAPI_ModelAccess_Get3DInfo (element.header, &info);
                    if (geometryError == NoError) {
                        GS::ObjectState box;
                        box.Add ("minX", info.bounds.xMin);
                        box.Add ("minY", info.bounds.yMin);
                        box.Add ("minZ", info.bounds.zMin);
                        box.Add ("maxX", info.bounds.xMax);
                        box.Add ("maxY", info.bounds.yMax);
                        box.Add ("maxZ", info.bounds.zMax);
                        items.Push (box);
                        status = "Success";
                    }
                    else {
                        status = "Unavailable";
                        diagnostic = GS::UniString::Printf (
                            "3D bounds unavailable in this model context (SDK error %d).",
                            static_cast<int> (geometryError));
                    }
                }
            }
            else if (kind == "properties") {
                if (mode == "Element settings") {
                    const auto addNumber = [&items] (const char* name, double number, const char* unit = "metres") {
                        GS::ObjectState field;
                        field.Add ("key", GS::UniString (APIGuidToString (APINULLGuid).ToCStr ()));
                        field.Add ("name", GS::UniString (name));
                        field.Add ("group", GS::UniString (GS::UniString ("Native element settings (") +
                                                GS::UniString (unit) + GS::UniString (")")));
                        field.Add ("dataType", (GS::Int32) API_PropertyRealValueType);
                        field.Add ("definitionType", (GS::Int32) 0);
                        field.Add ("userDefined", false);
                        field.Add ("collectionType", (GS::Int32) API_PropertySingleCollectionType);
                        field.Add ("number", number);
                        field.Add ("hasNumber", true);
                        field.Add ("boolean", false);
                        field.Add ("hasBoolean", false);
                        field.Add ("valueStatus", GS::UniString ("HasValue"));
                        field.Add ("value", GS::UniString::Printf ("%.6f", number));
                        items.Push (field);
                    };
                    if (element.header.type.typeID == API_WallID) {
                        addNumber ("Height", element.wall.height);
                        addNumber ("Thickness at start", element.wall.thickness);
                        addNumber ("Thickness at end", element.wall.thickness1);
                        addNumber ("Base offset", element.wall.bottomOffset);
                        const double dx = element.wall.endC.x - element.wall.begC.x;
                        const double dy = element.wall.endC.y - element.wall.begC.y;
                        const double chord = std::hypot (dx, dy);
                        const double angle = std::abs (element.wall.angle);
                        if (element.wall.type != APIWtyp_Poly &&
                            (angle < 1e-9 || std::abs (std::sin (angle / 2)) > 1e-9))
                            addNumber ("Reference line length", angle < 1e-9 ? chord :
                                       chord * angle / (2 * std::sin (angle / 2)));
                        GS::ObjectState structure;
                        structure.Add ("key", GS::UniString (APIGuidToString (APINULLGuid).ToCStr ()));
                        structure.Add ("name", GS::UniString ("Structure"));
                        structure.Add ("group", GS::UniString ("Native wall settings"));
                        structure.Add ("dataType", (GS::Int32) API_PropertyStringValueType);
                        structure.Add ("definitionType", (GS::Int32) 0);
                        structure.Add ("userDefined", false);
                        structure.Add ("collectionType", (GS::Int32) API_PropertySingleCollectionType);
                        structure.Add ("number", 0.0);
                        structure.Add ("hasNumber", false);
                        structure.Add ("boolean", false);
                        structure.Add ("hasBoolean", false);
                        structure.Add ("valueStatus", GS::UniString ("HasValue"));
                        GS::UniString structureName = element.wall.modelElemStructureType == API_CompositeStructure
                            ? GS::UniString ("Composite") : element.wall.modelElemStructureType == API_ProfileStructure
                                ? GS::UniString ("Profile") : GS::UniString ("Basic");
                        if (element.wall.modelElemStructureType == API_CompositeStructure) {
                            API_Attribute attribute = {};
                            attribute.header.typeID = API_CompWallID;
                            attribute.header.index = element.wall.composite;
                            if (ACAPI_Attribute_Get (&attribute) == NoError)
                                structureName += ": " + GS::UniString (attribute.header.name);
                        }
                        structure.Add ("value", structureName);
                        items.Push (structure);
                        status = "Success";
                    }
                    else if (element.header.type.typeID == API_MeshID) {
                        addNumber ("Base level", element.mesh.level);
                        addNumber ("Skirt depth", element.mesh.skirtLevel);
                        status = "Success";
                    }
                    else if (element.header.type.typeID == API_SlabID) {
                        addNumber ("Level", element.slab.level);
                        addNumber ("Thickness", element.slab.thickness);
                        status = "Success";
                    }
                    else if (element.header.type.typeID == API_RoofID) {
                        addNumber ("Level", element.roof.shellBase.level);
                        addNumber ("Thickness", element.roof.shellBase.thickness);
                        if (element.roof.roofClass == API_PlaneRoofID)
                            addNumber ("Pitch", element.roof.u.planeRoof.angle, "radians");
                        status = "Success";
                    }
                    else if (element.header.type.typeID == API_BeamID) {
                        addNumber ("Level", element.beam.level);
                        addNumber ("Slant angle", element.beam.slantAngle, "radians");
                        status = "Success";
                    }
                    else if (element.header.type.typeID == API_ColumnID) {
                        addNumber ("Height", element.column.height);
                        addNumber ("Bottom offset", element.column.bottomOffset);
                        addNumber ("Slant angle", element.column.slantAngle, "radians");
                        status = "Success";
                    }
                    else if (element.header.type.typeID == API_ObjectID ||
                             element.header.type.typeID == API_LampID) {
                        addNumber ("Level", element.object.level);
                        addNumber ("X scale", element.object.xRatio, "ratio");
                        addNumber ("Y scale", element.object.yRatio, "ratio");
                        addNumber ("Plan angle", element.object.angle, "radians");
                        status = "Success";
                    }
                    else {
                        status = "NotApplicable";
                        diagnostic = "Native settings currently support walls, meshes, slabs, roofs, beams, columns and placed objects/lamps only.";
                    }
                }
                else {
                API_PropertyDefinitionFilter filter = API_PropertyDefinitionFilter_All;
                if (mode == "User")
                    filter = API_PropertyDefinitionFilter_UserDefined;
                else if (mode == "Built-in")
                    filter = API_PropertyDefinitionFilter_BuiltIn;
                GS::Array<API_PropertyDefinition> definitions, selected;
                const GSErrCode definitionError = ACAPI_Element_GetPropertyDefinitions (guid, filter, definitions);
                if (definitionError != NoError) {
                    status = "Unavailable";
                    diagnostic = GS::UniString::Printf ("Property definitions unavailable (SDK error %d).",
                                                         static_cast<int> (definitionError));
                }
                else {
                    GS::Int32 matched = 0;
                    for (const API_PropertyDefinition& definition : definitions) {
                        const GS::UniString id (APIGuidToString (definition.guid).ToCStr ());
                        if ((!selectors.IsEmpty () && !selectors.Contains (id)) ||
                            (!wanted.IsEmpty () && !definition.name.ToLowerCase ().Contains (wanted)))
                            continue;
                        if (mode == "Discover" && matched++ < offset)
                            continue;
                        if (mode == "Discover" && selected.GetSize () == 128) {
                            more = true;
                            break;
                        }
                        selected.Push (definition);
                    }
                    if (mode != "Discover" && selectors.IsEmpty ()) {
                        selected.Clear ();
                        status = "Unavailable";
                        diagnostic = "Select property definition GUIDs before requesting evaluated values.";
                    }
                    {
                        GS::Array<API_Property> values;
                        const bool fetch = mode != "Discover";
                        const GSErrCode valueError = fetch && !selected.IsEmpty ()
                                                         ? ACAPI_Element_GetPropertyValues (guid, selected, values)
                                                         : NoError;
                        if (valueError != NoError || (fetch && !selected.IsEmpty () &&
                                                      values.GetSize () != selected.GetSize ())) {
                            status = "Unavailable";
                            diagnostic = GS::UniString::Printf ("Property values unavailable (SDK error %d).",
                                                                 static_cast<int> (valueError));
                        }
                        else {
                            for (USize i = 0; i < selected.GetSize (); ++i) {
                                const API_PropertyDefinition& definition = selected[i];
                                API_PropertyGroup group = {};
                                group.guid = definition.groupGuid;
                                GS::ObjectState property;
                                property.Add ("key", GS::UniString (APIGuidToString (definition.guid).ToCStr ()));
                                property.Add ("name", definition.name);
                                property.Add ("group", ACAPI_Property_GetPropertyGroup (group) == NoError
                                                           ? group.name
                                                           : GS::UniString ());
                                property.Add ("dataType", static_cast<GS::Int32> (definition.valueType));
                                property.Add ("definitionType", static_cast<GS::Int32> (definition.definitionType));
                                property.Add ("userDefined",
                                              definition.definitionType == API_PropertyCustomDefinitionType);
                                property.Add ("collectionType", static_cast<GS::Int32> (definition.collectionType));
                                GS::UniString value, valueStatus ("Discovery");
                                double number = 0.0;
                                bool hasNumber = false, boolean = false, hasBoolean = false;
                                if (fetch) {
                                    const API_Property& prop = values[i];
                                    if (prop.status == API_Property_HasValue &&
                                        prop.value.variantStatus == API_VariantStatusNormal) {
                                        valueStatus = "HasValue";
                                        ACAPI_Property_GetPropertyValueString (prop, &value);
                                        if (value.GetLength () > 512) {
                                            value.Clear ();
                                            valueStatus = "Unavailable";
                                        }
                                        if (definition.collectionType == API_PropertySingleCollectionType) {
                                            const API_Variant& variant = prop.value.singleVariant.variant;
                                            if (variant.type == API_PropertyIntegerValueType) {
                                                number = static_cast<double> (variant.intValue);
                                                hasNumber = true;
                                            }
                                            else if (variant.type == API_PropertyRealValueType) {
                                                number = variant.doubleValue;
                                                hasNumber = true;
                                            }
                                            else if (variant.type == API_PropertyBooleanValueType) {
                                                boolean = variant.boolValue;
                                                hasBoolean = true;
                                            }
                                        }
                                    }
                                    else if (prop.status == API_Property_NotAvailable)
                                        valueStatus = "NotApplicable";
                                    else if (prop.status == API_Property_HasValue &&
                                             prop.value.variantStatus == API_VariantStatusUserUndefined)
                                        valueStatus = "Empty";
                                    else
                                        valueStatus = "Unavailable";
                                }
                                property.Add ("number", number);
                                property.Add ("hasNumber", hasNumber);
                                property.Add ("boolean", boolean);
                                property.Add ("hasBoolean", hasBoolean);
                                property.Add ("valueStatus", valueStatus);
                                property.Add ("value", value);
                                items.Push (property);
                            }
                            if (!items.IsEmpty ())
                                status = "Success";
                        }
                    }
                }
                }
            }
            else if (kind == "gdl") {
                const API_ElemTypeID type = element.header.type.typeID;
                if (type != API_ObjectID && type != API_LampID && type != API_WindowID && type != API_DoorID) {
                    status = "NotApplicable";
                    diagnostic = "This element does not carry placed GDL instance parameters.";
                }
                else {
                    API_ElementMemo memo = {};
                    const GSErrCode parameterError = ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_AddPars);
                    if (parameterError != NoError || memo.params == nullptr) {
                        status = "Unavailable";
                        diagnostic = GS::UniString::Printf (
                            "Placed GDL parameters unavailable (SDK error %d; check the library part).",
                            static_cast<int> (parameterError));
                    }
                    else {
                        const GSSize count = BMGetHandleSize ((GSHandle) memo.params) / sizeof (API_AddParType);
                        const Int32 libInd = type == API_WindowID ? element.window.openingBase.libInd
                                             : type == API_DoorID ? element.door.openingBase.libInd
                                                                  : element.object.libInd;
                        API_LibPart libPart = {};
                        libPart.index = libInd;
                        GS::UniString libraryName;
                        if (libInd > 0) {
                            const GSErrCode partError = ACAPI_LibraryPart_Get (&libPart);
                            if (partError == NoError || partError == APIERR_MISSINGDEF)
                                libraryName = GS::UniString (libPart.docu_UName);
                        }
                        delete libPart.location;
                        GS::UniString group;
                        GS::Int32 matched = 0;
                        for (GSIndex i = 0; i < count; ++i) {
                            const API_AddParType& par = (*memo.params)[i];
                            const GS::UniString name (par.name);
                            if (par.typeID == APIParT_Title) {
                                group = GS::UniString (par.uDescname);
                                continue;
                            }
                            const GS::UniString label (par.uDescname);
                            if (par.typeID == APIParT_Separator ||
                                (mode != "All parameters" && (par.flags & API_ParFlg_Hidden) != 0) ||
                                (!selectors.IsEmpty () && !selectors.Contains (name)) ||
                                (!wanted.IsEmpty () && !name.ToLowerCase ().Contains (wanted) &&
                                 !label.ToLowerCase ().Contains (wanted) && !group.ToLowerCase ().Contains (wanted)))
                                continue;
                            if (matched++ < offset)
                                continue;
                            if (items.GetSize () == 128) {
                                more = true;
                                break;
                            }
                            GS::ObjectState item;
                            item.Add ("key", name);
                            item.Add ("label", label);
                            item.Add ("group", group);
                            item.Add ("libraryPart", libraryName);
                            item.Add ("type", static_cast<GS::Int32> (par.typeID));
                            item.Add ("hidden", (par.flags & API_ParFlg_Hidden) != 0);
                            item.Add ("disabled", (par.flags & API_ParFlg_Disabled) != 0);
                            item.Add ("array", par.typeMod == API_ParArray);
                            item.Add ("dim1", static_cast<GS::Int32> (par.dim1));
                            item.Add ("dim2", static_cast<GS::Int32> (par.dim2));
                            const GS::UniString stringValue =
                                par.typeMod != API_ParArray && par.typeID == APIParT_CString
                                    ? GS::UniString (par.value.uStr)
                                    : GS::UniString ();
                            const bool oversized = stringValue.GetLength () > 512;
                            item.Add ("text", oversized ? GS::UniString () : stringValue);
                            item.Add ("number", par.typeMod == API_ParArray || par.typeID == APIParT_CString ||
                                                        par.typeID == APIParT_Dictionary
                                                    ? 0.0
                                                    : par.value.real);
                            item.Add ("valueStatus",
                                      oversized || par.typeMod == API_ParArray || par.typeID == APIParT_Dictionary
                                          ? GS::UniString ("Unavailable")
                                          : GS::UniString ("HasValue"));
                            items.Push (item);
                        }
                        if (status == "Empty")
                            status = items.IsEmpty () ? GS::UniString ("Empty") : GS::UniString ("Success");
                    }
                    ACAPI_DisposeElemMemoHdls (&memo);
                }
            }
            row.Add ("status", status);
            row.Add ("diagnostic", diagnostic);
            row.Add ("items", items);
            row.Add ("more", more);
            rows.Push (row);
        }
        GS::ObjectState result;
        result.Add ("elements", rows);
        return result;
    }
};

const NativeCommandRegistration GhElementQueryCommandRegistrations[] = {
    { "GetGhElementQuery", &MakeRegisteredNativeCommand<GetGhElementQueryCommand>, false,
      R"json({"type":"object","properties":{"guids":{"type":"array","maxItems":8,"items":{"type":"string","minLength":1,"maxLength":64}},"kind":{"type":"string","enum":["contours","relationships","properties","geometry","gdl"]},"mode":{"type":"string","maxLength":64},"search":{"type":"string","maxLength":128},"selectors":{"type":"array","maxItems":64,"items":{"type":"string","minLength":1,"maxLength":128}},"offset":{"type":"integer","minimum":0,"maximum":2048}},"additionalProperties":false,"required":["guids","kind","mode","search","selectors","offset"]})json", R"json({"type":"object","properties":{"elements":{"type":"array","maxItems":8,"items":{"type":"object","properties":{"guid":{"type":"string"},"status":{"type":"string","enum":["Success","Empty","NotApplicable","Unavailable","Stale","Error"]},"diagnostic":{"type":"string"},"more":{"type":"boolean"},"items":{"type":"array","maxItems":256,"items":{"type":"object","properties":{"role":{"type":"string"},"closed":{"type":"boolean"},"xy":{"type":"array","items":{"type":"number"}},"arcs":{"type":"array","items":{"type":"number"}},"target":{"type":"string"},"relation":{"type":"string"},"provenance":{"type":"string"},"minX":{"type":"number"},"minY":{"type":"number"},"minZ":{"type":"number"},"maxX":{"type":"number"},"maxY":{"type":"number"},"maxZ":{"type":"number"},"vertices":{"type":"array","maxItems":12288,"items":{"type":"number"}},"triangles":{"type":"array","maxItems":24576,"items":{"type":"integer","minimum":0}},"key":{"type":"string"},"name":{"type":"string"},"group":{"type":"string"},"dataType":{"type":"integer"},"definitionType":{"type":"integer"},"userDefined":{"type":"boolean"},"collectionType":{"type":"integer"},"valueStatus":{"type":"string"},"value":{"type":"string"},"number":{"type":"number"},"hasNumber":{"type":"boolean"},"boolean":{"type":"boolean"},"hasBoolean":{"type":"boolean"},"libraryPart":{"type":"string"},"label":{"type":"string"},"type":{"type":"integer"},"hidden":{"type":"boolean"},"disabled":{"type":"boolean"},"array":{"type":"boolean"},"dim1":{"type":"integer"},"dim2":{"type":"integer"},"text":{"type":"string"}},"additionalProperties":false}}},"additionalProperties":false,"required":["guid","status","diagnostic","items","more"]}}},"additionalProperties":false,"required":["elements"]})json" },
};

} // namespace

NativeCommandRegistrations GetGhElementQueryCommandRegistrations ()
{
    return MakeRegistrationView (GhElementQueryCommandRegistrations);
}

} // namespace geomsrv

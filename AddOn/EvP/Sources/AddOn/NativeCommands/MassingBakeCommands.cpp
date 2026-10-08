#include "APIEnvir.h"
#include "ACAPinc.h"
#include "NativeCommands/MassingBakeCommands.hpp"
#include "NativeCommands/DraftingFillCommands.hpp"
#include "NativeCommands/DraftingDatabaseTarget.hpp"
#include "NativeCommands/CommandUtils.hpp"
#include "ArchViz/MassingBake.hpp"
#include <algorithm>
#include <cmath>
namespace geomsrv {
namespace {
bool Construction (const GS::ObjectState& settings, bool wall, API_Element& element, GS::UniString& error)
{
    GS::UniString structure, name;
    API_AttributeIndex index;
    double thickness = 0;
    if (!settings.Get ("structure", structure) || !settings.Get ("attribute", name) ||
        !ReadFiniteNumber (settings, "thickness", thickness) || thickness <= 0 || thickness > 10) {
        error = "Construction needs structure, attribute and positive thickness.";
        return false;
    }
    if (structure == "composite") {
        API_Attribute attribute {};
        if (!AttributeNameToIndex (API_CompWallID, name, index)) {
            error = "Chosen composite no longer exists.";
            return false;
        }
        attribute.header.typeID = API_CompWallID;
        attribute.header.index = index;
        if (ACAPI_Attribute_Get (&attribute) != NoError ||
            !(attribute.compWall.head.flags & (wall ? APICWall_ForWall : APICWall_ForSlab))) {
            error = "Choose a compatible slab/wall composite.";
            return false;
        }
        thickness = attribute.compWall.totalThick;
        if (wall) {
            element.wall.modelElemStructureType = API_CompositeStructure;
            element.wall.composite = index;
        }
        else {
            element.slab.modelElemStructureType = API_CompositeStructure;
            element.slab.composite = index;
        }
    }
    else if (structure == "basic" && AttributeNameToIndex (API_BuildingMaterialID, name, index)) {
        if (wall) {
            element.wall.modelElemStructureType = API_BasicStructure;
            element.wall.buildingMaterial = index;
        }
        else {
            element.slab.modelElemStructureType = API_BasicStructure;
            element.slab.buildingMaterial = index;
        }
    }
    else {
        error = "Choose an existing Basic building material or compatible composite.";
        return false;
    }
    if (wall)
        element.wall.thickness = element.wall.thickness1 = thickness;
    else
        element.slab.thickness = thickness;
    return ResolveLayerParam (settings, element.header, error);
}
struct Regularized {
    API_ElementMemo input {};
    API_RegularizedPoly** polygons = nullptr;
    Int32 count = 0;
    ~Regularized ()
    {
        if (polygons) {
            for (Int32 i = 0; i < count; ++i)
                ACAPI_Polygon_DisposeRegularizedPoly (&(*polygons)[i]);
            BMKillHandle (reinterpret_cast<GSHandle*> (&polygons));
        }
        ACAPI_DisposeElemMemoHdls (&input);
    }
};
bool CleanSlabContour (GS::Array<GS::ObjectState>& vertices, size_t& points, GS::UniString& error)
{
    points += vertices.GetSize ();
    if (points > 100000) {
        error = "Slab bake exceeds the 100000 input-point budget.";
        return false;
    }
    archviz::slabslices::Ring input, cleaned;
    for (const auto& vertex : vertices) {
        double x = 0, y = 0;
        if (!ReadFiniteNumber (vertex, "x", x) || !ReadFiniteNumber (vertex, "y", y)) {
            error = "Slab bake needs finite XY coordinates.";
            return false;
        }
        input.xy.push_back (x);
        input.xy.push_back (y);
    }
    std::string why;
    if (!archviz::massingbake::CleanSlabRing (input, cleaned, why)) {
        error = GS::UniString (why.c_str (), CC_UTF8);
        return false;
    }
    vertices.Clear ();
    for (size_t i = 0; i < cleaned.xy.size (); i += 2) {
        GS::ObjectState vertex;
        vertex.Add ("x", cleaned.xy[i]);
        vertex.Add ("y", cleaned.xy[i + 1]);
        vertices.Push (vertex);
    }
    return true;
}
void LogRejectedSlab (const API_ElementMemo& memo, const API_Polygon& polygon, const std::string& group, double z,
                      uint64_t token, UIndex sourceIndex)
{
    std::vector<archviz::SliceChain> contours;
    Int32 first = 1;
    for (Int32 sub = 1; sub <= polygon.nSubPolys; ++sub) {
        const Int32 last = (*memo.pends)[sub];
        archviz::SliceChain contour;
        contour.closed = true;
        for (Int32 i = first; i < last; ++i) {
            const auto& a = (*memo.coords)[i];
            contour.xy.push_back (a.x);
            contour.xy.push_back (a.y);
        }
        contours.push_back (std::move (contour));
        first = last + 1;
    }
    archviz::massingbake::RejectedSlab (contours, group, z, token, sourceIndex);
}
class BakeMassingSlicesCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "BakeMassingSlices";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 token = 0;
        params.Get ("token", token);
        if (!archviz::massingbake::Current (uint64_t (token)))
            return NativeCommandResult::Failure ("Bake belongs to a closed/replaced session.");
        GS::Array<GS::ObjectState> slabs;
        if (!params.Get ("slabs", slabs) || slabs.IsEmpty () || slabs.GetSize () > 2048)
            return NativeCommandResult::Failure ("Bake requires 1..2048 slab contours.");
        AnchoredWorksheetDatabase database;
        GS::ObjectState target;
        target.Add ("floorPlan", true);
        GS::UniString databaseGuid, error;
        if (!database.Activate (target, databaseGuid, error))
            return NativeCommandResult::Failure (error);
        const auto storeys = archviz::ReadStoreys ();
        if (storeys.Empty () || storeys.indices.size () != storeys.levels.size ())
            return NativeCommandResult::Failure ("Cannot read existing Archicad home stories.");
        std::vector<double> elevations;
        std::vector<std::string> groups;
        for (const auto& item : slabs) {
            double z = 0, height = 0;
            GS::UniString group;
            if (!ReadFiniteNumber (item, "z", z) || std::abs (z) > 1e9 || !ReadFiniteNumber (item, "height", height) ||
                height <= 0 || !item.Get ("group", group))
                return NativeCommandResult::Failure ("Invalid physical bake elevation/height/building group.");
            elevations.push_back (z);
            groups.emplace_back (group.ToCStr (0, MaxUSize, CC_UTF8).Get ());
        }
        const auto homes = archviz::massingbake::HomeStoreys (storeys, elevations, groups);
        GS::Array<GS::ObjectState> results;
        size_t points = 0;
        const auto created = [&results] (const API_Element& element) {
            GS::ObjectState record, id;
            id.Add ("guid", GS::UniString (APIGuidToString (element.header.guid).ToCStr ()));
            record.Add ("elementId", id);
            results.Push (record);
        };
        for (UIndex at = 0; at < slabs.GetSize (); ++at) {
            const auto& item = slabs[at];
            API_Element slab {}, wall {};
            slab.header.type = API_SlabID;
            if (ACAPI_Element_GetDefaults (&slab, nullptr) != NoError || !Construction (item, false, slab, error))
                return NativeCommandResult::Failure (error.IsEmpty () ? GS::UniString ("Cannot read slab defaults.")
                                                                      : error);
            slab.header.floorInd = short (storeys.indices[homes[at]]);
            slab.slab.level = elevations[at] - storeys.levels[homes[at]];
            slab.slab.referencePlaneLocation = APISlabRefPlane_Top;
            slab.slab.linkToSettings = {};
            const auto* wallSettings = item.Get ("wall");
            if (wallSettings) {
                wall.header.type = API_WallID;
                if (ACAPI_Element_GetDefaults (&wall, nullptr) != NoError ||
                    !Construction (*wallSettings, true, wall, error))
                    return NativeCommandResult::Failure (error.IsEmpty () ? GS::UniString ("Cannot read wall defaults.")
                                                                          : error);
                wall.header.floorInd = slab.header.floorInd;
                wall.wall.type = APIWtyp_Normal;
                wall.wall.profileType = APISect_Normal;
                wall.wall.referenceLineLocation = APIWallRefLine_Center;
                wall.wall.bottomOffset = slab.slab.level;
                item.Get ("height", wall.wall.height);
                wall.wall.relativeTopStory = 0;
                wall.wall.topOffset = wall.wall.offset = wall.wall.angle = 0;
                wall.wall.linkToSettings = {};
            }
            GS::Array<GS::ObjectState> outline, holes;
            item.Get ("polygonOutline", outline);
            item.Get ("holes", holes);
            if (!CleanSlabContour (outline, points, error))
                return NativeCommandResult::Failure (error);
            for (auto& hole : holes) {
                GS::Array<GS::ObjectState> vertices;
                if (!hole.Get ("polygonOutline", vertices) || !CleanSlabContour (vertices, points, error))
                    return NativeCommandResult::Failure (error.IsEmpty () ? GS::UniString ("Invalid slab bake hole.")
                                                                          : error);
                hole.Clear ();
                hole.Add ("polygonOutline", vertices);
            }
            Regularized regularized;
            if (!BuildStraightPolygonMemo (outline, holes, slab.slab.poly, regularized.input, error))
                return NativeCommandResult::Failure (error);
            API_RegularizedPoly polygon {};
            polygon.coords = regularized.input.coords;
            polygon.pends = regularized.input.pends;
            polygon.parcs = regularized.input.parcs;
            // Regularize BEFORE any creation; never retry a failed write. SDK can
            // split touching/self-touching boundaries into multiple valid slabs.
            const auto normalized =
                ACAPI_Polygon_RegularizePolygon (&polygon, &regularized.count, &regularized.polygons);
            if (normalized != NoError || regularized.count <= 0)
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Polygon_RegularizePolygon", normalized, "Massing bake"));
            for (Int32 p = 0; p < regularized.count; ++p) {
                const auto& poly = (*regularized.polygons)[p];
                API_ElementMemo memo {};
                memo.coords = poly.coords;
                memo.pends = poly.pends;
                memo.parcs = poly.parcs;
                slab.slab.poly.nCoords =
                    Int32 (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.coords)) / sizeof (API_Coord)) - 1;
                slab.slab.poly.nSubPolys =
                    Int32 (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.pends)) / sizeof (Int32)) - 1;
                slab.slab.poly.nArcs =
                    memo.parcs
                        ? Int32 (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.parcs)) / sizeof (API_PolyArc))
                        : 0;
                const Int32 edgeCount = slab.slab.poly.nCoords + 1;
                Regularized edges;
                edges.input.edgeTrims = reinterpret_cast<API_EdgeTrim**> (
                    BMAllocateHandle (edgeCount * sizeof (API_EdgeTrim), ALLOCATE_CLEAR, 0));
                edges.input.sideMaterials = reinterpret_cast<API_OverriddenAttribute*> (
                    BMAllocatePtr (edgeCount * sizeof (API_OverriddenAttribute), ALLOCATE_CLEAR, 0));
                if (!edges.input.edgeTrims || !edges.input.sideMaterials)
                    return NativeCommandResult::Failure ("Out of memory allocating slab edges.");
                for (Int32 edge = 0; edge < edgeCount; ++edge) {
                    (*edges.input.edgeTrims)[edge].sideType = APIEdgeTrim_Vertical;
                    edges.input.sideMaterials[edge] = slab.slab.sideMat;
                }
                memo.edgeTrims = edges.input.edgeTrims;
                memo.sideMaterials = edges.input.sideMaterials;
                if (results.GetSize () >= 2048)
                    return NativeCommandResult::Failure ("Bake exceeds 2048 elements; entire transaction refused.");
                slab.header.guid = APINULLGuid;
                const auto slabError = ACAPI_Element_Create (&slab, &memo);
                if (slabError != NoError) {
                    try {
                        LogRejectedSlab (memo, slab.slab.poly, groups[at], elevations[at], uint64_t (token), at);
                    }
                    catch (...) {
                        // Best-effort forensic output must never mask the write
                        // error or prevent the dispatcher's transaction rollback.
                    }
                    return NativeCommandResult::Failure (
                        EVP_ACAPI_FAIL ("ACAPI_Element_Create", slabError, "Baked slab; transaction rolls back"));
                }
                created (slab);
                if (!wallSettings)
                    continue;
                Int32 first = 1;
                for (Int32 sub = 1; sub <= slab.slab.poly.nSubPolys; ++sub) {
                    const Int32 last = (*memo.pends)[sub];
                    for (Int32 edge = first; edge < last; ++edge) {
                        wall.header.guid = APINULLGuid;
                        wall.wall.begC = (*memo.coords)[edge];
                        wall.wall.endC = (*memo.coords)[edge + 1];
                        if (results.GetSize () >= 2048)
                            return NativeCommandResult::Failure (
                                "Bake exceeds 2048 elements; entire transaction refused.");
                        const auto wallError = ACAPI_Element_Create (&wall, nullptr);
                        if (wallError != NoError)
                            return NativeCommandResult::Failure (EVP_ACAPI_FAIL ("ACAPI_Element_Create", wallError,
                                                                                 "Baked wall; transaction rolls back"));
                        created (wall);
                    }
                    first = last + 1;
                }
            }
        }
        GS::ObjectState result;
        result.Add ("elements", results);
        result.Add ("count", GS::Int32 (results.GetSize ()));
        return result;
    }
};
class MassingBakeGuardCommand : public MainThreadCommand {
  public:
    GS::String GetName () const override
    {
        return "MassingBakeGuard";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 token = 0;
        params.Get ("token", token);
        GS::ObjectState result;
        result.Add ("current", archviz::massingbake::Current (uint64_t (token)));
        return result;
    }
};
class FinishMassingBakeCommand : public WriteCommand {
  public:
    GS::String GetName () const override
    {
        return "FinishMassingBake";
    }
    NativeCommandResult ExecuteNative (const GS::ObjectState& params, GS::ProcessControl&) const override
    {
        GS::Int64 token = 0;
        params.Get ("token", token);
        if (!archviz::massingbake::Current (uint64_t (token)))
            return NativeCommandResult::Failure ("Bake belongs to a closed/replaced session.");
        GS::Array<GS::ObjectState> items;
        if (!params.Get ("elements", items) || items.IsEmpty () || items.GetSize () > 2048)
            return NativeCommandResult::Failure ("Bake finalization needs 1..2048 created elements.");
        AnchoredWorksheetDatabase database;
        GS::ObjectState target;
        target.Add ("floorPlan", true);
        GS::UniString databaseGuid, databaseError;
        if (!database.Activate (target, databaseGuid, databaseError))
            return NativeCommandResult::Failure (databaseError);
        for (const auto& item : items) {
            const auto* identifier = item.Get ("elementId");
            GS::UniString guid;
            API_Element element {}, mask {};
            if (!identifier || !identifier->Get ("guid", guid))
                return NativeCommandResult::Failure ("Bake finalization needs an element GUID.");
            element.header.guid = APIGuidFromString (guid.ToCStr ().Get ());
            const auto readError = ACAPI_Element_Get (&element);
            if (readError != NoError ||
                (element.header.type.typeID != API_SlabID && element.header.type.typeID != API_MorphID &&
                 element.header.type.typeID != API_WallID))
                return NativeCommandResult::Failure ("Bake finalization target is not a current slab/Morph/wall.");
            GS::UniString layerError;
            if (!ResolveLayerParam (item, element.header, layerError))
                return NativeCommandResult::Failure (layerError);
            ACAPI_ELEMENT_MASK_CLEAR (mask);
            ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, layer);
            GS::UniString morphMaterial, morphSurface;
            if (item.Get ("morphMaterial", morphMaterial) || item.Contains ("morphSurface")) {
                API_AttributeIndex material, surface;
                if (element.header.type.typeID != API_MorphID || !item.Get ("morphSurface", morphSurface) ||
                    !AttributeNameToIndex (API_BuildingMaterialID, morphMaterial, material) ||
                    !AttributeNameToIndex (API_MaterialID, morphSurface, surface))
                    return NativeCommandResult::Failure (
                        "Morph needs existing building material and surface attributes.");
                // Morph's surface override must be applied by Change; Create can
                // silently ignore it in Archicad even when the call succeeds.
                element.morph.buildingMaterial = material;
                element.morph.material = surface;
                ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, buildingMaterial);
                ACAPI_ELEMENT_MASK_SET (mask, API_MorphType, material);
            }
            API_ElementMemo memo {};
            const auto* slab = item.Get ("slab");
            if (slab) {
                if (element.header.type.typeID != API_SlabID)
                    return NativeCommandResult::Failure ("Slab settings cannot be applied to a Morph.");
                GS::UniString structure, name;
                slab->Get ("structure", structure);
                slab->Get ("attribute", name);
                API_AttributeIndex index;
                if (structure == "composite") {
                    API_Attribute attribute {};
                    if (!AttributeNameToIndex (API_CompWallID, name, index))
                        return NativeCommandResult::Failure ("Slab composite no longer exists.");
                    attribute.header.typeID = API_CompWallID;
                    attribute.header.index = index;
                    if (ACAPI_Attribute_Get (&attribute) != NoError ||
                        !(attribute.compWall.head.flags & APICWall_ForSlab))
                        return NativeCommandResult::Failure ("Choose a slab-compatible composite.");
                    element.slab.modelElemStructureType = API_CompositeStructure;
                    element.slab.composite = index;
                    element.slab.thickness = attribute.compWall.totalThick;
                    ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, composite);
                }
                else if (structure == "basic") {
                    if (!AttributeNameToIndex (API_BuildingMaterialID, name, index) ||
                        !ReadFiniteNumber (*slab, "thickness", element.slab.thickness) || element.slab.thickness <= 0)
                        return NativeCommandResult::Failure (
                            "Basic slabs need an existing material and positive thickness.");
                    element.slab.modelElemStructureType = API_BasicStructure;
                    element.slab.buildingMaterial = index;
                    ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, buildingMaterial);
                }
                else
                    return NativeCommandResult::Failure ("Slab structure must be basic or composite.");
                element.slab.referencePlaneLocation = APISlabRefPlane_Top;
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, referencePlaneLocation);
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, modelElemStructureType);
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, thickness);
                GS::Array<GS::ObjectState> outline, holes;
                slab->Get ("polygonOutline", outline);
                slab->Get ("holes", holes);
                GS::UniString polygonError;
                if (!BuildStraightPolygonMemo (outline, holes, element.slab.poly, memo, polygonError)) {
                    ACAPI_DisposeElemMemoHdls (&memo);
                    return NativeCommandResult::Failure (polygonError);
                }
                // Contour replacement changes edge count (especially courtyards).
                // Never retain the creation defaults' shorter edge arrays.
                const Int32 count = element.slab.poly.nCoords + 1;
                memo.edgeTrims = reinterpret_cast<API_EdgeTrim**> (
                    BMAllocateHandle (count * sizeof (API_EdgeTrim), ALLOCATE_CLEAR, 0));
                memo.sideMaterials = reinterpret_cast<API_OverriddenAttribute*> (
                    BMAllocatePtr (count * sizeof (API_OverriddenAttribute), ALLOCATE_CLEAR, 0));
                if (!memo.edgeTrims || !memo.sideMaterials) {
                    ACAPI_DisposeElemMemoHdls (&memo);
                    return NativeCommandResult::Failure ("Out of memory allocating baked slab edges.");
                }
                for (Int32 i = 0; i < count; ++i) {
                    (*memo.edgeTrims)[i].sideType = APIEdgeTrim_Vertical;
                    memo.sideMaterials[i] = element.slab.sideMat;
                }
                ACAPI_ELEMENT_MASK_SET (mask, API_SlabType, poly);
            }
            const auto changed = ACAPI_Element_Change (
                &element, &mask, slab ? &memo : nullptr,
                slab ? APIMemoMask_Polygon | APIMemoMask_EdgeTrims | APIMemoMask_SideMaterials : 0, true);
            ACAPI_DisposeElemMemoHdls (&memo);
            if (changed != NoError)
                return NativeCommandResult::Failure (
                    EVP_ACAPI_FAIL ("ACAPI_Element_Change", changed, "Massing bake settings"));
        }
        GS::ObjectState result;
        result.Add ("count", GS::Int32 (items.GetSize ()));
        return result;
    }
};
const NativeCommandRegistration
    s_registrations[] = {
        { "BakeMassingSlices", &MakeRegisteredNativeCommand<BakeMassingSlicesCommand>, false,
          R"json({"type":"object","properties":{"token":{"type":"integer","minimum":1},"slabs":{"type":"array","minItems":1,"maxItems":2048,"items":{"type":"object","properties":{"group":{"type":"string"},"z":{"type":"number"},"height":{"type":"number","exclusiveMinimum":0},"layer":{"type":"string","minLength":1},"structure":{"type":"string","enum":["basic","composite"]},"attribute":{"type":"string","minLength":1},"thickness":{"type":"number","exclusiveMinimum":0,"maximum":10},"polygonOutline":{"type":"array","minItems":3,"maxItems":4096,"items":{"$ref":"#Point2D"}},"holes":{"type":"array","maxItems":1024,"items":{"type":"object","properties":{"polygonOutline":{"type":"array","minItems":3,"maxItems":4096,"items":{"$ref":"#Point2D"}}},"additionalProperties":false,"required":["polygonOutline"]}},"wall":{"type":"object","properties":{"layer":{"type":"string","minLength":1},"structure":{"type":"string","enum":["basic","composite"]},"attribute":{"type":"string","minLength":1},"thickness":{"type":"number","exclusiveMinimum":0,"maximum":10}},"additionalProperties":false,"required":["layer","structure","attribute","thickness"]}},"additionalProperties":false,"required":["group","z","height","layer","structure","attribute","thickness","polygonOutline","holes"]}}},"additionalProperties":false,"required":["token","slabs"]})json",
          R"json({"type":"object","properties":{"elements":{"type":"array","minItems":1,"maxItems":2048,"items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"}},"additionalProperties":false,"required":["elementId"]}},"count":{"type":"integer","minimum":1,"maximum":2048}},"additionalProperties":false,"required":["elements","count"]})json" },
        { "MassingBakeGuard", &MakeRegisteredNativeCommand<MassingBakeGuardCommand>, false,
          R"json({"type":"object","properties":{"token":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["token"]})json",
          R"json({"type":"object","properties":{"current":{"type":"boolean"}},"additionalProperties":false,"required":["current"]})json" },
        { "FinishMassingBake", &MakeRegisteredNativeCommand<FinishMassingBakeCommand>, false, R"json({"type":"object","properties":{"token":{"type":"integer","minimum":1},"elements":{"type":"array","minItems":1,"maxItems":2048,"items":{"type":"object","properties":{"elementId":{"$ref":"#ElementId"},"layer":{"type":"string","minLength":1},"morphMaterial":{"type":"string","minLength":1},"morphSurface":{"type":"string","minLength":1},"slab":{"type":"object","properties":{"structure":{"type":"string","enum":["basic","composite"]},"attribute":{"type":"string","minLength":1},"thickness":{"type":"number","exclusiveMinimum":0},"polygonOutline":{"type":"array","minItems":3,"maxItems":4096,"items":{"$ref":"#Point2D"}},"holes":{"type":"array","maxItems":1024,"items":{"type":"object","properties":{"polygonOutline":{"type":"array","minItems":3,"maxItems":4096,"items":{"$ref":"#Point2D"}}},"additionalProperties":false,"required":["polygonOutline"]}}},"additionalProperties":false,"required":["structure","attribute","thickness","polygonOutline","holes"]}},"additionalProperties":false,"required":["elementId","layer"]}}},"additionalProperties":false,"required":["token","elements"]})json",
          R"json({"type":"object","properties":{"count":{"type":"integer","minimum":1}},"additionalProperties":false,"required":["count"]})json" }
    };
} // namespace
NativeCommandRegistrations GetMassingBakeCommandRegistrations ()
{
    return MakeRegistrationView (s_registrations);
}
} // namespace geomsrv

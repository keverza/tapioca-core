#include "APIEnvir.h"
#include "ACAPinc.h"

#include "NodeGraph/ElementReader.hpp"

#include "Python/MainThreadGate.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

// THE ACAPI TRANSCRIPTION OF ElementClassification's table, and nothing else.
//
// ⚠️ THE SECOND AND LAST ACAPI TRANSLATION UNIT IN THE GRAPH RUNTIME. Everything
// above IArchicadHost is DevKit-free and covered by the offline suite; this file
// and ArchicadHostImpl.cpp are the parts that cannot be. It is deliberately
// DULL: a switch on the element type, then a run of field copies. There is no
// logic here to get subtly wrong, which is the only defence a file the test
// suite cannot reach has.
//
// ⚠️ EVERY SETTING IT WRITES IS NAMED IN ElementClassification.cpp, and every
// setting named there that this file does not write is simply ABSENT from the
// result. Absence is the honest encoding and the inspector renders it as such -
// see ElementDescription::settings. Writing a zero for a field this build cannot
// read would be indistinguishable from a field that really is zero.
//
// ⚠️ AND THE UNITS ARE THE DESCRIPTOR'S. An angle whose descriptor says "deg"
// is converted HERE; the runtime's radians never leave this file for those
// fields. See ElementSettingDescriptor::unit.
//
// Every ACAPI call runs inside MainThreadGate, and the gate lambda captures BY
// VALUE - MainThreadGate's own header explains why a by-reference capture is a
// use-after-free that only fires when the gate is slow.

namespace evp::nodegraph {
namespace {

constexpr int kGateTimeoutMs = evp::MainThreadGate::DefaultTimeoutMs;

// A polygon this size is not something an inspector row can show, and copying
// one per element turns a selection read into a memory event. The vertex COUNT
// is still reported, so a large outline says how large it is.
constexpr size_t kMaxOutlineVertices = 4096;

std::string Utf8 (const GS::UniString& text)
{
    return text.ToCStr (0, GS::MaxUSize, CC_UTF8).Get ();
}

double Degrees (double radians)
{
    return radians * 180.0 / 3.14159265358979323846;
}

using Settings = std::map<std::string, Value>;

void PutText (Settings& settings, const char* id, const GS::UniString& text)
{
    if (!text.IsEmpty ())
        settings.emplace (id, Value (Utf8 (text)));
}

void PutNumber (Settings& settings, const char* id, double value)
{
    if (std::isfinite (value))
        settings.emplace (id, Value (value));
}

void PutAngle (Settings& settings, const char* id, double radians)
{
    PutNumber (settings, id, Degrees (radians));
}

void PutBool (Settings& settings, const char* id, bool value)
{
    settings.emplace (id, Value (value));
}

void PutCount (Settings& settings, const char* id, int64_t value)
{
    settings.emplace (id, Value (value));
}

void PutPoint (Settings& settings, const char* id, double x, double y, double z)
{
    settings.emplace (id, Value (Point3 { x, y, z }));
}

// MAIN THREAD. An attribute's name, or empty when it has none - an unset
// building material, or an index this project does not have.
GS::UniString AttributeName (API_AttrTypeID typeId, const API_AttributeIndex& index)
{
    API_Attribute attribute = {};
    attribute.header.typeID = typeId;
    attribute.header.index = index;
    // ⚠️ THROUGH uniStringNamePtr, NOT header.name. The fixed char[] field is
    // truncated and in the local codepage, so a layer with a long or non-ASCII
    // name comes back mangled from it; Metadata/MetadataExtractor.cpp reads
    // attributes the same way for the same reason.
    GS::UniString name;
    attribute.header.uniStringNamePtr = &name;
    if (ACAPI_Attribute_Get (&attribute) != NoError)
        return {};
    return name;
}

// --- the closed spellings ---------------------------------------------------
//
// Returned as TEXT rather than as the API's integer, because the integer is
// meaningless to a client and would make every consumer carry its own copy of
// these tables. The catalog's ValueType for these settings is String for the
// same reason.

const char* StructureName (API_ModelElemStructureType structure)
{
    switch (structure) {
        case API_BasicStructure:
            return "Basic";
        case API_CompositeStructure:
            return "Composite";
        case API_ProfileStructure:
            return "Profiled";
    }
    return "";
}

const char* WallReferenceLineName (API_WallReferenceLineLocationID location)
{
    switch (location) {
        case APIWallRefLine_Outside:
            return "Outside";
        case APIWallRefLine_Center:
            return "Center";
        case APIWallRefLine_Inside:
            return "Inside";
        case APIWallRefLine_CoreOutside:
            return "Core Outside";
        case APIWallRefLine_CoreCenter:
            return "Core Center";
        case APIWallRefLine_CoreInside:
            return "Core Inside";
    }
    return "";
}

const char* WallShapeName (API_WallTypeID type)
{
    switch (type) {
        case APIWtyp_Normal:
            return "Straight";
        case APIWtyp_Trapez:
            return "Trapezoid";
        case APIWtyp_Poly:
            return "Polygonal";
    }
    return "";
}

const char* SlabReferencePlaneName (API_SlabReferencePlaneLocationID location)
{
    switch (location) {
        case APISlabRefPlane_Top:
            return "Top";
        case APISlabRefPlane_CoreTop:
            return "Core Top";
        case APISlabRefPlane_CoreBottom:
            return "Core Bottom";
        case APISlabRefPlane_Bottom:
            return "Bottom";
    }
    return "";
}

const char* BeamShapeName (API_BeamShapeTypeID shape)
{
    switch (shape) {
        case API_StraightBeam:
            return "Straight";
        case API_HorizontallyCurvedBeam:
            return "Horizontally Curved";
        case API_VerticallyCurvedBeam:
            return "Vertically Curved";
    }
    return "";
}

const char* MorphBodyName (API_MorphBodyTypeID body)
{
    switch (body) {
        case APIMorphBodyType_SurfaceBody:
            return "Surface";
        case APIMorphBodyType_SolidBody:
            return "Solid";
    }
    return "";
}

// The catalog id for an API element type. The one place the two vocabularies
// meet; an unnamed type falls through to the unclassified bucket, which is why
// a Hotlink or a Camera in a selection still gets a row.
const char* CatalogTypeId (API_ElemTypeID typeId)
{
    switch (typeId) {
        case API_WallID:
            return "wall";
        case API_ColumnID:
            return "column";
        case API_ColumnSegmentID:
            return "column";
        case API_BeamID:
            return "beam";
        case API_BeamSegmentID:
            return "beam";
        case API_SlabID:
            return "slab";
        case API_RoofID:
            return "roof";
        case API_ShellID:
            return "shell";
        case API_MeshID:
            return "mesh";
        case API_MorphID:
            return "morph";
        case API_ObjectID:
            return "object";
        case API_LampID:
            return "lamp";
        case API_ZoneID:
            return "zone";
        case API_DoorID:
            return "door";
        case API_WindowID:
            return "window";
        case API_SkylightID:
            return "skylight";
        case API_OpeningID:
            return "opening";
        case API_CurtainWallID:
            return "curtainWall";
        case API_StairID:
            return "stair";
        case API_RailingID:
            return "railing";
        case API_HatchID:
            return "hatch";
        case API_LineID:
            return "line";
        case API_PolyLineID:
            return "polyLine";
        case API_ArcID:
            return "arc";
        case API_CircleID:
            return "circle";
        case API_SplineID:
            return "spline";
        case API_TextID:
            return "text";
        case API_LabelID:
            return "label";
        case API_DimensionID:
            return "dimension";
        case API_RadialDimensionID:
            return "dimension";
        case API_LevelDimensionID:
            return "dimension";
        case API_AngleDimensionID:
            return "dimension";
        case API_HotspotID:
            return "hotspot";
        default:
            return kUnclassifiedElementTypeId;
    }
}

// --- stories ----------------------------------------------------------------
//
// Read ONCE for the whole batch, not once per element: the story table is a
// property of the project and does not change while a read is in flight.
struct Story {
    std::string name;
    double level = 0.0;
};

std::map<short, Story> ReadStories ()
{
    std::map<short, Story> stories;
    API_StoryInfo info = {};
    if (ACAPI_ProjectSetting_GetStorySettings (&info) != NoError || info.data == nullptr)
        return stories;
    const short count = static_cast<short> (info.lastStory - info.firstStory + 1);
    for (short i = 0; i < count; ++i) {
        const API_StoryType& story = (*info.data)[i];
        stories[story.index] = Story { Utf8 (GS::UniString (story.uName)), story.level };
    }
    // Ours to free, always - the same handle discipline ReadSelectionOnHostThread
    // follows for the marquee.
    BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
    return stories;
}

// --- outlines ---------------------------------------------------------------

void PutOutline (Settings& settings, const API_Guid& guid, const API_Polygon& polygon)
{
    PutCount (settings, "vertexCount", static_cast<int64_t> (polygon.nCoords));
    if (polygon.nCoords <= 0 || static_cast<size_t> (polygon.nCoords) > kMaxOutlineVertices)
        return;

    API_ElementMemo memo = {};
    if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_Polygon) == NoError && memo.coords != nullptr) {
        Polyline outline;
        // ⚠️ ONE-BASED, AND THE FIRST SLOT IS NOT A VERTEX. The coordinate array
        // is indexed from 1 and index 0 is a duplicate slot the API keeps for its
        // own use; reading from 0 puts a phantom vertex at the start of every
        // outline this build shows.
        for (Int32 i = 1; i <= polygon.nCoords; ++i) {
            const API_Coord& point = (*memo.coords)[i];
            outline.points.push_back (Point3 { point.x, point.y, 0.0 });
        }
        settings.emplace ("outline", Value (std::move (outline)));
    }
    ACAPI_DisposeElemMemoHdls (&memo);
}

// --- per-type transcription -------------------------------------------------

// The one structure setting that APPLIES, and not the two that do not.
//
// ⚠️ THE CONDITION IS THE CATALOG'S, AND THIS MUST AGREE WITH IT. See
// ElementClassification's Structure(): `buildingMaterial` is declared for
// "Basic", `composite` for "Composite", `profile` for "Profile", and the
// spellings here are the ones StructureName returns. Writing all three - which
// this did before - put a composite name on a Basic wall, where it is not what
// the wall is built from.
void PutStructure (Settings& settings, API_ModelElemStructureType structure, const API_AttributeIndex& buildingMaterial,
                   const API_AttributeIndex& composite, const API_AttributeIndex& profile)
{
    PutText (settings, "structure", StructureName (structure));
    switch (structure) {
        case API_BasicStructure:
            PutText (settings, "buildingMaterial", AttributeName (API_BuildingMaterialID, buildingMaterial));
            break;
        case API_CompositeStructure:
            PutText (settings, "composite", AttributeName (API_CompWallID, composite));
            break;
        case API_ProfileStructure:
            PutText (settings, "profile", AttributeName (API_ProfileID, profile));
            break;
        default:
            break;
    }
}

// One API_OverriddenAttribute as the two settings the catalog declares for it.
//
// ⚠️ THE BOOL IS ALWAYS WRITTEN, THE NAME ONLY WHEN OVERRIDDEN. That pairing is
// what lets a client tell an inherited surface from one it could not read; see
// WallSurfaces() in ElementClassification.cpp.
void PutSurface (Settings& settings, const char* nameId, const char* overriddenId,
                 const API_OverriddenAttribute& surface)
{
    PutBool (settings, overriddenId, surface.hasValue);
    if (surface.hasValue)
        PutText (settings, nameId, AttributeName (API_MaterialID, surface.value));
}

// The derived reference line, for the two types that have one. The arithmetic
// is ElementClassification's and is tested offline; this only supplies the
// three numbers and copies the answer back.
void PutReferenceLine (Settings& settings, const API_Coord& begin, const API_Coord& end, double angleRadians)
{
    const Point3 from { begin.x, begin.y, 0.0 };
    const Point3 to { end.x, end.y, 0.0 };
    PutNumber (settings, "length", ReferenceLineLength (from, to, angleRadians));
    Polyline path = ReferenceLinePath (from, to, angleRadians);
    if (!path.points.empty ())
        settings.emplace ("referenceLinePath", Value (std::move (path)));
}

void ReadWall (const API_WallType& wall, Settings& settings)
{
    PutText (settings, "referenceLine", WallReferenceLineName (wall.referenceLineLocation));
    PutNumber (settings, "thickness", wall.thickness);
    PutNumber (settings, "height", wall.height);
    PutNumber (settings, "bottomOffset", wall.bottomOffset);
    PutNumber (settings, "topOffset", wall.topOffset);
    PutPoint (settings, "begin", wall.begC.x, wall.begC.y, 0.0);
    PutPoint (settings, "end", wall.endC.x, wall.endC.y, 0.0);
    PutReferenceLine (settings, wall.begC, wall.endC, wall.angle);
    PutText (settings, "wallShape", WallShapeName (wall.type));
    PutAngle (settings, "slantAngle", wall.slantAlpha);
    PutBool (settings, "flipped", wall.flipped);
    PutStructure (settings, wall.modelElemStructureType, wall.buildingMaterial, wall.composite, wall.profileAttr);
    PutSurface (settings, "referenceSurface", "referenceSurfaceOverridden", wall.refMat);
    PutSurface (settings, "oppositeSurface", "oppositeSurfaceOverridden", wall.oppMat);
    PutSurface (settings, "sideSurface", "sideSurfaceOverridden", wall.sidMat);
}

// Windows, doors and skylights. API_DoorType IS API_WindowType, so the first two
// share every field; a skylight has the same opening base and a different owner,
// which is a relation rather than a setting and is not read here.
void ReadOpeningBase (const API_OpeningBaseType& opening, Settings& settings)
{
    PutNumber (settings, "width", opening.width);
    PutNumber (settings, "height", opening.height);
    PutBool (settings, "reflected", opening.reflected);
}

void ReadWindowDoor (const API_WindowType& window, Settings& settings)
{
    ReadOpeningBase (window.openingBase, settings);
    PutNumber (settings, "sillHeight", window.lower);
    PutNumber (settings, "positionAlongWall", window.objLoc);
}

void ReadSlab (const API_SlabType& slab, Settings& settings)
{
    PutNumber (settings, "thickness", slab.thickness);
    PutNumber (settings, "level", slab.level);
    PutText (settings, "referencePlane", SlabReferencePlaneName (slab.referencePlaneLocation));
    PutNumber (settings, "offsetFromTop", slab.offsetFromTop);
    // No profile: API_SlabType has no profileAttr. See StructureWithProfile().
    PutStructure (settings, slab.modelElemStructureType, slab.buildingMaterial, slab.composite, {});
    PutOutline (settings, slab.head.guid, slab.poly);
}

void ReadColumn (const API_ColumnType& column, Settings& settings)
{
    PutNumber (settings, "height", column.height);
    PutPoint (settings, "origin", column.origoPos.x, column.origoPos.y, 0.0);
    PutNumber (settings, "bottomOffset", column.bottomOffset);
    PutNumber (settings, "topOffset", column.topOffset);
    PutAngle (settings, "axisRotation", column.axisRotationAngle);
    PutBool (settings, "slanted", column.isSlanted);
    PutAngle (settings, "slantAngle", column.slantAngle);
    PutAngle (settings, "slantDirection", column.slantDirectionAngle);
}

void ReadBeam (const API_BeamType& beam, Settings& settings)
{
    PutPoint (settings, "begin", beam.begC.x, beam.begC.y, 0.0);
    PutPoint (settings, "end", beam.endC.x, beam.endC.y, 0.0);
    PutReferenceLine (settings, beam.begC, beam.endC, beam.curveAngle);
    PutNumber (settings, "level", beam.level);
    PutNumber (settings, "offset", beam.offset);
    PutText (settings, "shape", BeamShapeName (beam.beamShape));
    PutAngle (settings, "curveAngle", beam.curveAngle);
    PutBool (settings, "slanted", beam.isSlanted);
    PutAngle (settings, "slantAngle", beam.slantAngle);
    PutAngle (settings, "profileAngle", beam.profileAngle);
}

void ReadMorph (const API_MorphType& morph, Settings& settings)
{
    PutNumber (settings, "level", morph.level);
    PutText (settings, "bodyType", MorphBodyName (morph.bodyType));
    PutText (settings, "buildingMaterial", AttributeName (API_BuildingMaterialID, morph.buildingMaterial));
    PutBool (settings, "castShadow", morph.castShadow);
    PutBool (settings, "receiveShadow", morph.receiveShadow);
}

void ReadObject (const API_ObjectType& object, Settings& settings)
{
    PutPoint (settings, "position", object.pos.x, object.pos.y, 0.0);
    PutNumber (settings, "level", object.level);
    PutAngle (settings, "angle", object.angle);
    // xRatio/yRatio ARE the placed size for a fixed-size library part, which is
    // what "Size X" means in the object's own settings dialog.
    PutNumber (settings, "sizeX", object.xRatio);
    PutNumber (settings, "sizeY", object.yRatio);
    PutBool (settings, "reflected", object.reflected);
}

void ReadLamp (const API_ObjectType& lamp, Settings& settings)
{
    PutPoint (settings, "position", lamp.pos.x, lamp.pos.y, 0.0);
    PutNumber (settings, "level", lamp.level);
    PutAngle (settings, "angle", lamp.angle);
    PutBool (settings, "lightOn", lamp.lightIsOn);
}

void ReadShellBase (const API_ShellBaseType& base, Settings& settings)
{
    PutNumber (settings, "thickness", base.thickness);
    PutNumber (settings, "level", base.level);
    // No profile: API_ShellBaseType has no profileAttr either.
    PutStructure (settings, base.modelElemStructureType, base.buildingMaterial, base.composite, {});
}

void ReadMesh (const API_MeshType& mesh, Settings& settings)
{
    PutNumber (settings, "level", mesh.level);
    PutNumber (settings, "skirtLevel", mesh.skirtLevel);
    PutText (settings, "buildingMaterial", AttributeName (API_BuildingMaterialID, mesh.buildingMaterial));
    PutOutline (settings, mesh.head.guid, mesh.poly);
}

void ReadZone (const API_ZoneType& zone, Settings& settings)
{
    PutText (settings, "zoneName", GS::UniString (zone.roomName));
    PutText (settings, "zoneNumber", GS::UniString (zone.roomNoStr));
    PutNumber (settings, "baseLevel", zone.roomBaseLev);
    PutNumber (settings, "topOffset", zone.roomTopOffset);
    PutNumber (settings, "height", zone.roomHeight);
    PutNumber (settings, "floorThickness", zone.roomFlThick);
    PutOutline (settings, zone.head.guid, zone.poly);
}

// MAIN THREAD. Archicad's own model extent for one element.
//
// ⚠️ ASKED OF THE API RATHER THAN COMPUTED FROM THE OUTLINE. CalcBounds accounts
// for slant, thickness and the real solid; a box derived from `outline` would be
// the plan footprint wearing the name of a bounding box, and it would be wrong
// for every slanted wall and every object taller than its plan symbol.
//
// A failure is SILENT AND ABSENT rather than an error: a 2D element outside the
// model, or one Archicad declines to measure, simply has no bounds - and an
// element that could not be measured must not report a box at the origin.
void PutBounds (Settings& settings, const API_Elem_Head& head)
{
    API_Box3D extent = {};
    if (ACAPI_Element_CalcBounds (&head, &extent) != NoError)
        return;
    PutPoint (settings, "boundsMin", extent.xMin, extent.yMin, extent.zMin);
    PutPoint (settings, "boundsMax", extent.xMax, extent.yMax, extent.zMax);
}

// MAIN THREAD. One element, start to finish.
void ReadOne (const std::string& guid, const std::map<short, Story>& stories, ElementDescription& description)
{
    description.guid = guid;

    const API_Guid apiGuid = APIGuidFromString (guid.c_str ());
    if (apiGuid == APINULLGuid) {
        description.detail = "'" + guid + "' is not a valid element identifier";
        return;
    }

    API_Element element = {};
    element.header.guid = apiGuid;
    if (ACAPI_Element_Get (&element) != NoError) {
        // Deleted, never existed, or outside this user's Teamwork workspace -
        // the same three-way ambiguity ArchicadReferenceResolver names, and the
        // same refusal to guess which.
        description.detail = "element " + guid +
                             " is not in this project - it may have been deleted, or it may belong to "
                             "another user's Teamwork workspace";
        return;
    }

    description.available = true;
    description.elementType = CatalogTypeId (element.header.type.typeID);

    GS::UniString typeName;
    ACAPI_Element_GetElemTypeName (element.header.type, typeName);
    description.typeLabel = Utf8 (typeName);

    Settings& settings = description.settings;
    PutText (settings, "layer", AttributeName (API_LayerID, element.header.layer));

    const auto story = stories.find (element.header.floorInd);
    if (story != stories.end ()) {
        settings.emplace ("homeStory", Value (story->second.name));
        PutNumber (settings, "homeStoryLevel", story->second.level);
    }

    // The user-visible element ID, which lives in a memo rather than the header.
    API_ElementMemo memo = {};
    if (ACAPI_Element_GetMemo (apiGuid, &memo, APIMemoMask_ElemInfoString) == NoError &&
        memo.elemInfoString != nullptr) {
        PutText (settings, "elementId", *memo.elemInfoString);
    }
    ACAPI_DisposeElemMemoHdls (&memo);

    // Before the per-type switch, because it applies to every type - including
    // the ones below that fall through to `default`.
    PutBounds (settings, element.header);

    switch (element.header.type.typeID) {
        case API_WallID:
            ReadWall (element.wall, settings);
            break;
        case API_SlabID:
            ReadSlab (element.slab, settings);
            break;
        case API_ColumnID:
            ReadColumn (element.column, settings);
            break;
        case API_BeamID:
            ReadBeam (element.beam, settings);
            break;
        case API_MorphID:
            ReadMorph (element.morph, settings);
            break;
        case API_ObjectID:
            ReadObject (element.object, settings);
            break;
        case API_LampID:
            ReadLamp (element.lamp, settings);
            break;
        case API_MeshID:
            ReadMesh (element.mesh, settings);
            break;
        case API_ZoneID:
            ReadZone (element.zone, settings);
            break;
        case API_WindowID:
            ReadWindowDoor (element.window, settings);
            break;
        case API_DoorID:
            // API_DoorType IS API_WindowType - see the typedef in
            // APIdefs_Elements.h - but the UNION MEMBER is still `door`, and
            // reading `element.window` here would read the right bytes for the
            // wrong reason and break the day the typedef stops holding.
            ReadWindowDoor (element.door, settings);
            break;
        case API_SkylightID:
            // No sill and no position along a wall: those are API_WindowType's,
            // and a skylight sits in a roof.
            ReadOpeningBase (element.skylight.openingBase, settings);
            break;
        case API_RoofID:
            ReadShellBase (element.roof.shellBase, settings);
            break;
        case API_ShellID:
            ReadShellBase (element.shell.shellBase, settings);
            break;
        default:
            // Classified, common settings filled, nothing type-specific. See the
            // file header: a type this build does not transcribe shows what it
            // has rather than inventing what it does not.
            break;
    }
}

} // namespace

bool ReadElementDescriptions (const std::vector<ArchicadElementRef>& elements,
                              std::vector<ElementDescription>& descriptions, std::string& error)
{
    descriptions.clear ();
    if (elements.empty ())
        return true;

    // Captured by value into the gate lambda, and shared so the results survive
    // the call. MainThreadGate's header explains why a by-reference capture here
    // is a use-after-free that only fires when the gate is slow.
    auto guids = std::make_shared<std::vector<std::string>> ();
    for (const ArchicadElementRef& element : elements)
        guids->push_back (element.guid);
    auto results = std::make_shared<std::vector<ElementDescription>> (elements.size ());

    GS::UniString gateError;
    const bool delivered = evp::MainThreadGate::Get ().Invoke (
        [guids, results] {
            const std::map<short, Story> stories = ReadStories ();
            for (size_t i = 0; i < guids->size (); ++i)
                ReadOne ((*guids)[i], stories, (*results)[i]);
        },
        kGateTimeoutMs, gateError);

    if (!delivered) {
        error = gateError.IsEmpty () ? std::string ("Archicad did not respond") : Utf8 (gateError);
        return false;
    }

    descriptions = std::move (*results);
    return true;
}

} // namespace evp::nodegraph

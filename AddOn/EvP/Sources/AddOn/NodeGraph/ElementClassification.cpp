#include "NodeGraph/ElementClassification.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace evp::nodegraph {
namespace {

using S = ElementSettingDescriptor;
using G = SettingGroup;

constexpr const char kMetres[] = "m";
constexpr const char kDegrees[] = "deg";

// On EVERY type, and first, because they are the questions a user asks of an
// element before they ask anything type-specific: what is it called, what layer
// is it on, and which story is it on. `homeStory` carries the story's NAME
// rather than its index - "2. Floor" is what the user set and what they read in
// Archicad; the index is an implementation detail of the API.
std::vector<S> Common ()
{
    return {
        { "elementId", "ID", G::Identity, ValueType::String, {} },
        { "layer", "Layer", G::Identity, ValueType::String, {} },
        { "homeStory", "Home Story", G::Placement, ValueType::String, {} },
        { "homeStoryLevel", "Story Level", G::Placement, ValueType::Double, kMetres },
    };
}

std::vector<S> Join (std::vector<S> first, std::vector<S> second)
{
    first.insert (first.end (), second.begin (), second.end ());
    return first;
}

std::vector<S> With (std::vector<S> extra)
{
    return Join (Common (), std::move (extra));
}

SettingCondition WhenStructureIs (const char* text)
{
    return SettingCondition { "structure", text };
}

// The structure set, shared by every type that can be Basic or Composite.
// Repeating these descriptors on six types is how they end up spelled
// differently on one of them.
//
// ⚠️ THE MATERIAL ROWS ARE CONDITIONAL ON `structure`, and that is a
// correctness fix rather than a nicety. Before it, every wall showed a Building
// Material AND a Composite AND the reader filled both, so a Basic wall displayed
// a composite name that does not apply to it and a composite wall displayed a
// building material that is not what it is built from. APIdefs_Elements.h says
// so at the field itself: "Composite index of wall. Used only, if structure type
// is API_CompositeStructure." Now `structure` says which row is meaningful, the
// reader fills only that one, and a client with no value for the others knows
// they do not apply rather than that they could not be read. See
// SettingCondition.
std::vector<S> Structure ()
{
    return {
        { "structure", "Structure", G::Structure, ValueType::String, {} },
        { "buildingMaterial",
          "Building Material",
          G::Structure,
          ValueType::String,
          {},
          SettingOrigin::Archicad,
          WhenStructureIs ("Basic") },
        { "composite",
          "Composite",
          G::Structure,
          ValueType::String,
          {},
          SettingOrigin::Archicad,
          WhenStructureIs ("Composite") },
    };
}

// ⚠️ ONLY THE WALL GETS A PROFILE ROW, AND THAT IS THE API'S DOING, NOT A
// PREFERENCE. API_WallType carries `profileAttr`; API_SlabType and
// API_ShellBaseType carry `modelElemStructureType`, `buildingMaterial` and
// `composite` and NOTHING ELSE - grep them. Handing every structural type the
// same three-way row would declare a Profile setting on a slab that the reader
// has no field to fill, which is exactly the "declared but never written"
// failure this file exists to prevent.
//
// Columns and beams are profiled too, but per SEGMENT (API_ColumnType::nSegments)
// rather than on the element, so their structure is not a row here either.
std::vector<S> StructureWithProfile ()
{
    return Join (Structure (), { { "profile",
                                   "Profile",
                                   G::Structure,
                                   ValueType::String,
                                   {},
                                   SettingOrigin::Archicad,
                                   WhenStructureIs ("Profiled") } });
}

// The three surfaces of a wall, each as a NAME plus whether that name is an
// element-level override.
//
// ⚠️ TWO SETTINGS PER SURFACE, BECAUSE API_OverriddenAttribute IS TWO THINGS.
// It is APIOptional<API_AttributeIndex>: an attribute index and a `hasValue`
// saying whether the element overrides its building material's surface or
// inherits it. Collapsed into one row, an inherited surface and a surface this
// build failed to read would render identically - and they are opposite facts.
// The bool is ALWAYS filled; the name only when there is an override to name.
// So an absent name beside `false` reads as "inherited", and an absent name
// beside `true` reads as a failed attribute lookup, which is visible.
//
// ⚠️ AND THERE IS NO "THE" INHERITED SURFACE TO RESOLVE. For a Basic wall it
// would be the building material's; for a composite it is per skin, and for a
// profile per component. Resolving one of those and printing it under a single
// label would be an answer to a question the model does not have.
std::vector<S> WallSurfaces ()
{
    return {
        { "referenceSurface", "Reference Surface", G::Display, ValueType::String, {} },
        { "referenceSurfaceOverridden", "Reference Surface Overridden", G::Display, ValueType::Bool, {} },
        { "oppositeSurface", "Opposite Surface", G::Display, ValueType::String, {} },
        { "oppositeSurfaceOverridden", "Opposite Surface Overridden", G::Display, ValueType::Bool, {} },
        { "sideSurface", "Side Surface", G::Display, ValueType::String, {} },
        { "sideSurfaceOverridden", "Side Surface Overridden", G::Display, ValueType::Bool, {} },
    };
}

// What every window, door and skylight has, from API_OpeningBaseType. Doors are
// API_WindowType by typedef, so one list genuinely serves all three.
std::vector<S> Opening ()
{
    return {
        { "width", "Width", G::Geometry, ValueType::Double, kMetres },
        { "height", "Height", G::Geometry, ValueType::Double, kMetres },
        { "reflected", "Mirrored", G::Geometry, ValueType::Bool, {} },
    };
}

// A type this build classifies but whose struct the reader does not yet
// transcribe. It gets the common settings and NOTHING ELSE - which is the honest
// state and a visible one. Inventing plausible-looking fields the reader never
// fills would be worse: an empty field and an unread field look identical.
ElementTypeDescriptor CommonOnly (std::string id, std::string label, std::string plural, bool container = true)
{
    return { std::move (id), std::move (label), std::move (plural), container, Common () };
}

const std::vector<ElementTypeDescriptor>& Catalog ()
{
    static const std::vector<ElementTypeDescriptor> catalog = [] {
        std::vector<ElementTypeDescriptor> types;

        types.push_back (
            { "wall", "Wall", "Walls", true,
              With (
                  Join (Join (
                            {
                                // The five a user names when asked what a
                                // wall is: where its reference line runs,
                                // how thick, how tall, how long, and how it
                                // sits on its story.
                                //
                                // ⚠️ `referenceLine` IS A LOCATION, NOT A
                                // CURVE, and its label now says so. It
                                // carries API_WallReferenceLineLocationID -
                                // "Center", "Outside" - and was previously
                                // labelled "Reference Line", which read as
                                // a promise of geometry that a String
                                // cannot keep. The id is UNCHANGED on
                                // purpose: a saved promotion addresses this
                                // setting by id, and re-pointing an id at a
                                // different ValueType is the one schema
                                // change no reader can detect.
                                { "referenceLine", "Reference Line Location", G::Placement, ValueType::String, {} },
                                // The curve itself, under its own id.
                                { "referenceLinePath", "Reference Line", G::Geometry, ValueType::Polyline, kMetres },
                                { "thickness", "Thickness", G::Geometry, ValueType::Double, kMetres },
                                { "height", "Height", G::Geometry, ValueType::Double, kMetres },
                                // ⚠️ DERIVED. API_WallType HAS NO LENGTH -
                                // it has begC, endC and angle. See
                                // ReferenceLineLength for the arithmetic
                                // and SettingOrigin for why the row says so.
                                { "length", "Length", G::Geometry, ValueType::Double, kMetres, SettingOrigin::Derived },
                                { "bottomOffset", "Bottom Offset", G::Placement, ValueType::Double, kMetres },
                                { "topOffset", "Top Offset", G::Placement, ValueType::Double, kMetres },
                                { "begin", "Begin", G::Geometry, ValueType::Point3, kMetres },
                                { "end", "End", G::Geometry, ValueType::Point3, kMetres },
                                { "wallShape", "Shape", G::Geometry, ValueType::String, {} },
                                { "slantAngle", "Slant Angle", G::Geometry, ValueType::Double, kDegrees },
                                { "flipped", "Flipped", G::Display, ValueType::Bool, {} },
                            },
                            StructureWithProfile ()),
                        WallSurfaces ())) });

        types.push_back ({ "slab", "Slab", "Slabs", true,
                           With (Join (
                               {
                                   { "thickness", "Thickness", G::Geometry, ValueType::Double, kMetres },
                                   // The slab's own elevation relative to its home
                                   // story, which is what a user means by a slab's
                                   // level. API_SlabType::level.
                                   { "level", "Level", G::Placement, ValueType::Double, kMetres },
                                   { "referencePlane", "Reference Plane", G::Placement, ValueType::String, {} },
                                   { "offsetFromTop", "Offset From Top", G::Placement, ValueType::Double, kMetres },
                                   { "outline", "Outline", G::Geometry, ValueType::Polyline, kMetres },
                                   { "vertexCount", "Vertices", G::Geometry, ValueType::Integer, {} },
                               },
                               Structure ())) });

        types.push_back ({ "column", "Column", "Columns", true,
                           With ({
                               { "height", "Height", G::Geometry, ValueType::Double, kMetres },
                               { "origin", "Origin", G::Placement, ValueType::Point3, kMetres },
                               { "bottomOffset", "Bottom Offset", G::Placement, ValueType::Double, kMetres },
                               { "topOffset", "Top Offset", G::Placement, ValueType::Double, kMetres },
                               { "axisRotation", "Axis Rotation", G::Placement, ValueType::Double, kDegrees },
                               { "slanted", "Slanted", G::Geometry, ValueType::Bool, {} },
                               { "slantAngle", "Slant Angle", G::Geometry, ValueType::Double, kDegrees },
                               { "slantDirection", "Slant Direction", G::Geometry, ValueType::Double, kDegrees },
                           }) });

        types.push_back ({ "beam", "Beam", "Beams", true,
                           With ({
                               { "begin", "Begin", G::Geometry, ValueType::Point3, kMetres },
                               { "end", "End", G::Geometry, ValueType::Point3, kMetres },
                               // ⚠️ DERIVED, AND IT IS THE PLAN LENGTH. Same
                               // arithmetic as a wall's, over begC/endC and
                               // curveAngle. A SLANTED beam's axis is longer than
                               // this by 1/cos(slantAngle) - the slant is reported
                               // beside it rather than folded in, because a length
                               // that silently means two different things
                               // depending on another field is worse than one that
                               // means one thing.
                               { "length", "Length", G::Geometry, ValueType::Double, kMetres, SettingOrigin::Derived },
                               { "referenceLinePath", "Reference Axis", G::Geometry, ValueType::Polyline, kMetres },
                               { "level", "Level", G::Placement, ValueType::Double, kMetres },
                               { "offset", "Reference Axis Offset", G::Placement, ValueType::Double, kMetres },
                               { "shape", "Shape", G::Geometry, ValueType::String, {} },
                               { "curveAngle", "Curve Angle", G::Geometry, ValueType::Double, kDegrees },
                               { "slanted", "Slanted", G::Geometry, ValueType::Bool, {} },
                               { "slantAngle", "Slant Angle", G::Geometry, ValueType::Double, kDegrees },
                               { "profileAngle", "Profile Angle", G::Geometry, ValueType::Double, kDegrees },
                           }) });

        types.push_back ({ "morph", "Morph", "Morphs", true,
                           With ({
                               { "level", "Level", G::Placement, ValueType::Double, kMetres },
                               { "bodyType", "Body", G::Geometry, ValueType::String, {} },
                               { "buildingMaterial", "Building Material", G::Structure, ValueType::String, {} },
                               { "castShadow", "Casts Shadow", G::Display, ValueType::Bool, {} },
                               { "receiveShadow", "Receives Shadow", G::Display, ValueType::Bool, {} },
                           }) });

        types.push_back ({ "object", "Object", "Objects", true,
                           With ({
                               { "libraryPart", "Library Part", G::Identity, ValueType::String, {} },
                               { "position", "Position", G::Placement, ValueType::Point3, kMetres },
                               { "level", "Level", G::Placement, ValueType::Double, kMetres },
                               { "angle", "Rotation", G::Placement, ValueType::Double, kDegrees },
                               { "sizeX", "Size X", G::Geometry, ValueType::Double, kMetres },
                               { "sizeY", "Size Y", G::Geometry, ValueType::Double, kMetres },
                               { "reflected", "Mirrored", G::Geometry, ValueType::Bool, {} },
                           }) });

        types.push_back ({ "lamp", "Lamp", "Lamps", true,
                           With ({
                               { "libraryPart", "Library Part", G::Identity, ValueType::String, {} },
                               { "position", "Position", G::Placement, ValueType::Point3, kMetres },
                               { "level", "Level", G::Placement, ValueType::Double, kMetres },
                               { "angle", "Rotation", G::Placement, ValueType::Double, kDegrees },
                               { "lightOn", "Light On", G::Display, ValueType::Bool, {} },
                           }) });

        types.push_back ({ "roof", "Roof", "Roofs", true,
                           With (Join (
                               {
                                   { "roofClass", "Roof Class", G::Geometry, ValueType::String, {} },
                                   { "thickness", "Thickness", G::Geometry, ValueType::Double, kMetres },
                                   { "level", "Level", G::Placement, ValueType::Double, kMetres },
                               },
                               Structure ())) });

        types.push_back ({ "shell", "Shell", "Shells", true,
                           With (Join (
                               {
                                   { "shellClass", "Shell Class", G::Geometry, ValueType::String, {} },
                                   { "thickness", "Thickness", G::Geometry, ValueType::Double, kMetres },
                                   { "level", "Level", G::Placement, ValueType::Double, kMetres },
                                   { "flipped", "Flipped", G::Display, ValueType::Bool, {} },
                               },
                               Structure ())) });

        types.push_back ({ "mesh", "Mesh", "Meshes", true,
                           With ({
                               { "level", "Level", G::Placement, ValueType::Double, kMetres },
                               { "skirtLevel", "Skirt Level", G::Placement, ValueType::Double, kMetres },
                               { "outline", "Outline", G::Geometry, ValueType::Polyline, kMetres },
                               { "vertexCount", "Vertices", G::Geometry, ValueType::Integer, {} },
                               { "buildingMaterial", "Building Material", G::Structure, ValueType::String, {} },
                           }) });

        types.push_back ({ "zone", "Zone", "Zones", true,
                           With ({
                               { "zoneName", "Zone Name", G::Identity, ValueType::String, {} },
                               { "zoneNumber", "Zone Number", G::Identity, ValueType::String, {} },
                               { "baseLevel", "Base Level", G::Placement, ValueType::Double, kMetres },
                               { "topOffset", "Top Offset", G::Placement, ValueType::Double, kMetres },
                               { "height", "Height", G::Geometry, ValueType::Double, kMetres },
                               { "floorThickness", "Floor Thickness", G::Geometry, ValueType::Double, kMetres },
                               { "outline", "Outline", G::Geometry, ValueType::Polyline, kMetres },
                               { "vertexCount", "Vertices", G::Geometry, ValueType::Integer, {} },
                           }) });

        // Composite and library-driven types. They are containers because a
        // selection genuinely holds them and a graph should be able to say "the
        // curtain walls"; their settings live in nested structures this build's
        // reader does not transcribe, so they show the common four and say so by
        // showing nothing else.
        types.push_back (CommonOnly ("curtainWall", "Curtain Wall", "Curtain Walls"));
        types.push_back (CommonOnly ("stair", "Stair", "Stairs"));
        types.push_back (CommonOnly ("railing", "Railing", "Railings"));

        // THE OPENINGS. Windows, doors and skylights share API_OpeningBaseType,
        // and API_DoorType IS API_WindowType by typedef - so the width and height
        // a user asks an opening for come from one place and are read once.
        //
        // A window and a door additionally sit somewhere ALONG their wall, which
        // a skylight does not: `objLoc` and `lower` are API_WindowType's, not the
        // opening base's.
        const std::vector<S> inWall = {
            { "sillHeight", "Sill Height", G::Placement, ValueType::Double, kMetres },
            { "positionAlongWall", "Position Along Wall", G::Placement, ValueType::Double, kMetres },
        };
        types.push_back ({ "door", "Door", "Doors", true, With (Join (Opening (), inWall)) });
        types.push_back ({ "window", "Window", "Windows", true, With (Join (Opening (), inWall)) });
        types.push_back ({ "skylight", "Skylight", "Skylights", true, With (Opening ()) });

        // NOT an opening in the API_OpeningBaseType sense. API_OpeningID is the
        // standalone void cut through an element, with its own struct, and giving
        // it a width and a height this build does not read would be inventing two
        // fields to make a name look consistent.
        types.push_back (CommonOnly ("opening", "Opening", "Openings"));

        // 2D and documentation elements. Classified so a mixed selection groups
        // them honestly, but given NO container node: a graph that operates on
        // dimensions and labels is not what this track builds, and a palette full
        // of nodes nothing consumes is worse than their absence.
        types.push_back (CommonOnly ("hatch", "Fill", "Fills", false));
        types.push_back (CommonOnly ("line", "Line", "Lines", false));
        types.push_back (CommonOnly ("polyLine", "Polyline", "Polylines", false));
        types.push_back (CommonOnly ("arc", "Arc", "Arcs", false));
        types.push_back (CommonOnly ("circle", "Circle", "Circles", false));
        types.push_back (CommonOnly ("spline", "Spline", "Splines", false));
        types.push_back (CommonOnly ("text", "Text", "Texts", false));
        types.push_back (CommonOnly ("label", "Label", "Labels", false));
        types.push_back (CommonOnly ("dimension", "Dimension", "Dimensions", false));
        types.push_back (CommonOnly ("hotspot", "Hotspot", "Hotspots", false));

        // Last, always. Everything this build does not name lands here rather
        // than disappearing out of the stack.
        types.push_back (CommonOnly (kUnclassifiedElementTypeId, "Other", "Other Elements", false));
        return types;
    }();
    return catalog;
}

// One pass over the catalog rather than one lookup per element: a thousand-
// element selection would otherwise be a thousand linear scans of a thirty-entry
// table for an answer that is the same every time.
std::vector<ElementTypeGroup> Collect (const std::vector<std::string>& typeOf,
                                       const std::function<void (size_t, ElementTypeGroup&)>& emit)
{
    std::map<std::string, std::vector<size_t>> byType;
    for (size_t i = 0; i < typeOf.size (); ++i) {
        const bool known = FindElementType (typeOf[i]) != nullptr;
        byType[known ? typeOf[i] : std::string (kUnclassifiedElementTypeId)].push_back (i);
    }

    std::vector<ElementTypeGroup> groups;
    for (const ElementTypeDescriptor& type : Catalog ()) {
        const auto found = byType.find (type.id);
        if (found == byType.end ())
            continue;
        ElementTypeGroup group;
        group.elementType = type.id;
        group.label = type.plural;
        for (const size_t index : found->second)
            emit (index, group);
        groups.push_back (std::move (group));
    }
    return groups;
}

} // namespace

const char* const kUnclassifiedElementTypeId = "other";

const char* SettingOriginName (SettingOrigin origin)
{
    switch (origin) {
        case SettingOrigin::Archicad:
            return "archicad";
        case SettingOrigin::Derived:
            return "derived";
    }
    return "archicad";
}

// The arc through `begin` and `end` subtending `angleRadians`, as centre and
// radius. Returns false for the cases that have no arc: a straight segment, a
// degenerate chord, or a full turn.
namespace {

constexpr double kPi = 3.14159265358979323846;

bool ArcOf (const Point3& begin, const Point3& end, double angleRadians, Point3& centre, double& radius,
            double& startAngle)
{
    if (!std::isfinite (angleRadians) || angleRadians == 0.0)
        return false;

    const double dx = end.x - begin.x;
    const double dy = end.y - begin.y;
    const double chord = std::sqrt (dx * dx + dy * dy);
    if (!(chord > 0.0))
        return false;

    const double half = std::sin (angleRadians / 2.0);
    // A chord subtending a zero or full turn has no finite circumscribing arc.
    if (std::abs (half) < 1e-12)
        return false;

    radius = chord / (2.0 * half);
    if (!std::isfinite (radius))
        return false;

    // The centre sits off the chord's midpoint along its perpendicular, on the
    // side the SIGN of the angle chooses. Getting that sign wrong mirrors every
    // curved wall in the model about its own chord, which is a failure that
    // looks like a modelling mistake rather than a reader one.
    const double midX = (begin.x + end.x) / 2.0;
    const double midY = (begin.y + end.y) / 2.0;
    const double apothem = radius * std::cos (angleRadians / 2.0);
    centre = Point3 { midX - apothem * (dy / chord), midY + apothem * (dx / chord), begin.z };

    // ⚠️ THE RADIUS GOES POSITIVE ONLY NOW, AND NOT ONE LINE EARLIER. Both terms
    // above are signed, and it is their sign that puts the centre on the correct
    // side of the chord for a clockwise sweep. But the caller uses the radius to
    // step AROUND that centre, and a negative one there reflects every point
    // through it - which put both sweep directions on the same side of the
    // chord, the exact mirroring this arithmetic exists to get right.
    radius = std::abs (radius);
    startAngle = std::atan2 (begin.y - centre.y, begin.x - centre.x);
    return true;
}

} // namespace

double ReferenceLineLength (const Point3& begin, const Point3& end, double angleRadians)
{
    Point3 centre;
    double radius = 0.0;
    double startAngle = 0.0;
    if (ArcOf (begin, end, angleRadians, centre, radius, startAngle))
        return std::abs (radius * angleRadians);

    const double dx = end.x - begin.x;
    const double dy = end.y - begin.y;
    const double length = std::sqrt (dx * dx + dy * dy);
    return std::isfinite (length) ? length : 0.0;
}

Polyline ReferenceLinePath (const Point3& begin, const Point3& end, double angleRadians, int segments)
{
    Polyline path;

    Point3 centre;
    double radius = 0.0;
    double startAngle = 0.0;
    if (!ArcOf (begin, end, angleRadians, centre, radius, startAngle)) {
        // ⚠️ EXACTLY TWO POINTS, NO TESSELLATION. A straight wall is the common
        // case; putting `segments` collinear vertices on it would make every
        // rectangular building's outline twenty times larger for no information.
        path.points.push_back (begin);
        path.points.push_back (end);
        return path;
    }

    // At least one segment per quarter turn, so a shallow arc does not get the
    // full budget and a near-full circle is not drawn as a triangle.
    const int steps = std::max (
        2,
        std::min (segments, std::max (2, static_cast<int> (std::ceil (std::abs (angleRadians) / (kPi / 2.0) * 8.0)))));
    path.points.reserve (static_cast<size_t> (steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double> (i) / static_cast<double> (steps);
        const double a = startAngle + angleRadians * t;
        path.points.push_back (Point3 { centre.x + radius * std::cos (a), centre.y + radius * std::sin (a), begin.z });
    }
    // The endpoints are the model's, not the arithmetic's: a rounding drift at
    // the last vertex would leave a visible gap where two walls meet.
    path.points.front () = begin;
    path.points.back () = end;
    return path;
}

const char* SettingGroupName (SettingGroup group)
{
    switch (group) {
        case SettingGroup::Identity:
            return "Identity";
        case SettingGroup::Placement:
            return "Placement";
        case SettingGroup::Geometry:
            return "Geometry";
        case SettingGroup::Structure:
            return "Structure";
        case SettingGroup::Display:
            return "Display";
    }
    return "Identity";
}

const std::vector<ElementTypeDescriptor>& ElementTypeCatalog ()
{
    return Catalog ();
}

const ElementTypeDescriptor* FindElementType (const std::string& id)
{
    const std::vector<ElementTypeDescriptor>& catalog = Catalog ();
    const auto found = std::find_if (catalog.begin (), catalog.end (),
                                     [&id] (const ElementTypeDescriptor& type) { return type.id == id; });
    return found == catalog.end () ? nullptr : &*found;
}

std::vector<ElementTypeGroup> GroupByElementType (const std::vector<ElementDescription>& elements)
{
    std::vector<std::string> typeOf;
    typeOf.reserve (elements.size ());
    for (const ElementDescription& element : elements)
        typeOf.push_back (element.elementType);

    return Collect (
        typeOf, [&elements] (size_t index, ElementTypeGroup& group) { group.elements.push_back (elements[index]); });
}

std::vector<ElementTypeGroup> GroupByCapturedTypes (const std::vector<std::string>& guids,
                                                    const std::vector<std::string>& elementTypes)
{
    std::vector<std::string> typeOf;
    typeOf.reserve (guids.size ());
    for (size_t i = 0; i < guids.size (); ++i)
        typeOf.push_back (i < elementTypes.size () ? elementTypes[i] : std::string (kUnclassifiedElementTypeId));

    return Collect (typeOf, [&guids] (size_t index, ElementTypeGroup& group) {
        ElementDescription element;
        element.guid = guids[index];
        element.elementType = group.elementType;
        // NOT `available`: a captured type id says what the element WAS when the
        // set was captured. Whether it is still there is a question only a read
        // can answer, and this function does not read.
        group.elements.push_back (std::move (element));
    });
}

} // namespace evp::nodegraph

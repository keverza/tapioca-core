#include "NodeGraph/ElementClassification.hpp"

#include <algorithm>
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

// The structure trio, shared by every type that can be Basic, Composite or
// Profiled. Repeating the three descriptors on six types is how they end up
// spelled differently on one of them.
std::vector<S> Structure ()
{
    return {
        { "structure", "Structure", G::Structure, ValueType::String, {} },
        { "buildingMaterial", "Building Material", G::Structure, ValueType::String, {} },
        { "composite", "Composite", G::Structure, ValueType::String, {} },
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

        types.push_back ({ "wall", "Wall", "Walls", true,
                           With (Join (
                               {
                                   // The five a user names when asked what a wall
                                   // is: where its reference line runs, how thick,
                                   // how tall, and how it sits on its story.
                                   { "referenceLine", "Reference Line", G::Placement, ValueType::String, {} },
                                   { "thickness", "Thickness", G::Geometry, ValueType::Double, kMetres },
                                   { "height", "Height", G::Geometry, ValueType::Double, kMetres },
                                   { "bottomOffset", "Bottom Offset", G::Placement, ValueType::Double, kMetres },
                                   { "topOffset", "Top Offset", G::Placement, ValueType::Double, kMetres },
                                   { "begin", "Begin", G::Geometry, ValueType::Point3, kMetres },
                                   { "end", "End", G::Geometry, ValueType::Point3, kMetres },
                                   { "wallShape", "Shape", G::Geometry, ValueType::String, {} },
                                   { "slantAngle", "Slant Angle", G::Geometry, ValueType::Double, kDegrees },
                                   { "flipped", "Flipped", G::Display, ValueType::Bool, {} },
                               },
                               Structure ())) });

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
        types.push_back (CommonOnly ("door", "Door", "Doors"));
        types.push_back (CommonOnly ("window", "Window", "Windows"));
        types.push_back (CommonOnly ("skylight", "Skylight", "Skylights"));
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

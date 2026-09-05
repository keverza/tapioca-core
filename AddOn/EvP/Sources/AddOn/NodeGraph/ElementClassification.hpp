#ifndef EVP_NODEGRAPH_ELEMENTCLASSIFICATION_HPP
#define EVP_NODEGRAPH_ELEMENTCLASSIFICATION_HPP

// WHAT AN ARCHICAD ELEMENT TYPE IS, as far as the graph is concerned: a stable
// id, a name, and the ORDERED list of settings that are worth showing for it.
//
// ⚠️ ONE TABLE, BECAUSE FOUR THINGS NEED THE SAME ANSWER. The container nodes
// need to know which types exist, the selection node needs to group a mixed
// selection by type, the inspector needs to know which fields a wall has and
// what to call them, and the ACAPI reader needs to know which fields to fill.
// Written four times, those four drift, and the failure is silent: an inspector
// showing a field the reader never fills looks exactly like an element whose
// value happens to be zero. Written once, a type that the reader does not fill
// is a type with no settings, which is visible.
//
// ⚠️ AND IT IS DEVKIT-FREE, so it is covered by the offline suite. The ACAPI
// half - ElementReaderImpl.cpp - is the only part that cannot be, and it is
// written as a straight transcription of this table so that the untested part
// stays as small and as dull as possible.
//
// ⚠️ READ-ONLY, DELIBERATELY. ADR-007 excludes model writes from this track, so
// nothing here edits an element: a setting is something the graph SHOWS you
// about the model, not a field you type into. The descriptors carry no default
// and no widget for that reason - offering an editor for a value that cannot be
// written back is a worse lie than offering nothing.

#include "NodeGraph/Value.hpp"

#include <map>
#include <string>
#include <vector>

namespace evp::nodegraph {

// The id every element whose type this build does not classify is given. Not an
// error: a Hotspot in a selection is a real element and the container that holds
// it should say "Other", not vanish.
extern const char* const kUnclassifiedElementTypeId;

// The groups a settings tree is divided into, in the order the inspector shows
// them. A flat vocabulary rather than a path, for the same reason ParameterUi's
// `section` is flat: a nested settings tree is a separate decision and not one
// to guess at while the reader fills four types.
enum class SettingGroup {
    Identity,
    Placement,
    Geometry,
    Structure,
    Display,
};

const char* SettingGroupName (SettingGroup group);

// WHERE A SETTING'S VALUE COMES FROM. Two members, because two is what this
// build can honestly distinguish today.
//
// ⚠️ IT IS NOT DECORATION. A user reading "Length" on a wall is entitled to know
// that Archicad has no such field - API_WallType carries begC, endC and angle,
// and the number in the row is arithmetic this build did. That matters the
// moment the number disagrees with a schedule: a native field that disagrees is
// a bug in the reader, and a derived one is a disagreement about definition.
enum class SettingOrigin {
    // Transcribed from an ACAPI struct field.
    Archicad,

    // Computed by Tapioca from fields that are. See ReferenceLineLength.
    Derived,
};

const char* SettingOriginName (SettingOrigin origin);

// WHEN A SETTING APPLIES AT ALL, as a value another setting takes.
//
// ⚠️ THIS EXISTS TO SEPARATE "NOT APPLICABLE" FROM "NOT READ", which the panel
// previously could not do. A Basic wall has no composite; the reader therefore
// writes none, and without this the inspector would report it as a field the
// build cannot read yet - telling the user the panel is short when it is
// complete. With it, the material rows are conditional on `structure` and
// exactly one of them is expected for any given wall.
//
// Deliberately ONE equality against ONE sibling, not an expression language. The
// cases that exist are `structure == "Basic"`, `"Composite"` and `"Profiled"` -
// the spellings SettingGroupName's sibling StructureName actually emits. A
// predicate grammar to serve three equalities would be a second thing to test
// and a second thing for every client to implement.
struct SettingCondition {
    // Empty when the setting always applies, which is the default.
    std::string settingId;

    // The TEXT the sibling's value must render as. Text rather than a Value
    // because the settings this gates are all closed String spellings, and
    // comparing rendered text is what a client can do without carrying the
    // runtime's Value at all.
    std::string equalsText;

    bool Always () const
    {
        return settingId.empty ();
    }
};

struct ElementSettingDescriptor {
    std::string id;
    std::string label;
    SettingGroup group = SettingGroup::Identity;
    ValueType valueType = ValueType::Absent;

    // ⚠️ THE UNIT OF THE STORED VALUE, NOT A LABEL STUCK ON AFTERWARDS. An
    // angle described as "deg" IS in degrees by the time it reaches a client;
    // the reader converts. The alternative - radians in the value and "deg" on
    // the field - is the kind of quiet disagreement that makes a 90 degree wall
    // read as 1.57.
    std::string unit;

    SettingOrigin origin = SettingOrigin::Archicad;

    // Empty settingId means unconditional, which is what almost every setting is.
    SettingCondition appliesWhen;
};

struct ElementTypeDescriptor {
    std::string id;     // "wall"
    std::string label;  // "Wall"
    std::string plural; // "Walls"

    // Whether this build registers a container node for the type. Every type is
    // classified; only the ones a graph can usefully hold get a node.
    bool container = false;

    std::vector<ElementSettingDescriptor> settings;
};

// In the order containers should be stacked: the load-bearing types first, then
// the rest, with the unclassified bucket last. Stable, because it is the order
// the user sees and an order that changed with the selection would make the
// stack unreadable.
const std::vector<ElementTypeDescriptor>& ElementTypeCatalog ();

const ElementTypeDescriptor* FindElementType (const std::string& id);

// ---------------------------------------------------------------------------
// THE DERIVED GEOMETRY OF A REFERENCE LINE.
//
// ⚠️ HERE RATHER THAN IN THE READER, AND THAT IS THE WHOLE POINT. Archicad
// stores a wall's reference line as two endpoints and a central angle; the
// length and the curve are arithmetic. Arithmetic in ElementReaderImpl.cpp
// would be arithmetic the offline suite cannot reach, in the one file whose
// defence is that it contains nothing to get wrong. So the reader hands over
// three doubles and copies back the answer, and the sign conventions, the
// degenerate chord and the straight-line case are all tested here.
//
// `angleRadians` is the SIGNED central angle - API_WallType::angle,
// API_BeamType::curveAngle. Zero means straight, which is the common case and
// the one that must not go through the arc path at all.

// The plan length of the reference line. Zero for a degenerate chord, which is
// a real thing a model can contain and not an error to refuse.
double ReferenceLineLength (const Point3& begin, const Point3& end, double angleRadians);

// The reference line as a polyline, tessellated when it is an arc.
//
// A straight line is exactly two points and no tessellation - so a rectangular
// building does not acquire a hundred collinear vertices per wall. `segments`
// caps the arc; the default is what an inspector row and a downstream curve can
// both live with.
Polyline ReferenceLinePath (const Point3& begin, const Point3& end, double angleRadians, int segments = 24);

// One element as the host read it.
struct ElementDescription {
    std::string guid;
    std::string elementType = kUnclassifiedElementTypeId;

    // Archicad's own name for the type ("Wall", "Curtain Wall Panel"). Kept
    // beside our id because they are different things: the id is a contract, the
    // label is localised and changes with the user's Archicad.
    std::string typeLabel;

    // Absent keys are ABSENT, never zero. A wall whose thickness could not be
    // read and a wall 0 mm thick are different facts.
    std::map<std::string, Value> settings;

    // False when the element could not be read at all - deleted, or outside this
    // user's Teamwork workspace. `detail` says which.
    bool available = false;
    std::string detail;
};

struct ElementTypeGroup {
    std::string elementType;
    std::string label;
    std::vector<ElementDescription> elements;
};

// Catalog order, empty groups omitted, element order preserved inside a group.
std::vector<ElementTypeGroup> GroupByElementType (const std::vector<ElementDescription>& elements);

// The same grouping from nothing but the type ids, which is what the selection
// node's stored capture holds. Takes the guids so the groups still name their
// members; a shorter type list than guid list leaves the remainder unclassified
// rather than dropping it.
std::vector<ElementTypeGroup> GroupByCapturedTypes (const std::vector<std::string>& guids,
                                                    const std::vector<std::string>& elementTypes);

} // namespace evp::nodegraph

#endif

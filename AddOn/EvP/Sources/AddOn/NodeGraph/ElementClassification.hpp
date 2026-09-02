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

#include "NodeGraph/ArchicadNodes.hpp"
#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/NodeExecution.hpp"
#include "NodeGraph/NodeRegistry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace evp::nodegraph;

// ---------------------------------------------------------------------------
// The element type table, and the containers generated from it.
//
// ⚠️ WHAT THESE TESTS ARE FOR. Four things read this table - the container
// nodes, the selection node's grouping, the inspector, and the ACAPI reader -
// and the failure mode when they disagree is SILENT: an inspector row for a
// setting the reader never fills looks exactly like a value that happens to be
// zero. The table itself is pure, so the parts of that contract that can be
// checked offline are checked here.
// ---------------------------------------------------------------------------

namespace {

const ElementTypeDescriptor& Type (const std::string& id)
{
    const ElementTypeDescriptor* type = FindElementType (id);
    EXPECT_NE (nullptr, type) << id;
    return *type;
}

bool HasSetting (const ElementTypeDescriptor& type, const std::string& settingId)
{
    return std::any_of (type.settings.begin (), type.settings.end (),
                        [&settingId] (const ElementSettingDescriptor& s) { return s.id == settingId; });
}

const ElementSettingDescriptor& Setting (const ElementTypeDescriptor& type, const std::string& settingId)
{
    const auto found = std::find_if (type.settings.begin (), type.settings.end (),
                                     [&settingId] (const ElementSettingDescriptor& s) { return s.id == settingId; });
    EXPECT_NE (type.settings.end (), found) << type.id << "." << settingId;
    return *found;
}

ElementDescription Described (const std::string& guid, const std::string& elementType)
{
    ElementDescription description;
    description.guid = guid;
    description.elementType = elementType;
    description.available = true;
    return description;
}

} // namespace

TEST (ElementClassification, EveryTypeIdIsUniqueAndNamed)
{
    std::set<std::string> ids;
    for (const ElementTypeDescriptor& type : ElementTypeCatalog ()) {
        EXPECT_FALSE (type.id.empty ());
        EXPECT_FALSE (type.label.empty ()) << type.id;
        EXPECT_FALSE (type.plural.empty ()) << type.id;
        EXPECT_TRUE (ids.insert (type.id).second) << "duplicate type id " << type.id;
    }
}

TEST (ElementClassification, EverySettingIsUniqueWithinItsTypeAndCarriesAValueType)
{
    // A duplicate id inside one type would make the settings map ambiguous, and
    // the second row would silently shadow the first in the inspector.
    for (const ElementTypeDescriptor& type : ElementTypeCatalog ()) {
        std::set<std::string> ids;
        for (const ElementSettingDescriptor& setting : type.settings) {
            EXPECT_TRUE (ids.insert (setting.id).second) << type.id << " repeats " << setting.id;
            EXPECT_FALSE (setting.label.empty ()) << type.id << "." << setting.id;
            EXPECT_NE (ValueType::Absent, setting.valueType) << type.id << "." << setting.id;
        }
    }
}

TEST (ElementClassification, EveryTypeAsksTheFourQuestionsAskedOfAnyElement)
{
    // ID, layer and story are what a user asks before they ask anything
    // type-specific, so they are on EVERY type - including the ones whose struct
    // this build does not transcribe. A type missing them would have a settings
    // panel that could not answer "what layer is this on".
    for (const ElementTypeDescriptor& type : ElementTypeCatalog ()) {
        EXPECT_TRUE (HasSetting (type, "elementId")) << type.id;
        EXPECT_TRUE (HasSetting (type, "layer")) << type.id;
        EXPECT_TRUE (HasSetting (type, "homeStory")) << type.id;
        EXPECT_TRUE (HasSetting (type, "homeStoryLevel")) << type.id;
    }
}

TEST (ElementClassification, AWallExposesWhatAUserCallsAWall)
{
    const ElementTypeDescriptor& wall = Type ("wall");
    EXPECT_TRUE (HasSetting (wall, "referenceLine"));
    EXPECT_TRUE (HasSetting (wall, "thickness"));
    EXPECT_TRUE (HasSetting (wall, "height"));
    EXPECT_TRUE (HasSetting (wall, "bottomOffset"));
    EXPECT_TRUE (HasSetting (wall, "homeStory"));
    EXPECT_TRUE (HasSetting (wall, "structure"));
}

TEST (ElementClassification, ASlabExposesItsOutlineThicknessLevelAndOffset)
{
    const ElementTypeDescriptor& slab = Type ("slab");
    EXPECT_TRUE (HasSetting (slab, "outline"));
    EXPECT_TRUE (HasSetting (slab, "thickness"));
    EXPECT_TRUE (HasSetting (slab, "level"));
    EXPECT_TRUE (HasSetting (slab, "offsetFromTop"));
    EXPECT_TRUE (HasSetting (slab, "referencePlane"));
    EXPECT_EQ (ValueType::Polyline, Setting (slab, "outline").valueType);
}

TEST (ElementClassification, TheUnitIsTheUnitOfTheStoredValue)
{
    // ⚠️ NOT A LABEL STUCK ON AFTERWARDS. A length says "m" and an angle says
    // "deg" because that is what the reader puts in the value; the pairing is
    // checked here so a new setting cannot quietly describe radians as degrees.
    EXPECT_EQ ("m", Setting (Type ("wall"), "thickness").unit);
    EXPECT_EQ ("deg", Setting (Type ("column"), "slantAngle").unit);
    EXPECT_EQ ("deg", Setting (Type ("object"), "angle").unit);
    // A name or a flag has no unit at all, rather than an empty-looking one.
    EXPECT_EQ ("", Setting (Type ("wall"), "referenceLine").unit);
    EXPECT_EQ ("", Setting (Type ("morph"), "castShadow").unit);
}

TEST (ElementClassification, TheUnclassifiedBucketExistsAndComesLast)
{
    // Everything this build cannot name still gets a row. Dropping it would make
    // a selection of eight elements show a stack that adds up to six.
    const std::vector<ElementTypeDescriptor>& catalog = ElementTypeCatalog ();
    ASSERT_FALSE (catalog.empty ());
    EXPECT_EQ (kUnclassifiedElementTypeId, catalog.back ().id);
    EXPECT_FALSE (catalog.back ().container);
}

TEST (ElementClassification, GroupingFollowsTheCatalogOrderNotTheSelectionOrder)
{
    // A stack whose order changed with what happened to be selected first would
    // be unreadable: the same model, clicked in a different order, would give a
    // different panel.
    const std::vector<ElementTypeGroup> groups = GroupByElementType ({
        Described ("c", "column"),
        Described ("s", "slab"),
        Described ("w", "wall"),
        Described ("w2", "wall"),
    });

    ASSERT_EQ (3U, groups.size ());
    EXPECT_EQ ("wall", groups[0].elementType);
    EXPECT_EQ ("slab", groups[1].elementType);
    EXPECT_EQ ("column", groups[2].elementType);
    EXPECT_EQ ("Walls", groups[0].label);
    // Order INSIDE a group is the order given, which is Archicad's selection
    // order and therefore the one the user can recognise.
    ASSERT_EQ (2U, groups[0].elements.size ());
    EXPECT_EQ ("w", groups[0].elements[0].guid);
    EXPECT_EQ ("w2", groups[0].elements[1].guid);
}

TEST (ElementClassification, AnUnknownTypeIdLandsInTheUnclassifiedGroupRatherThanVanishing)
{
    const std::vector<ElementTypeGroup> groups =
        GroupByElementType ({ Described ("a", "wall"), Described ("b", "notAType"), Described ("c", "") });

    ASSERT_EQ (2U, groups.size ());
    EXPECT_EQ ("wall", groups[0].elementType);
    EXPECT_EQ (kUnclassifiedElementTypeId, groups[1].elementType);
    EXPECT_EQ (2U, groups[1].elements.size ());
}

TEST (ElementClassification, EmptyGroupsAreOmitted)
{
    EXPECT_TRUE (GroupByElementType ({}).empty ());
    EXPECT_EQ (1U, GroupByElementType ({ Described ("a", "beam") }).size ());
}

TEST (ElementClassification, ACapturedTypeListShorterThanItsGuidsLeavesTheRemainderUnclassified)
{
    // The two parameters are parallel, and a client that hand-edited one is the
    // reason this cannot simply trust the sizes. Dropping the remainder would
    // make elements disappear from the stack; filing them under the LAST known
    // type would be worse still.
    const std::vector<ElementTypeGroup> groups = GroupByCapturedTypes ({ "a", "b", "c" }, { "wall" });

    ASSERT_EQ (2U, groups.size ());
    EXPECT_EQ ("wall", groups[0].elementType);
    EXPECT_EQ (1U, groups[0].elements.size ());
    EXPECT_EQ (kUnclassifiedElementTypeId, groups[1].elementType);
    EXPECT_EQ (2U, groups[1].elements.size ());
}

TEST (ElementClassification, ACapturedGroupDoesNotClaimTheElementIsStillThere)
{
    // GroupByCapturedTypes reads no host, so it cannot know. Reporting
    // `available` from a capture would make a deleted element look present.
    const std::vector<ElementTypeGroup> groups = GroupByCapturedTypes ({ "a" }, { "wall" });
    ASSERT_EQ (1U, groups.size ());
    ASSERT_EQ (1U, groups[0].elements.size ());
    EXPECT_FALSE (groups[0].elements[0].available);
    EXPECT_EQ ("a", groups[0].elements[0].guid);
}

// --- the generated containers ----------------------------------------------

TEST (ElementContainers, EveryContainerTypeInTheCatalogIsRegisteredAsANode)
{
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    size_t containers = 0;
    for (const ElementTypeDescriptor& type : ElementTypeCatalog ()) {
        const std::string nodeType = ElementContainerNodeType (type.id);
        if (!type.container) {
            // A documentation element gets no node. A palette full of nodes
            // nothing consumes is worse than their absence.
            EXPECT_TRUE (nodeType.empty ()) << type.id;
            continue;
        }
        ++containers;
        ASSERT_FALSE (nodeType.empty ()) << type.id;
        const NodeType* node = registry.Find (nodeType);
        ASSERT_NE (nullptr, node) << nodeType;
        EXPECT_EQ (type.plural, node->label);
        EXPECT_EQ ("Archicad Elements", node->category);
    }
    // Not an arbitrary number: it is the count the table declares, and a type
    // that quietly lost its container would drop it.
    EXPECT_EQ (18U, containers);
}

TEST (ElementContainers, AContainerIsReadModelBecauseTheTypeIsAQuestionAboutTheModel)
{
    // ⚠️ THE DIFFERENCE FROM THE SELECTION SET, WHICH IS Pure. A captured set is
    // a thing the user holds; a container is a question that has to be asked
    // again when the model moves, so it declares the Project generation and gets
    // re-run when that changes.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* walls = registry.Find (ElementContainerNodeType ("wall"));
    ASSERT_NE (nullptr, walls);
    EXPECT_EQ (EffectKind::ReadModel, walls->effect);
    EXPECT_EQ (ExecutionDomain::ArchicadMainThread, walls->executionDomain);
    EXPECT_TRUE (walls->generations.Contains (GenerationDomain::Project));

    const NodeType* selection = registry.Find (kSelectionSetNodeType);
    ASSERT_NE (nullptr, selection);
    EXPECT_EQ (EffectKind::Pure, selection->effect);
}

TEST (ElementContainers, AContainerIsNotBypassableBecauseItsCountHasNowhereToComeFrom)
{
    // Passing the elements through IS what bypassing a filter should mean - but
    // a bypass table has to feed every output, and `count` has no input to come
    // from. The registry refuses a partial table, correctly: the alternative is
    // a bypassed container reporting a count that came from nowhere.
    const NodeRegistry registry = MakeRuntimeNodeRegistry ();
    const NodeType* slabs = registry.Find (ElementContainerNodeType ("slab"));
    ASSERT_NE (nullptr, slabs);
    EXPECT_TRUE (slabs->bypassMappings.empty ());
}

TEST (ElementContainers, TheNodeTypeAndTheElementTypeRoundTrip)
{
    EXPECT_EQ ("archicad.container.wall", ElementContainerNodeType ("wall"));
    EXPECT_EQ ("wall", ElementTypeOfContainerNode ("archicad.container.wall"));

    // A classified type with no container is not a container node, and neither
    // is a plausible-looking id for a type that does not exist.
    EXPECT_EQ ("", ElementContainerNodeType ("text"));
    EXPECT_EQ ("", ElementTypeOfContainerNode ("archicad.container.text"));
    EXPECT_EQ ("", ElementTypeOfContainerNode ("archicad.container.banana"));
    EXPECT_EQ ("", ElementTypeOfContainerNode ("archicad.getSelection"));
    EXPECT_EQ ("", ElementTypeOfContainerNode (""));
}

TEST (ElementContainers, TheCapturedTypeListRoundTripsThroughAValue)
{
    const std::vector<std::string> types { "wall", "slab", "other" };
    const std::vector<std::string> read = TypesFromValue (ValueFromTypes (types));
    EXPECT_EQ (types, read);

    // A non-string entry reads back as unclassified rather than being dropped:
    // a SHORTER list would shift every following element into its neighbour's
    // container, which looks plausible and is entirely wrong.
    std::vector<Value> mixed;
    mixed.emplace_back (std::string ("wall"));
    mixed.emplace_back (int64_t { 7 });
    mixed.emplace_back (std::string ("slab"));
    const std::vector<std::string> recovered = TypesFromValue (Argument::FromItems (std::move (mixed)));
    ASSERT_EQ (3U, recovered.size ());
    EXPECT_EQ ("wall", recovered[0]);
    EXPECT_EQ (kUnclassifiedElementTypeId, recovered[1]);
    EXPECT_EQ ("slab", recovered[2]);

    // Anything that is not a list at all is an empty capture, not a crash.
    EXPECT_TRUE (TypesFromValue (Value (std::string ("wall"))).empty ());
    EXPECT_TRUE (TypesFromValue (Value ()).empty ());
}

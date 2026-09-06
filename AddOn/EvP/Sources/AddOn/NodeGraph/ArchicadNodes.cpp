#include "NodeGraph/ArchicadNodes.hpp"

#include "NodeGraph/ArchicadHost.hpp"
#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/Json.hpp"
#include "NodeGraph/ParameterDescriptors.hpp"

#include <stdexcept>

namespace evp::nodegraph {
namespace {

constexpr const char kGetSelection[] = "archicad.getSelection";
constexpr const char kSetSelection[] = "archicad.setSelection";
constexpr const char kContainerPrefix[] = "archicad.container.";
constexpr const char kLibraryPart[] = "archicad.libraryPart";
constexpr const char kElementSetting[] = "archicad.element.setting";
constexpr const char kCamera[] = "archicad.camera";

std::vector<Value> ToValueList (const std::vector<ArchicadElementRef>& elements)
{
    std::vector<Value> list;
    list.reserve (elements.size ());
    for (const ArchicadElementRef& element : elements)
        list.emplace_back (element);
    return list;
}

std::vector<Value> ToStringList (const std::vector<std::string>& values)
{
    std::vector<Value> list;
    list.reserve (values.size ());
    for (const std::string& value : values)
        list.emplace_back (value);
    return list;
}

// ---------------------------------------------------------------------------
// THE CAMERA CODEC.
//
// ⚠️ THE FIELD NAMES ARE StartDiligentCapture'S, NOT THIS FILE'S CHOICE.
// A camera captured here is handed to the renderer verbatim by the capture node,
// so writing any other spelling would put a translation step between the two
// halves of one feature - and a translation step is where an axis goes missing.
// The four fields this interface does not carry are written anyway, with the
// values the renderer's own schema requires, because that schema demands all
// eleven and a partial object is refused rather than defaulted.
// The field whose presence says "this string is a camera at all". Any of the
// eleven would do; `valid` is the one a hand-written stub is likeliest to carry,
// so requiring it rejects the least and still rejects a string that is not a
// camera object.
constexpr const char kCameraProbeField[] = "valid";

double ReadNumber (const json::JsonValue& object, const char* key)
{
    const json::JsonValue* found = object.Find (key);
    double value = 0.0;
    if (found != nullptr)
        found->AsDouble (value);
    return value;
}

} // namespace

std::string EncodeCamera (const ViewCamera& camera)
{
    json::JsonObject fields;
    fields["valid"] = json::JsonValue::Bool (camera.valid);
    fields["source"] = json::JsonValue::String (camera.source);
    // Always false, and written rather than omitted: the renderer's schema
    // requires the field, and this path never captures a parallel projection -
    // ReadViewCameraOnHostThread refuses one before it gets here.
    fields["orthographic"] = json::JsonValue::Bool (false);
    // Likewise always false. `viewMoving` means "Archicad was provably scrolling
    // while this was read", which is a live-sync concern; a camera the user
    // captured by pressing a button was read from a settled view.
    fields["viewMoving"] = json::JsonValue::Bool (false);
    fields["eyeX"] = json::JsonValue::Double (camera.eye[0]);
    fields["eyeY"] = json::JsonValue::Double (camera.eye[1]);
    fields["eyeZ"] = json::JsonValue::Double (camera.eye[2]);
    fields["targetX"] = json::JsonValue::Double (camera.target[0]);
    fields["targetY"] = json::JsonValue::Double (camera.target[1]);
    fields["targetZ"] = json::JsonValue::Double (camera.target[2]);
    fields["viewConeDegreesHorizontal"] = json::JsonValue::Double (camera.viewConeDegreesHorizontal);
    // ⚠️ THE SUN TRAVELS IN THE SAME ROW AS THE CAMERA, NOT IN A SECOND
    // PARALLEL LIST. The selection set already carries a pair of parallel lists
    // (guids and their types) and its own comments record the cost: when one is
    // shorter than the other every entry after the gap is wrong about itself,
    // and the panel still adds up. One row that holds everything about one
    // capture cannot fall out of step with itself.
    //
    // These four names are outside StartDiligentCapture's camera schema, which
    // sets additionalProperties false - so the capture node sends the camera
    // fields and the sun fields to different places rather than forwarding this
    // object whole. That is a slice 4 concern; storing them together is what
    // makes it possible at all.
    fields["hasSun"] = json::JsonValue::Bool (camera.hasSun);
    fields["sunAzimuthDegrees"] = json::JsonValue::Double (camera.sunAzimuthDegrees);
    fields["sunAltitudeDegrees"] = json::JsonValue::Double (camera.sunAltitudeDegrees);
    fields["sunBearingDegrees"] = json::JsonValue::Double (camera.sunBearingDegrees);
    fields["sunSource"] = json::JsonValue::String (camera.sunSource);
    // ⚠️ ARCHICAD'S OWN SUN STRUCT, STORED SO RESTORE SURVIVES A SAVE.
    // The derived angles above cannot be written back - their convention is this
    // repository's, not the DevKit's - so without these a graph reloaded from
    // disk could position the camera and not the light. See ViewCamera.
    fields["sunFromDate"] = json::JsonValue::Bool (camera.sunFromDate);
    fields["sunRawAzimuth"] = json::JsonValue::Double (camera.sunRawAzimuth);
    fields["sunRawAltitude"] = json::JsonValue::Double (camera.sunRawAltitude);
    fields["sunYear"] = json::JsonValue::Integer (camera.sunYear);
    fields["sunMonth"] = json::JsonValue::Integer (camera.sunMonth);
    fields["sunDay"] = json::JsonValue::Integer (camera.sunDay);
    fields["sunHour"] = json::JsonValue::Integer (camera.sunHour);
    fields["sunMinute"] = json::JsonValue::Integer (camera.sunMinute);
    fields["sunSecond"] = json::JsonValue::Integer (camera.sunSecond);
    fields["sunSummerTime"] = json::JsonValue::Bool (camera.sunSummerTime);
    // One line: this is a parameter value, not a file a human reads in a diff.
    return json::Write (json::JsonValue::Object (std::move (fields)), 0);
}

bool DecodeCamera (const std::string& encoded, ViewCamera& camera)
{
    const json::ParseResult parsed = json::Parse (encoded);
    if (!parsed.ok || parsed.value.Find (kCameraProbeField) == nullptr)
        return false;

    bool valid = false;
    const json::JsonValue* validField = parsed.value.Find ("valid");
    if (validField != nullptr)
        validField->AsBool (valid);

    ViewCamera read;
    read.valid = valid;
    const json::JsonValue* source = parsed.value.Find ("source");
    if (source != nullptr)
        source->AsString (read.source);
    read.eye[0] = ReadNumber (parsed.value, "eyeX");
    read.eye[1] = ReadNumber (parsed.value, "eyeY");
    read.eye[2] = ReadNumber (parsed.value, "eyeZ");
    read.target[0] = ReadNumber (parsed.value, "targetX");
    read.target[1] = ReadNumber (parsed.value, "targetY");
    read.target[2] = ReadNumber (parsed.value, "targetZ");
    read.viewConeDegreesHorizontal = ReadNumber (parsed.value, "viewConeDegreesHorizontal");
    // Absent means NO SUN, which is exactly what a graph saved before this
    // existed should read as - not a sun at azimuth zero, which is a real
    // direction and would light every restored frame from due east.
    const json::JsonValue* hasSun = parsed.value.Find ("hasSun");
    bool sun = false;
    if (hasSun != nullptr)
        hasSun->AsBool (sun);
    read.hasSun = sun;
    read.sunAzimuthDegrees = ReadNumber (parsed.value, "sunAzimuthDegrees");
    read.sunAltitudeDegrees = ReadNumber (parsed.value, "sunAltitudeDegrees");
    read.sunBearingDegrees = ReadNumber (parsed.value, "sunBearingDegrees");
    const json::JsonValue* sunSource = parsed.value.Find ("sunSource");
    if (sunSource != nullptr)
        sunSource->AsString (read.sunSource);

    const auto flag = [&parsed] (const char* key) {
        const json::JsonValue* found = parsed.value.Find (key);
        bool value = false;
        if (found != nullptr)
            found->AsBool (value);
        return value;
    };
    const auto whole = [&parsed] (const char* key) {
        const json::JsonValue* found = parsed.value.Find (key);
        int64_t value = 0;
        if (found != nullptr)
            found->AsInteger (value);
        return static_cast<int> (value);
    };
    read.sunFromDate = flag ("sunFromDate");
    read.sunRawAzimuth = ReadNumber (parsed.value, "sunRawAzimuth");
    read.sunRawAltitude = ReadNumber (parsed.value, "sunRawAltitude");
    read.sunYear = whole ("sunYear");
    read.sunMonth = whole ("sunMonth");
    read.sunDay = whole ("sunDay");
    read.sunHour = whole ("sunHour");
    read.sunMinute = whole ("sunMinute");
    read.sunSecond = whole ("sunSecond");
    read.sunSummerTime = flag ("sunSummerTime");
    camera = read;
    return true;
}

std::vector<ViewCamera> CamerasFromValue (const Argument& value)
{
    std::vector<ViewCamera> cameras;

    // ⚠️ A LONE CAMERA IS A ONE-ITEM LIST, NOT A MISTAKE, AND READING IT
    // AS EMPTY WAS A REAL BUG. The tree layer projects a branch holding one item
    // back as that ITEM rather than as a list - ordinary tree semantics, and the
    // same reason `structureOfValue` calls a non-list an item. So a camera node
    // holding exactly one camera hands the capture node a String, and a decoder
    // that insisted on a List would have rendered nothing while reporting
    // success. One camera is the commonest list there is.
    if (value.Type () == ValueType::String) {
        ViewCamera camera;
        if (DecodeCamera (std::get<std::string> (value.DataValue ()), camera))
            cameras.push_back (camera);
        return cameras;
    }

    if (value.Type () != ValueType::List)
        return cameras;
    for (const Value& item : value.Items ()) {
        if (item.Type () != ValueType::String)
            continue;
        ViewCamera camera;
        // ⚠️ AN UNREADABLE ROW IS DROPPED, NOT SUBSTITUTED. It can only come
        // from a hand-edited document, and a zeroed camera would take its place
        // in the list looking exactly like a real one - then point the 3D window
        // at the origin when somebody pressed Restore on it.
        if (DecodeCamera (std::get<std::string> (item.DataValue ()), camera))
            cameras.push_back (camera);
    }
    return cameras;
}

std::string EncodeCameraSun (const ViewCamera& camera)
{
    json::JsonObject fields;
    // ⚠️ SetDiligentSun'S OWN FIELD NAMES. `enabled` is that command's word
    // for "override the project's sun with these angles", and it is false for a
    // camera that carries none - so wiring a sunless camera into anything that
    // consumes this leaves the renderer's own sun alone instead of pointing it
    // at due east.
    fields["enabled"] = json::JsonValue::Bool (camera.hasSun);
    // The MODEL angle, CCW from +X. Not the bearing - see ViewCamera.
    fields["azimuthDegrees"] = json::JsonValue::Double (camera.sunAzimuthDegrees);
    fields["altitudeDegrees"] = json::JsonValue::Double (camera.sunAltitudeDegrees);
    // Carried alongside for a reader rather than for the renderer, which never
    // asks for it. A panel showing "-80" would send somebody hunting for a bug
    // that is a convention.
    fields["bearingDegrees"] = json::JsonValue::Double (camera.sunBearingDegrees);
    // Which rule produced the angles. Published rather than kept internal because
    // the two ways this can be wrong - a frozen sun and a discarded typed one -
    // look identical from the numbers, and this is the field that tells them
    // apart without a rebuild.
    fields["source"] = json::JsonValue::String (camera.sunSource);
    return json::Write (json::JsonValue::Object (std::move (fields)), 0);
}

Argument SunFromCameras (const std::vector<ViewCamera>& cameras)
{
    std::vector<Value> list;
    list.reserve (cameras.size ());
    for (const ViewCamera& camera : cameras)
        list.emplace_back (EncodeCameraSun (camera));
    return Argument::FromItems (std::move (list));
}

Argument ValueFromCameras (const std::vector<ViewCamera>& cameras)
{
    std::vector<Value> list;
    list.reserve (cameras.size ());
    for (const ViewCamera& camera : cameras)
        list.emplace_back (EncodeCamera (camera));
    return Argument::FromItems (std::move (list));
}

const char* const kSelectionSetNodeType = kGetSelection;
const char* const kSelectionSetParameter = "elements";
const char* const kSelectionTypesParameter = "elementTypes";
const char* const kElementContainerPrefix = kContainerPrefix;
const char* const kCameraSetNodeType = kCamera;
const char* const kCameraSetParameter = "cameras";

std::string ElementContainerNodeType (const std::string& elementTypeId)
{
    const ElementTypeDescriptor* type = FindElementType (elementTypeId);
    if (type == nullptr || !type->container)
        return {};
    return std::string (kContainerPrefix) + type->id;
}

std::string ElementTypeOfContainerNode (const std::string& nodeTypeId)
{
    const std::string prefix (kContainerPrefix);
    if (nodeTypeId.rfind (prefix, 0) != 0)
        return {};
    const std::string elementType = nodeTypeId.substr (prefix.size ());
    const ElementTypeDescriptor* type = FindElementType (elementType);
    return (type != nullptr && type->container) ? elementType : std::string {};
}

std::vector<std::string> TypesFromValue (const Argument& value)
{
    std::vector<std::string> types;
    if (value.Type () != ValueType::List)
        return types;
    for (const Value& item : value.Items ()) {
        types.push_back (item.Type () == ValueType::String ? std::get<std::string> (item.DataValue ())
                                                           : std::string (kUnclassifiedElementTypeId));
    }
    return types;
}

Argument ValueFromTypes (const std::vector<std::string>& types)
{
    return Argument::FromItems (ToStringList (types));
}

std::vector<ArchicadElementRef> ElementsFromValue (const Argument& value)
{
    std::vector<ArchicadElementRef> elements;
    if (value.Type () != ValueType::List)
        return elements;
    for (const Value& item : value.Items ()) {
        if (item.Type () == ValueType::ArchicadElementRef)
            elements.push_back (std::get<ArchicadElementRef> (item.DataValue ()));
    }
    return elements;
}

Argument ValueFromElements (const std::vector<ArchicadElementRef>& elements)
{
    return Argument::FromItems (ToValueList (elements));
}

void RegisterElementContainers (NodeRegistry& registry);

void RegisterArchicadNodes (NodeRegistry& registry)
{
    std::string error;

    // A SELECTION SET the user captures, not a live mirror of Archicad's
    // selection - the command palette's Update/Add/Remove/Reselect/Clear rows,
    // as a node.
    //
    // ⚠️ THIS IS WHY IT IS Pure/Worker AND DECLARES NO GENERATION. The node's
    // output IS its stored parameter, so evaluating it reads nothing from the
    // host. That is the whole behavioural point: a graph whose source tracked
    // the live selection silently changed its answer every time the user
    // clicked in the model, and every downstream node went dirty with it. A
    // captured set changes only when the user presses one of its buttons, and
    // those buttons evaluate what they affect on the spot - so nobody has to
    // press Evaluate to see the result of pressing Update.
    //
    // It also means the set PERSISTS with the graph: it is an ordinary
    // parameter, so it saves, loads and round-trips with no extra machinery,
    // and §7.2's rule holds - references stay references and are re-resolved
    // when something actually uses them.
    NodeType getSelection;
    getSelection.id = kGetSelection;
    getSelection.label = "Get Selection";
    getSelection.category = "Archicad";
    getSelection.description = "A set of Archicad elements you capture. Update replaces it with the current selection, "
                               "Add and Remove change it, Reselect selects it in Archicad, Clear empties it.";
    getSelection.executionDomain = ExecutionDomain::Worker;
    getSelection.effect = EffectKind::Pure;
    getSelection.display = NodeDisplay::SelectionSet;
    // NO DEFAULT VALUE, deliberately. An absent parameter already means an
    // empty set - ExecuteArchicadNode reads it that way - so a default would be
    // a second spelling of the same state, and it would put a list into the
    // catalog's defaultValue where every other node has a scalar.
    getSelection.parameters.push_back ({ kSelectionSetParameter, "Elements", ValueType::List, false });
    // Parallel to the guids, one type id each. See kSelectionTypesParameter for
    // why the type is captured rather than looked up.
    getSelection.parameters.push_back ({ kSelectionTypesParameter, "Element Types", ValueType::List, false });
    getSelection.outputs.push_back ({ "elements", "Elements", ValueType::List });
    getSelection.outputs.push_back ({ "count", "Count", ValueType::Integer });
    if (!registry.Register (std::move (getSelection), error))
        throw std::logic_error (error);

    // A LIST OF CAMERAS the user captures from Archicad's 3D window, one press
    // at a time. The capture node renders one frame per entry.
    //
    // ⚠️ Pure/Worker AND IT DECLARES NO GENERATION, for exactly the reason
    // the selection set does not: the node's output IS its stored parameter, so
    // evaluating it reads nothing from the host. A node that tracked the live 3D
    // window would change its answer - and dirty everything downstream of it -
    // every time the user orbited, which is the opposite of what a captured
    // camera is for. The set changes when a button is pressed and at no other
    // time.
    //
    // ⚠️ AND THE CAMERA COMES FROM ARCHICAD'S 3D WINDOW, NOT THE VIEWER.
    // A user decision, and it is what makes the loop "orbit in Archicad, press
    // Add, orbit, press Add" rather than a second navigation surface to learn.
    NodeType camera;
    camera.id = kCamera;
    camera.label = "Camera";
    camera.category = "Archicad";
    camera.description =
        "Camera positions you capture from Archicad's 3D window. Add stores where you are looking now, "
        "Restore points the 3D window back at one, Remove and Clear change the list.";
    camera.executionDomain = ExecutionDomain::Worker;
    camera.effect = EffectKind::Pure;
    camera.display = NodeDisplay::CameraSet;
    // NO DEFAULT VALUE, deliberately, exactly as the selection set has none: an
    // absent parameter already means an empty list, so a default would be a
    // second spelling of the same state and would put a list where every other
    // node's defaultValue is a scalar.
    camera.parameters.push_back ({ kCameraSetParameter, "Cameras", ValueType::List, false });
    camera.outputs.push_back ({ "cameras", "Cameras", ValueType::List });
    // ⚠️ A SEPARATE OUTPUT, INDEX-PARALLEL TO `cameras`, RATHER THAN A
    // FIELD INSIDE THEM. The two are wired to different places - a camera
    // positions the frame and a sun lights it - and a consumer that wanted only
    // the lighting would otherwise have to parse a camera to find it. Parallel
    // by construction rather than by promise: both are projected from the SAME
    // stored rows, so they cannot come out different lengths.
    camera.outputs.push_back ({ "sun", "Sun", ValueType::List });
    camera.outputs.push_back ({ "count", "Count", ValueType::Integer });
    if (!registry.Register (std::move (camera), error))
        throw std::logic_error (error);

    NodeType setSelection;
    setSelection.id = kSetSelection;
    setSelection.label = "Set Selection";
    setSelection.category = "Archicad";
    setSelection.description = "Selects the given elements in Archicad. Runs only on an explicit run.";
    setSelection.executionDomain = ExecutionDomain::ArchicadMainThread;
    // HostUiWrite, not ReadModel: this is why the node is deferred to the second
    // phase and refused on a preview.
    setSelection.effect = EffectKind::HostUiWrite;
    setSelection.generations = { GenerationDomain::Project };
    setSelection.inputs.push_back ({ "elements", "Elements", ValueType::List });
    setSelection.outputs.push_back ({ "count", "Count", ValueType::Integer });
    if (!registry.Register (std::move (setSelection), error))
        throw std::logic_error (error);

    // ONE GDL LIBRARY PART, chosen from what the project has loaded.
    //
    // ⚠️ Pure/Worker AND IT READS NOTHING, exactly like the selection set and for
    // the same reason: the choice is a thing the USER made and stored, not a
    // question about the model. Evaluating it parses its own parameter, so a
    // saved graph names its objects with no project open - which is what makes a
    // workflow portable between files.
    //
    // ⚠️ THE PARAMETER CARRIES THE PALETTE'S OWN JSON, not just a name. The
    // catalogue's header says it plainly: a document name is unique only in the
    // sense that Archicad registers the NEWEST part carrying it, so two loaded
    // libraries shipping "Chair 01" leave one of them invisible. The unID is what
    // survives a session and a library reload, so it travels with the label -
    // and it travels in the SAME shape Palette/CatalogPicker.cpp stores, so a
    // part means one thing in the palette and in the graph.
    NodeType libraryPart;
    libraryPart.id = kLibraryPart;
    libraryPart.label = "Library Part";
    libraryPart.category = "Archicad";
    libraryPart.description =
        "One GDL object chosen from the loaded libraries. Browse the folders Object Settings shows.";
    libraryPart.executionDomain = ExecutionDomain::Worker;
    libraryPart.effect = EffectKind::Pure;
    {
        ParameterSchema part { "part", "Object", ValueType::String, false, Value (std::string {}) };
        ParameterUi ui;
        ui.widget = ParameterWidget::LibraryPart;
        ui.section = "Object";
        ui.help = "The loaded libraries, in the folders Archicad's own Object Settings shows.";
        part.ui = ui;
        libraryPart.parameters.push_back (std::move (part));
    }
    // Split out for downstream use rather than left as one opaque blob: a graph
    // that wants to place this object needs the NAME, and one that wants to
    // record which object it was needs the unID. Making a consumer parse JSON out
    // of a string output would put a second parser in every such node.
    libraryPart.outputs.push_back ({ "name", "Name", ValueType::String });
    libraryPart.outputs.push_back ({ "unID", "Unique ID", ValueType::String });
    libraryPart.outputs.push_back ({ "type", "Type", ValueType::String });
    if (!registry.Register (std::move (libraryPart), error))
        throw std::logic_error (error);

    // ONE SETTING OF THE ELEMENTS GIVEN TO IT.
    //
    // ⚠️ THIS IS WHAT A PROMOTED ROW *IS*. The property browser's promote
    // action creates one of these, wires it from the source node's elements,
    // and marks it in editor metadata as docked under its host; the editor then
    // draws it as a row with an output nub instead of as a box. There is no
    // second representation and no conversion between them - see §45 of
    // HANDOFF-TAPIOCA-GraphUI-Property-Browser.md. "Convert to explicit node"
    // clears a metadata field, which is why it cannot lose a downstream link.
    //
    // ⚠️ AND IT IS ReadModel WITH A Project GENERATION, WHICH THE INSPECTOR IS
    // NOT. GraphDescribeElements answers the panel without touching the
    // evaluator, deliberately - opening a browser must not dirty a graph. This
    // node is the opposite by design: promoting a setting is asking the graph to
    // depend on it, so the answer has to go stale when the model moves.
    NodeType setting;
    setting.id = kElementSetting;
    setting.label = "Element Setting";
    setting.category = "Archicad Elements";
    setting.description = "One setting read from each element given to it, in the same order.";
    setting.executionDomain = ExecutionDomain::ArchicadMainThread;
    setting.effect = EffectKind::ReadModel;
    setting.generations = { GenerationDomain::Project };
    // Not required, for the same reason a container's input is not: this is a
    // thing you drop and wire afterwards.
    setting.inputs.push_back ({ "elements", "Elements", ValueType::List, false });
    {
        ParameterSchema which { "setting", "Setting", ValueType::String, false, Value (std::string {}) };
        ParameterUi ui;
        // ⚠️ NOT A Select, AND NOT AN optionSource EITHER. Which settings exist
        // depends on the ELEMENT TYPE, so there is no one list to enumerate and
        // no host question that would produce one. The property browser is the
        // picker; this parameter is what it writes.
        ui.widget = ParameterWidget::Text;
        ui.section = "Setting";
        ui.help =
            "The catalog id of the setting to read - promote one from the property browser rather than typing it.";
        which.ui = ui;
        setting.parameters.push_back (std::move (which));
    }
    {
        // ⚠️ THE TYPE THE PROMOTION WAS MADE AGAINST, STORED RATHER THAN
        // INFERRED. A promotion is made against one type's schema; rewiring the
        // source to a different type has to be VISIBLE rather than silently
        // producing nothing, and without the type recorded there is nothing to
        // compare the incoming elements against.
        ParameterSchema type { "elementType", "Element Type", ValueType::String, false, Value (std::string {}) };
        ParameterUi ui;
        ui.widget = ParameterWidget::ReadOnly;
        ui.section = "Setting";
        ui.help = "The element type this setting was promoted from. Empty accepts any type.";
        type.ui = ui;
        setting.parameters.push_back (std::move (type));
    }
    // ⚠️ ONE OUTPUT, FLAT, AND INDEX-PARALLEL TO THE INPUT. Not one branch per
    // element: see §43.1. An element with no answer holds an ABSENT at its
    // index rather than being dropped, because a list that compacts itself
    // re-indexes every element after the gap - which adds up and is wrong about
    // all of them.
    setting.outputs.push_back ({ "values", "Values", ValueType::List });
    if (!registry.Register (std::move (setting), error))
        throw std::logic_error (error);

    RegisterElementContainers (registry);
}

// ---------------------------------------------------------------------------
// THE CONTAINERS. One node per native element type, generated from
// ElementClassification's catalog rather than written out fifteen times.
//
// ⚠️ GENERATED, BECAUSE THE ALTERNATIVE IS FIFTEEN NEAR-IDENTICAL BLOCKS that
// drift. A Wall container whose port was called "elements" and a Slab container
// whose port was called "items" would make every graph type-specific for no
// reason a user could see, and the fifteenth block is where the typo lives.
// Adding a container is now one `container = true` in the table.
//
// ⚠️ AND THEY ARE ReadModel, NOT Pure, WHICH THE SELECTION SET IS NOT. Deciding
// whether an element is a wall means asking Archicad, so a container genuinely
// depends on host state and says so by declaring the Project generation. That is
// the difference between the two nodes: a captured set is a thing the user
// holds, a container is a question about the model, and a question has to be
// asked again when the model moves.
void RegisterElementContainers (NodeRegistry& registry)
{
    std::string error;
    for (const ElementTypeDescriptor& type : ElementTypeCatalog ()) {
        if (!type.container)
            continue;

        NodeType container;
        container.id = std::string (kContainerPrefix) + type.id;
        container.label = type.plural;
        container.category = "Archicad Elements";
        container.description = "Keeps only the " + type.plural + " out of the elements given to it.";
        container.executionDomain = ExecutionDomain::ArchicadMainThread;
        container.effect = EffectKind::ReadModel;
        container.generations = { GenerationDomain::Project };
        // NOT REQUIRED. A container is a thing you drop on the canvas and wire
        // up afterwards; an unwired one is EMPTY, and reporting a broken graph
        // for the seconds between those two acts would be noise.
        container.inputs.push_back ({ "elements", "Elements", ValueType::List, false });
        container.outputs.push_back ({ "elements", type.plural, ValueType::List });
        container.outputs.push_back ({ "count", "Count", ValueType::Integer });
        // NO BYPASS, and the registry is what settled it. Passing the input
        // through is the only reading of bypass that means anything for a node
        // whose job is to remove things - but a bypass table must feed EVERY
        // output, and `count` has no input to come from. Rather than drop the
        // count (which is the whole point of a container in a stack) or invent a
        // number for it, the type simply is not bypassable, which is the default.
        if (!registry.Register (std::move (container), error))
            throw std::logic_error (error);
    }
}

bool IsArchicadNodeType (const std::string& nodeTypeId)
{
    return nodeTypeId == kGetSelection || nodeTypeId == kSetSelection || nodeTypeId == kLibraryPart ||
           nodeTypeId == kElementSetting || nodeTypeId == kCamera || !ElementTypeOfContainerNode (nodeTypeId).empty ();
}

bool ExecuteArchicadNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                          ValueMap& outputs, std::string& error)
{
    // The selection set evaluates to what it holds. No host, no project, no
    // generation - which is what lets it run offline and stay clean while the
    // user clicks around in the model.
    if (node.nodeType == kGetSelection) {
        const auto stored = node.parameters.find (kSelectionSetParameter);
        const std::vector<ArchicadElementRef> elements =
            stored == node.parameters.end () ? std::vector<ArchicadElementRef> {} : ElementsFromValue (stored->second);
        outputs.emplace ("count", Value (static_cast<int64_t> (elements.size ())));
        outputs.emplace ("elements", ValueFromElements (elements));
        return true;
    }

    // The camera list evaluates to what it holds, like the selection set and for
    // the same reason: no host, no project, no generation, so it runs offline and
    // stays clean while the user navigates.
    if (node.nodeType == kCamera) {
        const auto stored = node.parameters.find (kCameraSetParameter);
        const std::vector<ViewCamera> cameras =
            stored == node.parameters.end () ? std::vector<ViewCamera> {} : CamerasFromValue (stored->second);
        outputs.emplace ("count", Value (static_cast<int64_t> (cameras.size ())));
        outputs.emplace ("cameras", ValueFromCameras (cameras));
        outputs.emplace ("sun", SunFromCameras (cameras));
        return true;
    }

    // Reads its own parameter and nothing else, so it runs with no host at all.
    if (node.nodeType == kLibraryPart) {
        const auto stored = node.parameters.find ("part");
        std::string encoded;
        if (stored != node.parameters.end () && stored->second.Type () == ValueType::String)
            encoded = std::get<std::string> (stored->second.DataValue ());

        // An unchosen part is EMPTY, not an error: the node is something you drop
        // and then browse from, and a red node for the seconds in between is
        // noise. A blob that will not parse is treated the same way rather than
        // failing the graph - it can only come from a hand-edited file, and
        // losing the choice is better than refusing to open the document.
        std::string name, unID, type;
        if (!encoded.empty ()) {
            const json::ParseResult parsed = json::Parse (encoded);
            if (parsed.ok) {
                const auto read = [&parsed] (const char* key, std::string& out) {
                    const json::JsonValue* found = parsed.value.Find (key);
                    if (found != nullptr)
                        found->AsString (out);
                };
                read ("name", name);
                read ("unID", unID);
                read ("type", type);
            }
        }
        outputs.emplace ("name", Value (name));
        outputs.emplace ("unID", Value (unID));
        outputs.emplace ("type", Value (type));
        return true;
    }

    if (context.archicad == nullptr || !context.archicad->IsAvailable ()) {
        // Belt and braces: the plan already refuses this, so reaching here means
        // the project closed between planning and execution.
        error = "the Archicad project is no longer available";
        return false;
    }

    const std::string containerType = ElementTypeOfContainerNode (node.nodeType);
    if (!containerType.empty ()) {
        const auto found = inputs.find ("elements");
        std::vector<ArchicadElementRef> candidates;
        if (found != inputs.end ())
            candidates = ElementsFromValue (found->second);

        // An unwired container is EMPTY, not an error. It is a thing you drop on
        // the canvas and wire up afterwards, and a node that reported a failure
        // for the ten seconds between those two acts would be noise.
        std::vector<ArchicadElementRef> kept;
        if (!candidates.empty ()) {
            std::vector<ElementDescription> descriptions;
            // ONE read for the whole input list; see IArchicadHost.
            if (!context.archicad->DescribeElements (candidates, descriptions, error))
                return false;
            for (const ElementDescription& description : descriptions) {
                // An element that could not be read is NOT kept. "I could not
                // tell what this is" must not answer "it is a wall": a container
                // that quietly admitted unreadable elements would hand them
                // downstream to nodes that assume the type.
                if (!description.available || description.elementType != containerType)
                    continue;
                kept.push_back (ArchicadElementRef { description.guid });
            }
        }

        outputs.emplace ("elements", ValueFromElements (kept));
        outputs.emplace ("count", Value (static_cast<int64_t> (kept.size ())));
        return true;
    }

    // ONE SETTING, ONE VALUE PER INPUT ELEMENT, SAME ORDER.
    if (node.nodeType == kElementSetting) {
        const auto text = [&node] (const char* id) -> std::string {
            const auto stored = node.parameters.find (id);
            if (stored == node.parameters.end () || stored->second.Type () != ValueType::String)
                return {};
            return std::get<std::string> (stored->second.DataValue ());
        };
        const std::string settingId = text ("setting");
        const std::string wantedType = text ("elementType");

        const auto found = inputs.find ("elements");
        std::vector<ArchicadElementRef> elements;
        if (found != inputs.end ())
            elements = ElementsFromValue (found->second);

        // An unchosen setting on an unwired node is EMPTY, not an error - the
        // seconds between dropping a node and configuring it are not a broken
        // graph. A setting id that names nothing is a different matter and is
        // caught below, once there are elements to fail against.
        if (elements.empty ()) {
            outputs.emplace ("values", Argument::FromItems ({}));
            return true;
        }
        if (settingId.empty ()) {
            error = "no setting has been chosen";
            return false;
        }

        std::vector<ElementDescription> descriptions;
        // ONE read for the whole list, exactly as the container does.
        if (!context.archicad->DescribeElements (elements, descriptions, error))
            return false;

        std::vector<Value> values;
        values.reserve (elements.size ());
        for (size_t i = 0; i < elements.size (); ++i) {
            // ⚠️ THE LOOP IS OVER THE INPUT, NOT OVER THE DESCRIPTIONS, and a
            // short answer leaves Absent at the tail rather than shortening the
            // output. Index parity with the elements that went IN is the whole
            // contract of this node (§43.1); a host that answered for fewer
            // elements than it was asked about must not be able to shift every
            // value one place to the left.
            if (i >= descriptions.size ()) {
                values.emplace_back ();
                continue;
            }
            const ElementDescription& description = descriptions[i];
            // Unreadable, or not the type this setting was promoted against.
            // Both are ABSENT rather than an error: one bad element in a
            // hundred should not fail the graph, and the gap is visible at the
            // index where it happened.
            if (!description.available || (!wantedType.empty () && description.elementType != wantedType)) {
                values.emplace_back ();
                continue;
            }
            const auto value = description.settings.find (settingId);
            values.emplace_back (value == description.settings.end () ? Value {} : value->second);
        }

        outputs.emplace ("values", Argument::FromItems (std::move (values)));
        return true;
    }

    if (node.nodeType == kSetSelection) {
        const auto found = inputs.find ("elements");
        if (found == inputs.end () || found->second.Type () != ValueType::List) {
            error = "the elements input must be a list";
            return false;
        }

        std::vector<ArchicadElementRef> elements;
        for (const Value& item : found->second.Items ()) {
            if (item.Type () != ValueType::ArchicadElementRef) {
                error = "the elements input must contain only Archicad element references";
                return false;
            }
            elements.push_back (std::get<ArchicadElementRef> (item.DataValue ()));
        }

        // Resolve BEFORE asking the host to change anything, and resolve them all
        // in ONE call - a per-element resolve would cross onto the host thread
        // once per element. A stale reference must fail this node, not silently
        // select the subset that still exists: a partial selection looks like a
        // correct answer and is not one.
        std::vector<Reference> references;
        references.reserve (elements.size ());
        for (const ArchicadElementRef& element : elements)
            references.push_back (Reference { ReferenceKind::Element, element.guid, {} });

        const std::vector<ReferenceResolution> resolutions = context.references->ResolveAll (references);
        for (size_t i = 0; i < resolutions.size (); ++i) {
            if (!resolutions[i].Usable ()) {
                error = "cannot select: " + resolutions[i].detail;
                return false;
            }
        }

        if (context.cancellation.IsCancelled ()) {
            error = "the evaluation was cancelled before the selection was applied";
            return false;
        }

        if (!context.archicad->SetSelection (elements, error))
            return false;
        outputs.emplace ("count", Value (static_cast<int64_t> (elements.size ())));
        return true;
    }

    error = "unknown Archicad node type: " + node.nodeType;
    return false;
}

} // namespace evp::nodegraph

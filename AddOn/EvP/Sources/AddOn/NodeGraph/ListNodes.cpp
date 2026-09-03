#include "NodeGraph/ListNodes.hpp"

#include "NodeGraph/ParameterDescriptors.hpp"
#include "NodeGraph/ValueText.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace evp::nodegraph {
namespace {

PortSchema Port (const char* id, ValueType valueType, bool required = false)
{
    return { id, id, valueType, required, false };
}

NodeType ListNode (const char* id, const char* label, const char* description)
{
    NodeType type { id, label, "List", description };
    type.effect = EffectKind::Pure;
    return type;
}

// The items of a List-access port. A port that got nothing is an EMPTY list
// rather than a missing one, which is what lets every node below answer without
// a special case: the length of nothing is zero, the reverse of nothing is
// nothing. Argument::Items() is already empty for a non-list argument, so there
// is no separate "not a list" case to handle here.
const std::vector<Value>& Items (const ValueMap& inputs, const char* portId)
{
    static const std::vector<Value> empty;
    const auto found = inputs.find (portId);
    return found == inputs.end () ? empty : found->second.Items ();
}

// A whole number from a wired port first, then from the node's own parameter.
// Ports win because a wired value is the more specific statement, which is the
// same precedence NodeInputs uses for every other typed input.
int64_t Whole (const ValueMap& inputs, const Node& node, const char* id, int64_t fallback)
{
    const auto wired = inputs.find (id);
    if (wired != inputs.end ()) {
        if (const auto* held = std::get_if<int64_t> (&wired->second.DataValue ()))
            return *held;
        // A Double arriving on an Integer port is a rounding question, not an
        // error: sliders and arithmetic both produce doubles, and refusing them
        // here would make Item unreachable from half the catalog.
        if (const auto* real = std::get_if<double> (&wired->second.DataValue ()))
            return static_cast<int64_t> (*real);
    }
    const auto parameter = node.parameters.find (id);
    if (parameter != node.parameters.end ()) {
        if (const auto* held = std::get_if<int64_t> (&parameter->second.DataValue ()))
            return *held;
    }
    return fallback;
}

// Resolves an index against a list, or returns false when it addresses nothing.
//
// ⚠️ A NEGATIVE INDEX COUNTS FROM THE END, and an out-of-range one ADDRESSES
// NOTHING rather than being clamped to an end. Clamping would answer a question
// nobody asked: asking for item 9 of a 3-item list and receiving item 2 reads
// as a real answer, and the caller has no way to tell it apart from one. The
// data model already has a state for "a site with no value" - a null item - so
// that is what an out-of-range read produces (§7.5).
bool ResolveIndex (int64_t index, size_t size, size_t& resolved)
{
    if (size == 0)
        return false;
    const int64_t count = static_cast<int64_t> (size);
    const int64_t from = index < 0 ? count + index : index;
    if (from < 0 || from >= count)
        return false;
    resolved = static_cast<size_t> (from);
    return true;
}

// The sort key of one item as a number, or false when it is not one.
//
// ⚠️ NUMBERS ONLY. A key list is what ORDERS the sort, and ordering is a
// numeric question here by decision: areas, distances, heights, indices. Text
// has an order too, but mixing the two has none, and a node that accepted both
// would have to answer "is 10 before or after \"apple\"" - so it accepts one.
// An Integer key is a number, because Integer widens to Double everywhere else.
bool SortKey (const Value& value, double& number)
{
    if (const auto* real = std::get_if<double> (&value.DataValue ())) {
        number = *real;
        return true;
    }
    if (const auto* whole = std::get_if<int64_t> (&value.DataValue ())) {
        number = static_cast<double> (*whole);
        return true;
    }
    return false;
}

// The order `keys` would be in once sorted, as indices into it.
//
// ⚠️ AN ORDER, NOT A SORTED LIST, because the caller has a second list to move
// the same way. Sorting the keys and then "sorting" the values separately would
// pair item 3 with key 3 only by luck.
//
// STABLE, so equal keys keep the order they arrived in. An unstable sort would
// let two runs over identical input produce different graphs, which is the one
// thing a solution that re-runs on every keystroke cannot afford.
bool SortOrder (const std::vector<Value>& keys, std::vector<size_t>& order, std::string& error)
{
    std::vector<double> numbers;
    numbers.reserve (keys.size ());
    for (const Value& key : keys) {
        double number = 0.0;
        if (!SortKey (key, number)) {
            error = "sort keys must be numbers; got '" + DescribeValue (key) + "'";
            return false;
        }
        numbers.push_back (number);
    }

    order.resize (keys.size ());
    for (size_t index = 0; index < keys.size (); ++index)
        order[index] = index;
    std::stable_sort (order.begin (), order.end (),
                      [&numbers] (size_t left, size_t right) { return numbers[left] < numbers[right]; });
    return true;
}

// `items` rearranged into `order`.
//
// ⚠️ A VALUES LIST SHORTER THAN THE KEYS LEAVES THE MISSING SITES ABSENT rather
// than shifting later items into them. The node's contract is that the two lists
// are the same length and position n means the same thing in both; quietly
// closing a gap would break that pairing everywhere after it while producing a
// list that still looks well formed.
std::vector<Value> Reorder (const std::vector<Value>& items, const std::vector<size_t>& order)
{
    std::vector<Value> moved;
    moved.reserve (order.size ());
    for (const size_t from : order)
        moved.push_back (from < items.size () ? items[from] : Value {});
    return moved;
}

} // namespace

void RegisterListNodes (NodeRegistry& registry)
{
    std::string error;

    NodeType length = ListNode ("list.length", "Length", "How many items one list holds.");
    length.inputs.push_back (Port ("list", ValueType::List));
    length.outputs.push_back (Port ("length", ValueType::Integer));
    if (!registry.Register (std::move (length), error))
        throw std::logic_error (error);

    // ⚠️ THE INDEX IS A PORT, NOT ONLY A PARAMETER, AND THAT IS WHAT MAKES THIS
    // NODE PLURAL FOR FREE. `list` is List access and `index` is Item access, so
    // the iteration engine walks the indices while handing the whole list to
    // each step: wiring three indices in returns three items, with no loop
    // written here. See NodeLifting.hpp.
    NodeType item = ListNode ("list.item", "Item", "One item out of a list, by position.");
    item.inputs.push_back (Port ("list", ValueType::List));
    item.inputs.push_back (Port ("index", ValueType::Integer));
    item.outputs.push_back (Port ("item", ValueType::Absent));
    ParameterSchema indexParameter { "index", "Index", ValueType::Integer, false, Value (static_cast<int64_t> (0)) };
    indexParameter.ui = CountUi ("Item", 0, "Which item. Negative counts back from the end.", -100000, 100000);
    item.parameters.push_back (std::move (indexParameter));
    if (!registry.Register (std::move (item), error))
        throw std::logic_error (error);

    NodeType reverse = ListNode ("list.reverse", "Reverse", "The same items in the opposite order.");
    reverse.inputs.push_back (Port ("list", ValueType::List));
    reverse.outputs.push_back (Port ("list", ValueType::List));
    reverse.bypassMappings.push_back ({ "list", "list" });
    if (!registry.Register (std::move (reverse), error))
        throw std::logic_error (error);

    // ⚠️ ONE NUMERIC KEY LIST ORDERS THE SORT; A CONNECTED LIST OF THE SAME
    // LENGTH IS MOVED WITH IT. That is the shape because the useful question is
    // almost never "put these numbers in order" - it is "put these WALLS in
    // order of their area", and the areas are computed by nodes upstream. A node
    // that could only sort what it compared would leave that unanswerable
    // without a bespoke sort node per kind of key.
    //
    // `values` is optional: wire keys alone and it is an ordinary sort.
    NodeType sort =
        ListNode ("list.sort", "Sort", "Puts a numeric list in order, moving a connected list along with it.");
    sort.inputs.push_back (Port ("keys", ValueType::List));
    sort.inputs.push_back (Port ("values", ValueType::List));
    sort.outputs.push_back (Port ("keys", ValueType::List));
    sort.outputs.push_back (Port ("values", ValueType::List));
    ParameterSchema descending { "descending", "Descending", ValueType::Bool, false, Value (false) };
    descending.ui = BooleanUi ("Sort", 0, "Largest first instead of smallest first.");
    sort.parameters.push_back (std::move (descending));
    if (!registry.Register (std::move (sort), error))
        throw std::logic_error (error);

    NodeType slice = ListNode ("list.slice", "Slice", "A run of items taken out of a list.");
    slice.inputs.push_back (Port ("list", ValueType::List));
    slice.outputs.push_back (Port ("list", ValueType::List));
    ParameterSchema start { "start", "Start", ValueType::Integer, false, Value (static_cast<int64_t> (0)) };
    start.ui = CountUi ("Slice", 0, "Where to start. Negative counts back from the end.", -100000, 100000);
    slice.parameters.push_back (std::move (start));
    // ⚠️ COUNT 0 MEANS "EVERYTHING FROM `start`", and that is a documented
    // convention rather than a hidden sentinel. The alternative was a default
    // that returns the empty list, which is the one answer nobody drops this
    // node on the canvas to get; a caller who genuinely wants nothing does not
    // need a node to produce it.
    ParameterSchema count { "count", "Count", ValueType::Integer, false, Value (static_cast<int64_t> (0)) };
    count.ui = CountUi ("Slice", 1, "How many items. 0 takes everything from the start position.", 0, 100000);
    slice.parameters.push_back (std::move (count));
    if (!registry.Register (std::move (slice), error))
        throw std::logic_error (error);
}

bool ExecuteListNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext&, ValueMap& outputs,
                      std::string& error)
{
    const std::vector<Value>& items = Items (inputs, "list");

    if (node.nodeType == "list.length") {
        outputs.emplace ("length", Value (static_cast<int64_t> (items.size ())));
    }
    else if (node.nodeType == "list.item") {
        size_t resolved = 0;
        if (ResolveIndex (Whole (inputs, node, "index", 0), items.size (), resolved))
            outputs.emplace ("item", items[resolved]);
        // Else nothing is emplaced, which the lift writes as a null item at this
        // site - the honest answer to "give me item 9 of three".
    }
    else if (node.nodeType == "list.reverse") {
        std::vector<Value> reversed (items.rbegin (), items.rend ());
        outputs.emplace ("list", Argument::FromItems (std::move (reversed)));
    }
    else if (node.nodeType == "list.slice") {
        size_t first = 0;
        std::vector<Value> taken;
        if (ResolveIndex (Whole (inputs, node, "start", 0), items.size (), first)) {
            const int64_t wanted = Whole (inputs, node, "count", 0);
            const size_t available = items.size () - first;
            // Clamping the COUNT is not the same as clamping the index: asking
            // for ten items and receiving the four that exist is a complete
            // answer to "take up to ten", whereas asking for item ten and
            // receiving item four is not.
            const size_t take = wanted <= 0 ? available : std::min (available, static_cast<size_t> (wanted));
            taken.assign (items.begin () + static_cast<std::ptrdiff_t> (first),
                          items.begin () + static_cast<std::ptrdiff_t> (first + take));
        }
        outputs.emplace ("list", Argument::FromItems (std::move (taken)));
    }
    else if (node.nodeType == "list.sort") {
        const std::vector<Value>& keys = Items (inputs, "keys");
        const std::vector<Value>& values = Items (inputs, "values");
        std::vector<size_t> order;
        if (!SortOrder (keys, order, error))
            return false;

        const auto descending = node.parameters.find ("descending");
        const bool reversed = descending != node.parameters.end () &&
                              std::get_if<bool> (&descending->second.DataValue ()) != nullptr &&
                              std::get<bool> (descending->second.DataValue ());
        if (reversed) {
            // Reversing the ORDER, not sorting with a flipped comparator, so
            // that a descending sort is exactly the ascending one read
            // backwards - including how it broke ties.
            std::reverse (order.begin (), order.end ());
        }

        outputs.emplace ("keys", Argument::FromItems (Reorder (keys, order)));
        outputs.emplace ("values", Argument::FromItems (Reorder (values, order)));
    }
    else {
        error = "unknown list node type: " + node.nodeType;
        return false;
    }

    error.clear ();
    return true;
}

bool IsListNodeType (const std::string& nodeTypeId)
{
    // Prefix-matched like every other family. `makeList` and `scaleList` are
    // deliberately NOT in it: they predate this family and keep their names, so
    // no saved graph changes meaning because a family arrived.
    return nodeTypeId.rfind ("list.", 0) == 0;
}

} // namespace evp::nodegraph

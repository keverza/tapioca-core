#include "NodeGraph/BuiltinNodes.hpp"

#include "NodeGraph/ValueText.hpp"

#include <stdexcept>
#include <tuple>

namespace evp::nodegraph {
namespace {

PortSchema Port (const char* id, ValueType valueType, bool multiple = false)
{
    return { id, id, valueType, true, multiple };
}

NodeType PureNode (const char* id, const char* label, const char* description)
{
    return { id, label, "Core", description };
}

double Number (const Value& value)
{
    return std::get<double> (value.DataValue ());
}

} // namespace

NodeRegistry MakeBuiltinNodeRegistry ()
{
    NodeRegistry registry;
    std::string error;

    NodeType number = PureNode ("number", "Number", "Provides a numeric constant.");
    number.outputs.push_back (Port ("value", ValueType::Double));
    number.parameters.push_back ({ "value", "Value", ValueType::Double, true });
    if (!registry.Register (std::move (number), error))
        throw std::logic_error (error);

    for (const auto& [id, label, description] : { std::tuple { "add", "Add", "Adds two numbers." },
                                                  std::tuple { "multiply", "Multiply", "Multiplies two numbers." } }) {
        NodeType arithmetic = PureNode (id, label, description);
        arithmetic.inputs = { Port ("left", ValueType::Double), Port ("right", ValueType::Double) };
        arithmetic.outputs.push_back (Port ("value", ValueType::Double));
        if (!registry.Register (std::move (arithmetic), error))
            throw std::logic_error (error);
    }

    NodeType list = PureNode ("makeList", "Make List", "Collects connected numbers in connection order.");
    list.inputs.push_back (Port ("items", ValueType::Double, true));
    list.outputs.push_back (Port ("value", ValueType::List));
    if (!registry.Register (std::move (list), error))
        throw std::logic_error (error);

    NodeType scale = PureNode ("scaleList", "Scale List", "Multiplies every number in a list.");
    scale.inputs = { Port ("list", ValueType::List), Port ("factor", ValueType::Double) };
    scale.outputs.push_back (Port ("value", ValueType::List));
    if (!registry.Register (std::move (scale), error))
        throw std::logic_error (error);

    // The Grasshopper-panel equivalent: wire anything into it and read what came
    // out. Its input is declared Absent, which the edit rules read as "any type",
    // so one node inspects every value the runtime has rather than there being a
    // panel per type.
    NodeType panel = PureNode ("panel", "Panel", "Shows whatever is wired into it as readable text.");
    panel.category = "Inspect";
    panel.inputs.push_back ({ "value", "Value", ValueType::Absent, true, false });
    panel.outputs.push_back ({ "text", "Text", ValueType::String });
    panel.outputs.push_back ({ "lines", "Lines", ValueType::List });
    panel.outputs.push_back ({ "count", "Count", ValueType::Integer });
    panel.outputs.push_back ({ "summary", "Summary", ValueType::String });
    if (!registry.Register (std::move (panel), error))
        throw std::logic_error (error);

    NodeType watch = PureNode ("watch", "Watch", "Reports a list without changing it.");
    watch.inputs.push_back (Port ("value", ValueType::List));
    watch.outputs.push_back (Port ("value", ValueType::List));
    if (!registry.Register (std::move (watch), error))
        throw std::logic_error (error);
    return registry;
}

bool ExecuteBuiltinNode (const Node& node, const ValueMap& inputs, const NodeExecutionContext& context,
                         ValueMap& outputs, std::string& error)
{
    // Pure nodes read nothing outside their inputs; that is what makes them pure.
    (void) context;

    if (node.nodeType == "number")
        outputs.emplace ("value", node.parameters.at ("value"));
    else if (node.nodeType == "add")
        outputs.emplace ("value", Value (Number (inputs.at ("left")) + Number (inputs.at ("right"))));
    else if (node.nodeType == "multiply")
        outputs.emplace ("value", Value (Number (inputs.at ("left")) * Number (inputs.at ("right"))));
    else if (node.nodeType == "makeList")
        outputs.emplace ("value", inputs.at ("items"));
    else if (node.nodeType == "scaleList") {
        Value::List scaled;
        const double factor = Number (inputs.at ("factor"));
        for (const Value& item : std::get<Value::List> (inputs.at ("list").DataValue ()))
            scaled.emplace_back (Number (item) * factor);
        outputs.emplace ("value", Value (std::move (scaled)));
    }
    else if (node.nodeType == "panel") {
        const Value& value = inputs.at ("value");
        const std::vector<std::string> lines = FormatValueLines (value);
        Value::List lineValues;
        lineValues.reserve (lines.size ());
        std::string joined;
        for (size_t i = 0; i < lines.size (); ++i) {
            lineValues.emplace_back (lines[i]);
            if (i != 0)
                joined += "\n";
            joined += lines[i];
        }
        // Both shapes, because a client should not have to split a string to
        // render a list, nor join a list to show one line.
        outputs.emplace ("text", Value (joined));
        outputs.emplace ("lines", Value (std::move (lineValues)));
        outputs.emplace ("count", Value (static_cast<int64_t> (value.Type () == ValueType::List
                                                                   ? std::get<Value::List> (value.DataValue ()).size ()
                                                                   : 1)));
        outputs.emplace ("summary", Value (DescribeValue (value)));
    }
    else if (node.nodeType == "watch")
        outputs.emplace ("value", inputs.at ("value"));
    else {
        error = "unknown built-in node type: " + node.nodeType;
        return false;
    }
    error.clear ();
    return true;
}

} // namespace evp::nodegraph

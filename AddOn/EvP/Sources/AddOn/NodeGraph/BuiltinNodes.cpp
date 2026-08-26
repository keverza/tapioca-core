#include "NodeGraph/BuiltinNodes.hpp"

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

    NodeType watch = PureNode ("watch", "Watch", "Reports a list without changing it.");
    watch.inputs.push_back (Port ("value", ValueType::List));
    watch.outputs.push_back (Port ("value", ValueType::List));
    if (!registry.Register (std::move (watch), error))
        throw std::logic_error (error);
    return registry;
}

bool ExecuteBuiltinNode (const Node& node, const ValueMap& inputs, ValueMap& outputs, std::string& error)
{
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

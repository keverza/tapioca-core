#include "NodeGraph/NodeInputs.hpp"

#include <cstdint>

namespace evp::nodegraph {

bool IsNumericValue (const Argument& value)
{
    return value.Type () == ValueType::Double || value.Type () == ValueType::Integer;
}
double ParameterNumber (const Node& node, const char* id, double fallback)
{
    const auto found = node.parameters.find (id);
    return found == node.parameters.end () ? fallback : AnyNumber (found->second, fallback);
}
// A number from the wire if there is one, else the typed-in parameter.
//
// ⚠️ EITHER NUMERIC TYPE, VIA AnyNumber. A parameter authored as an Integer - a
// segment count, a decimal-place count - reaching a reader that only accepted
// Double silently returned the FALLBACK, which is the worst shape of bug on this
// path: the node runs, produces a plausible result, and ignores what the user
// typed.
double ScalarFrom (const ValueMap& inputs, const Node& node, const char* id, double fallback)
{
    const auto wired = inputs.find (id);
    if (wired != inputs.end () && IsNumericValue (wired->second))
        return AnyNumber (wired->second, fallback);
    const auto stored = node.parameters.find (id);
    if (stored != node.parameters.end () && IsNumericValue (stored->second))
        return AnyNumber (stored->second, fallback);
    return fallback;
}
bool BoolFrom (const ValueMap& inputs, const Node& node, const char* id, bool fallback)
{
    const auto wired = inputs.find (id);
    if (wired != inputs.end () && wired->second.Type () == ValueType::Bool)
        return std::get<bool> (wired->second.DataValue ());
    const auto stored = node.parameters.find (id);
    if (stored != node.parameters.end () && stored->second.Type () == ValueType::Bool)
        return std::get<bool> (stored->second.DataValue ());
    return fallback;
}
// The stored parameter, verbatim, for the nodes whose whole job is to hold one.
Argument StoredOr (const Node& node, const char* id, Argument fallback)
{
    const auto found = node.parameters.find (id);
    if (found == node.parameters.end () || found->second.Type () == ValueType::Absent)
        return fallback;
    return found->second;
}
// Degrees in, radians out. See AngleUi: every angle in this catalog is AUTHORED
// in degrees, and this is the one place that converts.
double Radians (double degrees)
{
    return degrees * 3.14159265358979323846 / 180.0;
}
// A number wherever it came from. A slider's bound is authored as a Double and
// its decimals as an Integer, and both reach ExecuteBuiltinNode as parameters -
// so the one place that reads them must accept either rather than throwing a
// bad_variant_access at the user.
double AnyNumber (const Argument& value, double fallback)
{
    if (value.Type () == ValueType::Double)
        return std::get<double> (value.DataValue ());
    if (value.Type () == ValueType::Integer)
        return static_cast<double> (std::get<int64_t> (value.DataValue ()));
    return fallback;
}
// A Point3 wherever it came from: a wired point wins over the typed-in one, the
// same precedence every other input here follows.
Point3 Point3From (const ValueMap& inputs, const Node& node, const char* id)
{
    const auto wired = inputs.find (id);
    if (wired != inputs.end () && wired->second.Type () == ValueType::Point3)
        return std::get<Point3> (wired->second.DataValue ());
    const auto stored = node.parameters.find (id);
    if (stored != node.parameters.end () && stored->second.Type () == ValueType::Point3)
        return std::get<Point3> (stored->second.DataValue ());
    return Point3 { 0.0, 0.0, 0.0 };
}

} // namespace evp::nodegraph

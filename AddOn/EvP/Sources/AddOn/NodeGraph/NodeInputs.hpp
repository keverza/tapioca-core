#ifndef EVP_NODEGRAPH_NODEINPUTS_HPP
#define EVP_NODEGRAPH_NODEINPUTS_HPP

// Reading a node body's arguments.
//
// ⚠️ A WIRED INPUT WINS OVER THE TYPED-IN ONE, EVERY TIME, AND THAT PRECEDENCE
// IS STATED ONCE HERE. It is the rule the whole catalog rests on: a node holds
// its own values so it works the moment it is placed, and its ports exist so
// something upstream can take over. Every family reads its arguments through
// these, so no node can implement that backwards.
//
// ⚠️ AND A NUMBER IS EITHER NUMERIC TYPE. A parameter authored as an Integer - a
// segment count, a decimal-place count - reaching a reader that accepted only
// Double silently returned the FALLBACK: the node ran, produced a plausible
// result, and ignored what the user typed. That is the worst shape of bug on
// this path, and it is why these are shared rather than rewritten per family.

#include "NodeGraph/Evaluator.hpp"
#include "NodeGraph/Graph.hpp"

namespace evp::nodegraph {

bool IsNumericValue (const Argument& value);
double AnyNumber (const Argument& value, double fallback);
double ParameterNumber (const Node& node, const char* id, double fallback);
double ScalarFrom (const ValueMap& inputs, const Node& node, const char* id, double fallback);
bool BoolFrom (const ValueMap& inputs, const Node& node, const char* id, bool fallback);
Point3 Point3From (const ValueMap& inputs, const Node& node, const char* id);
Argument StoredOr (const Node& node, const char* id, Argument fallback);
double Radians (double degrees);

} // namespace evp::nodegraph

#endif

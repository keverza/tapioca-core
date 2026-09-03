#ifndef EVP_NODEGRAPH_VALUETEXT_HPP
#define EVP_NODEGRAPH_VALUETEXT_HPP

// Values, as a person reads them.
//
// One renderer for every readout: the `text` and `summary` that EVERY output
// carries in GraphGetNodeResults, which is what a Panel node's rows and a
// property browser's rows are both built from. Sharing it means what a panel
// shows and what an inspector shows cannot disagree, and it means a new value
// type becomes readable everywhere at once.
//
// Bounded on purpose. A debugging aid that renders a million-item list is not a
// debugging aid; it is a hang. Both entry points truncate and SAY they
// truncated, because a silently shortened list is worse than no list.

#include "NodeGraph/Value.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace evp::nodegraph {

// One line, whatever the value. A list renders inline as [a, b, c]; nesting past
// `maxDepth` renders as its shape rather than its contents. A plain Value
// converts implicitly, so a scalar caller passes one exactly as before.
std::string FormatValue (const Argument& value, size_t maxDepth = 3, size_t maxItems = 24);

// One line per top-level item, for a caller rendering a whole list as text. A
// non-list value yields a single line. Truncation appends one final line saying
// how many were left out.
std::vector<std::string> FormatValueLines (const Argument& value, size_t maxLines = 500);

// A short type-and-size label - "List of 12", "Mesh", "Double" - for a header or
// a collapsed row.
std::string DescribeValue (const Argument& value);

} // namespace evp::nodegraph

#endif

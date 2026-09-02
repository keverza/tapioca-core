#ifndef EVP_NODEGRAPH_ELEMENTREADER_HPP
#define EVP_NODEGRAPH_ELEMENTREADER_HPP

// The ACAPI half of the element classification, behind a DevKit-free
// declaration.
//
// ⚠️ ITS OWN TRANSLATION UNIT ON PURPOSE. ArchicadHostImpl.cpp's header explains
// why the ACAPI surface of the graph runtime is kept small and dull; a
// transcription of a dozen element structs is neither small nor interesting, and
// putting it there would double that file and bury the two hazards its header
// warns about. This is the second and last ACAPI translation unit in the
// runtime, and it does exactly one thing.
//
// ⚠️ MAIN THREAD. Every ACAPI call it makes runs inside MainThreadGate, and the
// whole batch crosses ONCE - see IArchicadHost::DescribeElements.

#include "NodeGraph/ElementClassification.hpp"
#include "NodeGraph/Value.hpp"

#include <string>
#include <vector>

namespace evp::nodegraph {

bool ReadElementDescriptions (const std::vector<ArchicadElementRef>& elements,
                              std::vector<ElementDescription>& descriptions, std::string& error);

} // namespace evp::nodegraph

#endif
